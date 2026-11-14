package me.totb.multitap_client

import android.app.Activity
import android.content.Context
import android.hardware.input.InputManager
import android.os.Build
import android.os.Handler
import android.os.VibrationEffect
import android.os.Vibrator
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import kotlin.math.max
import kotlin.math.roundToInt

// gamepad-v1 (docs/remote-session-protocol.md "Gamepads"): this phone's game controllers for the session's Host.
// The channel "multitap/android/gamepad": Dart sends "start" and "stop" and "rumble" {slot, low, high}; this side
// sends "state" {slot, buttons, lt, rt, lx, ly, rx, ry} (XInput's ranges and button bits) on every change and
// "removed" slot. A slot is the InputDevice id. While started, the keys and motion of a game controller are taken
// here (B is no Back, the D-pad moves nothing in the app); other devices are untouched. A game controller has
// sticks (a joystick source) or the gamepad buttons; a TV remote or a keyboard that Android also lists as a gamepad
// but has neither keeps steering the app. A controller already connected when a session starts, or connected
// during it, gets its neutral state at once, so the desktop has its virtual controller before the first press.
class AndroidGamepad(activity: Activity, engine: FlutterEngine) {
    private val channel = MethodChannel(engine.dartExecutor.binaryMessenger, "multitap/android/gamepad")
    private val inputManager = activity.getSystemService(Context.INPUT_SERVICE) as InputManager
    private var running = false

    private class Pad(val analogLt: Boolean, val analogRt: Boolean) {
        var buttons = 0          // from keys
        var hat = 0              // D-pad bits from AXIS_HAT_X/Y
        var keyLt = false; var keyRt = false
        var lt = 0; var rt = 0; var lx = 0; var ly = 0; var rx = 0; var ry = 0
        var sent: IntArray? = null
    }
    private val pads = HashMap<Int, Pad>()

