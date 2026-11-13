package me.totb.multitap_client

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files

/** JVM-only test of the production update policy; no PackageInstaller/device claim. */
object AndroidUpdatePolicyCheck {
    @JvmStatic
    fun main(args: Array<String>) {
        var checks = 0
        fun verify(value: Boolean, label: String) { check(value) { label }; checks++ }
        val all = listOf("arm64-v8a", "armeabi-v7a", "x86_64")

        // Installed split from Flutter's 1000 * ABI + version code.
        verify(AndroidUpdatePolicy.installedAbi(2004, all) == "arm64-v8a", "arm64")
        verify(AndroidUpdatePolicy.installedAbi(1004, all) == "armeabi-v7a", "armv7")
        verify(AndroidUpdatePolicy.installedAbi(4004, all) == "x86_64", "x86_64")
        verify(AndroidUpdatePolicy.installedAbi(4004, listOf("arm64-v8a")) == null, "unsupported abi")
        verify(AndroidUpdatePolicy.installedAbi(4, all) == null, "debug build has no split")
        verify(AndroidUpdatePolicy.installedAbi(3004, all) == null, "unknown split")

        // Same split and strictly newer only.
        verify(AndroidUpdatePolicy.acceptableVersionCode(2004, 2005), "newer same split")
        verify(!AndroidUpdatePolicy.acceptableVersionCode(2004, 2004), "same version")
        verify(!AndroidUpdatePolicy.acceptableVersionCode(2005, 2004), "older")
        verify(!AndroidUpdatePolicy.acceptableVersionCode(2004, 1005), "other split")
        verify(!AndroidUpdatePolicy.acceptableVersionCode(2004, 4005), "other split 2")

        // 1.0.0 (base code 100, build.gradle.kts) installs over the last preview (65) in every split;
        // a base code of 1000 or more (10000 for 1.0.0) would land in another ABI thousand and be refused.
        verify(AndroidUpdatePolicy.acceptableVersionCode(2065, 2100), "1.0.0 over preview.65 arm64")
        verify(AndroidUpdatePolicy.acceptableVersionCode(1065, 1100), "1.0.0 over preview.65 armv7")
        verify(AndroidUpdatePolicy.acceptableVersionCode(4065, 4100), "1.0.0 over preview.65 x86_64")
        verify(AndroidUpdatePolicy.acceptableVersionCode(2100, 2101), "1.0.1 over 1.0.0")
        verify(AndroidUpdatePolicy.installedAbi(2100, all) == "arm64-v8a", "1.0.0 split")
        verify(!AndroidUpdatePolicy.acceptableVersionCode(2065, 12000), "a 10000-based code is another split")

        // Request: exact keys, server version/filename rules, file directly in the directory.
        val root = Files.createTempDirectory("multitap-update-check").toFile()
        val dir = File(root, "updates").apply { mkdirs() }
        val apk = File(dir, "Multitap-0.1.0-preview.5-android-arm64-v8a.apk").apply { writeBytes(ByteArray(10) { it.toByte() }) }
        val sha = "a".repeat(64)
        fun args(path: String = apk.path, version: Any = "0.1.0-preview.5", size: Any = 10, digest: Any = sha) =
            mapOf("path" to path, "version" to version, "size" to size, "sha256" to digest)
        verify(AndroidUpdatePolicy.request(args(), dir)?.size == 10L, "valid request")
        verify(AndroidUpdatePolicy.request(args(size = 10L), dir) != null, "long size")
        verify(AndroidUpdatePolicy.request(args() + ("extra" to 1), dir) == null, "extra key")
        verify(AndroidUpdatePolicy.request(args(version = "0.1.0-preview.6"), dir) == null, "name/version mismatch")
        verify(AndroidUpdatePolicy.request(args(version = "0.1.0-beta.5"), dir) == null, "version rule")
        verify(AndroidUpdatePolicy.request(args(size = 0), dir) == null, "zero size")
        verify(AndroidUpdatePolicy.request(args(size = 128L * 1024 * 1024 + 1), dir) == null, "too large")
        verify(AndroidUpdatePolicy.request(args(digest = "A".repeat(64)), dir) == null, "uppercase digest")
        verify(AndroidUpdatePolicy.request(args(digest = 5), dir) == null, "digest type")
        verify(AndroidUpdatePolicy.request(args(size = "10"), dir) == null, "size type")
        val outside = File(root, apk.name).apply { writeBytes(ByteArray(10)) }
        verify(AndroidUpdatePolicy.request(args(path = outside.path), dir) == null, "outside directory")
        verify(AndroidUpdatePolicy.request(args(path = File(dir, "../" + apk.name).path), dir) == null, "dot-dot path")
        verify(AndroidUpdatePolicy.request(args(path = File(dir, "missing.apk").path), dir) == null, "missing file")
        val nested = File(dir, "nested").apply { mkdirs() }
        val deeper = File(nested, apk.name).apply { writeBytes(ByteArray(10)) }
        verify(AndroidUpdatePolicy.request(args(path = deeper.path), dir) == null, "nested directory")
        verify(AndroidUpdatePolicy.request("not a map", dir) == null, "not a map")
        val release = File(dir, "Multitap-1.0.0-android-arm64-v8a.apk").apply { writeBytes(ByteArray(10)) }
        verify(AndroidUpdatePolicy.request(args(path = release.path, version = "1.0.0"), dir)?.version == "1.0.0", "1.0.0 request")

        // The installer receives exactly the bytes that were hashed.
        val bytes = ByteArray(200_000) { (it * 31).toByte() }
        val out = ByteArrayOutputStream()
        val hex = AndroidUpdatePolicy.copyVerified(ByteArrayInputStream(bytes), out, bytes.size.toLong())
        verify(hex == AndroidUpdatePolicy.hex(bytes), "digest of copied bytes")
        verify(out.toByteArray().contentEquals(bytes), "copied bytes")
        // The dialog's bar: the bytes written so far, rising to exactly the size; the copy is the same.
        val seen = mutableListOf<Long>()
        val measured = ByteArrayOutputStream()
        val measuredHex = AndroidUpdatePolicy.copyVerified(ByteArrayInputStream(bytes), measured, bytes.size.toLong()) { seen.add(it) }
        verify(measuredHex == hex && measured.toByteArray().contentEquals(bytes), "progress changes nothing copied")
        verify(seen.size > 1 && seen == seen.sorted() && seen.toSet().size == seen.size, "progress only rises")
        verify(seen.first() > 0 && seen.last() == bytes.size.toLong(), "progress ends at the size")
        val refused = mutableListOf<Long>()
        verify(
            AndroidUpdatePolicy.copyVerified(ByteArrayInputStream(bytes), ByteArrayOutputStream(), bytes.size - 1L) { refused.add(it) } == null &&
                refused.all { it < bytes.size },
            "a longer source never reports more than the size",
        )
        verify(AndroidUpdatePolicy.copyVerified(ByteArrayInputStream(bytes), ByteArrayOutputStream(), bytes.size - 1L) == null, "longer source")
        verify(AndroidUpdatePolicy.copyVerified(ByteArrayInputStream(bytes), ByteArrayOutputStream(), bytes.size + 1L) == null, "shorter source")
        verify(AndroidUpdatePolicy.hex(ByteArray(0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha-256 of empty")

        // Signers: neither an unreadable installed nor archive set authorizes install.
        verify(AndroidUpdatePolicy.signersAcceptable(setOf("a"), setOf("a")), "same signer")
        verify(!AndroidUpdatePolicy.signersAcceptable(setOf("a"), setOf("b")), "other signer")
        verify(!AndroidUpdatePolicy.signersAcceptable(setOf("a"), emptySet()), "unreadable archive rejected")
        verify(!AndroidUpdatePolicy.signersAcceptable(emptySet(), setOf("a")), "unknown installed signer")
        verify(!AndroidUpdatePolicy.signersAcceptable(emptySet(), emptySet()), "both signers unknown")

        // PackageInstaller.STATUS_* values from android.jar 36.
        verify(AndroidUpdatePolicy.statusCode(0) == "installed", "success")
        verify(AndroidUpdatePolicy.statusCode(1) == "install_failed", "failure")
        verify(AndroidUpdatePolicy.statusCode(2) == "blocked", "blocked")
        verify(AndroidUpdatePolicy.statusCode(3) == "aborted", "aborted")
        verify(AndroidUpdatePolicy.statusCode(4) == "invalid", "invalid")
        verify(AndroidUpdatePolicy.statusCode(5) == "conflict", "conflict")
        verify(AndroidUpdatePolicy.statusCode(6) == "storage", "storage")
        verify(AndroidUpdatePolicy.statusCode(7) == "incompatible", "incompatible")
        verify(AndroidUpdatePolicy.statusCode(8) == "timeout", "timeout")
        verify(AndroidUpdatePolicy.statusCode(99) == "install_failed", "unknown status")

        root.deleteRecursively()
        println("{\"checks\":$checks,\"productionUpdatePolicy\":true,\"packageInstallerExecuted\":false}")
    }
}
