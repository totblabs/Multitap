package me.totb.multitap_client

/** Actual production policy on private maps/strings only; no Android or network. */
object AndroidTransportPolicyCheck {
    @JvmStatic fun main(args: Array<String>) {
        var checks = 0
        fun verify(value: Boolean, label: String) { check(value) { label }; checks++ }
        for ((input, expected) in listOf(
            "https://EXAMPLE.test:443/" to "https://example.test",
            "https://example.test:8443" to "https://example.test:8443",
            "http://192.0.2.10:18400/" to "http://192.0.2.10:18400",
            "http://example.test:80" to "http://example.test",
            "http://[2001:0DB8:0:0:0:0:0:1]:80/" to "http://[2001:db8::1]",
            "http://[::ffff:192.0.2.1]" to "http://[::ffff:c000:201]",
            "https://example.test." to "https://example.test.",
            "http://192.0.2.1." to "http://192.0.2.1"
        )) verify(AndroidTransportPolicy.normalize(input) == expected, "canonical $input")
        for (input in listOf("http://127.1", "http://0177.0.0.1", "http://0x7f.0.0.1", "http://example.test:0",
            "http://example.test:65536", "http://example.test:+80", "http://example.test:8e1", "http://example.test/path",
            "http://user@example.test", "http://example.test?x", "http://example.test#x", "http://example.test\\evil",
            "http://[fe80::1%25eth0]", " http://example.test", "http://example.test\n", "ftp://example.test"))
            verify(AndroidTransportPolicy.normalize(input) == null, "unsafe $input")
        val http = "http://example.test:18400"
        val stored = mapOf("transportVersion" to 2, "origin" to http, "httpConsentOrigin" to http)
        verify(AndroidTransportPolicy.settings(stored) == mapOf("version" to 2, "serverOrigin" to http, "httpConsentOrigin" to http), "schema2 DTO")
        verify(AndroidTransportPolicy.credentialOriginAllowed(http, stored), "saved exact consent")
        verify(!AndroidTransportPolicy.credentialOriginAllowed("http://example.test", stored), "port differs")
        verify(!AndroidTransportPolicy.credentialOriginAllowed("http://other.test:18400", stored), "host differs")
        verify(!AndroidTransportPolicy.credentialOriginAllowed(http, emptyMap<String, Any>()), "no saved consent")
        verify(!AndroidTransportPolicy.credentialOriginAllowed(http, stored - "transportVersion"), "legacy HTTP refused")
        verify(!AndroidTransportPolicy.credentialOriginAllowed(http, stored + ("httpConsentOrigin" to "https://example.test:18400")), "scheme differs")
        verify(!AndroidTransportPolicy.credentialOriginAllowed(http, stored + ("transportVersion" to "2")), "version type")
        verify(AndroidTransportPolicy.credentialOriginAllowed("https://example.test", emptyMap<String, Any>()), "legacy HTTPS credential")
        verify(AndroidTransportPolicy.settings(mapOf("origin" to "https://example.test")) == null, "legacy native method may fall back to HTTPS origin")
        verify(AndroidTransportPolicy.settings(mapOf("origin" to "https://example.test", "transportVersion" to 2)) ==
            mapOf("version" to 2, "serverOrigin" to "https://example.test"), "new HTTPS settings")
        verify(AndroidTransportPolicy.saveRequest(mapOf("origin" to http, "httpConsentOrigin" to http)) != null, "explicit save")
        verify(AndroidTransportPolicy.saveRequest(mapOf("origin" to http)) == null, "missing save consent")
        verify(AndroidTransportPolicy.saveRequest(mapOf("origin" to http, "httpConsentOrigin" to true)) == null, "no boolean substitute")
        verify(AndroidTransportPolicy.saveRequest(mapOf("origin" to http, "httpConsentOrigin" to http, "extra" to true)) == null, "exact RPC fields")
        verify(AndroidTransportPolicy.saveRequest(mapOf("origin" to "https://example.test")) != null, "HTTPS save")
        verify(AndroidTransportPolicy.saveRequest(mapOf("origin" to "https://example.test", "httpConsentOrigin" to http)) == null, "HTTPS cannot retain HTTP consent")
        println("{\"checks\":$checks,\"productionPolicy\":true,\"androidStorageCalls\":0,\"networkCalls\":0}")
    }
}
