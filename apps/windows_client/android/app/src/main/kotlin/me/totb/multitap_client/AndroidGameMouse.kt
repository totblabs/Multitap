package me.totb.multitap_client

import android.os.Build
import android.os.Handler
import android.os.SystemClock
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import io.flutter.embedding.android.FlutterView
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel

/**
 * 게임 마우스 on Android: Android's pointer capture feeds the virtual USB
 * mouse or moveRelative (GameMouse.kt, JVM-tested). Dart decides when
 * (lib/data/relative_mouse.dart, lib/android/android_remote.dart), over the
 * Windows runner's channels:
 *
 * "multitap/relative-mouse": "engage" takes the window's pointer capture and
 * answers once Android granted it (0 refused, 1 moveRelative, 2 the virtual
 * USB mouse); "release" lets go; "armKey" {bool} keeps Ctrl+Alt+Shift+Z here;
 * "mouse" says whether a mouse is connected. Native "released" {reason}:
 * capture ended here ("focus": Android took it back or the app left the
 * front; "lost": the VM stopped reading the virtual mouse); "toggle": the key;
 * "mouse" {bool}: a mouse came or the last one left.
 *
 * "multitap/game-mouse-usb": "attached" {device}, "detached", "frame" (answered
 * "ok", "older" or "invalid"); native "send" (a frame for the relay) and
 * "confirmed" (a Host that reads ahead), as usb_game_mouse.dart expects.
 *
 * Main thread only. Touchpads deliver absolute positions under capture and
 * move nothing; a finger or a pen on the screen ends capture in Dart.
 */
