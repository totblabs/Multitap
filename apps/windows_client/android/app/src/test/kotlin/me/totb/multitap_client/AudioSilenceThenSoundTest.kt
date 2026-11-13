package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.PI
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.roundToLong
import kotlin.math.sin

/**
 * The owner's report (2026-11-13, Android 1.1.3 published minutes before):
 * the desktop was silent for a while, then a video was opened on it and the
 * phone played nothing. The Host sends nothing while the VM is silent and
 * stamps every frame from its own wall clock, so the first frame of the video
 * comes with a PTS seconds to minutes after the last one (or after the
 * stream's start). Here the production playout (AudioJitterBuffer with
 * AudioJitterConfig.ANDROID, AudioFeed every 5 ms) runs that against the
 * mixer paths a phone can give the track, including the one AudioPlayoutTest
 * has no model of: Android 12 or later with the start threshold applied but
 * the fast path refused (a normal mixer that reads 20 ms periods and leaves a
 * partial period in the track when the sound stops).
 *
 * The simulated mixer is AudioPlayoutTest's, with two platform behaviours
 * that differ between devices made switchable: whether a normal-path period
 * the track could not fill counts in its own underrun count, and whether its
 * play head keeps moving while the track is dry (the read position counting
 * the silence it was given). Either way the sound must come back.
 */
class AudioSilenceThenSoundTest {
    private class Track(
        var buffer: Int,
        val capacity: Int = 9600,
        val quantum: Int,
        val periodUs: Long,
        val fast: Boolean,
        var startLevel: Int,
        val startIsBuffer: Boolean,
        val retries: Int = 50,
        /** A normal-path period the track could not fill counts in its underrun count. */
        val countsNormalUnderruns: Boolean = true,
        /** The play head also counts the silence the mixer played for the dry track. */
        val headCountsSilence: Boolean = false,
    ) : AudioFeed.Sink {
        var queued = 0
        var played = 0L
        /** Frames of received sound the mixer played (not silence). */
        var sound = 0L
        var underrunCount = 0L
        private var inUnderrun = false
        private var dry = 0
        var filling = true
        var disabled = false
        val starts = ArrayList<Long>()

        override fun head(): Int = played.toInt()
        override fun underruns(): Long = underrunCount
        override fun write(buffer: ByteArray, offset: Int, bytes: Int): Int {
            val frames = min(bytes / 4, max(0, min(this.buffer, capacity) - queued))
            queued += frames
            // AudioTrack.releaseBuffer: a track the mixer disabled for running dry is started again (filling).
            if (frames > 0 && disabled) { disabled = false; filling = true }
            return frames * 4
        }
        override fun bufferSize(frames: Int): Int {
            buffer = frames.coerceIn(1, capacity)
            if (startIsBuffer) startLevel = buffer
            return buffer
        }

        fun mix(nowUs: Long) {
            if (disabled) return
            if (filling) {
                if (queued > 0 && queued >= min(startLevel, buffer)) { filling = false; starts.add(nowUs) } else return
            }
            if (queued >= quantum) { queued -= quantum; played += quantum; sound += quantum; inUnderrun = false; dry = 0; return }
            if (!inUnderrun && (fast || countsNormalUnderruns)) { inUnderrun = true; underrunCount++ }
            if (fast) { played += queued; sound += queued; queued = 0 }
            if (headCountsSilence) played += if (fast) 0 else quantum
            if (++dry >= retries) { disabled = true; dry = 0; inUnderrun = false }
        }
    }

    private class Rig(val track: Track, follows: Boolean) {
        val playout = AudioJitterBuffer(AudioJitterConfig.ANDROID)
        val feed = AudioFeed(playout, 9600)
        var now = 0L
        private val arrivals = ArrayList<LongArray>()
        private var next = 0
        private val bytes = ByteArray(1920)

