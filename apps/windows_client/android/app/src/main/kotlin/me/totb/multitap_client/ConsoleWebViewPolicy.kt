package me.totb.multitap_client

import java.net.URI
import java.security.MessageDigest

internal data class ConsoleWebViewTarget(val origin: String, val desktopId: String, val attachPath: String) {
    val consolePath get() = "/console/$desktopId"
    val consoleUrl get() = origin + consolePath
    val attachUrl get() = origin + attachPath
    val cookieName: String get() {
        val digest = MessageDigest.getInstance("SHA-256").digest(origin.toByteArray(Charsets.UTF_8))
        return "__Secure-multitap-console-" + digest.take(8).joinToString("") { "%02x".format(it.toInt() and 255) }
    }
    fun sameOrigin(url: String): Boolean = try {
        val value = URI(url)
        value.scheme == "https" && value.rawUserInfo == null &&
            value.rawAuthority == URI(origin).rawAuthority && value.rawFragment == null
    } catch (_: Exception) { false }
    fun navigation(url: String): Boolean = sameOrigin(url) && try {
        val value = URI(url)
        value.rawQuery == null && value.rawPath == consolePath
    } catch (_: Exception) { false }
    fun resource(url: String): Boolean = sameOrigin(url) && try {
        val value = URI(url); val path = value.rawPath
        value.rawQuery == null && '%' !in path && '\\' !in path &&
            path.split('/').none { it == "." || it == ".." } &&
            (path == consolePath || path.startsWith("/console/assets/") ||
                path == "/console/api/session" || path == "/console/api/logout" ||
                path == "/console/api/desktops/$desktopId/connect" ||
                // .../ime: the VM's 한/영 mode read (ime-state-v1). Without it in this list the
                // phone Console types only Latin letters.
                Regex("^/console/api/desktops/${Regex.escape(desktopId)}/sessions/[a-fA-F0-9-]{36}(/ime)?$").matches(path))
    } catch (_: Exception) { false }
    fun cookie(header: String): String? {
        if (header.length !in 100..1024 || header.any { it.code < 32 || it.code > 126 }) return null
        val fields = header.split(';').map { it.trim() }
        if (fields.firstOrNull()?.let { Regex("^${Regex.escape(cookieName)}=[A-Za-z0-9_-]{43}$").matches(it) } != true) return null
        val flags = fields.drop(1).map { it.lowercase() }
        if (flags.size != flags.toSet().size || flags.toSet() != setOf("path=/console", "httponly", "secure", "samesite=strict")) return null
        return header
    }
}

internal object ConsoleWebViewPolicy {
    fun ownerId(code: String): String = MessageDigest.getInstance("SHA-256").digest(code.toByteArray(Charsets.US_ASCII)).joinToString("") { "%02x".format(it.toInt() and 255) }
    fun target(origin: String, desktopId: String, attachPath: String): ConsoleWebViewTarget? {
        val console = ConsoleBrowserTarget.build(origin, desktopId) ?: return null
        val normalized = console.removeSuffix("/console/$desktopId")
        // Native callers receive Dart's canonical origin, never an alias path.
        if (origin != normalized || attachPath != "/console/$desktopId/attach") return null
        return ConsoleWebViewTarget(normalized, desktopId, attachPath)
    }
    fun code(value: String): Boolean = Regex("^[A-Za-z0-9_-]{43}$").matches(value)
}
