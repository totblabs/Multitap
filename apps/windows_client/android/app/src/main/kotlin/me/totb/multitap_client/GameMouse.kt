package me.totb.multitap_client

import kotlin.math.max

/*
 * 게임 마우스 on Android, the pure part (docs/usb-redirection.md,
 * "마우스 잠금"). A port of windows/runner/game_mouse_usb.h (usb-v1 relay frames,
 * the 7-byte reports, the virtual USB mouse's answers and pacing) and of the
 * injected path's 4 ms deltas (windows/runner/relative_mouse.h), fed by the
 * captured mouse as Android reports it. No Android types here, so the JVM
 * tests cover all of it; AndroidGameMouse.kt is the glue.
 */

/** A usb-v1 relay frame (docs/usb-redirection.md): 24-byte little-endian header. */
internal class GameMouseFrame(
    val type: Int,
    val device: Int,
    val endpoint: Int = 0,
    val transfer: Int = 0,
    val seq: Int = 0,
    /** Unsigned on the wire; an unlink names the read it cancels here. */
    val length: Int = 0,
    val status: Int = 0,
    val setup: ByteArray = ByteArray(8),
    val payload: ByteArray = ByteArray(0),
) {
    fun encode(): ByteArray {
        val out = ByteArray(24 + payload.size)
        out[0] = type.toByte(); out[1] = device.toByte(); out[2] = endpoint.toByte(); out[3] = transfer.toByte()
        put(out, 4, seq); put(out, 8, length); put(out, 12, status)
        setup.copyInto(out, 16, 0, 8)
        payload.copyInto(out, 24)
        return out
    }

    companion object {
        const val SUBMIT = 1
        const val RETURN = 2
        const val UNLINK = 3
        const val UNLINK_RETURN = 4
        const val STALL = -32
        const val RESET = -104
        const val ENDPOINT = 0x81
        const val INTERRUPT = 3

        fun decode(bytes: ByteArray): GameMouseFrame? {
            if (bytes.size < 24) return null
            fun byte(at: Int) = bytes[at].toInt() and 255
            fun le32(at: Int) = byte(at) or (byte(at + 1) shl 8) or (byte(at + 2) shl 16) or (byte(at + 3) shl 24)
            return GameMouseFrame(byte(0), byte(1), byte(2), byte(3), le32(4), le32(8), le32(12),
                bytes.copyOfRange(16, 24), bytes.copyOfRange(24, bytes.size))
        }

        private fun put(out: ByteArray, at: Int, value: Int) {
            for (i in 0 until 4) out[at + i] = (value ushr (8 * i)).toByte()
        }
    }
}

/** 5 button bits, X and Y as signed 16-bit, wheel and pan (AC Pan) signed 8-bit. */
internal object GameMouseReports {
    const val LENGTH = 7

    fun make(buttons: Int, dx: Int, dy: Int, wheel: Int = 0, pan: Int = 0) = byteArrayOf(
        (buttons and 0x1F).toByte(), dx.toByte(), (dx shr 8).toByte(), dy.toByte(), (dy shr 8).toByte(),
        wheel.toByte(), pan.toByte())

    fun buttons(report: ByteArray) = report[0].toInt() and 0x1F
    fun x(report: ByteArray) = ((report[1].toInt() and 255) or (report[2].toInt() shl 8)).toShort().toInt()
    fun y(report: ByteArray) = ((report[3].toInt() and 255) or (report[4].toInt() shl 8)).toShort().toInt()
    fun wheel(report: ByteArray) = report[5].toInt()
    fun pan(report: ByteArray) = report[6].toInt()

    /** One movement as reports: beyond 16 bits (or a wheel beyond 8) it is split, nothing lost. */
    fun split(buttons: Int, dx: Long, dy: Long, wheel: Long, pan: Long): List<ByteArray> {
        var x = dx; var y = dy; var w = wheel; var p = pan
        val out = ArrayList<ByteArray>(1)
        do {
            val sx = x.coerceIn(-32767L, 32767L); val sy = y.coerceIn(-32767L, 32767L)
            val sw = w.coerceIn(-127L, 127L); val sp = p.coerceIn(-127L, 127L)
            out += make(buttons, sx.toInt(), sy.toInt(), sw.toInt(), sp.toInt())
            x -= sx; y -= sy; w -= sw; p -= sp
        } while (x != 0L || y != 0L || w != 0L || p != 0L)
        return out
    }

