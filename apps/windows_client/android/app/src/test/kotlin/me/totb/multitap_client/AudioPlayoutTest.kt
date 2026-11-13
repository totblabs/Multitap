package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Random
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.roundToLong
import kotlin.math.sin

/**
 * The Android playout as it runs (AudioJitterConfig.ANDROID, AudioFeed every
 * 5 ms) against a simulated AudioTrack and the Host's real send cadence: what
 * the owner hears as gaps, crackle and silence, in numbers. No device, no
 * AudioTrack, no network.
 *
 * The simulated track (AOSP AudioFlinger as far as it matters here; what is
 * not certain is said in AndroidAudioJitter.kt): a mixer reads [quantum]
 * frames every period. A track on the normal mixer path is mixed only when a
 * whole period is ready, otherwise that period is silent; the fast path takes
 * what there is. A run of silent periods is one underrun in the track's own
 * count. After [retries] such periods the mixer disables the track; a track
 * that is filling (after play(), or after being disabled) starts only once
 * min(start level, buffer) is in it.
 *
 * The Host (apps/windows_host/src/media.cpp) sends every complete 10 ms frame
 * when its capture loop wakes, about every 15.625 ms (a 1-2-1-2 cadence), and
 * nothing while the VM is silent.
 */
class AudioPlayoutTest {
    private class FakeTrack(
        var buffer: Int,
        val capacity: Int = 9600,
        val quantum: Int = 48,
        val periodUs: Long = 1000,
        val fast: Boolean = true,
        var startLevel: Int = 1440,
        val startIsBuffer: Boolean = false,
        val retries: Int = 50,
        val acceptLimit: Int = Int.MAX_VALUE,
        val granule: Int = 1,
        val record: Boolean = false,
    ) : AudioFeed.Sink {
        private val ring = IntArray(capacity)
        private var readAt = 0
        var queued = 0
        var played = 0L
        var underrunCount = 0L
        private var inUnderrun = false
        private var dry = 0
        var filling = true
        /** Times (µs) of underrun events: periods the track had too little for, after it had started. */
        val events = ArrayList<Long>()
        /** What the mixer played (packed stereo), with silence for the periods it had nothing. */
        var out = IntArray(if (record) 1 shl 20 else 0)
        var outSize = 0
        /** Frames in the track each time it started (after play() or after being disabled). */
        val starts = ArrayList<Int>()

        override fun head(): Int = played.toInt()
        override fun underruns(): Long = underrunCount
        override fun write(buffer: ByteArray, offset: Int, bytes: Int): Int {
            val frames = min(min(bytes / 4, acceptLimit), max(0, min(this.buffer, capacity) - queued))
            for (k in 0 until frames) {
                val at = offset + k * 4
                ring[(readAt + queued) % capacity] = (PcmFade.sample(buffer, at) and 0xffff) or (PcmFade.sample(buffer, at + 2) shl 16)
                queued++
            }
            return frames * 4
        }
        override fun bufferSize(frames: Int): Int {
            buffer = (ceil(frames.toDouble() / granule).toInt() * granule).coerceIn(granule, capacity)
            if (startIsBuffer) startLevel = buffer
            return buffer
        }

        fun mix(nowUs: Long) {
            if (filling) {
                if (queued > 0 && queued >= min(startLevel, buffer)) { filling = false; starts.add(queued) } else { emit(0); return }
            }
            if (queued >= quantum) { consume(quantum); inUnderrun = false; dry = 0; return }
            if (!inUnderrun) { inUnderrun = true; underrunCount++; events.add(nowUs) }
            emit(quantum - if (fast) queued else 0)
            if (fast) consume(queued)
            if (++dry >= retries) { filling = true; dry = 0; inUnderrun = false }
        }

        private fun consume(frames: Int) {
            for (k in 0 until frames) { log(ring[readAt]); readAt = (readAt + 1) % capacity }
            queued -= frames; played += frames
        }
        private fun emit(silent: Int) { for (k in 0 until silent) log(0) }
        private fun log(frame: Int) {
            if (!record) return
            if (outSize == out.size) out = out.copyOf(out.size * 2)
            out[outSize++] = frame
        }
    }