        init {
            playout.clear(); feed.newStream()
            val buffer = track.bufferSize(if (follows) playout.target else track.capacity)
            feed.track(follows, buffer, track.capacity, if (follows) buffer else track.startLevel, fineReads = track.fast)
            for (j in 0 until 480) { val v = (8000 * sin(2 * PI * j / 48)).roundToInt(); PcmFade.put(bytes, j * 4, v); PcmFade.put(bytes, j * 4 + 2, v) }
        }

        /** The Host's sends: its loop wakes every 15.625 ms and sends each 10 ms frame of sound; none while [silent]. */
        fun host(untilUs: Long, silent: (Long) -> Boolean) {
            var sent = 0L
            var k = 1L
            while (true) {
                val wake = (k * 15_625.0).roundToLong()
                if (wake > untilUs) break
                val complete = floor(wake / 10_000.0).toLong()
                while (sent < complete) { if (!silent(sent)) arrivals.add(longArrayOf(wake, sent * 10_000)); sent++ }
                k++
            }
        }

        fun run(untilUs: Long) {
            var nextMix = now + track.periodUs
            var nextTick = now + 2_300
            while (true) {
                val arrival = if (next < arrivals.size) arrivals[next][0] else Long.MAX_VALUE
                val t = min(arrival, min(nextMix, nextTick))
                if (t > untilUs) break
                now = t
                if (nextMix == t) { track.mix(now); nextMix += track.periodUs }
                while (next < arrivals.size && arrivals[next][0] <= now) {
                    assertTrue(synchronized(playout) { playout.push(arrivals[next][1], bytes, 0, bytes.size, now) })
                    next++
                }
                if (nextTick == t) { assertEquals(0, feed.tick(track, now * 1000)); nextTick += 5_000 }
            }
        }

        /** Sound frames the mixer played between [fromUs] and [untilUs] (run in two steps). */
        fun soundBetween(fromUs: Long, untilUs: Long): Long {
            run(fromUs)
            val before = track.sound
            run(untilUs)
            return track.sound - before
        }
    }

    private val second = 1_000_000L

    private fun paths(countsNormalUnderruns: Boolean = true, headCountsSilence: Boolean = false) = listOf(
        "fast path (API 31+)" to { Rig(Track(9600, quantum = 48, periodUs = 1000, fast = true, startLevel = 1440, startIsBuffer = false,
            countsNormalUnderruns = countsNormalUnderruns, headCountsSilence = headCountsSilence), follows = false) },
        "normal path, API 31+ (start threshold, 20 ms periods)" to { Rig(Track(9600, quantum = 960, periodUs = 20_000, fast = false, startLevel = 1440,
            startIsBuffer = false, countsNormalUnderruns = countsNormalUnderruns, headCountsSilence = headCountsSilence), follows = false) },
        "normal path, Android 8-11 (buffer is the target)" to { Rig(Track(9600, quantum = 960, periodUs = 20_000, fast = false, startLevel = 9600,
            startIsBuffer = true, countsNormalUnderruns = countsNormalUnderruns, headCountsSilence = headCountsSilence), follows = true) },
    )

    private fun assertPlays(name: String, rig: Rig, fromUs: Long, untilUs: Long) {
        val sound = rig.soundBetween(fromUs, untilUs)
        val expected = (untilUs - fromUs) * 48 / 1000
        val c = rig.playout.counters
        assertTrue("$name: ${sound / 48} ms of sound played in ${(untilUs - fromUs) / 1000} ms (playing ${rig.feed.playing}, level ${rig.playout.level}, " +
            "target ${rig.playout.target}, device ${rig.feed.device}, silences ${c.silences}, underruns ${c.underruns}, track ${c.trackUnderruns}, tails ${c.tails}, " +
            "overruns ${c.overruns}, starts ${rig.track.starts.size})", sound >= expected * 95 / 100)
    }

