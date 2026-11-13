package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Random
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.roundToLong

/**
 * AudioJitterBuffer (AndroidAudioJitter.kt) against the Windows client's own
 * cases, one for one (apps/windows_client/test/native/audio_jitter_test.cpp),
 * with the Windows settings (AudioJitterConfig()) and the same simulation: no
 * track, no network, no sound. A virtual device plays 48 frames per
 * millisecond; the Host sends a 10 ms packet per 10 ms of the VM's audio
 * clock, delayed as each case says. The render thread is modelled as the
 * Windows client runs it: woken by every packet and at least every 15 ms.
 * The Android additions are inert here (no track underrun count, no tail).
 */
class AudioJitterBufferTest {
    private val ms = AudioJitterBuffer.Companion

    private class Sim {
        val buffer = AudioJitterBuffer()
        val capacity = 4800                 // the device buffer, 100 ms
        var device = 0.0                    // frames the device still holds
        var now = 0L                        // µs, this device
        var nextWake = 0L
        var renderStalled = false           // the render thread does not run
        var dryMs = 0L                      // device empty after the first write, while the VM was not silent
        var everWritten = false
        var silentAt: (Long) -> Boolean = { false }
        val scratch = ByteArray(4800 * 4)
        val levels = ArrayList<Double>()    // ms after each write while playing
        val arrivals = ArrayList<LongArray>() // (at, pts)
        var next = 0

        // Packet n carries VM audio [n*10, n*10+10) ms; it is sent when complete,
        // `drift` faster on the VM's clock, plus `delay(n)` ms on the way here.
        fun schedule(packets: Long, drift: Double, delay: (Long) -> Double, skip: (Long) -> Boolean) {
            var last = 0L
            for (n in 0 until packets) {
                if (skip(n)) continue
                var at = ((n + 1) * 10000.0 / (1.0 + drift) + delay(n) * 1000.0).roundToLong()
                at = max(at, last); last = at   // one stream: in order
                arrivals.add(longArrayOf(at, n * 10000))
            }
        }

        fun wake() {
            if (renderStalled) return
            val padding = device.toInt()
            val written = buffer.pull(scratch, capacity - padding, padding, now)
            device += written; everWritten = everWritten || written > 0
            if (written > 0 && buffer.playing) levels.add(buffer.level / 48.0)
        }

        fun run(untilMs: Long, tick: ((Sim) -> Unit)? = null) {
            val packet = ByteArray(1920) { 1 }
            while (now < untilMs * 1000) {
                now += 1000
                device = max(0.0, device - 48.0)
                if (everWritten && device == 0.0 && !silentAt(now)) dryMs++
                var pushed = false
                while (next < arrivals.size && arrivals[next][0] <= now) {
                    assertTrue("A valid packet was refused", buffer.push(arrivals[next][1], packet, 0, packet.size, now))
                    next++; pushed = true
                }
                tick?.invoke(this)
                if (pushed || now >= nextWake) { wake(); nextWake = now + 15000 }
            }
        }

        fun meanLevel(last: Int): Double {
            if (levels.isEmpty()) return 0.0
            val from = max(0, levels.size - last)
            return levels.subList(from, levels.size).average()
        }

        val targetMs get() = buffer.target / 48.0
    }

    private val none: (Long) -> Double = { 0.0 }
    private val all: (Long) -> Boolean = { false }

    @Test fun steady() {
        val sim = Sim(); sim.schedule(3000, 0.0, none, all); sim.run(30000)
        val c = sim.buffer.counters
        assertTrue("Steady audio had an underrun, overrun or gap", c.underruns == 0L && c.overruns == 0L && c.silences == 0L && sim.dryMs == 0L)
        // Two calm periods (10 s each) take the 40 ms start to the 30 ms floor; it stays there.
        assertEquals("A calm stream did not shrink to the floor: ${sim.targetMs}", ms.ms(30), sim.buffer.target)
        val level = sim.meanLevel(500)
        assertTrue("Steady depth is not at the target: $level", level in 20.0..34.0)
        // Only the two shrinks' 10 ms leave by trimming; nothing is ever repeated.
        assertTrue("A steady stream was corrected: ${c.insertedFrames} inserted, ${c.trimmedFrames} trimmed",
            c.insertedFrames == 0L && c.trimmedFrames <= ms.ms(12))
    }

    @Test fun stalls() {
        // A 120 ms stall every 3 s (the packets then arrive together, PTS continuous).
        val sim = Sim(); val underrunAt = ArrayList<Long>()
        sim.schedule(3000, 0.0, { n -> val at = (n + 1) * 10 % 3000; if (at in 1501L..1620L) (1620 - at).toDouble() else 0.0 }, all)
        var seen = 0L
        sim.run(30000) { s -> if (s.buffer.counters.underruns != seen) { seen = s.buffer.counters.underruns; underrunAt.add(s.now) } }
        val c = sim.buffer.counters
        assertTrue("Stalls did not cause a bounded number of underruns: ${c.underruns}", c.underruns in 1L..7L)
        assertTrue("The target never grew past the stall: last underrun at ${underrunAt.lastOrNull()}", underrunAt.isNotEmpty() && underrunAt.last() < 21_000_000)
        assertTrue("The target did not settle above the stall: ${sim.targetMs}", sim.targetMs in 120.0..200.0)
        assertEquals("A late network was read as silence", 0L, c.silences)
    }

