package me.totb.multitap_client

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build

/**
 * The Wi-Fi lock of a stream that is shown (WifiManager.WifiLock).
 * Without one the Wi-Fi radio keeps its power save and background scans
 * while a stream runs, which shows as late packets in bursts.
 *
 * From API 29 WIFI_MODE_FULL_LOW_LATENCY (4): Android itself applies it only
 * while the device is on an access point, the screen is on and the app is in
 * the foreground, and names real-time games as its use. Before it
 * WIFI_MODE_FULL_HIGH_PERF (3, no power save), which has no such limit and
 * is therefore held only as [StreamLockPolicy] says. On mobile data, Ethernet
 * or without Wi-Fi the lock does nothing.
 */
internal fun streamWifiLockMode(api: Int): Int = if (api >= 29) WIFI_LOCK_LOW_LATENCY else WIFI_LOCK_HIGH_PERF

internal const val WIFI_LOCK_HIGH_PERF = 3
internal const val WIFI_LOCK_LOW_LATENCY = 4

/** Stream stats `wifiLock`: 0 not held, 1 low latency, 2 high performance. */
internal fun streamWifiLockStat(held: Boolean, mode: Int): Int =
    if (!held) 0 else if (mode == WIFI_LOCK_LOW_LATENCY) 1 else 2

/**
 * When the stream's Wi-Fi lock is held: while a stream runs and the app is
 * visible, never otherwise. A refused lock (no permission, no Wi-Fi service)
 * is counted and not asked for again until the next stream: the stream goes
 * on exactly as before the lock existed. Pure, so the JVM tests run it.
 */
internal class StreamLockPolicy {
    enum class Step { NONE, ACQUIRE, RELEASE }

    var streaming = false
        private set
    var visible = true
        private set
    var held = false
        private set
    private var gaveUp = false
    /** Refused acquisitions of this app run. */
    var refused = 0
        private set

    fun streaming(on: Boolean): Step {
        if (!on) gaveUp = false
        streaming = on
        return step()
    }

    fun visible(shown: Boolean): Step {
        visible = shown
        return step()
    }

    /** The answer to an ACQUIRE step. */
    fun acquired(ok: Boolean) {
        if (ok) held = true else { refused++; gaveUp = true }
    }

    /** The answer to a RELEASE step (released or not, it is no longer counted on). */
    fun released() { held = false }

    private fun step(): Step {
        val wanted = streaming && visible
        return when {
            wanted && !held && !gaveUp -> Step.ACQUIRE
            !wanted && held -> Step.RELEASE
            else -> Step.NONE
        }
    }
}

/** Main thread only; [stat] and [refused] are read by the statistics on another thread. */
internal class AndroidStreamWifi(context: Context) {
    private val application = context.applicationContext
    private val policy = StreamLockPolicy()
    private val mode = streamWifiLockMode(Build.VERSION.SDK_INT)
    private var lock: WifiManager.WifiLock? = null

    /** Stream stats `wifiLock` (streamWifiLockStat) and `wifiLockRefused`. */
    @Volatile var stat = 0
        private set
    @Volatile var refused = 0
        private set

    fun streaming(on: Boolean) = perform(policy.streaming(on))
    fun visible(shown: Boolean) = perform(policy.visible(shown))

    private fun perform(step: StreamLockPolicy.Step) {
        when (step) {
            StreamLockPolicy.Step.ACQUIRE -> policy.acquired(acquire())
            StreamLockPolicy.Step.RELEASE -> { try { lock?.release() } catch (_: Exception) {}; policy.released() }
            StreamLockPolicy.Step.NONE -> {}
        }
        stat = streamWifiLockStat(policy.held, mode)
        refused = policy.refused
    }

    /** False when the device refuses: no WAKE_LOCK permission (SecurityException), no Wi-Fi service. */
    private fun acquire(): Boolean = try {
        val made = lock ?: (application.getSystemService(Context.WIFI_SERVICE) as WifiManager)
            .createWifiLock(mode, "Multitap stream").also { it.setReferenceCounted(false); lock = it }
        made.acquire()
        made.isHeld
    } catch (_: Exception) { false }
}
