package me.totb.multitap_client

/** Pure fake-map checks of production preference policy; no Android storage/device. */
object AndroidPreferencePolicyCheck {
    @JvmStatic fun main(args: Array<String>) {
        val first = "organize.https://vdi.example|테스터"
        val account = "organize.https://vdi.example|other"
        val server = "organize.https://other.example:8443|테스터"
        val ipv6 = "organize.https://[::1]:8443|tester"
        val supplementary = "\uD840\uDC00" // U+20000, one code point / two UTF-16 units.
        check(AndroidPreferencePolicy.accountLengthAllowed(supplementary.repeat(2)))
        check(AndroidPreferencePolicy.accountLengthAllowed(supplementary.repeat(64)))
        check(!AndroidPreferencePolicy.accountLengthAllowed(supplementary))
        check(!AndroidPreferencePolicy.accountLengthAllowed(supplementary.repeat(65)))
        check(AndroidPreferencePolicy.accountLengthAllowed("한" + supplementary))
        check(AndroidPreferencePolicy.allows("organize.https://vdi.example|" + supplementary.repeat(64)))
        check(!AndroidPreferencePolicy.allows("organize.https://vdi.example|" + supplementary))
        check(!AndroidPreferencePolicy.allows("organize.https://vdi.example|" + supplementary.repeat(65)))
        for (key in listOf(first, account, server, ipv6, "appearance.theme", "session.input", "launcher.view", "launcher.previews", "session.screen.television")) {
            check(AndroidPreferencePolicy.allows(key)) { key }
        }
        for (key in listOf("password:https://vdi.example", "id:https://vdi.example",
            "token", "session.unknown", "organize.http://vdi.example|tester",
            "organize.https://user@vdi.example|tester", "organize.https://vdi.example/path|tester",
            "organize.https://vdi.example?x=1|tester", "organize.https://vdi.example#x|tester",
            "organize.https://vdi.example|", "organize.https://vdi.example|x",
            "organize.https://vdi.example|a\nb")) {
            check(!AndroidPreferencePolicy.allows(key)) { key }
        }
        val fake = mutableMapOf<String, Any>(
            "pref:appearance.theme" to "dark",
            "pref:$first" to "first-choice",
            "pref:$account" to "other-account-choice",
            "pref:$server" to "other-server-choice",
            "password:https://vdi.example" to "not-returned",
            "pref:password:https://vdi.example" to "not-returned",
            "pref:session.unknown" to "not-returned",
            "pref:$ipv6" to 42,
        )
        val restored = AndroidPreferencePolicy.read(fake)
        check(AndroidPreferencePolicy.read(mapOf("pref:session.screen.television" to "225|auto|landscape")) ==
            mapOf("session.screen.television" to "225|auto|landscape"))
        check(AndroidPreferencePolicy.read(mapOf("pref:launcher.view" to "docking")) ==
            mapOf("launcher.view" to "docking"))
        check(AndroidPreferencePolicy.allowsValue(first, "x".repeat(512)))
        check(!AndroidPreferencePolicy.allowsValue(first, "x".repeat(4097)))
        check(!AndroidPreferencePolicy.allowsValue("appearance.theme", "x".repeat(129)))
        check(!AndroidPreferencePolicy.allowsValue(first, ""))
        check(AndroidPreferencePolicy.read(mapOf(
            "pref:$first" to "x".repeat(4097),
            "pref:appearance.theme" to "x".repeat(129),
            "pref:session.input" to "",
        )).isEmpty())
        check(AndroidPreferencePolicy.read(mapOf(
            "pref:$first" to "x".repeat(4096),
            "pref:appearance.scene" to "x".repeat(128),
        )).size == 2)
        check(restored == mapOf("appearance.theme" to "dark", first to "first-choice",
            account to "other-account-choice", server to "other-server-choice"))
        check(fake["password:https://vdi.example"] == "not-returned")
        val http = "http://vdi.example:18400"
        val httpKey = "organize.$http|tester"
        val consent = mapOf("transportVersion" to 2, "origin" to http, "httpConsentOrigin" to http)
        check(!AndroidPreferencePolicy.allows(httpKey))
        check(AndroidPreferencePolicy.allowsValue(httpKey, "choice", consent))
        check(!AndroidPreferencePolicy.allowsValue("organize.http://vdi.example|tester", "choice", consent))
        check(AndroidPreferencePolicy.read(consent + ("pref:$httpKey" to "choice")) == mapOf(httpKey to "choice"))
        check(AndroidPreferencePolicy.read(mapOf("pref:$httpKey" to "choice")).isEmpty())
        check(AndroidPreferencePolicy.read((consent - "httpConsentOrigin") + ("pref:$httpKey" to "choice")).isEmpty())
        check(AndroidPreferencePolicy.allows("organize.https://vdi.example|tester", consent))
        println("Android preference policy checks passed (fake map only)")
    }
}