    @Test fun silence() {
        // 2 s sound, 1 s nothing (the VM is silent), 2 s, 1 s nothing, 2 s.
        val quiet: (Long) -> Boolean = { n -> n in 200L until 300L || n in 500L until 600L }
        val sim = Sim(); sim.schedule(800, 0.0, none, quiet)
        sim.silentAt = { us -> val at = us / 1000; at in 2000L until 3100L || at in 5000L until 6100L }
        sim.run(8000)
        val c = sim.buffer.counters
        assertTrue("A silent VM was counted as underruns: ${c.underruns}/${c.silences}", c.silences == 2L && c.underruns == 0L)
        assertEquals("Silence changed the target", ms.ms(40), sim.buffer.target)
    }

    @Test fun renderStall() {
        // This device's render thread stops for 400 ms; packets keep coming.
        val sim = Sim(); sim.schedule(600, 0.0, none, all); var after = 0.0; var checked = false
        sim.run(6000) { s ->
            s.renderStalled = s.now >= 3_000_000 && s.now < 3_400_000
            if (s.now == 3_500_000L) { after = s.buffer.level / 48.0; checked = true }
        }
        val c = sim.buffer.counters
        assertTrue("A 400 ms backlog was kept instead of dropped", c.overruns >= 1 && c.overrunFrames > 0)
        assertTrue("Latency stayed high after the backlog: $after ms", checked && after <= sim.targetMs + 40 + 1)
        assertEquals("The dry device after the stall was not one underrun", 1L, c.underruns)
    }

    private fun drift(rate: Double) {
        val sim = Sim(); sim.schedule(6000, rate, none, all); sim.run(60000)
        val c = sim.buffer.counters
        val expected = abs(rate) * 48000 * 60
        val corrected = (if (rate > 0) c.trimmedFrames else c.insertedFrames).toDouble()
        assertTrue("Clock drift $rate was not absorbed: underruns ${c.underruns}, overruns ${c.overruns}", c.underruns == 0L && c.overruns == 0L)
        assertTrue("Drift correction $corrected frames, expected about $expected", corrected in expected * 0.6..expected * 1.4)
        val level = sim.meanLevel(300)
        assertTrue("Drift moved the depth: $level ms for target ${sim.targetMs}", level in sim.targetMs - 12..sim.targetMs + 6)
    }

    @Test fun fastDrift() = drift(0.003)

    @Test fun slowDrift() = drift(-0.003)

    @Test fun jitter() {
        // std::mt19937(7) in the Windows case; any fixed seed of a uniform 0..25 ms spread is the same case.
        val random = Random(7)
        val delays = DoubleArray(3000) { random.nextDouble() * 25 }
        val sim = Sim(); sim.schedule(3000, 0.0, { n -> delays[n.toInt()] }, all); sim.run(30000)
        val c = sim.buffer.counters
        assertTrue("25 ms of jitter grew the buffer: ${c.underruns} underruns, target ${sim.targetMs}", c.underruns <= 1 && sim.targetMs <= 60)
        assertTrue("25 ms of jitter left audible gaps: ${sim.dryMs} ms", sim.dryMs <= 40)
    }

    @Test fun playingPts() {
        val sim = Sim(); sim.schedule(1000, 0.0, none, all); var samples = 0; var previous = 0L
        sim.run(9000) { s ->
            if (s.now < 2_000_000 || s.now % 97_000 != 0L) return@run
            val pts = s.buffer.playingPtsUs(s.now)
            assertNotNull("No play head while playing", pts)
            // The packet that ends at local time t holds VM audio up to t: the play
            // head is that far back by the device's and the buffer's depth.
            val expected = s.now - (s.device * 1_000_000 / 48000).toLong() - s.buffer.queuedFrames.toLong() * 1_000_000 / 48000
            assertTrue("Play head $pts differs from $expected", abs(pts!! - expected) <= 16000)
            assertTrue("The play head went back", pts >= previous); previous = pts; samples++
        }
        assertTrue("Too few play-head samples", samples > 50)
        assertNull("A play head before any audio", AudioJitterBuffer().playingPtsUs(0))
    }

    @Test fun malformed() {
        val buffer = AudioJitterBuffer(); val bytes = ByteArray(AudioJitterBuffer.MAX_PACKET_BYTES + 4)
        for (bad in intArrayOf(0, 3, 6, AudioJitterBuffer.MAX_PACKET_BYTES + 4)) assertFalse("Malformed PCM accepted", buffer.push(0, bytes, 0, bad, 0))
        assertTrue("The largest valid packet", buffer.queuedFrames == 0 && buffer.push(0, bytes, 0, AudioJitterBuffer.MAX_PACKET_BYTES, 0))
        // One 200 ms packet is beyond target + headroom: kept down to the target, newest last.
        assertTrue("An oversized backlog was not cut to the target", buffer.counters.overruns == 1L && buffer.queuedFrames == ms.ms(40))
        assertFalse("A packet past the end of its array", buffer.push(0, bytes, bytes.size - 1916, 1920, 0))
    }
}
