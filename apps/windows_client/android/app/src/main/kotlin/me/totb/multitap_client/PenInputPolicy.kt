package me.totb.multitap_client

/**
 * OS-free decisions for the missing Android pen signals, not session readiness.
 *
 * Device identity deliberately matches the pinned Flutter 3.47.7 engine:
 * AndroidTouchProcessor.uniquePointerIdByType = (pointerId << 3) | toolType.
 * Android pointer IDs are 0..31; only TOOL_TYPE_STYLUS (2) and ERASER (4) enter
 * this adapter. Do not substitute MotionEvent.deviceId or Flutter kind values.
 * Recheck this contract whenever upgrading the Flutter embedding/SDK.
 * Source: https://raw.githubusercontent.com/flutter/flutter/3.47.7/engine/src/flutter/shell/platform/android/io/flutter/embedding/android/AndroidTouchProcessor.java
 * Locally reviewed source SHA-256:
 * f47b5f7b4ba15aaf02c1a3e4fc763be4d2ed30eb33f2d9d11454287b06440dad.
 * Action/tool constants below are Android's public MotionEvent values, so this
 * policy can be checked on a plain JVM without creating any Android event.
 */
internal class PenInputPolicy {
    enum class ButtonDispatch { TOUCH_MOVE, HOVER_MOVE }
    data class Exit(val device: Int, val kind: String, val timeMicros: Long)

    private val contacts = mutableSetOf<Int>() // At most 32 IDs x two pen types.

    /** Called only for real touch events, never for synthesized button moves. */
    fun touch(action: Int, pointerId: Int, toolType: Int, canceled: Boolean): Boolean {
        val key = deviceKey(pointerId, toolType)
        // FLAG_CANCELED also marks rejected finger/palm pointers. No canceled
        // UP may become a successful tap, even when its tool/key is unknown.
        // Cancel the whole motion set instead of inventing a successful UP.
        if (canceled && (action == UP || action == POINTER_UP)) {
            clear()
            return true
        }
        when (action) {
            DOWN, POINTER_DOWN -> if (key != null) contacts.add(key)
            UP, POINTER_UP -> if (key != null) contacts.remove(key)
            CANCEL -> clear()
        }
        return false
    }

    fun buttonDispatch(action: Int, pointerId: Int, toolType: Int): ButtonDispatch? {
        if (action != BUTTON_PRESS && action != BUTTON_RELEASE) return null
        val key = deviceKey(pointerId, toolType) ?: return null
        return if (key in contacts) ButtonDispatch.TOUCH_MOVE else ButtonDispatch.HOVER_MOVE
    }

    /** Retire this exact pen; true asks the adapter to CANCEL its motion set. */
    fun hoverExit(pointerId: Int, toolType: Int): Boolean {
        val key = deviceKey(pointerId, toolType) ?: return false
        return contacts.remove(key)
    }

    fun clear() { contacts.clear() }

    companion object {
        const val PROTOCOL = "pen-signals-v1"
        const val DOWN = 0
        const val UP = 1
        const val CANCEL = 3
        const val POINTER_DOWN = 5
        const val POINTER_UP = 6
        const val BUTTON_PRESS = 11
        const val BUTTON_RELEASE = 12
        const val STYLUS = 2
        const val ERASER = 4

        fun deviceKey(pointerId: Int, toolType: Int): Int? {
            if (pointerId !in 0..31 || (toolType != STYLUS && toolType != ERASER)) return null
            return (pointerId shl 3) or toolType
        }

        fun exit(pointerId: Int, toolType: Int, eventTimeMillis: Long): Exit? {
            val key = deviceKey(pointerId, toolType) ?: return null
            if (eventTimeMillis < 0 || eventTimeMillis > Long.MAX_VALUE / 1000L) return null
            return Exit(key, if (toolType == STYLUS) "stylus" else "invertedStylus", eventTimeMillis * 1000L)
        }
    }
}