    /** [into] with the motion of [next] (same buttons, sums within range), or null: a button change never joins. */
    fun join(into: ByteArray, next: ByteArray): ByteArray? {
        if (into[0] != next[0]) return null
        val x = x(into) + x(next); val y = y(into) + y(next)
        val w = wheel(into) + wheel(next); val p = pan(into) + pan(next)
        if (x !in -32767..32767 || y !in -32767..32767 || w !in -127..127 || p !in -127..127) return null
        return make(buttons(into), x, y, w, p)
    }
}

/**
 * The captured mouse as reports. [input]'s buttonState is MotionEvent's: its
 * low five bits (primary, secondary, tertiary, back, forward) are the report's
 * five buttons in that order. Buttons already down when capture began
 * ([ignored]) stay out until they are let go, so the click that handed the
 * mouse over never reaches the VM. A wheel step of 1.0 is one notch; finer
 * wheels and fractional motion keep their remainders.
 */
internal class GameMouseState(ignored: Int = 0) {
    private var ignored = ignored and 0x1F
    private val x = Rest(); private val y = Rest(); private val wheel = Rest(); private val pan = Rest()

    /** The buttons the VM was told are down. */
    var held = 0
        private set

    fun input(buttonState: Int, dx: Float, dy: Float, wheel: Float = 0f, pan: Float = 0f): List<ByteArray> {
        val physical = buttonState and 0x1F
        ignored = ignored and physical
        val buttons = physical and ignored.inv()
        val mx = x.take(dx); val my = y.take(dy); val mw = this.wheel.take(wheel); val mp = this.pan.take(pan)
        if (mx == 0L && my == 0L && mw == 0L && mp == 0L && buttons == held) return emptyList()
        held = buttons
        return GameMouseReports.split(buttons, mx, my, mw, mp)
    }

    /** Every button up, when capture ends: the report, or null when none was down. */
    fun releaseAll(): ByteArray? {
        val any = held != 0
        held = 0
        x.clear(); y.clear(); wheel.clear(); pan.clear()
        return if (any) GameMouseReports.make(0, 0, 0) else null
    }

    private class Rest {
        private var value = 0.0
        fun take(delta: Float): Long {
            if (delta.isNaN() || delta.isInfinite()) return 0
            value += delta
            val whole = value.toLong()
            value -= whole
            return whole
        }
        fun clear() { value = 0.0 }
    }
}

/**
 * The device's side of usb-v1 for one attachment (game_mouse_usb.h Device).
 * Reads the Host keeps waiting here are answered with reports as they happen;
 * without a read waiting, reports wait in order (motion joined) and ones older
 * than 300 ms are dropped, the newest standing for the state now. Answers are
 * paced: never more than one per millisecond (the relay's message budget),
 * otherwise spread over the round trip the Host's reads come back at; 4 ms
 * until the first round trip is known. A Host that sends a control request
 * first does not know the device (an older Host): the caller gives it up.
 */
internal class GameMouseDevice(private val id: Int, private val send: (ByteArray) -> Unit) {
    enum class Seen { OK, OLDER_HOST, INVALID }

    private class Queued(var report: ByteArray, var at: Long)

    private val held = ArrayDeque<Int>()
    private val queued = ArrayDeque<Queued>()
    private val answered = ArrayDeque<Long>()
    private var rttUs = 0L
    private var lastAnswerUs = Long.MIN_VALUE / 2
    private var lastReadUs = Long.MIN_VALUE / 2
    private var mostHeld = 1
    private var reading = false
    var older = false
        private set

    /** A Host that answers the device itself read it. */
    val confirmed get() = reading && !older
    val heldReads get() = held.size
    val queuedReports get() = queued.size

