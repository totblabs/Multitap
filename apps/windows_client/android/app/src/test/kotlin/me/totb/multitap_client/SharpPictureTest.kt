package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

// 선명하게: what a device says about itself (panel pixels, the automatic size, what its H.264
// decoder takes at the frame rate, memory, the connection's ceiling) -> the picture size and
// the Windows scale, and when the automatic size is used instead. Decision logic only: no
// MediaCodec, panel or Host was involved.
class SharpPictureTest {
    private val plenty = 2L * 1024 * 1024 * 1024

    /** A decoder as MediaCodecInfo describes one: largest sides and pixels per second. */
    private fun decoder(
        maxWidth: Int,
        maxHeight: Int,
        maxRate: Long = Long.MAX_VALUE,
        accelerated: Boolean = true,
        name: String = "c2.vendor.avc.decoder",
    ) = StreamSizeCandidate(name, maxWidth, maxHeight, maxWidth.toLong() * maxHeight, maxRate, 1, accelerated) { track ->
        when {
            track.width > maxWidth || track.height > maxHeight -> "unsupported_size"
            track.width.toLong() * track.height * track.fps > maxRate -> "unsupported_rate"
            else -> null
        }
    }

    private val uhd = listOf(decoder(4096, 2304))

    @Test fun aFoldablesInnerScreenGetsItsOwnPixelsAndTwiceTheScale() {
        // The owner's statistics: 1142x862 shown scaled up on the inner panel.
        val plan = sharpPicturePlan(SharpPictureRequest(2176, 1642, 1142, 862, 60), uhd, plenty)
        assertEquals(SharpPicturePlan(2176, 1642, 200, null, false), plan)
    }

    @Test fun theScaleFollowsHowManyPanelPixelsOneAutomaticPixelCovers() {
        // An 11-inch 2560x1600 tablet: 1.75 panel pixels per automatic pixel.
        assertEquals(SharpPicturePlan(2560, 1600, 175, null, false),
            sharpPicturePlan(SharpPictureRequest(2560, 1600, 1462, 914, 60), uhd, plenty))
        // A 1080x2400 phone upright.
        assertEquals(SharpPicturePlan(1080, 2400, 175, null, false),
            sharpPicturePlan(SharpPictureRequest(1080, 2400, 658, 1462, 60), listOf(decoder(2304, 4096)), plenty))
        // A low-density panel, 1.25 to one.
        assertEquals(SharpPicturePlan(1600, 900, 125, null, false),
            sharpPicturePlan(SharpPictureRequest(1600, 900, 1280, 720, 60), uhd, plenty))
    }

    @Test fun scaleStepsAreTheHostsFiveAndNeverBeyondThem() {
        assertEquals(100, sharpScale(1.0))
        assertEquals(100, sharpScale(1.12))
        assertEquals(100, sharpScale(1.125)) // between two steps: the lower
        assertEquals(125, sharpScale(1.13))
        assertEquals(150, sharpScale(1.6))
        assertEquals(175, sharpScale(1.65))
        assertEquals(200, sharpScale(1.9))
        assertEquals(200, sharpScale(3.0))
        assertEquals(100, sharpScale(0.4))
        assertEquals(100, sharpScale(Double.NaN))
    }

    @Test fun aPanelDenserThanTwoHundredPercentIsAskedForTwiceTheAutomaticSize() {
        // 1440x3200 with an automatic size of 658x1462: 2.19 to one. More pixels than
        // 200 % can carry would make everything smaller.
        val plan = sharpPicturePlan(SharpPictureRequest(1440, 3200, 658, 1462, 60), listOf(decoder(4096, 4096)), plenty)
        assertEquals(SharpPicturePlan(1316, 2924, 200, "scale_limit", false), plan)
        // Just above two to one is still the panel's own pixels (within half a step).
        val near = sharpPicturePlan(SharpPictureRequest(2700, 1350, 1320, 660, 60), uhd, plenty)
        assertEquals(SharpPicturePlan(2700, 1350, 200, null, false), near)
    }

    @Test fun aDecoderThatTakesLessGetsTheLargestSizeItTakes() {
        // Full HD only (1920x1088): the panel's 2176x1642 is too tall and too wide.
        val plan = sharpPicturePlan(SharpPictureRequest(2176, 1642, 1142, 862, 60), listOf(decoder(1920, 1088)), plenty)
        assertEquals("decoder_size", plan.reason)
        assertFalse(plan.fallback)
        assertTrue(plan.width in 1144..1920 && plan.height in 864..1088)
        assertEquals(0, plan.width % 2)
        assertEquals(0, plan.height % 2)
        // Still the screen's shape.
        assertEquals(1142.0 / 862, plan.width.toDouble() / plan.height, 0.01)
        // And the largest of the tried sizes the decoder takes.
        assertEquals(1400, plan.width)
        assertEquals(1056, plan.height)
        assertEquals(sharpScale(1400.0 / 1142), plan.scale)
        assertEquals(125, plan.scale)
    }

