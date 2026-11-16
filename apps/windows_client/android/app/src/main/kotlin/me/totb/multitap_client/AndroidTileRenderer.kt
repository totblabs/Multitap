package me.totb.multitap_client

import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.os.Handler
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/**
 * ToTBSeamless lossless tiles on the GPU, the Android counterpart of the
 * Windows client's GPU overlay (windows/runner/gpu_lossless.h).
 *
 * The decoder renders into a SurfaceTexture of our own ([decoderSurface]);
 * each latched frame is drawn into the Flutter texture's Surface with one
 * shader pass: the video, and where [LosslessCompositor.presentMask] says a
 * tile is present, the tile's exact pixels from an RGBA texture of the output's
 * size. Tile pixels and the mask reach the GPU only when tiles arrive or
 * change. The output ([width] x [height], LosslessOutput) is the video's own
 * size, where a frame without tiles is a plain copy of the video, or for a
 * downscaled stream the larger source size, where the video is upscaled
 * bilinearly (pixel centres aligned, as rgba_upscale.hpp on Windows) and a
 * tile is still shown at its exact source pixels. Tile pixel centres always
 * line up with the output's (nearest sampling), as the Windows client shows them.
 *
 * All calls on one thread (the engine's decoder worker), which holds the EGL
 * context current while it exists. The shared default EGL display is never
 * terminated: Flutter's own renderer uses it.
 */
