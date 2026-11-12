package me.totb.multitap_client

import android.content.ClipData
import android.content.ClipDescription
import android.content.ClipboardManager
import android.content.Context
import android.os.PersistableBundle
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.security.MessageDigest
import java.security.SecureRandom

/**
 * Foreground-only text clipboard, shared automatically (clipboard-sync-v1) like
 * the PC's Ctrl+C/Ctrl+V. Files, folders and images are refused before any write.
 *
 * Automatic sharing needs a change sequence. Android lets only the app in front
 * read the clipboard, so this one is foreground-only: it advances when the clip
 * changes while Multitap is in front and, on return to the app, when it changed
 * meanwhile. Only the clip's description is looked at for that (no content, so
 * no Android "pasted from clipboard" notice); content is read by `capture` alone.
 * A clip this app placed itself carries a mark in its description and is never
 * sent back (no echo), even when it was placed while another app had the focus.
 *
 * A VM copy arrives through ClipboardReceive: text, or the plain text of rich text.
 * The on-paste pair's offers (files, large copies) are refused: this side keeps
 * none, so withdrawing or closing them is nothing to do.
 */
class AndroidClipboard(context: Context, engine: FlutterEngine) {
    private val text = context.applicationContext
    private val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
    private val channel = MethodChannel(engine.dartExecutor.binaryMessenger, "multitap/clipboard")
    private var owner: String? = null
    private var source: ByteArray? = null
    private var request: String? = null
    private var receiving: ClipboardReceive? = null
    private var sequence = 1L
    private var seenStamp = Long.MIN_VALUE
    private var ownMark = 0L
    private val random = SecureRandom()
    private var watching: String? = null
    private var focused = true
    private val listener = ClipboardManager.OnPrimaryClipChangedListener { observe() }

    init {
        channel.setMethodCallHandler(::call)
    }

    /** The activity ends (or gets another engine): no listener outlives it. */
    fun dispose() {
        stopWatching()
        channel.setMethodCallHandler(null)
    }

    /** The activity's window focus: a return to the app looks for a change. */
    fun focusChanged(hasFocus: Boolean) {
        focused = hasFocus
        if (hasFocus && watching != null) observe()
    }

    private fun description(): ClipDescription? = try { clipboard.primaryClipDescription } catch (_: Exception) { null }
    private fun own(description: ClipDescription?) = ownMark != 0L && description?.extras?.getLong(OWN_MARK, 0L) == ownMark
    // A clip its app marked sensitive (a password manager's copy; Android 13's
    // ClipDescription.EXTRA_IS_SENSITIVE, which apps set on older versions too)
    // is never shared automatically. Sending it
    // on purpose (보내기) still works.
    private fun sensitive(description: ClipDescription?) =
        try { description?.extras?.getBoolean(ClipDescription.EXTRA_IS_SENSITIVE, false) == true } catch (_: Exception) { false }

    private fun observe() {
        if (!focused) return
        val description = try { clipboard.primaryClipDescription } catch (_: Exception) { return }
        val now = description?.timestamp ?: 0L
        if (now == seenStamp) return
        seenStamp = now
        // A received copy this app placed (its own change is already counted).
        if (own(description)) return
        sequence = (sequence + 1) and 0xffffffffL
        val current = watching ?: return
        channel.invokeMethod("changed", mapOf("owner" to current, "sequence" to sequence))
    }

    private fun stopWatching() {
        if (watching == null) return
        watching = null
        try { clipboard.removePrimaryClipChangedListener(listener) } catch (_: Exception) {}
    }