    private val listener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) { if (running) plugIn(InputDevice.getDevice(deviceId)) }
        override fun onInputDeviceChanged(deviceId: Int) {}
        override fun onInputDeviceRemoved(deviceId: Int) {
            if (pads.remove(deviceId) != null && running) channel.invokeMethod("removed", deviceId)
        }
    }

    init {
        inputManager.registerInputDeviceListener(listener, Handler(activity.mainLooper))
        channel.setMethodCallHandler { call, result ->
            when (call.method) {
                "start" -> {
                    running = true; pads.clear(); result.success(true)
                    for (id in inputManager.inputDeviceIds) plugIn(InputDevice.getDevice(id))
                }
                "stop" -> { stopRumble(); running = false; pads.clear(); result.success(null) }
                "rumble" -> {
                    val slot = call.argument<Int>("slot"); val low = call.argument<Int>("low"); val high = call.argument<Int>("high")
                    if (slot == null || low == null || high == null || low !in 0..255 || high !in 0..255) result.error("invalid_arguments", "Expected slot, low, high.", null)
                    else { rumble(slot, low, high); result.success(null) }
                }
                else -> result.notImplemented()
            }
        }
    }

    fun dispose() {
        channel.setMethodCallHandler(null)
        inputManager.unregisterInputDeviceListener(listener)
        stopRumble()
        running = false
    }

    // A game controller: sticks (a joystick source), or the gamepad buttons (A). Android also lists some TV remotes
    // and keyboards as gamepads; with neither, their arrows and Back stay with the app.
    private fun controller(device: InputDevice?): Boolean {
        val sources = device?.sources ?: return false
        if (sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK) return true
        return sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD &&
            device.hasKeys(KeyEvent.KEYCODE_BUTTON_A).firstOrNull() == true
    }
    private fun pad(device: InputDevice, id: Int): Pad = pads.getOrPut(id) {
        // An analog trigger is the trigger's value; L2/R2 keys then only say it passed a threshold.
        fun has(axis: Int) = device.getMotionRange(axis, InputDevice.SOURCE_JOYSTICK) != null
        Pad(has(MotionEvent.AXIS_LTRIGGER) || has(MotionEvent.AXIS_BRAKE), has(MotionEvent.AXIS_RTRIGGER) || has(MotionEvent.AXIS_GAS))
    }
    // Its neutral state now: the desktop plugs the virtual controller in before the first press.
    private fun plugIn(device: InputDevice?) {
        if (device == null || !controller(device) || pads.containsKey(device.id)) return
        emit(device.id, pad(device, device.id))
    }

    // The app's window lost focus: Android sends a controller's later moves elsewhere, so what is held now would
    // come back as held. Every pad is released; the next event says where it really is.
    fun released() {
        if (!running) return
        for ((id, pad) in pads) {
            pad.buttons = 0; pad.hat = 0; pad.keyLt = false; pad.keyRt = false
            pad.lt = 0; pad.rt = 0; pad.lx = 0; pad.ly = 0; pad.rx = 0; pad.ry = 0
            emit(id, pad)
        }
    }

    // A key of a game controller: true when taken.
    fun key(event: KeyEvent): Boolean {
        val device = event.device
        if (!running || !controller(device)) return false
        val code = event.keyCode
        val trigger = code == KeyEvent.KEYCODE_BUTTON_L2 || code == KeyEvent.KEYCODE_BUTTON_R2
        val bit = buttonBit(code)
        if (bit == 0 && !trigger) return false       // volume, power and the like stay with Android
        if (event.action != KeyEvent.ACTION_DOWN && event.action != KeyEvent.ACTION_UP) return true
        val down = event.action == KeyEvent.ACTION_DOWN
        val pad = pad(device, event.deviceId)
        when (code) {
            KeyEvent.KEYCODE_BUTTON_L2 -> pad.keyLt = down
            KeyEvent.KEYCODE_BUTTON_R2 -> pad.keyRt = down
            else -> pad.buttons = if (down) pad.buttons or bit else pad.buttons and bit.inv()
        }
        emit(event.deviceId, pad)
        return true
    }

    // The sticks, triggers and hat of a game controller: true when taken.
    fun motion(event: MotionEvent): Boolean {
        val device = event.device
        if (!running || !controller(device)) return false
        if (event.source and InputDevice.SOURCE_JOYSTICK != InputDevice.SOURCE_JOYSTICK || event.actionMasked != MotionEvent.ACTION_MOVE) return false
        val pad = pad(device, event.deviceId)
        fun axis(id: Int): Float = if (device.getMotionRange(id, event.source) != null) event.getAxisValue(id) else 0f
        fun has(id: Int): Boolean = device.getMotionRange(id, event.source) != null
        fun stick(value: Float): Int = (value.coerceIn(-1f, 1f) * 32767f).roundToInt().coerceIn(-32768, 32767)
        fun trigger(value: Float): Int = (value.coerceIn(0f, 1f) * 255f).roundToInt()
        pad.lx = stick(axis(MotionEvent.AXIS_X)); pad.ly = stick(-axis(MotionEvent.AXIS_Y))
        // The right stick: Z/RZ on most controllers, RX/RY on some.
        if (has(MotionEvent.AXIS_Z) || has(MotionEvent.AXIS_RZ)) { pad.rx = stick(axis(MotionEvent.AXIS_Z)); pad.ry = stick(-axis(MotionEvent.AXIS_RZ)) }
        else { pad.rx = stick(axis(MotionEvent.AXIS_RX)); pad.ry = stick(-axis(MotionEvent.AXIS_RY)) }
        pad.lt = trigger(max(axis(MotionEvent.AXIS_LTRIGGER), axis(MotionEvent.AXIS_BRAKE)))
        pad.rt = trigger(max(axis(MotionEvent.AXIS_RTRIGGER), axis(MotionEvent.AXIS_GAS)))
        val hatX = axis(MotionEvent.AXIS_HAT_X); val hatY = axis(MotionEvent.AXIS_HAT_Y)
        pad.hat = (if (hatY < -0.5f) UP else 0) or (if (hatY > 0.5f) DOWN else 0) or (if (hatX < -0.5f) LEFT else 0) or (if (hatX > 0.5f) RIGHT else 0)
        emit(event.deviceId, pad)
        return true
    }

    private fun emit(id: Int, pad: Pad) {
        // A digital L2/R2 counts only on a controller without the analog trigger (a pull past its threshold is not 255).
        val state = intArrayOf(
            (pad.buttons or pad.hat) and 0xF7FF,
            if (pad.keyLt && !pad.analogLt) 255 else pad.lt, if (pad.keyRt && !pad.analogRt) 255 else pad.rt,
            pad.lx, pad.ly, pad.rx, pad.ry)
        if (pad.sent?.contentEquals(state) == true) return
        pad.sent = state
        channel.invokeMethod("state", mapOf("slot" to id, "buttons" to state[0], "lt" to state[1], "rt" to state[2],
            "lx" to state[3], "ly" to state[4], "rx" to state[5], "ry" to state[6]))
    }

    // One motor's worth: Android's controller vibrators have one strength; the stronger of the two motors. It goes on
    // (a repeating second) until the Host's next level, as the game's rumble does. The pattern starts with an empty
    // segment: before Android 12 a controller's vibrator reads a waveform's timings as off/on pairs from "off".
    @Suppress("DEPRECATION")
    private fun rumble(id: Int, low: Int, high: Int) {
        val device = InputDevice.getDevice(id) ?: return
        val vibrator: Vibrator = (if (Build.VERSION.SDK_INT >= 31) device.vibratorManager.defaultVibrator else device.vibrator) ?: return
        if (!vibrator.hasVibrator()) return
        val strength = max(low, high)
        try {
            if (strength == 0) vibrator.cancel()
            else if (Build.VERSION.SDK_INT >= 26) vibrator.vibrate(VibrationEffect.createWaveform(longArrayOf(0L, 1000L),
                intArrayOf(0, if (vibrator.hasAmplitudeControl()) strength.coerceIn(1, 255) else VibrationEffect.DEFAULT_AMPLITUDE), 1))
            else vibrator.vibrate(longArrayOf(0L, 1000L), 1)
        } catch (_: Exception) { }
    }
    private fun stopRumble() { for (id in pads.keys.toList()) rumble(id, 0, 0) }

    private fun buttonBit(code: Int): Int = when (code) {
        KeyEvent.KEYCODE_BUTTON_A -> 0x1000
        KeyEvent.KEYCODE_BUTTON_B -> 0x2000
        KeyEvent.KEYCODE_BUTTON_X -> 0x4000
        KeyEvent.KEYCODE_BUTTON_Y -> 0x8000
        KeyEvent.KEYCODE_BUTTON_L1 -> 0x0100
        KeyEvent.KEYCODE_BUTTON_R1 -> 0x0200
        KeyEvent.KEYCODE_BUTTON_THUMBL -> 0x0040
        KeyEvent.KEYCODE_BUTTON_THUMBR -> 0x0080
        KeyEvent.KEYCODE_BUTTON_START -> 0x0010
        KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BACK -> 0x0020
        KeyEvent.KEYCODE_BUTTON_MODE -> 0x0400
        KeyEvent.KEYCODE_DPAD_UP -> UP
        KeyEvent.KEYCODE_DPAD_DOWN -> DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> RIGHT
        else -> 0
    }

    private companion object {
        const val UP = 0x0001
        const val DOWN = 0x0002
        const val LEFT = 0x0004
        const val RIGHT = 0x0008
    }
}
