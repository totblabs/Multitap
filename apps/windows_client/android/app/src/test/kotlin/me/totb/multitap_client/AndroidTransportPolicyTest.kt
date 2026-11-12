package me.totb.multitap_client

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** 비밀번호 기억 only for HTTPS servers. */
class AndroidTransportPolicyTest {
    @Test
    fun passwordMemoryOnlyForHttps() {
        assertTrue(AndroidTransportPolicy.passwordMemoryAllowed("https://vdi.example.test"))
        assertTrue(AndroidTransportPolicy.passwordMemoryAllowed("https://vdi.example.test:8443"))
        assertFalse(AndroidTransportPolicy.passwordMemoryAllowed("http://10.0.0.5:18480"))
        // Even an HTTP origin with saved consent keeps the ID only.
        val consented = mapOf("transportVersion" to 2, "origin" to "http://10.0.0.5:18480", "httpConsentOrigin" to "http://10.0.0.5:18480")
        assertTrue(AndroidTransportPolicy.credentialOriginAllowed("http://10.0.0.5:18480", consented))
        assertFalse(AndroidTransportPolicy.passwordMemoryAllowed("http://10.0.0.5:18480"))
        // Not a normalized origin: never.
        assertFalse(AndroidTransportPolicy.passwordMemoryAllowed("https://vdi.example.test/path"))
        assertFalse(AndroidTransportPolicy.passwordMemoryAllowed("HTTPS://VDI.EXAMPLE.TEST"))
        assertFalse(AndroidTransportPolicy.passwordMemoryAllowed(""))
    }
}