    /** A Host frame for this device (submit or unlink). */
    fun frame(bytes: ByteArray, nowUs: Long): Seen {
        val frame = GameMouseFrame.decode(bytes)
        if (frame == null || frame.device != id) return Seen.INVALID
        if (frame.type == GameMouseFrame.UNLINK) {
            val waiting = held.remove(frame.length)
            reply(GameMouseFrame.UNLINK_RETURN, frame.seq, 0, 0, if (waiting) GameMouseFrame.RESET else 0)
            if (waiting) reply(GameMouseFrame.RETURN, frame.length, GameMouseFrame.ENDPOINT, GameMouseFrame.INTERRUPT, GameMouseFrame.RESET)
            answered.clear() // the reads coming back no longer pair with answers
            return Seen.OK
        }
        if (frame.type != GameMouseFrame.SUBMIT) return Seen.INVALID
        val read = frame.transfer == GameMouseFrame.INTERRUPT && frame.endpoint == GameMouseFrame.ENDPOINT &&
            frame.length.toUInt() >= GameMouseReports.LENGTH.toUInt() && frame.payload.isEmpty()
        if (frame.transfer == 0 || older || !read) {
            // A Host that forwards control does not answer this device itself.
            reply(GameMouseFrame.RETURN, frame.seq, frame.endpoint, frame.transfer, GameMouseFrame.STALL)
            if (frame.transfer == 0 && !reading) older = true
            return if (older) Seen.OLDER_HOST else Seen.OK
        }
        reading = true
        held.addLast(frame.seq)
        mostHeld = max(mostHeld, held.size)
        lastReadUs = nowUs
        if (answered.isNotEmpty()) {
            // Reads come back in the order they were answered: the gap is the round trip.
            val sample = nowUs - answered.removeFirst()
            rttUs = if (rttUs != 0L) (7 * rttUs + sample) / 8 else sample
        }
        answer(nowUs)
        return Seen.OK
    }

    /** A report from this device's captured mouse. */
    fun report(report: ByteArray, nowUs: Long) {
        val last = queued.lastOrNull()
        val joined = last?.let { GameMouseReports.join(it.report, report) }
        if (last != null && joined != null) {
            last.report = joined
            last.at = nowUs
        } else {
            queued.addLast(Queued(report, nowUs))
            while (queued.size > 256) queued.removeFirst()
        }
        answer(nowUs)
    }

    fun tick(nowUs: Long) = answer(nowUs)

    /** When a paced report may go (the caller wakes then), or null: nothing to wait for. */
    fun wake(): Long? = if (held.isEmpty() || queued.isEmpty()) null else lastAnswerUs + interval()

    /** The VM reads it now: confirmed, and one of its reads waiting here now or within the last second. */
    fun takesInput(nowUs: Long) = confirmed && (held.isNotEmpty() || nowUs - lastReadUs < READING_US)

    fun interval(): Long {
        // Until a round trip is known, the injected path's pace: a first stroke
        // must not use every read at once and then wait a whole round trip.
        if (rttUs == 0L) return FIRST_INTERVAL_US
        return (rttUs / max(1, mostHeld)).coerceIn(MIN_INTERVAL_US, MAX_INTERVAL_US)
    }

    private fun answer(nowUs: Long) {
        if (held.isEmpty() || queued.isEmpty() || nowUs - lastAnswerUs < interval()) return
        // The oldest report; stale ones dropped, the newest of them as a state.
        while (queued.size > 1 && nowUs - queued.first().at > STALE_US) queued.removeFirst()
        val next = queued.removeFirst()
        val report = if (nowUs - next.at > STALE_US) GameMouseReports.make(GameMouseReports.buttons(next.report), 0, 0) else next.report
        reply(GameMouseFrame.RETURN, held.removeFirst(), GameMouseFrame.ENDPOINT, GameMouseFrame.INTERRUPT, 0, report)
        lastAnswerUs = nowUs
        answered.addLast(nowUs)
        while (answered.size > 64) answered.removeFirst()
    }

    private fun reply(type: Int, seq: Int, endpoint: Int, transfer: Int, status: Int, payload: ByteArray = ByteArray(0)) {
        send(GameMouseFrame(type, id, endpoint, transfer, seq, payload.size, status, ByteArray(8), payload).encode())
    }

    companion object {
        const val STALE_US = 300_000L
        const val READING_US = 1_000_000L
        const val MIN_INTERVAL_US = 1_000L
        const val FIRST_INTERVAL_US = 4_000L
        const val MAX_INTERVAL_US = 50_000L
    }
}

