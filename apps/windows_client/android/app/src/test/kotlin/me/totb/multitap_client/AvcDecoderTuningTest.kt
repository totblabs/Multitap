package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

// How AvcDecoders.configureAndStart configures an H.264 decoder: the format
// keys per decoder name and API level (avcTunings + avcTuningKeys), the
// fallback when configure refuses a key (avcConfigureFirst), and the
// stream stats value that records the result (avcTuningFlags).
class AvcDecoderTuningTest {
    /** Stands in for MediaCodec.configure/start: refuses a format with any of [refused]. */
    private class FakeDecoder(val name: String, val declared: Boolean, val refused: Set<String> = emptySet()) {
        val attempts = ArrayList<Map<String, Int>>()
        val indexes = ArrayList<Int>()

        /** What configureAndStart does; returns (keys used, stats flags, vendor code), null when nothing started. */
        fun start(api: Int, fps: Int = 60): Triple<Map<String, Int>, Int, Int>? {
            val vendor = AvcVendorKeys.of(name)
            var used: Map<String, Int>? = null
            val tuning = avcConfigureFirst(avcTunings(api, declared, vendor != null)) { index, rung ->
                indexes.add(index)
                val keys = avcTuningKeys(rung, vendor?.keys.orEmpty(), fps)
                attempts.add(keys)
                if (keys.keys.any { it in refused }) throw IllegalArgumentException("refused")
                used = keys
            } ?: return null
            return Triple(used!!, avcTuningFlags(tuning, declared), vendor?.code ?: 0)
        }
    }

    private val qti = mapOf("vendor.qti-ext-dec-picture-order.enable" to 1, "vendor.qti-ext-dec-low-latency.enable" to 1)

    @Test fun qualcommAndroid13GetsEverythingFirst() {
        val (keys, flags, vendor) = FakeDecoder("c2.qti.avc.decoder", declared = true).start(33)!!
        assertEquals(mapOf("priority" to 0, "operating-rate" to 60, "low-latency" to 1) + qti, keys)
        assertEquals(1 or 2 or 4 or 8 or 16, flags)
        assertEquals(1, vendor)
    }

    @Test fun refusedVendorKeysConfigureAgainWithoutThem() {
        val decoder = FakeDecoder("c2.qti.avc.decoder", declared = true, refused = qti.keys)
        val (keys, flags, vendor) = decoder.start(33)!!
        assertEquals(mapOf("priority" to 0, "operating-rate" to 60, "low-latency" to 1), keys)
        assertEquals(1 or 2 or 4 or 16, flags)
        assertEquals(1, vendor)
        assertEquals(listOf(0, 1), decoder.indexes)
    }

    @Test fun refusedLowLatencyKeyConfigureAgainWithoutIt() {
        // A decoder that does not declare FEATURE_LowLatency and refuses the key.
        val decoder = FakeDecoder("c2.exynos.h264.decoder", declared = false, refused = setOf("low-latency"))
        val (keys, flags, _) = decoder.start(31)!!
        assertEquals(mapOf("priority" to 0), keys)
        assertEquals(2, flags)
        assertEquals(3, decoder.attempts.size)
    }

    @Test fun refusedRealTimeEndsWithAPlainFormat() {
        val decoder = FakeDecoder("OMX.google.h264.decoder", declared = false, refused = setOf("priority"))
        val (keys, flags, vendor) = decoder.start(33)!!
        assertEquals(emptyMap<String, Int>(), keys)
        assertEquals(0, flags)
        assertEquals(0, vendor)
    }

    @Test fun refusedMediatekOptionConfiguresAgainWithoutIt() {
        // MediaTek's ACodec option on a decoder that refuses unknown keys.
        val decoder = FakeDecoder("c2.mtk.avc.decoder", declared = false, refused = setOf("vdec-lowlatency"))
        val (keys, flags, vendor) = decoder.start(30)!!
        assertEquals(mapOf("priority" to 0, "operating-rate" to 60, "low-latency" to 1), keys)
        assertEquals(1 or 2 or 4, flags)
        assertEquals(5, vendor)
    }

    @Test fun nothingStartsWhenEveryAttemptThrows() {
        var calls = 0
        assertNull(avcConfigureFirst(avcTunings(33, true, true)) { _, _ -> calls++; throw IllegalStateException("codec error") })
        assertEquals(4, calls)
    }

    @Test fun beforeAndroid11NoKeyLowLatency() {
        val (keys, flags, _) = FakeDecoder("OMX.qcom.video.decoder.avc", declared = false).start(29)!!
        assertEquals(mapOf("priority" to 0, "operating-rate" to 60) + qti, keys)
        assertEquals(2 or 4 or 8, flags)
        val decoder = FakeDecoder("OMX.qcom.video.decoder.avc", declared = false, refused = setOf("priority"))
        decoder.start(29)
        assertEquals(0, decoder.attempts.count { "low-latency" in it })
    }

    @Test fun operatingRateFollowsTheStream() {
        val (keys, _, _) = FakeDecoder("c2.android.avc.decoder", declared = false).start(33, fps = 30)!!
        assertEquals(mapOf("priority" to 0, "operating-rate" to 30, "low-latency" to 1), keys)
    }

    @Test fun kirinAndAmlogicCarryTheirCodes() {
        assertEquals(3, FakeDecoder("OMX.hisi.video.decoder.avc", declared = false).start(29)!!.third)
        val (keys, _, vendor) = FakeDecoder("c2.amlogic.avc.decoder", declared = true).start(34)!!
        assertEquals(4, vendor)
        assertEquals(1, keys["vendor.low-latency.enable"])
    }
}
