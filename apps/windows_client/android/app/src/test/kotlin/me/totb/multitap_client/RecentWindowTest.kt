package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

// The numbers the 연결 통계 dialog shows for this device: RecentWindow (the
// last second with frames) and the arrival time AndroidDecodeClock carries
// from a packet's acceptance to its decoded frame.
class RecentWindowTest {
    private val ms = 1_000_000L

    @Test fun nothingBeforeAWindowClosed() {
        val window = RecentWindow()
        assertNull(window.meanMs(0))
        window.add(4 * ms, 10 * ms)
        window.add(8 * ms, 500 * ms)
        assertNull("the first second is still open", window.meanMs(900 * ms))
        assertNull(window.maxMs(900 * ms))
    }

    @Test fun meanAndMaximumOfTheLastSecond() {
        val window = RecentWindow()
        window.add(4 * ms, 0)
        window.add(8 * ms, 400 * ms)
        window.add(6 * ms, 800 * ms)
        assertEquals(6.0, window.meanMs(1_000 * ms)!!, 1e-9)
        assertEquals(8.0, window.maxMs(1_000 * ms)!!, 1e-9)
        // The next second replaces it: the maximum comes down again.
        window.add(2 * ms, 1_100 * ms)
        window.add(3 * ms, 1_500 * ms)
        assertEquals(6.0, window.meanMs(1_900 * ms)!!, 1e-9)
        assertEquals(2.5, window.meanMs(2_050 * ms)!!, 1e-9)
        assertEquals(3.0, window.maxMs(2_050 * ms)!!, 1e-9)
    }

    @Test fun aSecondWithoutFramesKeepsTheLastValues() {
        // A still picture: the Host sends nothing, the dialog goes on showing the last frames' times.
        val window = RecentWindow()
        window.add(5 * ms, 0)
        assertEquals(5.0, window.meanMs(1_000 * ms)!!, 1e-9)
        assertEquals(5.0, window.meanMs(2_500 * ms)!!, 1e-9)
        assertEquals(5.0, window.meanMs(60_000 * ms)!!, 1e-9)
        // Frames again: what arrives after the pause is one window.
        window.add(9 * ms, 60_100 * ms)
        assertEquals(5.0, window.meanMs(60_200 * ms)!!, 1e-9)
        assertEquals(9.0, window.meanMs(61_100 * ms)!!, 1e-9)
    }

    @Test fun aValueAddedAfterALongPauseClosesTheOldWindowFirst() {
        val window = RecentWindow()
        window.add(4 * ms, 0)
        window.add(20 * ms, 5_000 * ms)
        // 4 ms belongs to the closed window; 20 ms to the one now open.
        assertEquals(4.0, window.meanMs(5_100 * ms)!!, 1e-9)
        assertEquals(20.0, window.meanMs(6_000 * ms)!!, 1e-9)
    }

    @Test fun resetAndNegativeValues() {
        val window = RecentWindow()
        window.add(-3 * ms, 0)
        assertEquals("never negative", 0.0, window.meanMs(1_000 * ms)!!, 1e-9)
        window.reset()
        assertNull(window.meanMs(2_000 * ms))
        assertNull(window.maxMs(5_000 * ms))
    }

    @Test fun customWindowLength() {
        val window = RecentWindow(windowNanos = 100 * ms)
        window.add(1 * ms, 0)
        window.add(3 * ms, 50 * ms)
        assertEquals(2.0, window.meanMs(100 * ms)!!, 1e-9)
    }

    @Test fun theDecodeClockCarriesThePacketsArrival() {
        val clock = AndroidDecodeClock()
        assertEquals(-1L, clock.lastArrival)
        // Arrived at 1 ms, waited for a decoder input until 7 ms, decoded at 12 ms.
        clock.queued(1000, 7 * ms, arrivedAt = 1 * ms)
        clock.queued(2000, 20 * ms, arrivedAt = 18 * ms)
        assertEquals(5 * ms, clock.decoded(1000, 12 * ms))
        assertEquals(1 * ms, clock.lastArrival)
        assertEquals(6 * ms, clock.decoded(2000, 26 * ms))
        assertEquals(18 * ms, clock.lastArrival)
        // An output the clock does not know has no arrival either.
        assertEquals(-1L, clock.decoded(3000, 30 * ms))
        assertEquals(-1L, clock.lastArrival)
    }

    @Test fun withoutAnArrivalItIsTheQueueTime() {
        val clock = AndroidDecodeClock()
        clock.queued(1000, 7 * ms)
        assertEquals(3 * ms, clock.decoded(1000, 10 * ms))
        assertEquals(7 * ms, clock.lastArrival)
    }

    @Test fun anInputTheCodecNeverOutputIsSkippedWithItsArrival() {
        val clock = AndroidDecodeClock()
        clock.queued(1000, 2 * ms, arrivedAt = 1 * ms)
        clock.queued(2000, 12 * ms, arrivedAt = 11 * ms)
        assertEquals(4 * ms, clock.decoded(2000, 16 * ms))
        assertEquals(11 * ms, clock.lastArrival)
    }
}
