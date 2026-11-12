package me.totb.multitap_client

import android.app.Activity
import android.content.Context
import android.os.Bundle
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.print.PageRange
import android.print.PrintAttributes
import android.print.PrintDocumentAdapter
import android.print.PrintDocumentInfo
import android.print.PrintJob
import android.print.PrintJobInfo
import android.print.PrintManager
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream

// print-v1 on Android (lib/android/android_print.dart): a PDF the VM printed
// to its Multitap printer goes to Android's own print dialog (PrintManager),
// where the user picks a printer or "Save as PDF". Only files this app keeps
// in its private cache/print or code_cache/print directories are accepted.
// Flutter's Directory.systemTemp normally selects codeCacheDir, not cacheDir.
// The answer is a fixed outcome; failures are distinct from a user cancellation.
class AndroidPrint(private val activity: Activity, engine: FlutterEngine) {
    private val channel = MethodChannel(engine.dartExecutor.binaryMessenger, "multitap/android/print")
    private var pending: ((Boolean, String) -> Unit)? = null
    private var disposed = false

    init {
        channel.setMethodCallHandler { call, result ->
            if (call.method != "print") {
                result.notImplemented()
                return@setMethodCallHandler
            }
            val events = AndroidPrintEvents(call.argument<String>("requestId")) {
                channel.invokeMethod("print.event", it)
            }
            events.stage(1)
            if (disposed || pending != null) {
                events.stage(9)
                events.close()
                result.success(mapOf("ok" to false, "code" to "print_failed"))
                return@setMethodCallHandler
            }
            var answered = false
            val answer = { written: Boolean, code: String ->
                if (!answered) {
                    answered = true
                    pending = null
                    if (code != "" && code != "print_cancelled") events.stage(9)
                    events.close()
                    result.success(mapOf("ok" to written, "code" to code))
                }
            }
            pending = answer
            val file = call.argument<String>("path")?.let { File(it) }
            val title = call.argument<String>("title").orEmpty()
            if (!AndroidPrintPolicy.allowsDocument(file, activity.cacheDir, activity.codeCacheDir)) {
                answer(false, "print_document_invalid")
                return@setMethodCallHandler
            }
            events.stage(2)
            val manager = activity.getSystemService(Context.PRINT_SERVICE) as? PrintManager
            if (manager == null) {
                answer(false, "print_unavailable")
                return@setMethodCallHandler
            }
            val name = title.ifBlank { "Multitap" }.take(100)
            var job: PrintJob? = null
            val finished = { written: Boolean, writeFailed: Boolean ->
                val state = try {
                    when (job?.info?.state) {
                        PrintJobInfo.STATE_CREATED -> AndroidPrintPolicy.JobState.created
                        PrintJobInfo.STATE_QUEUED -> AndroidPrintPolicy.JobState.queued
                        PrintJobInfo.STATE_STARTED -> AndroidPrintPolicy.JobState.started
                        PrintJobInfo.STATE_BLOCKED -> AndroidPrintPolicy.JobState.blocked
                        PrintJobInfo.STATE_COMPLETED -> AndroidPrintPolicy.JobState.completed
                        PrintJobInfo.STATE_FAILED -> AndroidPrintPolicy.JobState.failed
                        PrintJobInfo.STATE_CANCELED -> AndroidPrintPolicy.JobState.cancelled
                        else -> AndroidPrintPolicy.JobState.unknown
                    }
                } catch (_: Exception) { AndroidPrintPolicy.JobState.unknown }
                val code = AndroidPrintPolicy.finishCode(written, writeFailed, state)
                answer(code.isEmpty(), code)
            }
            try {
                if (activity.isFinishing || activity.isDestroyed) {
                    answer(false, "print_failed")
                    return@setMethodCallHandler
                }
                job = manager.print(
                    name, PdfAdapter(file!!, name, events, finished), PrintAttributes.Builder().build(),
                )
                // PrintManager may return null without invoking the adapter
                // (disabled printing or a print-screen launch failure).
                // Android's current @NonNull annotation conflicts with this
                // documented runtime result; keep the Java null guard.
                @Suppress("SENSELESS_COMPARISON")
                if (job == null) answer(false, "print_unavailable")
                else events.stage(3) // Launch requested; not proof that UI is visible.
            } catch (_: Exception) {
                answer(false, "print_failed")
            }
        }
    }

    fun dispose() {
        if (disposed) return
        disposed = true
        // Android's adapter delegate can discard onFinish on Activity destroy.
        // Resolve only our pending Dart call; do not cancel an OS-owned job.
        pending?.invoke(false, "print_failed")
        pending = null
        channel.setMethodCallHandler(null)
    }

    private class PdfAdapter(
        private val file: File,
        private val name: String,
        private val events: AndroidPrintEvents,
        private val done: (Boolean, Boolean) -> Unit,
    ) : PrintDocumentAdapter() {
        private var written = false
        private var writeFailed = false

        override fun onStart() { events.stage(4) }

        override fun onLayout(
            oldAttributes: PrintAttributes?,
            newAttributes: PrintAttributes?,
            cancellationSignal: CancellationSignal?,
            callback: LayoutResultCallback,
            extras: Bundle?,
        ) {
            if (cancellationSignal?.isCanceled == true) {
                callback.onLayoutCancelled()
                return
            }
            val info = PrintDocumentInfo.Builder(name).setContentType(PrintDocumentInfo.CONTENT_TYPE_DOCUMENT).build()
            callback.onLayoutFinished(info, true)
            events.stage(5)
        }

        override fun onWrite(
            pages: Array<out PageRange>?,
            destination: ParcelFileDescriptor,
            cancellationSignal: CancellationSignal?,
            callback: WriteResultCallback,
        ) {
            written = false
            writeFailed = false
            events.stage(6)
            try {
                FileInputStream(file).use { input ->
                    FileOutputStream(destination.fileDescriptor).use { output -> input.copyTo(output) }
                }
                if (cancellationSignal?.isCanceled == true) {
                    callback.onWriteCancelled()
                    return
                }
                written = true
                callback.onWriteFinished(arrayOf(PageRange.ALL_PAGES))
                events.stage(7)
            } catch (_: Exception) {
                writeFailed = true
                events.stage(9)
                callback.onWriteFailed(null)
            }
        }

        override fun onFinish() {
            events.stage(8)
            done(written, writeFailed)
        }
    }
}
