package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

// LosslessFrameOrder: the renderer applies drops by the pts of the frame it
// latched, which must be a frame the pacer rendered, in order.
class LosslessFrameOrderTest {
    @Test
    fun aLatchedTimestampIsThePtsOfARenderedFrame() {
        val order = LosslessFrameOrder()
        order.decoded(3, 1000); order.decoded(4, 1016); order.decoded(5, 1033)
        order.discarded(4) // replaced by a newer frame, never rendered
        order.rendered(3); order.rendered(5)
        assertEquals(1000L, order.latched(1_000_000L))
        assertEquals(1033L, order.latched(1_033_000L))
        assertNull("nothing more was rendered", order.latched(1_050_000L))
    }

    @Test
    fun aSurfaceThatSkippedAFrameStillMatchesTheNewerOne() {
        val order = LosslessFrameOrder()
        for ((index, pts) in listOf(0 to 100L, 1 to 116L, 2 to 133L)) { order.decoded(index, pts); order.rendered(index) }
        assertEquals(133L, order.latched(133_000L))
        assertNull("an older one cannot come after a newer one", order.latched(116_000L))
    }

    @Test
    fun aTimestampThatIsNoRenderedPtsIsRefused() {
        val order = LosslessFrameOrder()
        order.decoded(0, 100); order.rendered(0)
        assertNull("not a pts in nanoseconds", order.latched(100_000_123L))
        order.decoded(1, 200); order.rendered(1)
        assertNull("some other clock", order.latched(5_000_000_000L))
    }

    @Test
    fun receivedVideoNotYetLatchedIsAhead() {
        val order = LosslessFrameOrder()
        assertFalse(order.videoAhead())
        order.videoReceived(200)
        assertTrue(order.videoAhead())
        order.decoded(0, 200); order.rendered(0)
        order.latched(200_000L)
        assertFalse(order.videoAhead())
        order.clear()
        assertFalse(order.videoAhead())
        assertNull(order.latched(200_000L))
    }
}