    @Test fun aDecoderTooSlowForTheFrameRateIsAskedForFewerPixels() {
        // Takes the size, but only 1920x1080 at 60 per second in pixels.
        val slow = listOf(decoder(4096, 2304, maxRate = 1920L * 1080 * 60))
        val at60 = sharpPicturePlan(SharpPictureRequest(2176, 1642, 1142, 862, 60), slow, plenty)
        assertEquals("decoder_rate", at60.reason)
        assertFalse(at60.fallback)
        assertTrue(at60.width.toLong() * at60.height * 60 <= 1920L * 1080 * 60)
        assertTrue(at60.width > 1142)
        // At 30 frames the same decoder takes the whole panel.
        assertEquals(SharpPicturePlan(2176, 1642, 200, null, false),
            sharpPicturePlan(SharpPictureRequest(2176, 1642, 1142, 862, 30), slow, plenty))
    }

    @Test fun nothingSharperThanTodayIsAFallbackToTodaysSize() {
        val request = SharpPictureRequest(2176, 1642, 1142, 862, 60)
        // The decoder takes nothing above the automatic size.
        assertEquals(SharpPicturePlan(1142, 862, 100, "decoder_size", true), sharpPicturePlan(request, listOf(decoder(1142, 862)), plenty))
        // No decoder could be asked.
        assertEquals(SharpPicturePlan(1142, 862, 100, "decoder_unknown", true), sharpPicturePlan(request, emptyList(), plenty))
        // Not enough memory for a larger picture.
        assertEquals(SharpPicturePlan(1142, 862, 100, "memory_budget", true), sharpPicturePlan(request, uhd, 4L * 1024 * 1024))
        assertEquals(SharpPicturePlan(1142, 862, 100, "memory_budget", true), sharpPicturePlan(request, uhd, 0))
    }

    @Test fun memoryDecidesTooWithTheSameEstimateTheEngineStartsWith() {
        val request = SharpPictureRequest(2176, 1642, 1142, 862, 60)
        val whole = streamSizeMemoryBytes(listOf(StreamSizeTrack(2176, 1642, 60)))!!
        assertEquals(SharpPicturePlan(2176, 1642, 200, null, false), sharpPicturePlan(request, uhd, whole))
        val less = sharpPicturePlan(request, uhd, whole - 1)
        assertEquals("memory_budget", less.reason)
        assertFalse(less.fallback)
        assertTrue(less.width < 2176 && less.width > 1142)
        assertTrue(streamSizeMemoryBytes(listOf(StreamSizeTrack(less.width, less.height, 60)))!! <= whole - 1)
    }

    @Test fun theConnectionsCeilingBoundsThePictureBeforeTheDecoderIsAsked() {
        var asked = 0
        val counting = listOf(StreamSizeCandidate("c2.any.avc", 8192, 8192, 8192L * 8192, Long.MAX_VALUE, 1, true) { asked++; null })
        val plan = sharpPicturePlan(SharpPictureRequest(2176, 1642, 1142, 862, 60, limitWidth = 1920, limitHeight = 1200), counting, plenty)
        assertEquals("host_limit", plan.reason)
        assertFalse(plan.fallback)
        assertTrue(plan.width <= 1920 && plan.height <= 1200 && plan.width > 1142)
        // Only the first size within the ceiling was put to the decoder.
        assertEquals(1, asked)
        val pixels = sharpPicturePlan(SharpPictureRequest(2176, 1642, 1142, 862, 60, limitPixels = 2_000_000), uhd, plenty)
        assertEquals("host_limit", pixels.reason)
        assertTrue(pixels.width.toLong() * pixels.height <= 2_000_000)
        // A ceiling at today's size: nothing sharper.
        assertEquals(SharpPicturePlan(1142, 862, 100, "host_limit", true),
            sharpPicturePlan(SharpPictureRequest(2176, 1642, 1142, 862, 60, limitWidth = 1142, limitHeight = 862), uhd, plenty))
    }

    @Test fun aScreenThatAlreadyGetsItsOwnPixelsHasNothingToGain() {
        // A TV: the automatic size is the panel's.
        val plan = sharpPicturePlan(SharpPictureRequest(1920, 1080, 1920, 1080, 60), uhd, plenty)
        assertEquals(SharpPicturePlan(1920, 1080, 100, "no_gain", false), plan)
        assertEquals("no_gain", sharpPicturePlan(SharpPictureRequest(1281, 721, 1280, 720, 60), uhd, plenty).reason)
    }