    private class Rig(val track: FakeTrack, follows: Boolean, config: AudioJitterConfig = AudioJitterConfig.ANDROID,
        val content: (Long) -> Int = ::tone) {
        val playout = AudioJitterBuffer(config)
        val feed = AudioFeed(playout, 9600)
        var now = 0L
        private val arrivals = ArrayList<LongArray>()   // (at µs, pts µs, packet number)
        private var next = 0
        /** (µs, level ms) after each look while sound plays. */
        val levels = ArrayList<DoubleArray>()
        var firstSound = -1L
        var firstArrival = -1L
        var maxPending = 0
        private val bytes = ByteArray(1920)

        init {
            playout.clear(); feed.newStream()
            val buffer = track.bufferSize(if (follows) playout.target else track.capacity)
            feed.track(follows, buffer, track.capacity, if (follows) buffer else track.startLevel, fineReads = track.fast)
        }

        /**
         * The Host's sends until [untilUs]: its loop wakes every [wakeUs] of this
         * device's clock and sends every 10 ms frame the VM completed ([drift]:
         * how much faster the VM's clock runs), unless the VM is [silent] then;
         * [delayUs] is each packet's way here (one stream: in order).
         */
        fun host(untilUs: Long, wakeUs: Double = 15_625.0, drift: Double = 0.0, delayUs: (Long) -> Long = { 0 }, silent: (Long) -> Boolean = { false }) {
            var sent = 0L
            var k = 1L
            var last = 0L
            while (true) {
                val wake = (k * wakeUs).roundToLong()
                if (wake > untilUs) break
                val complete = floor(wake * (1 + drift) / 10_000).toLong()
                while (sent < complete) {
                    if (!silent(sent)) { last = max(last, wake + delayUs(sent)); arrivals.add(longArrayOf(last, sent * 10_000, sent)) }
                    sent++
                }
                k++
            }
        }

        fun run(untilUs: Long, onTick: (Rig) -> Unit = {}) {
            var nextMix = now + track.periodUs
            var nextTick = now + 2_300
            while (true) {
                val arrival = if (next < arrivals.size) arrivals[next][0] else Long.MAX_VALUE
                val t = min(arrival, min(nextMix, nextTick))
                if (t > untilUs) break
                now = t
                if (nextMix == t) { track.mix(now); nextMix += track.periodUs }
                while (next < arrivals.size && arrivals[next][0] <= now) {
                    val n = arrivals[next][2]
                    for (j in 0 until 480) {
                        val frame = content(n * 480 + j)
                        PcmFade.put(bytes, j * 4, frame.toShort().toInt()); PcmFade.put(bytes, j * 4 + 2, frame shr 16)
                    }
                    if (firstArrival < 0) firstArrival = now
                    assertTrue(synchronized(playout) { playout.push(arrivals[next][1], bytes, 0, bytes.size, now) })
                    next++
                }
                if (nextTick == t) {
                    assertEquals(0, feed.tick(track, now * 1000))
                    maxPending = max(maxPending, feed.pending)
                    if (feed.playing) { levels.add(doubleArrayOf(now.toDouble(), playout.level / 48.0)); if (firstSound < 0) firstSound = now }
                    onTick(this)
                    nextTick += 5_000
                }
            }
        }

        fun meanLevel(fromUs: Long, untilUs: Long): Double =
            levels.filter { it[0] >= fromUs && it[0] < untilUs }.map { it[1] }.average()
        fun eventsAfter(fromUs: Long) = track.events.count { it >= fromUs }
    }

    companion object {
        /** A 375 Hz tone (128 frames a period), the same on both sides, packed as the simulated track keeps it. */
        private val TONE = IntArray(128) { (8000 * sin(2 * PI * it / 128)).roundToInt() }
        fun tone(frame: Long): Int { val v = TONE[(frame % 128).toInt()]; return (v and 0xffff) or (v shl 16) }
        fun left(packed: Int) = packed.toShort().toInt()
        const val SECOND = 1_000_000L
    }

    /** A track on the fast mixer path from API 31: its buffer is its capacity, it starts at 30 ms. */
    private fun fastTrack(record: Boolean = false, acceptLimit: Int = Int.MAX_VALUE) =
        FakeTrack(buffer = 9600, quantum = 48, periodUs = 1000, fast = true, startLevel = 1440, record = record, acceptLimit = acceptLimit)
    /** A track on the normal mixer path before API 31: 20 ms periods, it starts only with a full buffer. */
    private fun normalTrack(record: Boolean = false, quantum: Int = 960, periodUs: Long = 20_000, granule: Int = 1) =
        FakeTrack(buffer = 9600, quantum = quantum, periodUs = periodUs, fast = false, startLevel = 9600, startIsBuffer = true, record = record, granule = granule)

