package me.totb.multitap_client

import me.totb.multitap_client.StreamLockPolicy.Step
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

// The stream's Wi-Fi lock (AndroidStreamWifi): which mode per API level, held
// only while a stream runs and is shown, and a refusal leaves the stream as
// it was before the lock existed.
class StreamLockPolicyTest {
    @Test fun lowLatencyFromAndroid10HighPerformanceBefore() {
        for (api in 26..28) assertEquals(WIFI_LOCK_HIGH_PERF, streamWifiLockMode(api))
        for (api in 29..36) assertEquals(WIFI_LOCK_LOW_LATENCY, streamWifiLockMode(api))
        // WifiManager.WIFI_MODE_FULL_HIGH_PERF and WIFI_MODE_FULL_LOW_LATENCY.
        assertEquals(3, WIFI_LOCK_HIGH_PERF)
        assertEquals(4, WIFI_LOCK_LOW_LATENCY)
    }

    @Test fun heldWhileAStreamRunsAndIsShown() {
        val policy = StreamLockPolicy()
        assertEquals(Step.NONE, policy.visible(true))
        assertEquals(Step.ACQUIRE, policy.streaming(true))
        policy.acquired(true)
        assertTrue(policy.held)
        // The display request is made again (rotation, a new surface): nothing more to do.
        assertEquals(Step.NONE, policy.streaming(true))
        assertEquals(Step.RELEASE, policy.streaming(false))
        policy.released()
        assertFalse(policy.held)
        assertEquals(Step.NONE, policy.streaming(false))
    }

    @Test fun releasedInTheBackgroundAndTakenAgainOnReturn() {
        val policy = StreamLockPolicy()
        policy.streaming(true); policy.acquired(true)
        assertEquals(Step.RELEASE, policy.visible(false))
        policy.released()
        assertEquals(Step.NONE, policy.visible(false))
        assertEquals(Step.ACQUIRE, policy.visible(true))
        policy.acquired(true)
        assertTrue(policy.held)
    }

    @Test fun aStreamStartedInTheBackgroundWaitsForTheAppToShow() {
        val policy = StreamLockPolicy()
        policy.visible(false)
        assertEquals(Step.NONE, policy.streaming(true))
        assertFalse(policy.held)
        assertEquals(Step.ACQUIRE, policy.visible(true))
    }

    @Test fun aRefusalIsCountedAndNotRepeatedUntilTheNextStream() {
        val policy = StreamLockPolicy()
        assertEquals(Step.ACQUIRE, policy.streaming(true))
        policy.acquired(false)
        assertFalse(policy.held)
        assertEquals(1, policy.refused)
        // Hidden and shown again, the request repeated: no second attempt for this stream.
        assertEquals(Step.NONE, policy.visible(false))
        assertEquals(Step.NONE, policy.visible(true))
        assertEquals(Step.NONE, policy.streaming(true))
        // Nothing to release at its end, and the next stream asks once more.
        assertEquals(Step.NONE, policy.streaming(false))
        assertEquals(Step.ACQUIRE, policy.streaming(true))
        policy.acquired(false)
        assertEquals(2, policy.refused)
    }

    @Test fun theStatNamesTheModeOnlyWhileHeld() {
        assertEquals(0, streamWifiLockStat(false, WIFI_LOCK_LOW_LATENCY))
        assertEquals(1, streamWifiLockStat(true, WIFI_LOCK_LOW_LATENCY))
        assertEquals(2, streamWifiLockStat(true, WIFI_LOCK_HIGH_PERF))
        assertEquals(0, streamWifiLockStat(false, WIFI_LOCK_HIGH_PERF))
    }
}
