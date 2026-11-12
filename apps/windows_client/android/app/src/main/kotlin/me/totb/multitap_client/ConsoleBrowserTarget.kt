package me.totb.multitap_client

import java.net.URI

internal object ConsoleBrowserTarget {
    fun build(origin: String, desktopId: String): String? {
        if (origin.length !in 9..2048 || origin.any { it.code > 127 } ||
            !Regex("^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$").matches(desktopId)) return null
        return try {
            val value = URI(origin)
            if (value.scheme != "https" || value.host.isNullOrEmpty() || value.rawUserInfo != null ||
                value.rawQuery != null || value.rawFragment != null ||
                value.rawPath !in listOf("", "/") || value.port == 0 || value.port > 65535) null
            else "https://${value.rawAuthority}/console/$desktopId"
        } catch (_: Exception) { null }
    }
}
