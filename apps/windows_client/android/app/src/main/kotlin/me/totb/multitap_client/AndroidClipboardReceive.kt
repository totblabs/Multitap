package me.totb.multitap_client

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.security.MessageDigest

/** More text than this phone takes: clipboard_limits_exceeded, detail `text`. */
class ClipboardTextTooLarge : IllegalArgumentException()

/**
 * A copy from the VM as AndroidClipboard receives it (clipboard-v1 begin, then
 * append and finish per item, then commit). No Android types: the JVM tests run it.
 *
 * Plain text is kept. Rich text (the formats profile: plain text, then HTML and
 * RTF) keeps its plain text; HTML and RTF are checked against their digests and
 * dropped. Files, folders and images are refused at begin ([begin] returns null),
 * before a byte of them arrives.
 */
class ClipboardReceive private constructor(private val sizes: LongArray) {
    private val kept = ByteArrayOutputStream(sizes[0].toInt())
    private val hash = MessageDigest.getInstance("SHA-256")
    private var index = 0
    private var offset = 0L
    val complete get() = index == sizes.size

    fun append(index: Int?, offset: Number?, bytes: ByteArray?) {
        require(!complete && index == this.index && offset?.toLong() == this.offset && bytes != null &&
            bytes.size in 1..MAX_CHUNK && bytes.size <= sizes[this.index] - this.offset)
        hash.update(bytes)
        if (index == 0) kept.write(bytes)
        this.offset += bytes.size
    }

    fun finish(index: Int?, sha256: String?) {
        require(!complete && index == this.index && offset == sizes[this.index] && hex(hash.digest()) == sha256)
        this.index++
        offset = 0
    }

    /** The plain text, once every item arrived and matched its digest. */
    fun text(): String {
        require(complete)
        val text = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(kept.toByteArray())).toString()
        require(!text.contains('\u0000'))
        return text
    }

    companion object {
        /** A chunk of either profile: 32768 bytes, or 46080 with the formats profile. */
        const val MAX_CHUNK = 65536

        /** UTF-8 bytes of text, either way: a clip crosses in one binder transaction (1 MiB). */
        const val MAX_TEXT_BYTES = 131072
        private val richOrder = listOf("text", "html", "rtf")

        /** null: a format this phone does not take. Throws for a malformed manifest. */
        fun begin(format: String?, items: List<*>?): ClipboardReceive? {
            require(items != null && items.isNotEmpty() && items.all { it is Map<*, *> })
            val parts = items.map { it as Map<*, *> }
            when (format) {
                "text" -> require(parts.size == 1 && parts[0]["name"] == null && parts[0]["kind"] == null)
                // Plain text first, then HTML, then RTF, each at most once.
                "richtext" -> require(parts.size <= 3 && parts.map { richOrder.indexOf(it["kind"]) }
                    .let { ranks -> ranks[0] == 0 && ranks.zipWithNext().all { (a, b) -> b > a } })
                else -> return null
            }
            val sizes = LongArray(parts.size) { (parts[it]["size"] as? Number)?.toLong() ?: -1L }
            require(sizes.all { it >= 0 })
            if (sizes[0] > MAX_TEXT_BYTES) throw ClipboardTextTooLarge()
            return ClipboardReceive(sizes)
        }

        fun hex(bytes: ByteArray) = bytes.joinToString("") { "%02x".format(it.toInt() and 255) }
    }
}
