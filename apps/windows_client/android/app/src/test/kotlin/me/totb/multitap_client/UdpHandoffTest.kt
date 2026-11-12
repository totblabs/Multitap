package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

// The leg's hand-off to the decoder (UdpHandoff) and the decoder queue's shares
// (AndroidMediaAdmission): a busy decoder must not turn into a key frame storm.
class UdpHandoffTest {
    private val accepted = UdpPathGate.Delivery.ACCEPTED
    private val full = UdpPathGate.Delivery.FULL
    private val dropped = UdpPathGate.Delivery.DROPPED

    private fun video(pts: Long, key: Boolean = false) = UdpReceiver.Message(true, key, 1, pts, ByteArray(20))
    private fun audio(pts: Long) = UdpReceiver.Message(false, false, 1, pts, ByteArray(20))

    /** A decoder that answers [answer] and records every offer. */
    private class Decoder(var answer: (UdpReceiver.Message) -> UdpPathGate.Delivery) {
        val offered = ArrayList<Long>()
        val taken = ArrayList<Long>()
        val handoff = UdpHandoff { offered.add(it.pts); answer(it) }
        fun pass(nowUs: Long, vararg messages: UdpReceiver.Message) = handoff.pass(messages.toMutableList(), nowUs) { taken.add(it.pts) }
    }

    @Test fun aBusyDecoderIsWaitedFor() {
        val d = Decoder { if (it.video) full else accepted }
        assertEquals(UdpHandoff.Hole.NONE, d.pass(0, video(1, true), audio(2), video(3)))
        assertTrue("Video held", d.handoff.holding)
        assertEquals("Audio does not wait behind held video", listOf(2L), d.taken)
        assertEquals("Later video waits behind the held one", listOf(1L, 2L), d.offered)
        assertEquals(UdpHandoff.Hole.NONE, d.pass(20000, video(4)))
        d.answer = { accepted }
        assertEquals(UdpHandoff.Hole.NONE, d.pass(40000))
        assertEquals("Delivered in order once the decoder took them", listOf(2L, 1L, 3L, 4L), d.taken)
        assertFalse(d.handoff.holding)
    }

    @Test fun aQueueStillFullLeavesAHole() {
        run {
            val d = Decoder { if (it.video) full else accepted }
            d.pass(0, video(1), video(2))
            assertEquals(UdpHandoff.Hole.NONE, d.pass(UdpHandoff.RETRY_US - 1))
            d.answer = { if (it.pts == 1L) full else accepted }
            assertEquals("A P-frame given up after the retry window", UdpHandoff.Hole.FULL, d.pass(UdpHandoff.RETRY_US))
            assertTrue("A P-frame behind the hole went to the decoder", d.taken.isEmpty() && d.offered.count { it == 2L } == 0)
            assertFalse(d.handoff.holding)
        }
        run {
            val d = Decoder { if (it.video) full else accepted }
            d.pass(0, video(1), video(2), video(3, key = true), video(4))
            d.answer = { if (it.pts == 1L) full else accepted }
            assertEquals("A key frame taken after the hole ends it: no request", UdpHandoff.Hole.NONE, d.pass(UdpHandoff.RETRY_US))
            assertEquals("Its P-frames go on; those before it do not", listOf(3L, 4L), d.taken)
        }
    }

    @Test fun refusalsThatBackOff() {
        run {  // A key frame still refused after the window.
            val d = Decoder { full }
            d.pass(0, video(1, key = true))
            assertEquals(UdpHandoff.Hole.REFUSED, d.pass(UdpHandoff.RETRY_US))
        }
        run {  // A failed decoder (or the rule): no retry, and the request backs off.
            val d = Decoder { dropped }
            assertEquals(UdpHandoff.Hole.REFUSED, d.pass(0, video(1), video(2)))
            assertEquals(listOf(1L), d.offered)
            assertFalse(d.handoff.holding)
        }
        run {  // Audio refused is just a lost chunk.
            val d = Decoder { full }
            assertEquals(UdpHandoff.Hole.NONE, d.pass(0, audio(1)))
            assertFalse(d.handoff.holding)
        }
    }

    @Test fun theWaitIsBounded() {
        val d = Decoder { if (it.video) full else accepted }
        d.pass(0, video(1))
        val many = Array(UdpHandoff.MAX_WAITING) { video(2L + it) }
        assertEquals("Too much behind a held message gives it up at once", UdpHandoff.Hole.FULL, d.pass(1000, *many))
    }

    @Test fun decoderShares() {
        val q = AndroidMediaAdmission(videoPackets = 8, videoBytes = 1000, audioPackets = 4)
        repeat(4) { assertFalse(q.full(false, 10)); q.added(false, 10) }
        assertTrue("Audio has a share of its own", q.full(false, 10))
        assertFalse("Audio takes no video slot", q.full(true, 10))
        repeat(8) { assertFalse(q.full(true, 10)); q.added(true, 10) }
        assertTrue(q.full(true, 10))
        q.removed(true, 10)
        assertFalse(q.full(true, 10))
        assertTrue("Video bytes are bounded", q.full(true, 1000 - 70 + 1))
        assertFalse("Audio bytes take no video bytes", q.full(true, 1000 - 70))
        assertEquals(4 + 7, q.packets)
        assertEquals(40L + 70L, q.bytes)
        repeat(4) { q.removed(false, 10) }
        assertEquals(7, q.packets)
    }
}
