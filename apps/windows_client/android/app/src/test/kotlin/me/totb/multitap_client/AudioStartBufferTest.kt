package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

// AndroidAudioTrim with the track's buffer and start level, as AndroidMedia uses them
// (audioBufferFollowsCap, AudioStartWatch): a track that plays only once a level is
// queued must always get that level, and the cap must keep its headroom above the
// target as the target grows after underruns.
class AudioStartBufferTest {
    // AndroidMedia's values (2026-11-11).
    private val capacity = 48L * 200
    private val cap = 48L * 80
    private val target = 48L * 40
    private val maxTarget = 48L * 120
    private val packet = ByteArray(480 * 4)
    private val ms = 1_000_000L

    private fun trim() = AndroidAudioTrim(targetFrames = target, capFrames = cap, maxTargetFrames = maxTarget, capacityFrames = capacity)

    /**
     * Frames queued in a track that has not started (nothing is played), after
     * [packets] 10 ms packets, with the buffer following the cap as AndroidMedia does.
     */
    private fun fill(trim: AndroidAudioTrim, packets: Int = 40, level: Long = trim.wantedCap): Long {
        var queued = 0L
        trim.startLevel = level
        for (index in 0 until packets) {
            val now = index * 10 * ms
            if (trim.starting) {
                val held = trim.start(packet, 0, packet.size, now)
                if (held < 0) continue
                queued += held / 4
            }
            // The track's write never takes more than its effective buffer (the cap here).
            if (trim.accept(queued, 480, now)) queued = minOf(trim.cap, queued + 480)
        }
        return queued
    }

    @Test fun theBufferFollowsTheCapBeforeAndroid12OrWithoutAStartThreshold() {
        for (api in 26..30) assertTrue(audioBufferFollowsCap(api, startThresholdApplied = true))
        assertFalse(audioBufferFollowsCap(31, startThresholdApplied = true))
        assertFalse(audioBufferFollowsCap(36, startThresholdApplied = true))
        assertTrue("a threshold that did not take: the track waits for its buffer", audioBufferFollowsCap(31, startThresholdApplied = false))
    }

    @Test fun theFirstFillReachesTheLevelAFullBufferTrackWaitsFor() {
        val trim = trim()
        val queued = fill(trim)
        assertEquals(cap, trim.startLevel)
        assertEquals("a track that waits for its whole buffer gets it", trim.startLevel, queued)
    }

    @Test fun theCapKeepsItsHeadroomAsTheTargetGrows() {
        // 507887d: before Android 12 the cap was the track's 80 ms buffer and the target grew to
        // the same 80 ms, so nothing was left above the target. Now the cap stays 40 ms above it.
        val trim = trim()
        var now = 0L
        repeat(8) { if (trim.starting) trim.start(packet, 0, packet.size, now); now += 10 * ms }
        assertTrue(trim.accept(target, 480, now, underruns = 0, ptsMicros = now / 1000))
        var underruns = 0L
        repeat(10) {
            now += 10 * ms
            trim.accept(0, 480, now, underruns = ++underruns, ptsMicros = now / 1000)
            assertEquals(trim.target + (cap - target), trim.cap)
        }
        assertEquals(maxTarget, trim.target)
        assertEquals(maxTarget + (cap - target), trim.cap)
        assertTrue(trim.cap <= capacity)
    }

    @Test fun aStartLevelAboveTheCapRaisesTheCap() {
        // A track that reports it waits for more than the trim would queue (a clamped buffer,
        // a start threshold the platform raised): the trim lets that much be queued.
        val trim = trim()
        trim.startLevel = 48L * 100
        assertEquals(48L * 100, trim.cap)
        assertEquals("the queue reaches the level the track waits for", 48L * 100, fill(trim, level = 48L * 100))
    }

    @Test fun afterItRanDryWholePacketsFillItAgain() {
        // A pause in the VM's sound: the track ran dry and waits for a full buffer again.
        val trim = trim()
        trim.startLevel = trim.wantedCap
        var queued = 0L
        var now = 0L
        repeat(8) { if (trim.starting) trim.start(packet, 0, packet.size, now); now += 10 * ms }
        repeat(100) { assertTrue(trim.accept(target, 480, now)); now += 10 * ms }
        now += 3_000 * ms
        repeat(20) { if (trim.accept(queued, 480, now)) queued += 480; now += 10 * ms }
        assertEquals(trim.cap, queued)
    }

    @Test fun aTrackWrittenToThatDoesNotPlayIsSeenAfterOneSecond() {
        val watch = AudioStartWatch()
        var now = 0L
        // Playing: the position moves, never stuck even when full.
        repeat(300) { assertFalse(watch.stuck(now, played = it * 480L, full = true)); now += 10 * ms }
        // Full and not moving: stuck after 1 s, not before.
        val played = 300 * 480L
        repeat(100) { assertFalse(watch.stuck(now, played, full = true)); now += 10 * ms }
        assertTrue(watch.stuck(now, played, full = true))
        // Not full (still filling, or a refill after a pause): never stuck.
        watch.reset()
        repeat(300) { assertFalse(watch.stuck(now, played, full = false)); now += 10 * ms }
    }
}
