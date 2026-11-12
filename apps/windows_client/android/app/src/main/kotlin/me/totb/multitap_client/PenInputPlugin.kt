package me.totb.multitap_client

import android.os.Build
import android.view.MotionEvent
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.EventChannel

/** Activity-owned supplement to Flutter's normal pen pointer processing. */
internal class PenInputPlugin(messenger: BinaryMessenger) {
    private val policy = PenInputPolicy()
    private val channel = EventChannel(messenger, "multitap/pen-input")
    private var sink: EventChannel.EventSink? = null
    private var disposed = false

    init {
        channel.setStreamHandler(object : EventChannel.StreamHandler {
            override fun onListen(arguments: Any?, events: EventChannel.EventSink) {
                if (disposed) return
                sink = events
                // Adapter support only: this is never Windows/stream readiness.
                events.success(mapOf("type" to "ready", "protocol" to PenInputPolicy.PROTOCOL))
            }
            override fun onCancel(arguments: Any?) { sink = null }
        })
    }

    private fun pointerIndex(event: MotionEvent): Int? {
        if (event.pointerCount !in 1..32) return null
        return event.actionIndex.takeIf { it in 0 until event.pointerCount }
    }

    fun dispatchTouch(event: MotionEvent, dispatch: (MotionEvent) -> Boolean): Boolean {
        if (disposed) return dispatch(event)
        val index = pointerIndex(event)
        val pointerId = if (index == null) -1 else event.getPointerId(index)
        val toolType = if (index == null) -1 else event.getToolType(index)
        val canceled = Build.VERSION.SDK_INT >= 33 && (event.flags and MotionEvent.FLAG_CANCELED) != 0
        if (!policy.touch(event.actionMasked, pointerId, toolType, canceled)) return dispatch(event)
        // Retain every pointer, source, coordinate and button in a canceled
        // touch set. The framework-owned original event must stay untouched.
        val copy = MotionEvent.obtain(event)
        try {
            copy.action = MotionEvent.ACTION_CANCEL
            return dispatch(copy)
        } finally { copy.recycle() }
    }

    fun dispatchGeneric(
        event: MotionEvent,
        touch: (MotionEvent) -> Boolean,
        generic: (MotionEvent) -> Boolean
    ): Boolean {
        if (disposed) return generic(event)
        val index = pointerIndex(event) ?: return generic(event)
        val pointerId = event.getPointerId(index)
        val toolType = event.getToolType(index)
        if (event.actionMasked == MotionEvent.ACTION_HOVER_EXIT) {
            var canceledContact = false
            if (policy.hoverExit(pointerId, toolType)) {
                // Unexpected contact exit must not leave Flutter down. Cancel
                // the actual supplied motion set, never synthesize a normal UP.
                // Retire its other pens too; unrelated devices/keyboard stay.
                for (p in 0 until event.pointerCount) {
                    policy.hoverExit(event.getPointerId(p), event.getToolType(p))
                }
                val copy = MotionEvent.obtain(event)
                try {
                    copy.action = MotionEvent.ACTION_CANCEL
                    canceledContact = touch(copy)
                } finally { copy.recycle() }
            }
            val signal = PenInputPolicy.exit(pointerId, toolType, event.eventTime)
            if (signal != null) sink?.success(mapOf(
                "type" to "exit", "device" to signal.device,
                "kind" to signal.kind, "timeMicros" to signal.timeMicros
            ))
            val handledExit = generic(event)
            return canceledContact || handledExit
        }
        val route = policy.buttonDispatch(event.actionMasked, pointerId, toolType) ?: return generic(event)
        // Flutter's generic path reads actionIndex (zero for HOVER_MOVE).
        // Clone only the selected pen, retaining its ID/tool and current axes,
        // so neither hit testing nor Flutter selects another pointer at zero.
        // This public obtain overload exists since API 14; no hidden split.
        val properties = MotionEvent.PointerProperties()
        val coordinates = MotionEvent.PointerCoords()
        event.getPointerProperties(index, properties)
        event.getPointerCoords(index, coordinates)
        val action = when (route) {
            PenInputPolicy.ButtonDispatch.TOUCH_MOVE -> MotionEvent.ACTION_MOVE
            PenInputPolicy.ButtonDispatch.HOVER_MOVE -> MotionEvent.ACTION_HOVER_MOVE
        }
        val copy = MotionEvent.obtain(
            event.downTime, event.eventTime, action, 1,
            arrayOf(properties), arrayOf(coordinates), event.metaState,
            event.buttonState, event.xPrecision, event.yPrecision,
            event.deviceId, event.edgeFlags, event.source, event.flags
        )
        try {
            return when (route) {
                PenInputPolicy.ButtonDispatch.TOUCH_MOVE -> touch(copy)
                PenInputPolicy.ButtonDispatch.HOVER_MOVE -> generic(copy)
            }
        } finally { copy.recycle() }
    }

    fun clear() { policy.clear() }
    fun dispose() {
        if (disposed) return
        disposed = true
        sink = null
        policy.clear()
        channel.setStreamHandler(null)
    }
}