    @Test fun theFirstSoundOfAStreamAfterHalfAMinuteOfSilencePlays() {
        for (counts in listOf(true, false)) for (head in listOf(false, true)) for ((name, make) in paths(counts, head)) {
            val rig = make()
            // The stream starts silent; a video starts 30 s later and plays for 20 s.
            rig.host(51 * second, silent = { n -> n < 3000 })
            assertPlays("$name, normal underruns counted $counts, head counts silence $head", rig, 31 * second, 50 * second)
        }
    }

    @Test fun soundAfterMinutesOfSilenceInTheMiddleOfAStreamPlays() {
        for (counts in listOf(true, false)) for (head in listOf(false, true)) for ((name, make) in paths(counts, head)) {
            for (pauseS in longArrayOf(2, 30, 300)) {
                val rig = make()
                // Clicks and UI sounds for 10 s, silence, then a video for 20 s.
                val videoAt = (10 + pauseS) * 100
                rig.host((31 + pauseS) * second, silent = { n -> n in 1000 until videoAt })
                assertPlays("$name, pause $pauseS s, normal underruns counted $counts, head counts silence $head", rig, 1 * second, 9 * second)
                assertPlays("$name, pause $pauseS s, normal underruns counted $counts, head counts silence $head (after the pause)", rig,
                    (11 + pauseS) * second, (30 + pauseS) * second)
            }
        }
    }

    @Test fun aTrackThatTakesNothingIsCalledStuckAndMadeAgain() {
        // A track whose writes take nothing from the first priming write on (non-blocking write() 0)
        // and that never plays: the feed waits with its priming write pending, the track holds nothing.
        // The level alone (held >= start level) never called that stuck; the pending write does.
        class Refusing : AudioFeed.Sink {
            override fun head() = 0
            override fun underruns() = 0L
            override fun write(buffer: ByteArray, offset: Int, bytes: Int) = 0
            override fun bufferSize(frames: Int) = frames
        }
        for (follows in listOf(false, true)) {
            val playout = AudioJitterBuffer(AudioJitterConfig.ANDROID)
            val feed = AudioFeed(playout, 9600)
            playout.clear(); feed.newStream()
            feed.track(follows, if (follows) playout.target else 9600, 9600, if (follows) playout.target else 1440, fineReads = true)
            val watch = AudioStartWatch()
            val levelOnly = AudioStartWatch()
            var levelOnlyStuck = false
            val sink = Refusing()
            val packet = ByteArray(1920)
            var now = 0L
            var stuckAt = -1L
            var pts = 30 * second
            while (now < 3 * second) {
                if (now % 10_000 == 0L) { synchronized(playout) { playout.push(pts, packet, 0, packet.size, now) }; pts += 10_000 }
                assertEquals(0, feed.tick(sink, now * 1000))
                if (stuckAt < 0 && watch.stuck(now * 1000, feed.head.toLong(), feed.waitsToPlay)) stuckAt = now
                // 1.1.3's rule (the level only) on the same looks.
                if (levelOnly.stuck(now * 1000, feed.head.toLong(), feed.playing && feed.device > 0 && feed.device >= feed.startLevel)) levelOnlyStuck = true
                now += 5_000
            }
            assertTrue("follows $follows: the level alone never calls it stuck", !levelOnlyStuck)
            assertTrue("follows $follows: the priming write is pending", feed.pending > 0 && feed.playing && feed.device == 0)
            assertTrue("follows $follows: called stuck at ${stuckAt / 1000} ms", stuckAt in 1_000_000L..1_200_000L)
        }
    }

    @Test fun manyShortSoundsThenAVideoStillPlay() {
        // Notification-like blips (60 ms of sound every 3 s, ending with partial periods in the track) for a minute, then a video.
        for (counts in listOf(true, false)) for ((name, make) in paths(counts)) {
            val rig = make()
            rig.host(90 * second, silent = { n -> n < 6000 && n % 300 >= 6 })
            assertPlays("$name, normal underruns counted $counts", rig, 61 * second, 89 * second)
        }
    }
}
