package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

// The track's start level, its buffer and how it is made again (AndroidAudioJitter.kt, as
// AndroidAudioOut uses them): a track that plays only once a level is in it must get that level
// from the first write after priming, and a stream must get its sound back after a later failure.
class AudioStartBufferTest {
    private val ms = 1_000_000L
    private val packet = ByteArray(1920)

    @Test fun theBufferIsTheTargetBeforeAndroid12OrWithoutAStartThreshold() {
        for (api in 26..30) assertTrue(audioBufferFollowsTarget(api, startThresholdApplied = true))
        assertFalse(audioBufferFollowsTarget(31, startThresholdApplied = true))
        assertFalse(audioBufferFollowsTarget(36, startThresholdApplied = true))
        assertTrue("a threshold that did not take: the track waits for its buffer", audioBufferFollowsTarget(31, startThresholdApplied = false))
    }

    /** One look of a track that has not started: [room] free, [device] in it. */
    private fun firstWrite(buffer: AudioJitterBuffer, device: Int, room: Int, nowUs: Long): Int =
        buffer.pull(ByteArray(9600 * 4), room, device, nowUs)

    @Test fun primingWritesTheWholeTargetAtOnce() {
        // Before API 31 the track's buffer is the target and it plays once that buffer is full.
        val buffer = AudioJitterBuffer(AudioJitterConfig.ANDROID)
        buffer.followDevice(1920)
        for (k in 0 until 3) { buffer.push(k * 10_000L, packet, 0, packet.size, k * 10_000L); assertEquals(0, firstWrite(buffer, 0, 1920, k * 10_000L)) }
        buffer.push(30_000, packet, 0, packet.size, 30_000)
        assertEquals("the first write fills the track's whole buffer", 1920, firstWrite(buffer, 0, 1920, 30_000))
    }

    /** [packets] 10 ms packets in real time (none when [silent]) into a track that plays 480 frames per 10 ms. */
    private class Steady(val buffer: AudioJitterBuffer, val bufferFrames: Int) {
        var now = 0L
        var device = 0
        var ptsUs = 0L
        val out = ByteArray(9600 * 4)
        fun run(packets: Int, silent: Boolean = false) = repeat(packets) {
            if (!silent) { buffer.push(ptsUs, ByteArray(1920), 0, 1920, now); ptsUs += 10_000 }
            device = maxOf(0, device - 480)
            device += buffer.pull(out, bufferFrames - device, device, now)
            now += 10_000
        }
    }

    @Test fun afterAPauseTheFirstWriteMeetsTheRestartLevelAgain() {
        // The VM fell silent long enough for the mixer to disable the track; its next start needs a full buffer.
        val buffer = AudioJitterBuffer(AudioJitterConfig.ANDROID)
        buffer.followDevice(2880)
        val track = Steady(buffer, 2880)
        track.run(100)
        assertTrue(buffer.playing)
        track.run(200, silent = true)
        assertEquals("ran dry", 0, track.device)
        track.ptsUs += 2_000_000
        for (k in 0 until 5) { buffer.push(track.ptsUs, packet, 0, packet.size, track.now); track.ptsUs += 10_000; assertEquals(0, firstWrite(buffer, 0, 2880, track.now)); track.now += 10_000 }
        buffer.push(track.ptsUs, packet, 0, packet.size, track.now)
        assertEquals("primed again: the restart level from the first write", 2880, firstWrite(buffer, 0, 2880, track.now))
        assertEquals(1L, buffer.counters.silences)
        assertEquals(0L, buffer.counters.underruns)
    }

    @Test fun aStartThresholdAboveTheTargetRaisesTheTarget() {
        // A threshold the platform raised (API 31+): the buffer never aims below it.
        val buffer = AudioJitterBuffer(AudioJitterConfig.ANDROID)
        buffer.raiseFloor(48 * 50)
        assertEquals(48 * 50, buffer.target)
        var now = 0L
        repeat(4) { buffer.push(now, packet, 0, packet.size, now); assertEquals(0, firstWrite(buffer, 0, 9600, now)); now += 10_000 }
        buffer.push(now, packet, 0, packet.size, now)
        assertEquals(48 * 50, firstWrite(buffer, 0, 9600, now))
        // Calm for a minute: the target does not shrink below the threshold.
        val calm = AudioJitterBuffer(AudioJitterConfig.ANDROID)
        calm.raiseFloor(48 * 50)
        Steady(calm, 9600).run(6000)
        assertEquals(48 * 50, calm.target)
        assertEquals(0L, calm.counters.underruns)
    }

    @Test fun theLevelIsWrittenMinusTheHeadAcrossTheWrap() {
        assertEquals(480, audioDeviceFrames(written = 1000, head = 520, capacity = 9600))
        // The head is a 32-bit counter: it wraps after 2^32 frames (about 25 h).
        assertEquals(100, audioDeviceFrames(written = (1L shl 32) + 50, head = (-50), capacity = 9600))
        assertEquals("a head past the writes is not a level", 0, audioDeviceFrames(written = 100, head = 200, capacity = 9600))
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

    @Test fun remakesAreRateLimitedNotCountedOutForTheStream() {
        val limiter = RemakeLimiter()
        val second = 1_000_000_000L
        assertTrue(limiter.allow(0))
        assertFalse("at most one a second", limiter.allow(second / 2))
        assertTrue(limiter.allow(2 * second))
        assertTrue(limiter.allow(4 * second))
        assertFalse("three in a minute", limiter.allow(10 * second))
        assertFalse(limiter.allow(59 * second))
        // A later route change (the old rule allowed three a stream, then never again): allowed again.
        assertTrue("the first left the minute", limiter.allow(60 * second))
        assertFalse(limiter.allow(61 * second + second / 2))
        assertTrue(limiter.allow(62 * second))
        var allowed = 0
        var now = 100 * second
        repeat(600) { if (limiter.allow(now)) allowed++; now += second }
        assertEquals("ten minutes of failures: three a minute", 30, allowed)
        limiter.reset()
        assertTrue(limiter.allow(now))
    }
}