    @Test fun soundOnsetHasNoGapsAtTheHostsCadence() {
        for ((name, rig) in listOf("fast path" to Rig(fastTrack(), follows = false), "normal path, Android 8-11" to Rig(normalTrack(), follows = true))) {
            val random = Random(3)
            val delays = LongArray(6001) { (random.nextDouble() * 4000).toLong() }
            rig.host(60 * SECOND, delayUs = { delays[it.toInt()] })
            rig.run(60 * SECOND)
            val c = rig.playout.counters
            assertTrue("$name: sound did not start", rig.firstSound > 0)
            assertTrue("$name: sound started ${(rig.firstSound - rig.firstArrival) / 1000} ms after the first packet",
                rig.firstSound - rig.firstArrival <= 40_000 + 15_625 + 5_000)
            assertEquals("$name: gaps after the sound started", 0, rig.eventsAfter(rig.firstSound + 1))
            assertEquals("$name: underruns", 0L, c.underruns)
            assertEquals("$name: fade-out tails", 0L, c.tails)
            assertEquals("$name: target", AudioJitterBuffer.ms(40), rig.playout.target)
        }
    }

    @Test fun shortSoundsEachStartPrimedWithoutGaps() {
        // The VM's UI sounds: 300 ms of sound, 1 s of silence, twenty times. Each onset is primed again.
        for ((name, rig) in listOf("fast path" to Rig(fastTrack(), follows = false), "normal path, Android 8-11" to Rig(normalTrack(), follows = true))) {
            rig.host(26 * SECOND, silent = { n -> n % 130 >= 30 })
            rig.run(26 * SECOND)
            val c = rig.playout.counters
            assertEquals("$name: each pause is a silence, not an underrun (underruns ${c.underruns}, track ${c.trackUnderruns}, tails ${c.tails}, late ${c.lateConcealments}, overruns ${c.overruns}, gaps ${rig.track.events.size}, starts ${rig.track.starts})", 19L, c.silences)
            assertEquals("$name: underruns", 0L, c.underruns)
            // The track runs dry once at the end of each sound and never while one plays.
            assertTrue("$name: ${rig.track.events.size} gaps for 20 sounds", rig.track.events.size <= 20)
            assertEquals("$name: target", AudioJitterBuffer.ms(40), rig.playout.target)
        }
    }

    @Test fun theLevelIsBackAtTheTargetAfterAPause() {
        for ((name, rig) in listOf("fast path" to Rig(fastTrack(), follows = false), "normal path, Android 8-11" to Rig(normalTrack(), follows = true))) {
            // 3 s of sound, 2 s of silence (the track is disabled meanwhile), 3 s of sound.
            rig.host(8 * SECOND, silent = { n -> n in 300L until 500L })
            rig.run(8 * SECOND)
            val target = rig.playout.target / 48.0
            val before = rig.meanLevel(2 * SECOND, 3 * SECOND)
            val after = rig.meanLevel(7 * SECOND, 8 * SECOND)
            assertTrue("$name: level before the pause $before ms, target $target", before in target - 15..target + 5)
            assertTrue("$name: level after the pause $after ms, target $target", after in target - 15..target + 5)
            assertEquals("$name: one silence", 1L, rig.playout.counters.silences)
            assertEquals("$name: no underrun", 0L, rig.playout.counters.underruns)
            assertTrue("$name: the track was disabled in the pause and started again", rig.track.starts.size == 2 && !rig.track.filling)
            // It runs dry once, at the end of the first sound; never while sound plays.
            assertEquals("$name: gaps while sound played", 0, rig.track.events.count { it !in 3_000_000L until 3_100_000L })
            // The track that waited for its start level after the pause got it from the first write.
            if (rig.track.starts.size >= 2) assertTrue("$name: restarted with ${rig.track.starts.last()} frames", rig.track.starts.last() >= rig.track.startLevel.coerceAtMost(rig.track.buffer))
        }
    }

