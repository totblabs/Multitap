package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.security.MessageDigest

// A VM copy as lib/data/native_clipboard.dart NativeClipboardBridge hands it to
// AndroidClipboard on `multitap/clipboard`: begin {format, items}, then append and
// finish per item, then commit.
class ClipboardReceiveTest {
    private fun sha(bytes: ByteArray) = ClipboardReceive.hex(MessageDigest.getInstance("SHA-256").digest(bytes))
    private fun item(size: Int, kind: String? = null): Map<String, Any?> =
        hashMapOf<String, Any?>("size" to size).apply { if (kind != null) put("kind", kind) }
    // AndroidClipboard answers any of these exceptions with clipboard_transfer_failed.
    private fun fails(what: String, block: () -> Unit) {
        try { block() } catch (_: Exception) { return }
        fail("accepted $what")
    }
    private fun deliver(receive: ClipboardReceive, parts: List<ByteArray>, chunk: Int) {
        for ((index, part) in parts.withIndex()) {
            var offset = 0
            while (offset < part.size) {
                val count = minOf(chunk, part.size - offset)
                receive.append(index, offset, part.copyOfRange(offset, offset + count))
                offset += count
            }
            receive.finish(index, sha(part))
        }
    }

    @Test fun textInChunksOfEitherProfile() {
        val text = "가".repeat(40000) // 120000 UTF-8 bytes: 46080-byte chunks (formats profile)
        val bytes = text.toByteArray()
        for (chunk in listOf(32768, 46080)) {
            val receive = ClipboardReceive.begin("text", listOf(item(bytes.size)))!!
            assertFalse(receive.complete)
            deliver(receive, listOf(bytes), chunk)
            assertTrue(receive.complete)
            assertEquals(text, receive.text())
        }
    }

    @Test fun richTextKeepsItsPlainText() {
        val parts = listOf("plain", "<b>plain</b>", "{\\rtf1 plain}").map { it.toByteArray() }
        val receive = ClipboardReceive.begin("richtext", listOf(item(parts[0].size, "text"), item(parts[1].size, "html"), item(parts[2].size, "rtf")))!!
        deliver(receive, parts, 4)
        assertEquals("plain", receive.text())
        assertNotNullBegin("richtext", listOf(item(1, "text"), item(1, "rtf")))
        assertNotNullBegin("richtext", listOf(item(0, "text")))
    }

    private fun assertNotNullBegin(format: String, items: List<Any?>) = assertTrue(ClipboardReceive.begin(format, items) != null)

    @Test fun filesFoldersAndImagesAreRefusedBeforeAByte() {
        for (format in listOf("files", "tree", "image", "other", null)) {
            assertNull(format, ClipboardReceive.begin(format, listOf(item(10, "png"))))
        }
    }

    @Test fun malformedManifests() {
        fails("no items") { ClipboardReceive.begin("text", emptyList<Any>()) }
        fails("null items") { ClipboardReceive.begin("text", null) }
        fails("two texts") { ClipboardReceive.begin("text", listOf(item(1), item(1))) }
        fails("a named text") { ClipboardReceive.begin("text", listOf(hashMapOf("size" to 1, "name" to "a.txt"))) }
        fails("a negative size") { ClipboardReceive.begin("text", listOf(item(-1))) }
        fails("no size") { ClipboardReceive.begin("text", listOf(hashMapOf<String, Any>())) }
        fails("HTML first") { ClipboardReceive.begin("richtext", listOf(item(1, "html"), item(1, "text"))) }
        fails("RTF before HTML") { ClipboardReceive.begin("richtext", listOf(item(1, "text"), item(1, "rtf"), item(1, "html"))) }
        fails("an unknown kind") { ClipboardReceive.begin("richtext", listOf(item(1, "text"), item(1, "png"))) }
        fails("four items") { ClipboardReceive.begin("richtext", listOf(item(1, "text"), item(1, "html"), item(1, "rtf"), item(1, "rtf"))) }
    }

    @Test fun tooMuchTextIsItsOwnRefusal() {
        assertTrue(ClipboardReceive.begin("text", listOf(item(ClipboardReceive.MAX_TEXT_BYTES))) != null)
        try { ClipboardReceive.begin("text", listOf(item(ClipboardReceive.MAX_TEXT_BYTES + 1))); fail() }
        catch (_: ClipboardTextTooLarge) {}
        try { ClipboardReceive.begin("richtext", listOf(item(ClipboardReceive.MAX_TEXT_BYTES + 1, "text"), item(1, "html"))); fail() }
        catch (_: ClipboardTextTooLarge) {}
        // A large HTML part is checked and dropped, never kept.
        assertTrue(ClipboardReceive.begin("richtext", listOf(item(5, "text"), item(8 * 1024 * 1024, "html"))) != null)
    }

    @Test fun outOfOrderOversizedOrCorruptDataIsRefused() {
        val bytes = "hello".toByteArray()
        fun fresh() = ClipboardReceive.begin("text", listOf(item(bytes.size)))!!
        fails("a wrong offset") { fresh().append(0, 1, bytes) }
        fails("a wrong item") { fresh().append(1, 0, bytes) }
        fails("more than announced") { fresh().append(0, 0, "hello!".toByteArray()) }
        fails("an empty chunk") { fresh().append(0, 0, ByteArray(0)) }
        fails("a chunk above 64 KiB") {
            ClipboardReceive.begin("text", listOf(item(70000)))!!.append(0, 0, ByteArray(ClipboardReceive.MAX_CHUNK + 1))
        }
        fails("finish before the end") { fresh().apply { append(0, 0, bytes.copyOf(2)) }.finish(0, sha(bytes)) }
        fails("a wrong digest") { fresh().apply { append(0, 0, bytes) }.finish(0, sha("other".toByteArray())) }
        fails("text before every item") {
            ClipboardReceive.begin("richtext", listOf(item(1, "text"), item(1, "html")))!!.apply {
                append(0, 0, "a".toByteArray()); finish(0, sha("a".toByteArray()))
            }.text()
        }
        val invalid = byteArrayOf(0xC3.toByte(), 0x28)
        fails("invalid UTF-8") { ClipboardReceive.begin("text", listOf(item(2)))!!.apply { deliver(this, listOf(invalid), 2) }.text() }
        val nul = "a\u0000b".toByteArray()
        fails("a NUL") { ClipboardReceive.begin("text", listOf(item(nul.size)))!!.apply { deliver(this, listOf(nul), 8) }.text() }
        fails("data after the end") {
            fresh().apply { deliver(this, listOf(bytes), 8) }.append(1, 0, bytes)
        }
    }
}
