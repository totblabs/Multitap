package me.totb.multitap_client

import android.content.Context
import android.os.Build
import android.os.PowerManager

/**
 * The device's thermal status while a stream runs (PowerManager
 * THERMAL_STATUS_*: 0 none, 1 light, 2 moderate, 3 severe, 4 critical,
 * 5 emergency, 6 shutdown; API 29+): the latest and the highest of this
 * stream. Reported only. The engine does not act on it: the Host already
 * lowers the picture when this client's decoder falls behind (stream-size-v1),
 * which is what throttling does to it. Pure, so the JVM tests run it.
 */
internal class ThermalWatch {
    @Volatile var status: Int? = null
        private set
    @Volatile var highest: Int? = null
        private set

    /** A stream starts: its highest status counts from here. */
    fun started() { status = null; highest = null }

    fun changed(value: Int) {
        if (value !in 0..6) return
        status = value
        val before = highest
        if (before == null || value > before) highest = value
    }
}

/** Main thread only; [watch] is read by the statistics on another thread. Nothing below API 29. */
internal class AndroidThermal(context: Context) {
    val watch = ThermalWatch()
    private val power = context.applicationContext.getSystemService(Context.POWER_SERVICE) as PowerManager?
    private var active = false
    private var listening: Any? = null

    /** Asked again while a stream runs (the display request repeats): nothing changes then. */
    fun streaming(on: Boolean) {
        if (Build.VERSION.SDK_INT < 29 || on == active) return
        active = on
        if (on) { watch.started(); listening = Api29.listen(power, watch) }
        else { Api29.forget(power, listening); listening = null }
    }

    // Apart from the class above, so a release before API 29 never resolves the listener type.
    private object Api29 {
        fun listen(power: PowerManager?, watch: ThermalWatch): Any? = try {
            if (power == null) null
            else {
                val listener = PowerManager.OnThermalStatusChangedListener { status -> watch.changed(status) }
                watch.changed(power.currentThermalStatus)
                // Called on the main thread at every change.
                power.addThermalStatusListener(listener)
                listener
            }
        } catch (_: Exception) { null }

        fun forget(power: PowerManager?, listener: Any?) {
            try {
                if (power != null && listener is PowerManager.OnThermalStatusChangedListener) power.removeThermalStatusListener(listener)
            } catch (_: Exception) {}
        }
    }
}