internal class AndroidTileRenderer private constructor(
    val videoWidth: Int,
    val videoHeight: Int,
    val width: Int,
    val height: Int,
    private val display: EGLDisplay,
    private val context: EGLContext,
    private val window: EGLSurface,
) {
    private var program = 0
    private var videoTexture = 0
    private var tileTexture = 0
    private var maskTexture = 0
    private var surfaceTexture: SurfaceTexture? = null
    /** The decoder's output surface. */
    var decoderSurface: Surface? = null
        private set
    private val columns = LosslessTiles.columns(width)
    private val rows = LosslessTiles.rows(height)
    private val transform = FloatArray(16)
    private var maskShown = -1
    private val maskBuffer = ByteBuffer.allocateDirect(columns * rows).order(ByteOrder.nativeOrder())
    private val tileBuffer = ByteBuffer.allocateDirect(LosslessTiles.TILE * LosslessTiles.TILE * 4).order(ByteOrder.nativeOrder())
    private val quad: FloatBuffer = ByteBuffer.allocateDirect(16 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer().apply {
        // x, y (clip), u, v (v = 0 at the picture's top row)
        put(floatArrayOf(-1f, -1f, 0f, 1f, 1f, -1f, 1f, 1f, -1f, 1f, 0f, 0f, 1f, 1f, 1f, 0f)); position(0)
    }
    private var position = -1
    private var uv = -1
    private var uTransform = -1
    private var uMaskScale = -1
    private var uTiles = -1
    /** A decoded frame was latched: the video texture holds a picture to redraw. */
    var latched = false
        private set
    var released = false
        private set

    companion object {
        /**
         * A renderer drawing [videoWidth] x [videoHeight] video into [output]
         * (the Flutter texture's Surface, whose buffers the caller sized
         * [width] x [height]); its decoder surface calls [onFrame] on
         * [handler]'s thread for every frame the decoder renders. Null when
         * this device cannot (no EGL/GLES 2 window, a texture or viewport
         * too large): the decoder then renders to [output] directly, without tiles.
         */
        fun create(output: Surface, videoWidth: Int, videoHeight: Int, width: Int, height: Int, handler: Handler,
            onFrame: (AndroidTileRenderer) -> Unit): AndroidTileRenderer? {
            if (!LosslessTiles.validFrame(width, height) || !LosslessTiles.validFrame(videoWidth, videoHeight) ||
                videoWidth > width || videoHeight > height) return null
            val display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
            if (display == EGL14.EGL_NO_DISPLAY) return null
            val version = IntArray(2)
            if (!EGL14.eglInitialize(display, version, 0, version, 1)) return null
            val configs = arrayOfNulls<EGLConfig>(1)
            val count = IntArray(1)
            val attributes = intArrayOf(
                EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT, EGL14.EGL_NONE)
            if (!EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0) || count[0] < 1) return null
            val config = configs[0] ?: return null
            val context = EGL14.eglCreateContext(display, config, EGL14.EGL_NO_CONTEXT, intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0)
            if (context == null || context == EGL14.EGL_NO_CONTEXT) return null
            val window = try { EGL14.eglCreateWindowSurface(display, config, output, intArrayOf(EGL14.EGL_NONE), 0) } catch (_: Exception) { null }
            if (window == null || window == EGL14.EGL_NO_SURFACE) { EGL14.eglDestroyContext(display, context); return null }
            val renderer = AndroidTileRenderer(videoWidth, videoHeight, width, height, display, context, window)
            return try {
                if (renderer.setUp(handler, onFrame)) renderer else { renderer.release(); null }
            } catch (_: Exception) {
                renderer.release(); null
            }
        }

        private const val VERTEX = """
attribute vec2 aPosition;
attribute vec2 aUv;
uniform mat4 uTransform;
uniform vec2 uMaskScale;
varying highp vec2 vVideo;
varying highp vec2 vUv;
varying highp vec2 vMask;
void main() {
  gl_Position = vec4(aPosition, 0.0, 1.0);
  vVideo = (uTransform * vec4(aUv.x, 1.0 - aUv.y, 0.0, 1.0)).xy;
  vUv = aUv;
  vMask = aUv * uMaskScale;
}"""

        // The texture coordinates go to texture2D unchanged (no arithmetic in
        // the fragment shader), so mediump-only GPUs sample exact texels too.
        // (A fragment input's precision need not match the vertex output's.)
        private const val FRAGMENT = """#extension GL_OES_EGL_image_external : require
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform samplerExternalOES uVideo;
uniform sampler2D uTileLayer;
uniform sampler2D uMask;
uniform float uTiles;
varying vec2 vVideo;
varying vec2 vUv;
varying vec2 vMask;
void main() {
  vec3 video = texture2D(uVideo, vVideo).rgb;
  vec3 tile = texture2D(uTileLayer, vUv).rgb;
  bool lossless = uTiles > 0.5 && texture2D(uMask, vMask).r > 0.5;
  gl_FragColor = vec4(lossless ? tile : video, 1.0);
}"""
    }

    private fun setUp(handler: Handler, onFrame: (AndroidTileRenderer) -> Unit): Boolean {
        if (!EGL14.eglMakeCurrent(display, window, window, context)) return false
        // A queued picture replaces an older one Flutter has not latched yet; a swap never waits.
        EGL14.eglSwapInterval(display, 0)
        val size = IntArray(1)
        val viewport = IntArray(2)
        GLES20.glGetIntegerv(GLES20.GL_MAX_TEXTURE_SIZE, size, 0)
        GLES20.glGetIntegerv(GLES20.GL_MAX_VIEWPORT_DIMS, viewport, 0)
        if (!LosslessOutput.fitsGpu(width, height, videoWidth, videoHeight, size[0], viewport[0], viewport[1])) return false
        program = link() ?: return false
        position = GLES20.glGetAttribLocation(program, "aPosition")
        uv = GLES20.glGetAttribLocation(program, "aUv")
        uTransform = GLES20.glGetUniformLocation(program, "uTransform")
        uMaskScale = GLES20.glGetUniformLocation(program, "uMaskScale")
        uTiles = GLES20.glGetUniformLocation(program, "uTiles")
        if (position < 0 || uv < 0) return false
        val names = IntArray(3)
        GLES20.glGenTextures(3, names, 0)
        videoTexture = names[0]; tileTexture = names[1]; maskTexture = names[2]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, videoTexture)
        parameters(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_LINEAR)
        GLES20.glPixelStorei(GLES20.GL_UNPACK_ALIGNMENT, 1)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tileTexture)
        parameters(GLES20.GL_TEXTURE_2D, GLES20.GL_NEAREST)
        GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, width, height, 0, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, maskTexture)
        parameters(GLES20.GL_TEXTURE_2D, GLES20.GL_NEAREST)
        GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_LUMINANCE, columns, rows, 0, GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE,
            ByteBuffer.allocateDirect(columns * rows))
        if (GLES20.glGetError() != GLES20.GL_NO_ERROR) return false
        GLES20.glUseProgram(program)
        GLES20.glUniform1i(GLES20.glGetUniformLocation(program, "uVideo"), 0)
        GLES20.glUniform1i(GLES20.glGetUniformLocation(program, "uTileLayer"), 1)
        GLES20.glUniform1i(GLES20.glGetUniformLocation(program, "uMask"), 2)
        GLES20.glUniform2f(uMaskScale, width.toFloat() / (columns * LosslessTiles.TILE), height.toFloat() / (rows * LosslessTiles.TILE))
        val texture = SurfaceTexture(videoTexture)
        surfaceTexture = texture
        texture.setDefaultBufferSize(videoWidth, videoHeight)
        texture.setOnFrameAvailableListener({ if (!released) onFrame(this) }, handler)
        decoderSurface = Surface(texture)
        return true
    }

    private fun parameters(target: Int, filter: Int) {
        GLES20.glTexParameteri(target, GLES20.GL_TEXTURE_MIN_FILTER, filter)
        GLES20.glTexParameteri(target, GLES20.GL_TEXTURE_MAG_FILTER, filter)
        GLES20.glTexParameteri(target, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(target, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
    }

    private fun link(): Int? {
        fun shader(type: Int, source: String): Int? {
            val shader = GLES20.glCreateShader(type)
            GLES20.glShaderSource(shader, source)
            GLES20.glCompileShader(shader)
            val status = IntArray(1)
            GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0)
            if (status[0] == 0) { GLES20.glDeleteShader(shader); return null }
            return shader
        }
        val vertex = shader(GLES20.GL_VERTEX_SHADER, VERTEX) ?: return null
        val fragment = shader(GLES20.GL_FRAGMENT_SHADER, FRAGMENT) ?: run { GLES20.glDeleteShader(vertex); return null }
        val program = GLES20.glCreateProgram()
        GLES20.glAttachShader(program, vertex)
        GLES20.glAttachShader(program, fragment)
        GLES20.glLinkProgram(program)
        GLES20.glDeleteShader(vertex)
        GLES20.glDeleteShader(fragment)
        val status = IntArray(1)
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, status, 0)
        if (status[0] == 0) { GLES20.glDeleteProgram(program); return null }
        return program
    }

    /** Latches the newest frame the decoder rendered; its timestamp in nanoseconds. */
    fun latch(): Long {
        val texture = surfaceTexture ?: throw IllegalStateException("released")
        texture.updateTexImage()
        texture.getTransformMatrix(transform)
        latched = true
        return texture.timestamp
    }

    /** One decoded tile (a [TileSink] call) into the tile texture. */
    fun upload(x: Int, y: Int, width: Int, height: Int, rgba: ByteArray) {
        if (released) return
        tileBuffer.clear(); tileBuffer.put(rgba, 0, width * height * 4); tileBuffer.flip()
        GLES20.glActiveTexture(GLES20.GL_TEXTURE1)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tileTexture)
        GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, x, y, width, height, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, tileBuffer)
    }

    /**
     * Draws the latched picture with the present tiles of [compositor] (none
     * when null or poisoned) to the Flutter texture. False when the texture's
     * surface no longer takes pictures.
     */
    fun draw(compositor: LosslessCompositor?): Boolean {
        if (released || !latched) return false
        val tiles = compositor != null && !compositor.poisoned && compositor.presentTiles > 0
        GLES20.glViewport(0, 0, width, height)
        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, videoTexture)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE1)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tileTexture)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE2)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, maskTexture)
        if (tiles && maskShown != compositor!!.maskVersion) {
            maskBuffer.clear(); maskBuffer.put(compositor.presentMask); maskBuffer.flip()
            GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, columns, rows, GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, maskBuffer)
            maskShown = compositor.maskVersion
        }
        GLES20.glUniform1f(uTiles, if (tiles) 1f else 0f)
        GLES20.glUniformMatrix4fv(uTransform, 1, false, transform, 0)
        quad.position(0)
        GLES20.glVertexAttribPointer(position, 2, GLES20.GL_FLOAT, false, 16, quad)
        GLES20.glEnableVertexAttribArray(position)
        quad.position(2)
        GLES20.glVertexAttribPointer(uv, 2, GLES20.GL_FLOAT, false, 16, quad)
        GLES20.glEnableVertexAttribArray(uv)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        return EGL14.eglSwapBuffers(display, window)
    }

    /** After the decoder that renders into [decoderSurface] was released. */
    fun release() {
        if (released) return
        released = true
        try { surfaceTexture?.setOnFrameAvailableListener(null) } catch (_: Exception) {}
        try { decoderSurface?.release() } catch (_: Exception) {}
        try { surfaceTexture?.release() } catch (_: Exception) {}
        decoderSurface = null; surfaceTexture = null
        try {
            if (EGL14.eglGetCurrentContext() == context) {
                if (program != 0) GLES20.glDeleteProgram(program)
                GLES20.glDeleteTextures(3, intArrayOf(videoTexture, tileTexture, maskTexture), 0)
            }
        } catch (_: Exception) {}
        try { EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT) } catch (_: Exception) {}
        try { EGL14.eglDestroySurface(display, window) } catch (_: Exception) {}
        try { EGL14.eglDestroyContext(display, context) } catch (_: Exception) {}
    }
}
