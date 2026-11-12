package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

// AndroidFramePacer while the app is not visible (a session still decodes
// 17-60 fps then, with nothing drawn). Decoding goes on (no client
// key frame request on WebSocket); no frame is rendered, and the newest one
// shows on return.
class AndroidFramePacerBackgroundTest {
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
    fun inTheBackgroundNothingIsRenderedAndTheNewestShowsOnReturn() {
        pacer.decoded(0, 0, sink)
        pacer.consumed(5 * ms, sink)
        pacer.background()
        for (frame in 1..3) pacer.decoded(frame, frame * 16 * ms, sink)
        assertEquals(listOf(0), sink.rendered)
        assertEquals(listOf(1, 2), sink.discarded)
        pacer.foreground(100 * ms, sink)
        assertEquals(listOf(0, 3), sink.rendered)
        assertFalse(pacer.hidden)
        // Visible: rendered as before.
        pacer.consumed(110 * ms, sink)
        pacer.decoded(4, 120 * ms, sink)
        assertEquals(listOf(0, 3, 4), sink.rendered)
    }

    @Test
    fun aFrameNotTakenYetGoesFirstThenTheNewest() {
        pacer.decoded(0, 0, sink)
        pacer.background()
        pacer.decoded(1, 16 * ms, sink)
        pacer.decoded(2, 32 * ms, sink)
        // Flutter latches the waiting frame as the app goes: the held one stays held.
        pacer.consumed(40 * ms, sink)
        assertEquals(listOf(0), sink.rendered)
        pacer.foreground(500 * ms, sink)
        assertEquals(listOf(0, 2), sink.rendered)

        val again = Sink()
        val other = AndroidFramePacer()
        other.decoded(0, 0, again)
        other.background()
        other.decoded(1, 16 * ms, again)
        other.foreground(500 * ms, again)
        assertEquals("the untaken frame still waits", listOf(0), again.rendered)
        other.consumed(510 * ms, again)
        assertEquals(listOf(0, 1), again.rendered)
    }

    @Test
    fun aDecoderMadeInTheBackgroundNeverRendersEverything() {
        pacer.background()
        for (frame in 0..5) pacer.decoded(frame, frame * 400 * ms, sink)
        assertEquals(-1L, pacer.checkAt())
        assertFalse(pacer.check(10_000 * ms, sink))
        assertTrue(sink.rendered.isEmpty())
        assertTrue(pacer.gated)
        pacer.foreground(10_000 * ms, sink)
        assertEquals(listOf(5), sink.rendered)
        // Visible now: the usual fallback if Flutter never takes a frame.
        pacer.decoded(6, 10_016 * ms, sink)
        assertEquals(10_000 * ms + AndroidFramePacer.UNCONSUMED_LIMIT_NANOS, pacer.checkAt())
    }

    @Test
    fun hiddenBeforeFlutterTookTheFirstFrameItDoesNotFallBackToRenderingEverything() {
        pacer.decoded(0, 0, sink)
        pacer.background()
        pacer.decoded(1, 116 * ms, sink)
        assertEquals(-1L, pacer.checkAt())
        assertFalse(pacer.check(2_000 * ms, sink))
        assertEquals(listOf(0), sink.rendered)
        pacer.foreground(3_000 * ms, sink)
        assertEquals("the untaken first frame still waits", listOf(0), sink.rendered)
        // Flutter gets its full time again from the return, not from the first frame.
        assertEquals(3_000 * ms + AndroidFramePacer.UNCONSUMED_LIMIT_NANOS, pacer.checkAt())
        pacer.consumed(3_010 * ms, sink)
        assertEquals(listOf(0, 1), sink.rendered)
        assertTrue(pacer.gated)
    }

    @Test
    fun aDecoderThatAlreadyRendersEverythingStillStopsInTheBackground() {
        pacer.decoded(0, 0, sink)
        pacer.decoded(1, 16 * ms, sink)
        assertTrue(pacer.check(2_000 * ms, sink))
        assertFalse(pacer.gated)
        val before = sink.rendered.size
        pacer.background()
        pacer.decoded(2, 2_016 * ms, sink)
        pacer.decoded(3, 2_032 * ms, sink)
        assertEquals(before, sink.rendered.size)
        pacer.foreground(3_000 * ms, sink)
        assertEquals(3, sink.rendered.last())
    }
}