    @Test fun theChosenDecoderIsTheEnginesFirstThatTakesItWhateverItIsCalled() {
        val request = SharpPictureRequest(2176, 1642, 1142, 862, 60)
        // The first cannot, the second can: as the engine would start them.
        val pair = listOf(decoder(1280, 720, name = "c2.one.avc"), decoder(4096, 2304, name = "OMX.two.avc"))
        assertEquals(SharpPicturePlan(2176, 1642, 200, null, false), sharpPicturePlan(request, pair, plenty))
        // The name changes nothing.
        for (name in listOf("c2.qti.avc.decoder", "c2.exynos.h264.decoder", "x")) {
            assertEquals(sharpPicturePlan(request, listOf(decoder(1920, 1088)), plenty),
                sharpPicturePlan(request, listOf(decoder(1920, 1088, name = name)), plenty))
        }
        // With hardware decoding turned off only a software decoder is asked.
        val mixed = listOf(decoder(4096, 2304, accelerated = true), decoder(1920, 1088, accelerated = false, name = "c2.android.avc.decoder"))
        assertNull(sharpPicturePlan(request, mixed, plenty).reason)
        val software = sharpPicturePlan(request.copy(hardware = false), mixed, plenty)
        assertEquals("decoder_size", software.reason)
        assertEquals(1400, software.width)
    }

    @Test fun aRequestThatMakesNoSenseIsTodaysSize() {
        for (bad in listOf(
            SharpPictureRequest(2176, 1642, 1143, 862, 60),
            SharpPictureRequest(2176, 1642, 0, 0, 60),
            SharpPictureRequest(0, 1642, 1142, 862, 60),
            SharpPictureRequest(2176, 1642, 1142, 862, 0),
            SharpPictureRequest(2176, 1642, 1142, 862, 120),
            SharpPictureRequest(40000, 1642, 1142, 862, 60),
        )) {
            val plan = sharpPicturePlan(bad, uhd, plenty)
            assertEquals("invalid_request", plan.reason)
            assertTrue(plan.fallback)
            assertEquals(bad.baseWidth, plan.width)
            assertEquals(100, plan.scale)
        }
    }

    @Test fun theChannelRequestIsReadStrictly() {
        val map = mapOf<String, Any?>("panelWidth" to 2176, "panelHeight" to 1642L, "baseWidth" to 1142, "baseHeight" to 862, "fps" to 60,
            "hardware" to false, "limitWidth" to 3840, "limitHeight" to 3840, "limitPixels" to 8294400L)
        assertEquals(SharpPictureRequest(2176, 1642, 1142, 862, 60, false, 3840, 3840, 8294400), sharpPictureRequest(map))
        assertEquals(SharpPictureRequest(2176, 1642, 1142, 862, 60), sharpPictureRequest(map - "hardware" - "limitWidth" - "limitHeight" - "limitPixels"))
        assertNull(sharpPictureRequest(map - "fps"))
        assertNull(sharpPictureRequest(map + ("panelWidth" to "2176")))
        assertNull(sharpPictureRequest(map + ("baseWidth" to -2)))
        assertNull(sharpPictureRequest("x"))
        assertNull(sharpPictureRequest(null))
    }

    @Test fun fallbacksAreCountedForTheStatistics() {
        val stats = SharpPictureStats()
        assertEquals(false, stats.values()["sharpRequested"])
        assertEquals(0, stats.values()["sharpFallbacks"])
        stats.planned(SharpPicturePlan(2176, 1642, 200, null, false))
        var values = stats.values()
        assertEquals(true, values["sharpRequested"])
        assertEquals(2176, values["sharpWidth"])
        assertEquals(1642, values["sharpHeight"])
        assertEquals(200, values["sharpScale"])
        assertNull(values["sharpReason"])
        assertEquals(0, values["sharpFallbacks"])
        // A smaller picture than the panel's is not a fallback; today's size is.
        stats.planned(SharpPicturePlan(1400, 1056, 125, "decoder_size", false))
        assertEquals(0, stats.values()["sharpFallbacks"])
        assertEquals("decoder_size", stats.values()["sharpReason"])
        stats.planned(SharpPicturePlan(1142, 862, 100, "decoder_rate", true))
        values = stats.values()
        assertEquals(1, values["sharpFallbacks"])
        assertEquals("decoder_rate", values["sharpFallbackReason"])
        // The Host's refusals are counted too, and nothing else is.
        stats.hostFallback("host_refused")
        stats.hostFallback("host_reduced")
        stats.hostFallback("anything else")
        values = stats.values()
        assertEquals(3, values["sharpFallbacks"])
        assertEquals("host_reduced", values["sharpFallbackReason"])
        // Turned off: no longer asked, the count of this app run stays.
        stats.off()
        values = stats.values()
        assertEquals(false, values["sharpRequested"])
        assertEquals(0, values["sharpWidth"])
        assertEquals(3, values["sharpFallbacks"])
    }
}