    private fun drift(rate: Double, rig: Rig, name: String) {
        val random = Random(11)
        rig.host(1800 * SECOND, drift = rate, delayUs = { (random.nextDouble() * 4000).toLong() })
        rig.run(1800 * SECOND)
        val c = rig.playout.counters
        val expected = abs(rate) * 48000 * 1800
        val corrected = (if (rate > 0) c.trimmedFrames else c.insertedFrames).toDouble()
        assertEquals("$name ${rate * 1e6} ppm: gaps over 30 minutes", 0, rig.eventsAfter(rig.firstSound + 1))
        assertTrue("$name ${rate * 1e6} ppm: underruns ${c.underruns}, overruns ${c.overruns}, tails ${c.tails}",
            c.underruns == 0L && c.overruns == 0L && c.tails == 0L)
        // Only single frames, at most one in every 200 written: as much as the drift, no more.
        assertTrue("$name ${rate * 1e6} ppm: corrected $corrected frames, expected about $expected", corrected in expected * 0.6..expected * 1.6)
        val opposite = (if (rate > 0) c.insertedFrames else c.trimmedFrames).toDouble()
        assertTrue("$name ${rate * 1e6} ppm: corrected the other way $opposite frames", opposite <= expected * 0.6)
        val level = rig.meanLevel(1790 * SECOND, 1800 * SECOND)
        val target = rig.playout.target / 48.0
        assertTrue("$name ${rate * 1e6} ppm: level $level ms for target $target", level in target - 15..target + 5)
    }

    @Test fun phoneSlowerByTwoHundredPpmForHalfAnHour() {
        drift(200e-6, Rig(fastTrack(), follows = false), "fast path")
        drift(200e-6, Rig(normalTrack(), follows = true), "normal path")
    }

    @Test fun phoneFasterByTwoHundredPpmForHalfAnHour() {
        drift(-200e-6, Rig(fastTrack(), follows = false), "fast path")
        drift(-200e-6, Rig(normalTrack(), follows = true), "normal path")
    }

    @Test fun aHundredMillisecondHoleFadesOutOnceAndPrimesAgain() {
        // Steady 10 ms packets; at 3 s the network holds 100 ms of them (they then come at once).
        val rig = Rig(fastTrack(record = true), follows = false)
        rig.host(6 * SECOND, wakeUs = 10_000.0, delayUs = { n -> if (n in 300L until 310L) (310 - n) * 10_000 else 0 })
        val target = rig.playout.target
        var resumedAt = -1L
        rig.run(6 * SECOND) { r -> if (resumedAt < 0 && r.now > 3_000_000 && r.feed.playing && r.playout.counters.underruns == 1L) resumedAt = r.now }
        val c = rig.playout.counters
        assertEquals("one fade-out tail for the hole", 1L, c.tails)
        assertEquals("the hole is one underrun (the sound went on)", 1L, c.underruns)
        assertEquals("not a silence", 0L, c.silences)
        assertEquals("the target grew one step", target + AudioJitterBuffer.ms(20), rig.playout.target)
        assertTrue("the sound came back", resumedAt in 3_100_000L..3_200_000L)
        // The track ran dry once (the hole) and started again with at least the old target in it at once.
        assertEquals("one gap", 1, rig.eventsAfter(rig.firstSound + 1))

        // What was heard: the tone, then the tail (the tone backwards, fading to nothing), silence, a fade-in.
        val out = rig.track.out
        val size = rig.track.outSize
        var silentStart = -1
        for (i in 48_000 until size) if (out[i] == 0 && (1..47).all { k -> i + k < size && out[i + k] == 0 }) { silentStart = i; break }
        assertTrue("the hole is silent", silentStart > 0)
        // The 10 ms before the silence: the tail. Its envelope falls to nothing, its first frame goes on from the tone.
        val tail = silentStart - 480
        val peakEarly = (tail until tail + 120).maxOf { abs(left(out[it])) }
        val peakLate = (tail + 360 until silentStart).maxOf { abs(left(out[it])) }
        assertTrue("the tail fades: early peak $peakEarly, late peak $peakLate", peakEarly > 6000 && peakLate < 1500)
        assertTrue("the tail goes on from the last frame (no edge)", abs(left(out[tail]) - left(out[tail - 1])) <= 600)
        // Nothing was repeated: no 10 ms of output equals the 10 ms before it.
        for (start in 480 until size - 480 step 120) {
            if ((0 until 480).all { out[start + it] == 0 }) continue
            assertTrue("10 ms at frame $start repeats the 10 ms before it", (0 until 480).any { out[start + it] != out[start - 480 + it] })
        }
        // After the hole the sound fades in from nothing.
        var resume = silentStart
        while (resume < size && out[resume] == 0) resume++
        assertTrue("the sound resumes", resume < size)
        assertTrue("fade-in: ${left(out[resume])}", abs(left(out[resume])) <= 200)
        val later = (resume + 200 until resume + 400).maxOf { abs(left(out[it])) }
        assertTrue("full level after the fade-in: $later", later > 7000)
    }

