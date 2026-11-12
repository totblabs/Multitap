package me.totb.multitap_client

import java.net.IDN
import java.util.Locale

/** Pure origin/consent policy: no socket, DNS lookup, preferences or key access. */
internal object AndroidTransportPolicy {
    fun normalize(value: String): String? = try {
        require(value.isNotEmpty() && value.length <= 512)
        require(value.none { it <= ' ' || it == '\u007f' || it in "\\@%?#" })
        val split = value.indexOf("://")
        require(split > 0)
        val scheme = value.substring(0, split).lowercase(Locale.ROOT)
        require(scheme == "http" || scheme == "https")
        var authority = value.substring(split + 3)
        if ('/' in authority) { require(authority.endsWith('/') && authority.count { it == '/' } == 1); authority = authority.dropLast(1) }
        require(authority.isNotEmpty())
        var portText: String? = null
        val host: String
        if (authority.startsWith('[')) {
            val end = authority.indexOf(']')
            require(end > 0)
            host = "[${ipv6(authority.substring(1, end))}]"
            val suffix = authority.substring(end + 1)
            if (suffix.isNotEmpty()) { require(suffix.startsWith(':')); portText = suffix.substring(1) }
        } else {
            require(authority.count { it == ':' } <= 1)
            val at = authority.indexOf(':')
            val name = if (at < 0) authority else authority.substring(0, at)
            if (at >= 0) portText = authority.substring(at + 1)
            val ascii = IDN.toASCII(name, IDN.USE_STD3_ASCII_RULES).lowercase(Locale.ROOT)
            require(ascii.isNotEmpty() && ascii.length <= 253)
            val labelsHost = ascii.removeSuffix(".")
            val labels = labelsHost.split('.')
            val last = labels.last()
            val numeric = last.all { it in '0'..'9' } || last.startsWith("0x")
            require(labels.all { it.isNotEmpty() && it.length <= 63 && it.first() != '-' && it.last() != '-' &&
                it.all { c -> c in 'a'..'z' || c in '0'..'9' || c == '-' } })
            if (numeric) {
                require(labels.size == 4 && labels.all { it.length <= 3 && (it.length == 1 || it[0] != '0') &&
                    it.all { c -> c in '0'..'9' } && it.toInt() <= 255 })
            }
            host = if (numeric) labelsHost else ascii
        }
        val defaultPort = if (scheme == "https") 443 else 80
        val port = if (portText == null) defaultPort else {
            require(portText.isNotEmpty() && portText.length <= 5 && portText.all { it in '0'..'9' })
            portText.toInt().also { require(it in 1..65535) }
        }
        "$scheme://$host" + if (port == defaultPort) "" else ":$port"
    } catch (_: Exception) { null }

    private fun ipv6(text: String): String {
        val gap = text.indexOf("::")
        require(gap < 0 || text.indexOf("::", gap + 2) < 0)
        fun part(value: String): List<Int> {
            if (value.isEmpty()) return emptyList()
            val result = mutableListOf<Int>()
            val words = value.split(':')
            words.forEachIndexed { index, word ->
                if ('.' in word) {
                    require(index == words.lastIndex)
                    val pieces = word.split('.')
                    require(pieces.size == 4 && pieces.all { it.isNotEmpty() && it.length <= 3 &&
                        (it.length == 1 || it[0] != '0') && it.all { c -> c in '0'..'9' } && it.toInt() <= 255 })
                    result += (pieces[0].toInt() shl 8) or pieces[1].toInt()
                    result += (pieces[2].toInt() shl 8) or pieces[3].toInt()
                } else {
                    require(word.isNotEmpty() && word.length <= 4 && word.all { it in '0'..'9' || it.lowercaseChar() in 'a'..'f' })
                    result += word.toInt(16)
                }
            }
            return result
        }
        val left = part(if (gap < 0) text else text.substring(0, gap))
        val right = if (gap < 0) emptyList() else part(text.substring(gap + 2))
        val words = if (gap < 0) { require(left.size == 8); left } else {
            require(left.size + right.size < 8); left + List(8 - left.size - right.size) { 0 } + right
        }
        var best = -1; var count = 1; var at = 0
        while (at < 8) {
            if (words[at] != 0) { at++; continue }
            val start = at
            while (at < 8 && words[at] == 0) at++
            if (at - start > count) { best = start; count = at - start }
        }
        val result = StringBuilder(); at = 0
        while (at < 8) {
            if (at == best) { result.append("::"); at += count; continue }
            if (result.isNotEmpty() && result.last() != ':') result.append(':')
            result.append(words[at++].toString(16))
        }
        return result.toString()
    }

    fun saveRequest(raw: Any?): Map<String, Any>? {
        val args = raw as? Map<*, *> ?: return null
        val origin = args["origin"] as? String ?: return null
        if (normalize(origin) != origin) return null
        val http = origin.startsWith("http://")
        if (args.keys != (if (http) setOf("origin", "httpConsentOrigin") else setOf("origin"))) return null
        if (http && args["httpConsentOrigin"] != origin) return null
        return buildMap { put("version", 2); put("serverOrigin", origin); if (http) put("httpConsentOrigin", origin) }
    }

    fun settings(values: Map<String, *>): Map<String, Any>? {
        if (values["transportVersion"] != 2) return null
        val origin = values["origin"] as? String ?: return null
        if (normalize(origin) != origin) return null
        val http = origin.startsWith("http://")
        if (http && values["httpConsentOrigin"] != origin) return null
        if (!http && values.containsKey("httpConsentOrigin")) return null
        return buildMap { put("version", 2); put("serverOrigin", origin); if (http) put("httpConsentOrigin", origin) }
    }

    fun credentialOriginAllowed(origin: String, values: Map<String, *>): Boolean =
        normalize(origin) == origin && (origin.startsWith("https://") || settings(values)?.get("serverOrigin") == origin)

    /** A password is remembered only for an HTTPS server;
     *  an HTTP server keeps the ID only, and a password stored
     *  earlier is never read back and goes at the next sign-in. */
    fun passwordMemoryAllowed(origin: String): Boolean = normalize(origin) == origin && origin.startsWith("https://")
}
