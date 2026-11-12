package me.totb.multitap_client

/** Non-secret UI preferences only; shared by the native read and write paths. */
internal object AndroidPreferencePolicy {
    fun accountLengthAllowed(account: String): Boolean =
        account.codePointCount(0, account.length) in 2..64
    private val fixed = setOf("appearance.theme", "appearance.scene", "appearance.language", "launcher.view", "launcher.previews", "stream.quality",
        "session.input", "session.pointer", "session.toolbar", "session.help",
        "session.screen.phone", "session.screen.fold", "session.screen.tablet", "session.screen.television")

    fun allows(key: String, values: Map<String, *> = emptyMap<String, Any>()): Boolean {
        if (key in fixed) return true
        if (!key.startsWith("organize.")) return false
        val split = key.indexOf('|', "organize.".length)
        if (split < 0) return false
        val origin = key.substring("organize.".length, split)
        val account = key.substring(split + 1)
        if (origin.length !in 1..2048 || !accountLengthAllowed(account) ||
            account.any { it.isISOControl() }) return false
        return AndroidTransportPolicy.credentialOriginAllowed(origin, values)
    }

    // Classification JSON carries a pool/tag name as well as status and sort.
    // Keep the old limit for every fixed preference and bound this namespace.
    fun allowsValue(key: String, value: String, values: Map<String, *> = emptyMap<String, Any>()): Boolean = allows(key, values) &&
        value.length in 1..(if (key.startsWith("organize.")) 4096 else 128)

    fun read(values: Map<String, *>): Map<String, String> = values.entries.mapNotNull { (key, value) ->
        if (!key.startsWith("pref:") || value !is String) return@mapNotNull null
        val name = key.removePrefix("pref:")
        if (allowsValue(name, value, values)) name to value else null
    }.toMap()
}
