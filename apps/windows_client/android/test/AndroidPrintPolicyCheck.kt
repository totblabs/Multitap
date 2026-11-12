package me.totb.multitap_client

import java.io.File
import java.nio.file.Files

/** JVM fixtures only: no Android runtime, print service, device or real links. */
object AndroidPrintPolicyCheck {
    @JvmStatic fun main(args: Array<String>) {
        val root = Files.createTempDirectory("multitap-print-policy-").toFile()
        var count = 0
        fun verify(value: Boolean, name: String) {
            check(value) { name }
            count++
        }
        try {
            val cache = File(root, "cache").apply { mkdirs() }
            val codeCache = File(root, "code_cache").apply { mkdirs() }
            fun document(base: File, path: String) = File(base, path).apply {
                requireNotNull(parentFile).mkdirs()
                writeText("%PDF-fixture")
            }
            val cached = document(cache, "print/session-one/job.pdf")
            // Flutter 3.47.7's PathUtils uses getCodeCacheDir first. This was
            // rejected by AndroidPrint's former cacheDir-only policy.
            val flutterTemp = document(codeCache, "print/session-two/job.pdf")
            verify(AndroidPrintPolicy.allowsDocument(cached, cache, codeCache), "cache print document")
            verify(AndroidPrintPolicy.allowsDocument(flutterTemp, cache, codeCache), "Flutter code-cache print document")
            verify(!flutterTemp.canonicalPath.startsWith(cache.canonicalPath + File.separator), "regression fixture is outside cacheDir")
            verify(AndroidPrintPolicy.allowsDocument(cached, cache, null), "cache available without code cache")
            verify(AndroidPrintPolicy.allowsDocument(flutterTemp, null, codeCache), "code cache available without cache")
            verify(!AndroidPrintPolicy.allowsDocument(cached, null, null), "unknown private roots")
            verify(!AndroidPrintPolicy.allowsDocument(null, cache, codeCache), "missing argument")
            verify(!AndroidPrintPolicy.allowsDocument(File(cache, "print/session-one/missing.pdf"), cache, codeCache), "missing document")
            verify(!AndroidPrintPolicy.allowsDocument(cached.parentFile, cache, codeCache), "directory is not a document")
            val outside = document(root, "outside.pdf")
            verify(!AndroidPrintPolicy.allowsDocument(outside, cache, codeCache), "outside private roots")
            for (base in listOf(cache, codeCache)) {
                val other = document(base, "other.pdf")
                val adjacent = document(base, "print-other/other.pdf")
                verify(!AndroidPrintPolicy.allowsDocument(other, cache, codeCache), "unrelated cache file")
                verify(!AndroidPrintPolicy.allowsDocument(adjacent, cache, codeCache), "print prefix is not a child")
                verify(!AndroidPrintPolicy.allowsDocument(File(base, "print/../other.pdf"), cache, codeCache), "dot-dot escapes print scope")
                // Simulate canonical link resolution without creating links or
                // changing Windows filesystem privilege/security settings.
                val leafEscape = CanonicalFile(File(base, "print/session/linked.pdf"), outside)
                val folderEscape = CanonicalFile(File(base, "print/linked/folder.pdf"), other)
                verify(!AndroidPrintPolicy.allowsDocument(leafEscape, cache, codeCache), "document link escape")
                verify(!AndroidPrintPolicy.allowsDocument(folderEscape, cache, codeCache), "print-directory link escape")
            }
            val unavailable = object : File(cached.path) {
                override fun getCanonicalPath(): String = throw java.io.IOException("fixture unavailable")
            }
            verify(!AndroidPrintPolicy.allowsDocument(unavailable, cache, codeCache), "unreadable canonical path")
            for (state in listOf(AndroidPrintPolicy.JobState.queued, AndroidPrintPolicy.JobState.started,
                AndroidPrintPolicy.JobState.blocked, AndroidPrintPolicy.JobState.completed)) {
                verify(AndroidPrintPolicy.finishCode(true, false, state).isEmpty(), "Android accepted $state")
                verify(AndroidPrintPolicy.finishCode(false, false, state) == "print_failed", "no document write $state")
                verify(AndroidPrintPolicy.finishCode(true, true, state) == "print_failed", "write failure $state")
            }
            verify(AndroidPrintPolicy.finishCode(true, false, AndroidPrintPolicy.JobState.cancelled) == "print_cancelled", "preview followed by cancellation")
            verify(AndroidPrintPolicy.finishCode(false, false, AndroidPrintPolicy.JobState.cancelled) == "print_cancelled", "cancel before document write")
            for (state in listOf(AndroidPrintPolicy.JobState.created, AndroidPrintPolicy.JobState.unknown,
                AndroidPrintPolicy.JobState.failed)) {
                verify(AndroidPrintPolicy.finishCode(true, false, state) == "print_failed", "preview alone is not accepted $state")
            }
            println("Android print path policy: $count checks passed (JVM fixtures only)")
        } finally {
            check(root.canonicalFile.parentFile == File(requireNotNull(System.getProperty("java.io.tmpdir"))).canonicalFile)
            check(root.name.startsWith("multitap-print-policy-"))
            root.deleteRecursively()
        }
    }

    private class CanonicalFile(path: File, private val target: File) : File(path.path) {
        override fun isFile(): Boolean = target.isFile
        override fun getCanonicalPath(): String = target.canonicalPath
    }
}
