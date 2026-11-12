package me.totb.multitap_client

import android.media.AudioFormat
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * audio-opus-v1 playback: the platform's own Opus decoder (MediaCodec
 * "audio/opus", libopus inside, in every Android since 5.0) in asynchronous
 * mode with its callbacks on the media worker [worker]. The worker never
 * waits on it: the video decoder's callbacks share that thread. Listing,
 * creating, configuring, starting and releasing decoders run on [setup]
 * instead (AndroidOpusPipe says why); everything else is the pipe's, on the
 * worker. Each 10 ms packet goes in as one input with its PTS; its PCM comes
 * back as one output, and [AndroidOpusStream] puts it behind the concealment
 * of any packet lost before it. Every piece goes to [emit] (pcm, bytes, PTS,
 * arrival) for the same track path as PCM packets.
 *
 * Worker thread only, except where marked.
 */
internal class AndroidOpusDecoder(
    private val worker: Handler,
    private val setup: Handler,
    emit: (ByteArray, Int, Long, Long) -> Unit,
) {
    // The decoder's own output (float stereo of a 20 ms packet at most) and the s16 stereo the track plays.
    private val raw = ByteArray(AndroidOpus.FRAME_BYTES * 4)
    private val pcm = ByteArray(AndroidOpus.FRAME_BYTES * 2)
    private val pipe = AndroidOpusPipe(object : AndroidOpusPipe.Codecs<MediaCodec> {
        override fun open(attempt: AndroidOpusPipe.Attempt<MediaCodec>) = setup.post { make(attempt) }
        override fun input(codec: MediaCodec, index: Int, bytes: ByteArray, offset: Int, length: Int, pts: Long) {
            val buffer = codec.getInputBuffer(index) ?: throw IllegalStateException()
            buffer.clear()
            buffer.put(bytes, offset, length)
            codec.queueInputBuffer(index, 0, length, pts, 0)
        }
        override fun retire(codec: MediaCodec) { if (!setup.post { release(codec) }) release(codec) }
    }, emit)

    val stream get() = pipe.stream
    /** No decoder could be made (or kept): this stream has no sound (and the app asks for PCM from now on). */
    val failed get() = pipe.failed.also { if (it) unusableThisRun = true }
    /** The decoder in use ("" before one was handed over). */
    val name get() = pipe.name

    /**
     * audio-path diagnostics (worker): the output format of the decoder in
     * use as it stated it (0 before one did), and whether that decoder is the
     * platform's own software one (null before one was handed over).
     */
    var outputRate = 0
        private set
    var outputChannels = 0
        private set
    var outputFloat = false
        private set
    val software: Boolean? get() = pipe.name.takeIf { it.isNotEmpty() }?.let { it in softwareNames }

    /** At the stream's start: a decoder is asked for before the first packet (made on [setup]). */
    fun prepare() = pipe.prepare()

    /** One audio-opus-v1 payload ([length] bytes at [offset], already valid), its PTS and arrival. */
    fun packet(bytes: ByteArray, offset: Int, length: Int, pts: Long, accepted: Long) {
        pipe.packet(bytes, offset, length, pts, accepted)
        if (pipe.failed) unusableThisRun = true
    }

    fun close() = pipe.close()

    /** [setup]: the first decoder in trial order that starts, handed to the worker. */
    private fun make(attempt: AndroidOpusPipe.Attempt<MediaCodec>) {
        val candidates = names().filter { it !in attempt.skip }
        var made: MediaCodec? = null
        var madeName = ""
        // Each decoder with 16-bit output asked for, then with the plain format
        // an Ogg/WebM source gives it (a decoder that refuses the key still decodes).
        search@ for (candidate in candidates) for (encodingKey in listOf(true, false)) {
            val next = try { MediaCodec.createByCodecName(candidate) } catch (_: Exception) { continue@search }
            try {
                // Its callbacks go to the worker from the start; the pipe keeps
                // the input buffers it offers before it is handed over.
                next.setCallback(Callbacks(attempt), worker)
                next.configure(format(encodingKey), null, null, 0)
                next.start()
            } catch (_: Exception) {
                release(next)
                continue
            }
            made = next
            madeName = candidate
            break@search
        }
        val codec = made
        if (!worker.post { pipe.opened(attempt, codec, madeName, candidates.isEmpty()) } && codec != null) release(codec)
    }

    private fun format(encodingKey: Boolean): MediaFormat =
        MediaFormat.createAudioFormat(AndroidOpus.MIME, AndroidOpus.RATE, AndroidOpus.CHANNELS).apply {
            val csd = AndroidOpus.codecSpecificData(ByteOrder.nativeOrder() == ByteOrder.LITTLE_ENDIAN)
            for ((index, data) in csd.withIndex()) setByteBuffer("csd-$index", ByteBuffer.wrap(data))
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, AndroidOpus.MAX_PACKET)
            // 16-bit is only the default: asked for in so many words. What the
            // decoder really puts out is read in onOutputFormatChanged and
            // converted (AndroidOpus.outputShape), so a decoder that still gives
            // float or mono plays instead of going silent.
            if (encodingKey) setInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_16BIT)
        }

    private inner class Callbacks(private val attempt: AndroidOpusPipe.Attempt<MediaCodec>) : MediaCodec.Callback() {
        // What the configured format promises until the decoder states its own.
        private var shape = PcmShape(AndroidOpus.CHANNELS, float = false)

        override fun onInputBufferAvailable(codec: MediaCodec, index: Int) = pipe.inputAvailable(attempt, index)

        override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
            if (!pipe.current(attempt)) return
            var size = 0
            try {
                if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0 && info.size in 1..raw.size) {
                    val buffer = codec.getOutputBuffer(index)
                    if (buffer != null) {
                        buffer.position(info.offset)
                        buffer.get(raw, 0, info.size)
                        size = AndroidOpus.toStereo16(raw, info.size, shape, pcm).coerceAtLeast(0)
                    }
                }
                codec.releaseOutputBuffer(index, false)
            } catch (_: Exception) {
                pipe.codecFailed(attempt)
                return
            }
            pipe.decoded(attempt, info.presentationTimeUs, pcm, size)
        }

        override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
            if (!pipe.current(attempt)) return
            val stated = try {
                val rate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                val channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                val encoding = if (format.containsKey(MediaFormat.KEY_PCM_ENCODING)) format.getInteger(MediaFormat.KEY_PCM_ENCODING) else null
                outputRate = rate; outputChannels = channels; outputFloat = encoding == AudioFormat.ENCODING_PCM_FLOAT
                AndroidOpus.outputShape(rate, channels, encoding)
            } catch (_: Exception) { null }
            if (stated == null) pipe.unusable(attempt) else shape = stated
        }

        override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) = pipe.codecFailed(attempt)
    }

    companion object {
        @Volatile private var listed: List<String>? = null
        @Volatile private var softwareNames: Set<String> = emptySet()

        /**
         * An Opus stream of this app run had no usable decoder (none listed,
         * none made or kept, no output format that plays). The Dart side then
         * asks for PCM (audio-opus-v1 not requested) from the next connection
         * on, so the sound comes back instead of staying silent on this device.
         */
        @Volatile var unusableThisRun = false

        /**
         * Names in trial order (AndroidOpus.decoderOrder); empty when the
         * platform list cannot be read. The first call reads the platform's
         * codec list (slow): AndroidMedia makes it on its setup thread at
         * start, before any stream asks.
         */
        fun names(): List<String> = listed ?: try {
            val candidates = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
                .filter { info ->
                    !info.isEncoder && info.supportedTypes.any { it.equals(AndroidOpus.MIME, true) } &&
                        !(Build.VERSION.SDK_INT >= 29 && info.isAlias)
                }
                .map { AvcDecoderCandidate(it.name, hardware(it)) }
            softwareNames = candidates.filter { !it.hardware }.map { it.name }.toSet()
            AndroidOpus.decoderOrder(candidates)
        } catch (_: Exception) {
            emptyList()
        }.also { listed = it; if (it.isEmpty()) unusableThisRun = true }

        private fun hardware(info: MediaCodecInfo): Boolean =
            if (Build.VERSION.SDK_INT >= 29) info.isHardwareAccelerated && !info.isSoftwareOnly
            else !avcSoftwareName(info.name)

        private fun release(codec: MediaCodec) {
            try { codec.stop() } catch (_: Exception) {}
            try { codec.release() } catch (_: Exception) {}
        }
    }
}