/**
 * Raw movement for moveRelative (relative_mouse.h RelativeDeltas): at most one
 * message per 4 ms, each axis within +-16384, never both zero. What a message
 * cannot carry yet stays for the next, so slow movement is not lost and a fast
 * mouse cannot flood the socket.
 */
internal class GameMouseDeltas {
    private var dx = 0L
    private var dy = 0L
    private var lastUs = 0L
    private var sent = false

    val pending get() = dx != 0L || dy != 0L

    fun add(x: Long, y: Long) { dx += x; dy += y }

    /** One message now, or null. [force]: whatever the pace (before a click, so it lands where the cursor went). */
    fun take(nowUs: Long, force: Boolean = false): Pair<Int, Int>? {
        if (!pending || (!force && sent && nowUs - lastUs < INTERVAL_US)) return null
        val x = dx.coerceIn(-LIMIT, LIMIT); val y = dy.coerceIn(-LIMIT, LIMIT)
        dx -= x; dy -= y
        sent = true
        lastUs = nowUs
        return x.toInt() to y.toInt()
    }

    /** When the next paced message may go, or null. */
    fun wake(): Long? = if (pending && sent) lastUs + INTERVAL_US else null

    companion object {
        const val LIMIT = 16384L
        const val INTERVAL_US = 4_000L
    }
}

/**
 * One engagement's input. With [device] (the VM reads Multitap's virtual USB
 * mouse) every report goes to it. Otherwise ([inject], relative-mouse-v1):
 * movement as moveRelative at most every 4 ms, buttons and the vertical wheel
 * as position-less inputs, each after the movement before it, so a click lands
 * where the cursor went. The horizontal wheel has no injected form.
 */
internal class GameMouseCapture(
    val device: GameMouseDevice?,
    ignored: Int,
    private val inject: (Map<String, Any>) -> Unit,
) {
    private val state = GameMouseState(ignored)
    private val deltas = GameMouseDeltas()
    private var buttons = 0

    val usb get() = device != null

    fun input(buttonState: Int, dx: Float, dy: Float, wheel: Float, pan: Float, nowUs: Long) {
        val reports = state.input(buttonState, dx, dy, wheel, pan)
        val device = device
        if (device != null) {
            for (report in reports) device.report(report, nowUs)
            return
        }
        for (report in reports) {
            deltas.add(GameMouseReports.x(report).toLong(), GameMouseReports.y(report).toLong())
            val pressed = GameMouseReports.buttons(report)
            var steps = GameMouseReports.wheel(report)
            if (pressed != buttons || steps != 0) flush(nowUs, force = true)
            press(pressed)
            while (steps != 0) {
                // The relay takes at most 100 notches (12000) in one wheel input.
                val part = steps.coerceIn(-100, 100)
                inject(mapOf("type" to "input", "event" to "wheel", "delta" to part * 120))
                steps -= part
            }
        }
        flush(nowUs)
    }

    /** The timer: paced movement goes; false when the virtual mouse stopped being read (capture ends). */
    fun tick(nowUs: Long): Boolean {
        val device = device ?: run { flush(nowUs); return true }
        device.tick(nowUs)
        return device.takesInput(nowUs)
    }

    fun wake(): Long? = if (device != null) device.wake() else deltas.wake()

    /** Capture ended: what is pending goes, then every button up. */
    fun end(nowUs: Long) {
        val up = state.releaseAll()
        val device = device
        if (device != null) {
            if (up != null) device.report(up, nowUs)
            return
        }
        flush(nowUs, force = true)
        press(0)
    }

    private fun press(target: Int) {
        for (bit in 0 until 5) {
            val mask = 1 shl bit
            if ((buttons xor target) and mask == 0) continue
            inject(mapOf("type" to "input", "event" to "button", "button" to BUTTONS[bit], "down" to (target and mask != 0)))
        }
        buttons = target
    }

    private fun flush(nowUs: Long, force: Boolean = false) {
        while (true) {
            val (dx, dy) = deltas.take(nowUs, force) ?: return
            inject(mapOf("type" to "input", "event" to "moveRelative", "dx" to dx, "dy" to dy))
            if (!force) return
        }
    }

    companion object {
        /** relative-mouse-v1's names, in the report's bit order. */
        val BUTTONS = listOf("left", "right", "middle", "x1", "x2")
    }
}
