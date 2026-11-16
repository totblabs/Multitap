package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

// The size AndroidTileRenderer draws a start at (LosslessOutput): the video's
// own, or for a downscaled single start the source size, within the device's
// memory reserve and GPU limits.
class LosslessOutputTest {
    private val unlimited = { Long.MAX_VALUE }

    @Test
    fun anUnscaledStartDrawsAtTheVideoSizeWithoutAskingForMemory() {
        val output = LosslessOutput.plan(1920, 1080, 1920, 1080, true) { throw AssertionError("not asked") }
        assertEquals(LosslessOutput(1920, 1080, false), output)
        assertEquals(LosslessOutput(1920, 1080, false), LosslessOutput.plan(1920, 1080, null, null, false) { throw AssertionError("not asked") })
        assertNull("no tiles-v1 frame", LosslessOutput.plan(1921, 1080, null, null, true, unlimited))
    }

    @Test
    fun aDownscaledSingleStartDrawsAtTheSourceSize() {
        // Field rows: 1264x1000 -> 1088x862 and 3840x2160 -> 2730x1536.
        assertEquals(LosslessOutput(1264, 1000, true), LosslessOutput.plan(1088, 862, 1264, 1000, true, unlimited))
        assertEquals(LosslessOutput(3840, 2160, true), LosslessOutput.plan(2730, 1536, 3840, 2160, true, unlimited))
        // One dimension downscaled is a scaled source too.
        assertEquals(LosslessOutput(1280, 722, true), LosslessOutput.plan(1280, 720, 1280, 722, true, unlimited))
    }

    @Test
    fun anOutputOfTracksOrAnImpossibleSourceDrawsNoTiles() {
        assertNull("tracks-v1 outputs are never scaled", LosslessOutput.plan(1088, 862, 1264, 1000, false, unlimited))
        assertNull("narrower source", LosslessOutput.plan(1088, 862, 1086, 1000, true, unlimited))
        assertNull("odd source", LosslessOutput.plan(1088, 862, 1265, 1000, true, unlimited))
        assertNull("beyond u16", LosslessOutput.plan(1088, 862, 65536, 1000, true, unlimited))
    }

    @Test
    fun aScaledOutputFitsTheMemoryReserveWithTheDecoder() {
        // 2730x1536 decoder estimate (36 bytes per 16x16-aligned pixel) plus 16 bytes per source pixel.
        val decoder = streamSizeMemoryBytes(listOf(StreamSizeTrack(2730, 1536, 1)))!!
        assertEquals(171L * 96 * 256 * 36, decoder)
        val needed = decoder + 3840L * 2160 * LosslessOutput.SCALED_BYTES_PER_PIXEL
        assertEquals(LosslessOutput(3840, 2160, true), LosslessOutput.plan(2730, 1536, 3840, 2160, true) { needed })
        assertNull(LosslessOutput.plan(2730, 1536, 3840, 2160, true) { needed - 1 })
        assertNull("no reserve (low memory)", LosslessOutput.plan(2730, 1536, 3840, 2160, true) { 0L })
    }

    @Test
    fun theGpuMustTakeTheOutputAndTheVideo() {
        assertTrue(LosslessOutput.fitsGpu(3840, 2160, 2730, 1536, 4096, 4096, 4096))
        assertTrue(LosslessOutput.fitsGpu(1264, 1000, 1088, 862, 2048, 2048, 2048))
        assertFalse("texture limit", LosslessOutput.fitsGpu(3840, 2160, 2730, 1536, 2048, 4096, 4096))
        assertFalse("viewport width", LosslessOutput.fitsGpu(3840, 2160, 2730, 1536, 4096, 3800, 4096))
        assertFalse("viewport height", LosslessOutput.fitsGpu(2160, 3840, 1536, 2730, 4096, 4096, 3800))
        assertFalse("the video itself", LosslessOutput.fitsGpu(2048, 2048, 4096, 1024, 2048, 4096, 4096))
        assertFalse("no GL answer", LosslessOutput.fitsGpu(1920, 1080, 1920, 1080, 0, 0, 0))
    }
}
