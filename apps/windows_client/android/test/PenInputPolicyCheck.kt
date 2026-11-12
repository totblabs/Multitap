package me.totb.multitap_client

/** Pure production-policy checks. No Android SDK, MotionEvent, device or input. */
object PenInputPolicyCheck {
    @JvmStatic
    fun main(args: Array<String>) {
        var checks = 0
        fun verify(value: Boolean, label: String) { check(value) { label }; checks++ }
        val policy = PenInputPolicy()
        val pen = PenInputPolicy.STYLUS
        val eraser = PenInputPolicy.ERASER
        val press = PenInputPolicy.BUTTON_PRESS
        val release = PenInputPolicy.BUTTON_RELEASE
        val touch = PenInputPolicy.ButtonDispatch.TOUCH_MOVE
        val hover = PenInputPolicy.ButtonDispatch.HOVER_MOVE

        verify(PenInputPolicy.PROTOCOL == "pen-signals-v1", "fixed adapter protocol")
        verify(PenInputPolicy.deviceKey(0, pen) == 2 && PenInputPolicy.deviceKey(0, eraser) == 4,
            "engine tool values, not Flutter kind numbers or Android device IDs")
        verify(PenInputPolicy.deviceKey(31, pen) == 250 && PenInputPolicy.deviceKey(31, eraser) == 252,
            "last Android pointer ID")
        verify(PenInputPolicy.deviceKey(-1, pen) == null && PenInputPolicy.deviceKey(32, pen) == null,
            "reject pointer ID outside Android bound")
        verify(listOf(-1, 0, 1, 3, 5, 8, Int.MAX_VALUE).all { PenInputPolicy.deviceKey(0, it) == null },
            "mouse, finger and unknown tools unchanged")
        verify((0..31).flatMap { id -> listOf(pen, eraser).map { PenInputPolicy.deviceKey(id, it) } }
            .toSet().size == 64, "bounded unique pen keys")
        verify(policy.buttonDispatch(press, 1, pen) == hover && policy.buttonDispatch(release, 1, pen) == hover,
            "buttons without real contact never invent a down")
        verify(!policy.touch(PenInputPolicy.DOWN, 1, pen, false), "ordinary down is unmodified")
        verify(policy.buttonDispatch(press, 1, pen) == touch && policy.buttonDispatch(release, 1, pen) == touch,
            "both generic button edges during contact become touch moves")
        verify(policy.buttonDispatch(press, 2, pen) == hover && policy.buttonDispatch(press, 1, eraser) == hover,
            "other pointer and inverted identity do not inherit contact")
        verify(policy.buttonDispatch(press, 1, 3) == null && policy.buttonDispatch(release, 1, 1) == null,
            "mouse and finger button events keep original route")
        verify(policy.buttonDispatch(2, 1, pen) == null && policy.buttonDispatch(10, 1, pen) == null,
            "ordinary move and hover exit are not button synthesis")
        policy.touch(PenInputPolicy.UP, 1, pen, false)
        verify(policy.buttonDispatch(press, 1, pen) == hover, "real up ends contact")

        policy.touch(PenInputPolicy.DOWN, 1, pen, false)
        policy.touch(PenInputPolicy.POINTER_DOWN, 2, eraser, false)
        policy.touch(PenInputPolicy.POINTER_UP, 1, pen, false)
        verify(policy.buttonDispatch(press, 1, pen) == hover && policy.buttonDispatch(press, 2, eraser) == touch,
            "ordinary secondary up ends only its exact contact")
        policy.touch(PenInputPolicy.POINTER_DOWN, 1, pen, false)
        verify(policy.touch(PenInputPolicy.POINTER_UP, 1, pen, true), "canceled pen pointer-up requests whole-set CANCEL")
        verify(policy.buttonDispatch(press, 1, pen) == hover && policy.buttonDispatch(press, 2, eraser) == hover,
            "whole-set cancellation clears every tracked pen")
        policy.touch(PenInputPolicy.DOWN, 3, pen, false)
        verify(policy.touch(PenInputPolicy.UP, 3, pen, true) && policy.buttonDispatch(press, 3, pen) == hover,
            "canceled primary up never becomes a normal successful up")
        policy.touch(PenInputPolicy.DOWN, 4, pen, false)
        policy.touch(PenInputPolicy.CANCEL, -1, -1, false)
        verify(policy.buttonDispatch(press, 4, pen) == hover, "real CANCEL clears even without an action pen index")
        policy.touch(PenInputPolicy.DOWN, 4, 1, false)
        verify(policy.buttonDispatch(press, 4, pen) == hover, "finger down does not admit pen touch moves")

        policy.touch(PenInputPolicy.DOWN, 4, pen, false)
        verify(!policy.touch(PenInputPolicy.UP, 5, 1, false) &&
            policy.buttonDispatch(press, 4, pen) == touch, "normal finger up passes and keeps unrelated pen")
        verify(!policy.touch(PenInputPolicy.UP, 5, 3, false) &&
            policy.buttonDispatch(press, 4, pen) == touch, "normal mouse up passes and keeps unrelated pen")
        verify(policy.touch(PenInputPolicy.UP, 5, 1, true) &&
            policy.buttonDispatch(press, 4, pen) == hover, "canceled finger or palm up requests whole-set CANCEL")
        policy.touch(PenInputPolicy.DOWN, 4, pen, false)
        verify(policy.touch(PenInputPolicy.POINTER_UP, 5, 0, true) &&
            policy.buttonDispatch(press, 4, pen) == hover, "unknown-tool canceled pointer-up cannot become successful UP")
        verify(policy.touch(PenInputPolicy.UP, -1, -1, true), "canceled UP does not require a recognized key")

        policy.touch(PenInputPolicy.DOWN, 6, pen, false)
        policy.touch(PenInputPolicy.POINTER_DOWN, 7, eraser, false)
        verify(!policy.hoverExit(6, eraser) && !policy.hoverExit(6, 3) && !policy.hoverExit(32, pen),
            "foreign kind, mouse and invalid exit do not retire a pen")
        verify(policy.hoverExit(6, pen), "contact pen exit requests CANCEL before exit signal")
        verify(!policy.hoverExit(6, pen) && policy.buttonDispatch(press, 6, pen) == hover,
            "contact exit retires once and no longer normalizes buttons as down")
        verify(policy.buttonDispatch(press, 7, eraser) == touch,
            "exit does not clear another pen absent from its actual motion set")
        verify(policy.hoverExit(7, eraser) && policy.buttonDispatch(press, 7, eraser) == hover,
            "another pen included in a canceled motion set is retired explicitly")
        verify(!policy.hoverExit(8, pen), "ordinary hover exit never invents contact CANCEL")

        for (id in 0..31) for (kind in listOf(pen, eraser)) policy.touch(PenInputPolicy.POINTER_DOWN, id, kind, false)
        verify((0..31).all { id -> listOf(pen, eraser).all { policy.buttonDispatch(press, id, it) == touch } },
            "all valid contact keys remain independent")
        policy.clear(); policy.clear()
        verify((0..31).all { id -> listOf(pen, eraser).all { policy.buttonDispatch(release, id, it) == hover } },
            "focus, pause and disposal reset is bounded and idempotent")

        verify(PenInputPolicy.exit(7, pen, 123L) == PenInputPolicy.Exit(58, "stylus", 123000L),
            "hover exit matches engine device and milliseconds-to-microseconds")
        verify(PenInputPolicy.exit(7, eraser, 0L) == PenInputPolicy.Exit(60, "invertedStylus", 0L),
            "inverted hover exit kind and zero uptime")
        verify(PenInputPolicy.exit(0, pen, -1L) == null && PenInputPolicy.exit(0, pen, Long.MAX_VALUE) == null,
            "invalid event time and multiplication overflow refused")
        verify(PenInputPolicy.exit(0, 3, 1L) == null && PenInputPolicy.exit(32, pen, 1L) == null,
            "no mouse signal or invalid engine key")
        println("PenInputPolicyCheck passed: $checks checks (pure policy only; no Android/device acceptance)")
    }
}
