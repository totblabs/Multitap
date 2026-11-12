package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

// ThermalWatch: the stream statistics' thermalStatus and thermalStatusMax.
class ThermalWatchTest {
    @Test fun unknownUntilTheDeviceSaysSomething() {
        val watch = ThermalWatch()
        assertNull(watch.status)
        assertNull(watch.highest)
    }

    @Test fun theLatestAndTheHighestOfAStream() {
        val watch = ThermalWatch()
        watch.started()
        watch.changed(0)
        watch.changed(2)
        watch.changed(3)
        watch.changed(1)
        assertEquals(1, watch.status)
        assertEquals(3, watch.highest)
    }

    @Test fun aNewStreamCountsItsHighestAgain() {
        val watch = ThermalWatch()
        watch.changed(4)
        watch.started()
        assertNull(watch.status)
        assertNull(watch.highest)
        watch.changed(1)
        assertEquals(1, watch.highest)
    }

    @Test fun aValueOutsideThePlatformRangeIsIgnored() {
        val watch = ThermalWatch()
        watch.changed(2)
        watch.changed(7)
        watch.changed(-1)
        assertEquals(2, watch.status)
        assertEquals(2, watch.highest)
    }
}
