package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

// AndroidFramePacer with lossless-tile republishes (AndroidTileRenderer): a
// redraw of the shown picture with new tiles is one more texture frame for
// Flutter, never one that piles up behind or ahead of a decoded frame.
class AndroidFramePacerRepublishTest {
    private class Sink : AndroidFrameSink {
        val rendered = ArrayList<Int>()
        val discarded = ArrayList<Int>()
        override fun render(frame: Int) { rendered.add(frame) }
        override fun discard(frame: Int) { discarded.add(frame) }
    }

    private val sink = Sink()
    private val pacer = AndroidFramePacer()
    private val ms = 1_000_000L

    @Test
    fun aRepublishWaitsForFlutterAndADecodedFrameWaitsBehindIt() {
        pacer.decoded(0, 0, sink)
        assertFalse("frame 0 not taken yet", pacer.republishAllowed())
        pacer.consumed(5 * ms, sink)
        assertTrue(pacer.republishAllowed())
        pacer.republished(10 * ms)
        assertFalse("one texture frame at a time", pacer.republishAllowed())
        // A frame decoded meanwhile is held, then shown when the republish is taken.
        pacer.decoded(1, 12 * ms, sink)
        assertEquals(listOf(0), sink.rendered)
        pacer.consumed(16 * ms, sink)
        assertEquals(listOf(0, 1), sink.rendered)
        pacer.consumed(32 * ms, sink)
        // The republish is not a decoded frame: taken counts frames 0 and 1 only.
        assertEquals(2L, pacer.taken)
        assertEquals(2L, pacer.rendered)
        assertEquals(1L, pacer.republishes)
    }

    @Test
    fun aHeldFrameCarriesTheTilesItselfSoNoRepublishIsDrawn() {
        pacer.decoded(0, 0, sink)
        pacer.decoded(1, 8 * ms, sink) // held behind frame 0
        assertFalse(pacer.republishAllowed())
        pacer.consumed(16 * ms, sink) // frame 1 shown now
        assertFalse("frame 1 waits", pacer.republishAllowed())
        pacer.consumed(32 * ms, sink)
        assertTrue(pacer.republishAllowed())
    }

    @Test
    fun noRepublishInTheBackgroundAndAResetForgetsAWaitingOne() {
        pacer.decoded(0, 0, sink)
        pacer.consumed(5 * ms, sink)
        pacer.background()
        assertFalse(pacer.republishAllowed())
        pacer.foreground(50 * ms, sink)
        assertTrue(pacer.republishAllowed())
        pacer.republished(60 * ms)
        pacer.reset()
        assertTrue(pacer.republishAllowed())
        pacer.decoded(2, 70 * ms, sink)
        assertEquals("rendered at once after the reset", listOf(0, 2), sink.rendered)
    }

    @Test
    fun withoutFrameCallbacksRepublishesAreDrawnAtOnce() {
        pacer.decoded(0, 0, sink)
        pacer.decoded(1, 8 * ms, sink)
        assertTrue(pacer.check(2_000 * ms, sink)) // Flutter never reported a take: ungated
        assertTrue(pacer.republishAllowed())
        pacer.republished(2_001 * ms)
        assertTrue(pacer.republishAllowed())
        assertEquals(1L, pacer.republishes)
    }
}