    private fun call(call: MethodCall, result: MethodChannel.Result) {
        try {
            val next = call.argument<String>("owner") ?: throw IllegalArgumentException()
            require(next.length in 1..100)
            when (call.method) {
                "watch" -> {
                    if (call.argument<Boolean>("enabled") == true) {
                        if (watching == null) clipboard.addPrimaryClipChangedListener(listener)
                        watching = next
                        if (seenStamp == Long.MIN_VALUE) seenStamp = description()?.timestamp ?: 0L else observe()
                    } else if (watching == next) stopWatching()
                    result.success(mapOf("sequence" to sequence))
                }
                "capture" -> {
                    require(owner == null || owner == next)
                    // Automatic sharing sends text only: an empty clipboard, an
                    // image, this app's own clip or a sensitive one is nothing
                    // to share (not a failure to report at every session start).
                    val automatic = call.argument<Boolean>("skipOwn") == true
                    if (automatic && description().let { own(it) || sensitive(it) }) return result.success(mapOf("own" to true, "sequence" to sequence))
                    val clip = clipboard.primaryClip
                    // The clip read is the one checked (it may have changed since).
                    if (automatic && clip != null && sensitive(clip.description)) return result.success(mapOf("own" to true, "sequence" to sequence))
                    val item = clip?.takeIf { it.itemCount == 1 }?.getItemAt(0)
                        ?: return if (automatic) result.success(mapOf("own" to true, "sequence" to sequence)) else result.error("clipboard_empty", null, null)
                    // Never resolve content URIs or HTML into text behind the user's back.
                    val text = if (item.uri != null || item.intent != null) null else item.text?.toString()
                    if (text == null) {
                        return if (automatic) result.success(mapOf("own" to true, "sequence" to sequence)) else result.error("unsupported_clipboard_format", null, null)
                    }
                    require(!text.contains('\u0000'))
                    val bytes = text.toByteArray(Charsets.UTF_8)
                    if (bytes.size > ClipboardReceive.MAX_TEXT_BYTES) return result.error("clipboard_limits_exceeded", "text", null)
                    owner = next; source = bytes
                    result.success(mapOf("format" to "text", "items" to listOf(mapOf("size" to bytes.size, "units" to text.length)), "sequence" to sequence))
                }
                "sourceChunk" -> {
                    require(owner == next && call.argument<Int>("index") == 0)
                    val bytes = source ?: throw IllegalStateException()
                    val offset = call.argument<Int>("offset") ?: -1
                    val count = call.argument<Int>("length") ?: -1
                    require(offset >= 0 && count in 1..ClipboardReceive.MAX_CHUNK && offset <= bytes.size - count)
                    result.success(bytes.copyOfRange(offset, offset + count))
                }
                "sourceDigest" -> { require(owner == next && call.argument<Int>("index") == 0); result.success(digest(source!!)) }
                "begin" -> {
                    val receive = try { ClipboardReceive.begin(call.argument<String>("format"), call.argument<List<*>>("items")) }
                        catch (_: ClipboardTextTooLarge) { return result.error("clipboard_limits_exceeded", "text", null) }
                        ?: return result.error("unsupported_clipboard_format", null, null)
                    require(owner == null || owner == next)
                    owner = next; request = call.argument<String>("requestId"); receiving = receive
                    result.success(null)
                }
                "append" -> {
                    checkTransfer(call, next)
                    receiving!!.append(call.argument<Int>("index"), call.argument<Number>("offset"), call.argument<ByteArray>("bytes"))
                    result.success(null)
                }
                "finish" -> {
                    checkTransfer(call, next)
                    receiving!!.finish(call.argument<Int>("index"), call.argument<String>("digest"))
                    result.success(null)
                }
                "commit" -> {
                    checkTransfer(call, next)
                    val text = receiving!!.text()
                    // The bytes this device sent from an unchanged clipboard are
                    // already on it: nothing to replace.
                    val unless = call.argument<Number>("unlessSequence")?.toLong()
                    if (unless == null || unless != sequence) {
                        val clip = ClipData.newPlainText("Multitap", text)
                        // Marked before it is placed: the change listener may run at once.
                        var mark: Long
                        do { mark = random.nextLong() } while (mark == 0L)
                        clip.description.extras = PersistableBundle().apply { putLong(OWN_MARK, mark) }
                        ownMark = mark
                        clipboard.setPrimaryClip(clip)
                        sequence = (sequence + 1) and 0xffffffffL
                    }
                    receiving = null; request = null
                    result.success(mapOf("sequence" to sequence))
                }
                "published" -> result.success(mapOf("owned" to own(description()), "sequence" to sequence))
                // Ends a transfer only; the watch stays with the session (it stops on watch false).
                "cancel", "release" -> {
                    if (owner == next) { source = null; receiving = null; request = null; owner = null }
                    result.success(null)
                }
                // The on-paste pair: this side publishes no offers, so there is
                // nothing to supersede, abort, retire, withdraw or close.
                "pullSupersede", "pullAbort", "pullRetire", "withdrawOffer", "pullShutdown", "pullClose", "closeOffers" -> result.success(null)
                "dropOffers" -> result.success(emptyList<String>())
                "publishOffer", "publishPendingOffer", "pullOpen" -> result.error("unsupported_clipboard_format", null, null)
                else -> result.notImplemented()
            }
        } catch (_: Exception) { result.error("clipboard_transfer_failed", MultitapText.get(text, R.string.clipboard_check_failed), null) }
    }
    private fun checkTransfer(call: MethodCall, next: String) {
        require(owner == next && request == call.argument<String>("requestId") && receiving != null)
    }
    private fun digest(bytes: ByteArray) = ClipboardReceive.hex(MessageDigest.getInstance("SHA-256").digest(bytes))

    private companion object {
        /** In the description of a clip this app placed: a random mark per received copy. */
        const val OWN_MARK = "me.totb.multitap_client.clip"
    }
}