class AndroidGameMouse internal constructor(
    private val activity: MainActivity,
    engine: FlutterEngine,
    private val media: AndroidMedia,
) {
    private val main = Handler(activity.mainLooper)
    private val relative = MethodChannel(engine.dartExecutor.binaryMessenger, "multitap/relative-mouse")
    private val usb = MethodChannel(engine.dartExecutor.binaryMessenger, "multitap/game-mouse-usb")
    private var device: GameMouseDevice? = null
    private var capture: GameMouseCapture? = null
    private var view: View? = null
    private var pending: MethodChannel.Result? = null
    private var armed = false
    private var keyHeld = false
    // The mouse's buttons as the window last saw them: held ones stay out of the capture until let go.
    private var buttons = 0
    private var mouse: Boolean? = null
    private val wake = Runnable { tick() }
    private val timeout = Runnable { answer(0) }

    init {
        relative.setMethodCallHandler(::relativeCall)
        usb.setMethodCallHandler(::usbCall)
    }

    private fun relativeCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "engage" -> engage(result)
            "release" -> {
                answer(0)
                letGo()
                result.success(null)
            }
            "armKey" -> {
                armed = call.arguments == true
                result.success(null)
            }
            "mouse" -> result.success(mousePresent().also { mouse = it })
            else -> result.notImplemented()
        }
    }

    private fun usbCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "attached" -> {
                val id = (call.arguments as? Number)?.toInt()
                if (id == null || id < 1 || id > 255) { result.error("invalid_arguments", "Expected a device number.", null); return }
                device = GameMouseDevice(id) { bytes -> usb.invokeMethod("send", bytes) }
                result.success(null)
            }
            "detached" -> {
                device = null
                if (capture?.usb == true) end("lost")
                result.success(null)
            }
            "frame" -> {
                val bytes = call.arguments as? ByteArray
                val device = device
                if (bytes == null || device == null) { result.success("invalid"); return }
                val was = device.confirmed
                val seen = device.frame(bytes, nowUs())
                if (!was && device.confirmed) usb.invokeMethod("confirmed", null)
                if (capture?.device === device) schedule()
                result.success(when (seen) {
                    GameMouseDevice.Seen.OK -> "ok"
                    GameMouseDevice.Seen.OLDER_HOST -> "older"
                    GameMouseDevice.Seen.INVALID -> "invalid"
                })
            }
            else -> result.notImplemented()
        }
    }

    private fun engage(result: MethodChannel.Result) {
        val current = capture
        if (current != null) { result.success(if (current.usb) 2 else 1); return }
        if (pending != null || !activity.hasWindowFocus()) { result.success(0); return }
        // Captured events go to the focused view: Flutter's own.
        val target = flutterView(activity.window.decorView)
        if (target == null) { result.success(0); return }
        if (!target.hasFocus()) target.requestFocus()
        pending = result
        view = target
        target.setOnCapturedPointerListener { _, event -> captured(event) }
        target.requestPointerCapture()
        main.postDelayed(timeout, GRANT_MS)
        if (target.hasPointerCapture()) granted()
    }

    /** MainActivity.onPointerCaptureChanged. */
    fun captureChanged(has: Boolean) {
        if (has) granted()
        else if (capture != null) end("focus")
        else answer(0)
    }

    private fun granted() {
        if (pending == null) return
        val now = nowUs()
        val reading = device?.takesInput(now) == true
        capture = GameMouseCapture(if (reading) device else null, buttons) { event -> media.pointerInput(event) }
        schedule()
        answer(if (reading) 2 else 1)
    }

    /** Answers a waiting engage; a refusal lets go of what it took. */
    private fun answer(path: Int) {
        val result = pending ?: return
        pending = null
        main.removeCallbacks(timeout)
        if (path == 0) letGo()
        result.success(path)
    }

    /** Lets go without telling Dart: what is pending goes, every button goes up. */
    private fun letGo() {
        main.removeCallbacks(wake)
        capture?.end(nowUs())
        capture = null
        val target = view ?: return
        view = null
        target.setOnCapturedPointerListener(null)
        if (target.hasPointerCapture()) target.releasePointerCapture()
    }

    /** Capture ended here: Dart hears why. */
    private fun end(reason: String) {
        if (capture == null && pending == null) return
        answer(0)
        letGo()
        relative.invokeMethod("released", reason)
    }

    private fun captured(event: MotionEvent): Boolean {
        val capture = capture ?: return true
        // Under capture a mouse reports movement; a touchpad positions, which move nothing here.
        if (!event.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE)) return true
        var dx = 0f; var dy = 0f; var wheel = 0f; var pan = 0f
        when (event.actionMasked) {
            // The press, release and scroll events of one report carry its
            // movement again: only the motion event's counts.
            MotionEvent.ACTION_MOVE, MotionEvent.ACTION_HOVER_MOVE, MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP -> {
                dx = event.x; dy = event.y
                for (i in 0 until event.historySize) { dx += event.getHistoricalX(i); dy += event.getHistoricalY(i) }
            }
            MotionEvent.ACTION_SCROLL -> {
                wheel = event.getAxisValue(MotionEvent.AXIS_VSCROLL)
                pan = event.getAxisValue(MotionEvent.AXIS_HSCROLL)
            }
        }
        capture.input(event.buttonState, dx, dy, wheel, pan, nowUs())
        schedule()
        return true
    }

    private fun schedule() {
        main.removeCallbacks(wake)
        val capture = capture ?: return
        val at = capture.wake()
        val delay = if (at == null) TICK_MS else ((at - nowUs() + 999) / 1000).coerceIn(0, TICK_MS)
        main.postDelayed(wake, delay)
    }

    private fun tick() {
        val capture = capture ?: return
        if (!capture.tick(nowUs())) { end("lost"); return }
        schedule()
    }

    /** MainActivity.dispatchKeyEvent: Ctrl+Alt+Shift+Z (both its down and its up) stays here while armed. */
    fun key(event: KeyEvent): Boolean {
        if (event.keyCode != KeyEvent.KEYCODE_Z) return false
        if (event.action == KeyEvent.ACTION_UP && keyHeld) { keyHeld = false; return true }
        if (!armed || event.action != KeyEvent.ACTION_DOWN || !event.isCtrlPressed || !event.isAltPressed || !event.isShiftPressed) return false
        keyHeld = true
        if (event.repeatCount == 0) relative.invokeMethod("toggle", null)
        return true
    }

    /** MainActivity's dispatch of an ordinary (uncaptured) event: a mouse's buttons. */
    fun observe(event: MotionEvent) {
        if (event.isFromSource(InputDevice.SOURCE_MOUSE)) buttons = event.buttonState and 0x1F
    }

    /** The window lost focus or the app left the front. */
    fun background() = end("focus")

    /** InputManager: a device came, went or changed. */
    fun devicesChanged() {
        val now = mousePresent()
        if (mouse != null && mouse != now) relative.invokeMethod("mouse", now)
        mouse = now
    }

    fun dispose() {
        answer(0)
        letGo()
        device = null
        relative.setMethodCallHandler(null)
        usb.setMethodCallHandler(null)
    }

    private fun mousePresent() = InputDevice.getDeviceIds().any { id ->
        val device = InputDevice.getDevice(id)
        device != null && !device.isVirtual && device.supportsSource(InputDevice.SOURCE_MOUSE) &&
            (Build.VERSION.SDK_INT < 29 || device.isExternal)
    }

    private fun flutterView(view: View): View? {
        if (view is FlutterView) return view
        if (view is ViewGroup) for (i in 0 until view.childCount) flutterView(view.getChildAt(i))?.let { return it }
        return null
    }

    private fun nowUs() = SystemClock.elapsedRealtimeNanos() / 1000

    companion object {
        /** Android grants capture within a frame or two; longer is a refusal. */
        const val GRANT_MS = 500L
        /** The liveness check while held (the Windows window timer's pace). */
        const val TICK_MS = 16L
    }
}
