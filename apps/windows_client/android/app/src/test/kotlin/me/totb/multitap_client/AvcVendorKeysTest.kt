package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

// Vendor low-latency decoder keys by decoder name (AndroidMediaPacing.kt
// AvcVendorKeys): the keys, values and prefixes Moonlight-Android sets
// (MediaCodecHelper.setDecoderLowLatencyOptions).
class AvcVendorKeysTest {
    @Test fun qualcommAndExynosKeys() {
        val qti = mapOf("vendor.qti-ext-dec-picture-order.enable" to 1, "vendor.qti-ext-dec-low-latency.enable" to 1)
        assertEquals(qti, avcVendorLowLatencyKeys("c2.qti.avc.decoder"))
        assertEquals(qti, avcVendorLowLatencyKeys("OMX.qcom.video.decoder.avc"))
        val exynos = mapOf("vendor.rtc-ext-dec-low-latency.enable" to 1)
        assertEquals(exynos, avcVendorLowLatencyKeys("C2.Exynos.H264.Decoder"))
        assertEquals(exynos, avcVendorLowLatencyKeys("OMX.Exynos.avc.dec"))
    }

    @Test fun kirinKeys() {
        val kirin = mapOf(
            "vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-req" to 1,
            "vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-rdy" to -1)
        assertEquals(kirin, avcVendorLowLatencyKeys("OMX.hisi.video.decoder.avc"))
        assertEquals(kirin, avcVendorLowLatencyKeys("c2.hisi.avc.decoder"))
    }

    @Test fun amlogicKey() {
        val amlogic = mapOf("vendor.low-latency.enable" to 1)
        assertEquals(amlogic, avcVendorLowLatencyKeys("OMX.amlogic.avc.decoder.awesome"))
        assertEquals(amlogic, avcVendorLowLatencyKeys("c2.amlogic.avc.decoder"))
    }

    @Test fun mediatekKey() {
        val mediatek = mapOf("vdec-lowlatency" to 1)
        assertEquals(mediatek, avcVendorLowLatencyKeys("OMX.MTK.VIDEO.DECODER.AVC"))
        assertEquals(mediatek, avcVendorLowLatencyKeys("c2.mtk.avc.decoder"))
    }

    @Test fun otherDecodersGetNoVendorKeys() {
        for (name in listOf("c2.android.avc.decoder", "OMX.google.h264.decoder", "OMX.Nvidia.h264.decode",
                "c2.qtiextra.avc.decoder", "omx.mtkx.avc", "c2.amlogicavc", "")) {
            assertTrue(name, avcVendorLowLatencyKeys(name).isEmpty())
        }
    }
}