    @Test fun aLateBurstWithinTheTailGrowsTheTargetWithoutAGap() {
        // A 30 ms late burst: the level falls under the starving band, the tail starts, and the
        // late audio comes before the track ran dry: late (the target grows), not an underrun.
        val rig = Rig(fastTrack(), follows = false)
        rig.host(6 * SECOND, wakeUs = 10_000.0, delayUs = { n -> if (n in 300L until 303L) (303 - n) * 10_000 + 2_000 else 0 })
        rig.run(6 * SECOND)
        val c = rig.playout.counters
        assertEquals("one tail", 1L, c.tails)
        assertEquals("late, once", 1L, c.lateConcealments)
        assertEquals("not an underrun", 0L, c.underruns)
        assertEquals("not a silence", 0L, c.silences)
        assertEquals("the target grew one step", AudioJitterBuffer.ms(60), rig.playout.target)
    }

    @Test fun aShortWriteKeepsItsTail() {
        // The track takes at most 400 frames a write (the 40 ms priming write is 1920): the rest is
        // written first at the next look.
        val rig = Rig(fastTrack(record = true, acceptLimit = 400), follows = false, content = { it.toInt() and 0x7fff or ((it.toInt() shr 15 and 0x7fff) shl 16) })
        rig.host(5 * SECOND, wakeUs = 10_000.0)
        rig.run(5 * SECOND)
        val c = rig.playout.counters
        assertTrue("writes were short", rig.maxPending > 0)
        assertEquals("nothing cut", 0L, c.overrunFrames)
        val out = rig.track.out
        var first = 0
        while (out[first] == 0) first++
        // After the stream's fade-in every frame played is the next one sent, or the drift correction's
        // single repeated (+0) or dropped (+2) frame, each counted: none lost, none reordered.
        val from = first + 200
        var repeated = 0L
        var skipped = 0L
        for (i in from + 1 until rig.track.outSize) {
            val frame = (out[i] and 0x7fff) or ((out[i] shr 16 and 0x7fff) shl 15)
            val before = (out[i - 1] and 0x7fff) or ((out[i - 1] shr 16 and 0x7fff) shl 15)
            when (frame - before) { 1 -> {}; 0 -> repeated++; 2 -> skipped++; else -> throw AssertionError("frame $i: $before then $frame") }
        }
        assertTrue("repeats $repeated, inserted ${c.insertedFrames}; skips $skipped, trimmed ${c.trimmedFrames}",
            repeated <= c.insertedFrames && skipped <= c.trimmedFrames)
        assertEquals("no gap", 0, rig.eventsAfter(rig.firstSound + 1))
    }

    @Test fun aTrackReadInLongPeriodsGrowsTheTargetByItsOwnUnderrunCount() {
        // Some devices' normal mixer reads 30 to 50 ms at once. 30 ms fits under the 40 ms target;
        // 40 and 50 ms do not: the track's own count (it holds audio and is not mixed) grows the target.
        for (quantumMs in intArrayOf(30, 40, 50)) {
            val rig = Rig(normalTrack(quantum = quantumMs * 48, periodUs = quantumMs * 1000L), follows = true)
            rig.host(60 * SECOND)
            rig.run(60 * SECOND)
            val c = rig.playout.counters
            if (quantumMs > 30) {
                assertTrue("$quantumMs ms periods: underruns ${c.underruns}", c.underruns in 1L..4L)
                assertTrue("$quantumMs ms periods: target ${rig.playout.target / 48} ms", rig.playout.target >= AudioJitterBuffer.ms(quantumMs + 10))
            } else assertEquals("$quantumMs ms periods: target", AudioJitterBuffer.ms(40), rig.playout.target)
            assertTrue("$quantumMs ms periods: sound started", rig.firstSound > 0 && rig.track.played > 48L * 50_000)
            assertEquals("$quantumMs ms periods: gaps in the last 40 s (at ${rig.track.events.map { it / 1000 }} ms; underruns ${c.underruns}, track ${c.trackUnderruns}, target ${rig.playout.target / 48})", 0, rig.eventsAfter(20 * SECOND))
        }
    }

    @Test fun aRoundedBufferIsTheTarget() {
        // A platform that rounds the buffer up to 30 ms steps: the target is what it took.
        val rig = Rig(normalTrack(granule = 1440), follows = true)
        assertEquals(2880, rig.track.buffer)
        assertEquals(2880, rig.playout.target)
        rig.host(10 * SECOND)
        rig.run(10 * SECOND)
        assertEquals("plays without gaps", 0, rig.eventsAfter(rig.firstSound + 1))
        assertEquals(rig.track.buffer, rig.playout.target)
    }
}
