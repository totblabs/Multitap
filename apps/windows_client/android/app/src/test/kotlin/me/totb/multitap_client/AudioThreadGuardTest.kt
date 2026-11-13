package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.concurrent.atomic.AtomicInteger

// The "Multitap audio" thread (AndroidAudioOut): a Java Error in a look no longer ends it without a trace
// (overruns rising, the level frozen, no sound for the rest of the stream). AudioLoopGuard counts it and the
// thread goes on; AudioThreadKeeper starts a thread that ended anyway again; SoundNotice reports sound that
// goes on once a second (audio focus is asked again from it).
class AudioThreadGuardTest {
    @Test
    fun anErrorInALookIsCountedAndTheLoopGoesOn() {
        val guard = AudioLoopGuard()
        var looks = 0
        val results = (0 until 6).map { n ->
            guard.look(n * 5_000_000L) {
                looks++
                when (n) {
                    1 -> throw StackOverflowError()
                    2 -> throw IllegalStateException("track")
                    4 -> throw OutOfMemoryError()
                }
            }
        }
        assertEquals(listOf(true, false, false, true, false, true), results)
        assertEquals(6, looks)
        assertEquals(3L, guard.errors)
        assertEquals("the last was an Error", 2, guard.lastKind)
        guard.reset()
        assertEquals(0L, guard.errors)
    }

    @Test
    fun aRealThreadSurvivesAnErrorAndTheTrackIsMadeAgainWithinTheRate() {
        // The loop shape of AndroidAudioOut.loop with a stand-in track: a look that throws an Error gives the
        // track up; the next looks make it again at most as often as RemakeLimiter allows.
        val guard = AudioLoopGuard()
        val limiter = RemakeLimiter(perWindow = 3, windowNanos = 60_000_000_000L, gapNanos = 1_000_000_000L)
        var track: Any? = Any()
        var made = 0
        val ticks = AtomicInteger()
        val thread = Thread {
            var now = 0L
            while (ticks.get() < 2000) {
                val ok = guard.look(now) {
                    if (track == null) { if (limiter.allow(it)) { made++; track = Any() }; return@look }
                    if (ticks.get() % 400 == 7) throw AssertionError("an Error inside the track's write")
                }
                if (!ok) track = null
                ticks.incrementAndGet()
                now += 5_000_000L
            }
        }
        thread.start()
        thread.join(5000)
        assertFalse("the loop ran to its end", thread.isAlive)
        assertEquals(2000, ticks.get())
        // Over 10 s of looks (2000 x 5 ms) the track is lost four times and made again three times (the
        // minute's window); without a track the fifth would-be failure never happens.
        assertEquals(4L, guard.errors)
        assertEquals(3, made)
    }

    @Test
    fun aThreadThatEndedIsStartedAgainWithinTheRate() {
        val keeper = AudioThreadKeeper(RemakeLimiter(perWindow = 3, windowNanos = 60_000_000_000L, gapNanos = 1_000_000_000L))
        assertFalse("alive: nothing to do", keeper.restart(running = true, alive = true, now = 0))
        assertFalse("the stream ended: not again", keeper.restart(running = false, alive = false, now = 0))
        assertTrue(keeper.restart(running = true, alive = false, now = 0))
        assertFalse("not twice within a second", keeper.restart(true, false, 500_000_000L))
        assertTrue(keeper.restart(true, false, 1_000_000_000L))
        assertTrue(keeper.restart(true, false, 2_000_000_000L))
        assertFalse("three a minute", keeper.restart(true, false, 3_000_000_000L))
        assertTrue("a minute later again", keeper.restart(true, false, 61_000_000_000L))
        assertEquals(4L, keeper.restarts)
        keeper.reset()
        assertEquals(0L, keeper.restarts)
    }

    @Test
    fun aDeadThreadIsFoundAndReplaced() {
        // A thread that died of something outside the guarded look: the worker's next statistics reading
        // finds it not alive and starts another.
        val keeper = AudioThreadKeeper()
        var thread = Thread { throw StackOverflowError() }.apply { setUncaughtExceptionHandler { _, _ -> }; start() }
        thread.join(1000)
        val ran = AtomicInteger()
        if (keeper.restart(running = true, alive = thread.isAlive, now = 0)) thread = Thread { ran.incrementAndGet() }.apply { start() }
        thread.join(1000)
        assertEquals(1, ran.get())
        assertEquals(1L, keeper.restarts)
    }

    @Test
    fun soundThatGoesOnIsReportedOnceASecond() {
        val notice = SoundNotice(periodNanos = 1_000_000_000L)
        var reported = 0
        var now = 0L
        // Starts, plays 3 s, silence 1 s, starts again.
        for (n in 0 until 600) {
            val playing = n < 300 || n >= 400
            val started = n == 0 || n == 400
            if (notice.due(now, started, playing)) reported++
            now += 10_000_000L
        }
        // 0 s, 1 s, 2 s; 4 s, 5 s.
        assertEquals(5, reported)
    }
}
