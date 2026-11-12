package me.totb.multitap_client

import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.nio.file.Files
import java.security.MessageDigest

/**
 * The in-app update rules that need no Android types, so the JVM check runs
 * this exact production code. Android-specific steps live in AndroidUpdate.
 */
internal object AndroidUpdatePolicy {
    /**
     * Flutter split APKs: versionCode = 1000 * ABI number + base code below 1000
     * (the preview number, 100 + z from 1.0.z on; build.gradle.kts).
     */
    private val abiNumbers = mapOf(1L to "armeabi-v7a", 2L to "arm64-v8a", 4L to "x86_64")
    private val version =
        Regex("^(?:0|[1-9][0-9]{0,5})\\.(?:0|[1-9][0-9]{0,5})\\.(?:0|[1-9][0-9]{0,5})(?:-preview(?:\\.[1-9][0-9]{0,5})?)?$")
    private val digest = Regex("^[a-f0-9]{64}$")
    const val maxSize = 128L * 1024 * 1024

    /** The ABI of the installed split APK when this device supports it. */
    fun installedAbi(versionCode: Long, supported: List<String>): String? {
        val abi = abiNumbers[versionCode / 1000] ?: return null
        return if (abi in supported) abi else null
    }

    /** The archive must be the same split (ABI) and strictly newer. */
    fun acceptableVersionCode(installed: Long, archive: Long): Boolean =
        installed / 1000 == archive / 1000 && abiNumbers.containsKey(archive / 1000) && archive > installed

    class Request(val file: File, val version: String, val size: Long, val sha256: String)

    /** Exactly {path, version, size, sha256}; the file is a regular, non-linked APK directly in [directory]. */
    fun request(arguments: Any?, directory: File): Request? {
        val values = arguments as? Map<*, *> ?: return null
        if (values.keys != setOf("path", "version", "size", "sha256")) return null
        val path = values["path"] as? String ?: return null
        val name = values["version"] as? String ?: return null
        val size = when (val raw = values["size"]) {
            is Int -> raw.toLong()
            is Long -> raw
            else -> return null
        }
        val sha = values["sha256"] as? String ?: return null
        if (!version.matches(name) || size !in 1..maxSize || !digest.matches(sha)) return null
        val file = File(path)
        if (!Regex("^Multitap-${Regex.escape(name)}-android-(?:arm64-v8a|armeabi-v7a|x86_64)\\.apk$").matches(file.name)) return null
        if (!inside(directory, file)) return null
        return Request(file, name, size, sha)
    }

    fun inside(directory: File, file: File): Boolean = try {
        val expected = File(directory.canonicalFile, file.name)
        !Files.isSymbolicLink(file.toPath()) && file.isFile &&
            file.canonicalFile.path == expected.path && file.absoluteFile.parentFile?.canonicalFile == directory.canonicalFile
    } catch (_: Exception) { false }

    /**
     * Copies exactly [size] bytes and returns the SHA-256 (hex) of the bytes
     * written, or null when the source is shorter or longer. The installer
     * receives precisely the bytes that were hashed.
     */
    fun copyVerified(input: InputStream, output: OutputStream, size: Long): String? {
        val sha = MessageDigest.getInstance("SHA-256")
        val buffer = ByteArray(64 * 1024)
        var total = 0L
        while (true) {
            val read = input.read(buffer)
            if (read < 0) break
            total += read
            if (total > size) return null
            sha.update(buffer, 0, read)
            output.write(buffer, 0, read)
        }
        if (total != size) return null
        return sha.digest().joinToString("") { "%02x".format(it) }
    }

    /** Both publisher observations must be readable and match before installer handoff. */
    fun signersAcceptable(installed: Set<String>, archive: Set<String>): Boolean =
        installed.isNotEmpty() && archive.isNotEmpty() && archive == installed

    /** PackageInstaller.STATUS_* (android.jar 36) as fixed codes for Dart. */
    fun statusCode(status: Int): String = when (status) {
        0 -> "installed"
        2 -> "blocked"
        3 -> "aborted"
        4 -> "invalid"
        5 -> "conflict"
        6 -> "storage"
        7 -> "incompatible"
        8 -> "timeout"
        else -> "install_failed"
    }

    fun hex(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
}
