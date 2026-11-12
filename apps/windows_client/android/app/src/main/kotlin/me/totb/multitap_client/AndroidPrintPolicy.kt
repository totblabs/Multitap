package me.totb.multitap_client

import java.io.File

/** Documents received by PrintSession, in the app's private print folders. */
object AndroidPrintPolicy {
    enum class JobState { created, queued, started, blocked, completed, failed, cancelled, unknown }

    /** Empty means Android accepted the document, never physical-paper proof. */
    fun finishCode(written: Boolean, writeFailed: Boolean, state: JobState): String = when {
        writeFailed -> "print_failed"
        state == JobState.cancelled -> "print_cancelled"
        written && state in setOf(JobState.queued, JobState.started, JobState.blocked, JobState.completed) -> ""
        else -> "print_failed"
    }

    fun allowsDocument(file: File?, cache: File?, codeCache: File?): Boolean = try {
        if (file == null || !file.isFile) false
        else {
            val document = file.canonicalPath
            listOfNotNull(cache, codeCache).any { base ->
                // Canonicalize the private base, not the print directory: a
                // print-directory or document link must not expand this scope.
                val print = File(base.canonicalFile, "print").path + File.separator
                document.startsWith(print)
            }
        }
    } catch (_: Exception) {
        false
    }
}
