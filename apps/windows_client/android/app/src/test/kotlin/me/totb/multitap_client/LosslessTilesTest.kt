package me.totb.multitap_client

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

// tiles-v1 on Android (LosslessTiles.kt) against the shared C++: the payloads in
// lossless_tiles_vectors.txt were encoded by the Host's PayloadBuilder and the
// states were composed by the Windows client's Compositor
// (apps/windows_client/test/native/lossless_tiles_vectors.cpp).
class LosslessTilesTest {
    private class Vectors(val width: Int, val height: Int, val steps: List<List<String>>)

    private fun vectors(): Vectors {
        val text = javaClass.classLoader!!.getResourceAsStream("lossless_tiles_vectors.txt")!!.bufferedReader().readText()
        val lines = text.lines().filter { it.isNotBlank() && !it.startsWith("#") }.map { it.trim().split(' ') }
        val frame = lines.first { it[0] == "frame" }
        return Vectors(frame[1].toInt(), frame[2].toInt(), lines.filter { it[0] != "frame" })
    }

    private fun hex(text: String) = ByteArray(text.length / 2) { text.substring(it * 2, it * 2 + 2).toInt(16).toByte() }

    /** The C++ test's CPU picture: the tiles the sink got, over flat video where present. */
    private class Layer(val width: Int, val height: Int) : TileSink {
        val rgba = ByteArray(width * height * 4)
        override fun tile(index: Int, x: Int, y: Int, width: Int, height: Int, rgba: ByteArray) {
            for (row in 0 until height) System.arraycopy(rgba, row * width * 4, this.rgba, ((y + row) * this.width + x) * 4, width * 4)
        }
        fun compose(compositor: LosslessCompositor): ByteArray {
            val frame = ByteArray(width * height * 4)
            for (i in frame.indices step 4) { frame[i] = 0x10; frame[i + 1] = 0x20; frame[i + 2] = 0x30; frame[i + 3] = -1 }
            val columns = LosslessTiles.columns(width)
            for (y in 0 until height) for (x in 0 until width) {
                if (!compositor.present((y / 64) * columns + x / 64)) continue
                System.arraycopy(rgba, (y * width + x) * 4, frame, (y * width + x) * 4, 4)
            }
            return frame
        }
    }

    private fun fnv(bytes: ByteArray): String {
        var hash = 1469598103934665603uL
        for (byte in bytes) { hash = hash xor (byte.toInt() and 255).toULong(); hash *= 1099511628211uL }
        return hash.toString(16).padStart(16, '0')
    }

    @Test
    fun theAndroidPortReachesTheSameTilesAndPixelsAsTheSharedCpp() {
        val vectors = vectors()
        val compositor = LosslessCompositor(vectors.width, vectors.height)
        val layer = Layer(vectors.width, vectors.height)
        var states = 0
        for (step in vectors.steps) when (step[0]) {
            "packet" -> {
                val payload = hex(step[1])
                val expected = if (step[2] == "1") LosslessCompositor.Outcome.ACCEPTED else LosslessCompositor.Outcome.IGNORED
                assertEquals(expected, compositor.accept(payload, 0, payload.size, layer))
            }
            "state" -> {
                compositor.applyDecoded(step[1].toLong())
                val present = (0 until compositor.count).joinToString("") { if (compositor.present(it)) "1" else "0" }
                assertEquals("present tiles at pts ${step[1]}", step[2], present)
                assertEquals("composed picture at pts ${step[1]}", step[3], fnv(layer.compose(compositor)))
                states++
            }
        }
        assertEquals(6, states)
        assertTrue("the sequence gap poisoned", compositor.poisoned)
    }

    @Test
    fun aTilePacketInsideAMediaPacketIsReadAtItsOffset() {
        val payload = hex(vectors().steps.first { it[0] == "packet" }[1])
        val packet = ByteArray(12) + payload
        assertTrue(LosslessTiles.isTilePayload(packet, 12, payload.size))
        assertFalse("H.264 starts with a zero byte", LosslessTiles.isTilePayload(byteArrayOf(0, 0, 0, 1, 0x65), 0, 5))
        val compositor = LosslessCompositor(144, 80)
        val added = ArrayList<Int>()
        assertEquals(LosslessCompositor.Outcome.ACCEPTED, compositor.accept(packet, 12, payload.size) { index, _, _, _, _, _ -> added.add(index) })
        assertEquals(listOf(0, 1, 2, 3, 4, 5), added)
        assertTrue(compositor.republishPending)
        compositor.clearAdded()
        assertFalse(compositor.republishPending)
        assertEquals(6, compositor.presentTiles)
    }

    @Test
    fun aDamagedPacketPoisonsAndNoTileIsShownAgain() {
        val payload = hex(vectors().steps.first { it[0] == "packet" }[1])
        for (damage in listOf("truncated", "longer", "size", "codec", "index")) {
            val compositor = LosslessCompositor(144, 80)
            val bytes = when (damage) {
                "truncated" -> payload.copyOf(payload.size - 1)
                "longer" -> payload + byteArrayOf(0)
                "size" -> payload.copyOf().also { it[4] = 0x92.toByte() } // 146 wide
                "codec" -> payload.copyOf().also { it[LosslessTiles.HEADER + 2] = 2 }
                else -> payload.copyOf().also { it[LosslessTiles.HEADER] = 6 } // tile 6 of 6
            }
            assertEquals(damage, LosslessCompositor.Outcome.IGNORED, compositor.accept(bytes, 0, bytes.size) { _, _, _, _, _, _ -> })
            assertTrue(damage, compositor.poisoned)
            assertEquals(damage, 0, compositor.presentTiles)
            assertFalse(damage, compositor.republishPending)
            // Even a good packet is ignored from now on.
            assertEquals(damage, LosslessCompositor.Outcome.IGNORED, compositor.accept(payload, 0, payload.size) { _, _, _, _, _, _ -> })
            assertTrue(damage, compositor.presentMask.all { it.toInt() == 0 })
        }
    }

    @Test
    fun aScaledSourceIsNotCompositedOnAndroid() {
        // The first packet declares 144 x 80 tiles over 128 x 72 video.
        val payload = hex(vectors().steps.first { it[0] == "packet" }[1])
        val compositor = LosslessCompositor(128, 72)
        assertEquals(LosslessCompositor.Outcome.IGNORED, compositor.accept(payload, 0, payload.size) { _, _, _, _, _, _ -> })
        assertTrue(compositor.poisoned)
    }

    @Test
    fun dropsCompareTheirTimestampsUnsigned() {
        val vectors = vectors()
        val compositor = LosslessCompositor(vectors.width, vectors.height)
        val packets = vectors.steps.filter { it[0] == "packet" }.map { hex(it[1]) }
        compositor.accept(packets[0], 0, packets[0].size) { _, _, _, _, _, _ -> }
        compositor.accept(packets[1], 0, packets[1].size) { _, _, _, _, _, _ -> }
        val version = compositor.maskVersion
        compositor.applyDecoded(1999)
        assertEquals("nothing due yet", version, compositor.maskVersion)
        compositor.applyDecoded(Long.MAX_VALUE)
        assertFalse(compositor.present(1))
        assertFalse(compositor.present(4))
        assertTrue(compositor.maskVersion > version)
        assertEquals(4, compositor.presentTiles)
    }

    @Test
    fun tq1RejectsInputThatWouldReadAboveTheFirstRowOrOverrun() {
        val out = ByteArray(4 * 4 * 4)
        assertFalse("copy-from-above on row 0", LosslessTiles.decodeTq1(byteArrayOf(0xff.toByte(), 0), 0, 2, 4, 4, out, 0, 16))
        assertFalse("run past the tile", LosslessTiles.decodeTq1(byteArrayOf(0xfd.toByte()) /* a run of 62 */, 0, 1, 4, 4, out, 0, 16))
        // A 2x1 tile: one RGB pixel, then a run of one.
        val two = ByteArray(8)
        assertTrue(LosslessTiles.decodeTq1(byteArrayOf(0xfe.toByte(), 1, 2, 3, 0xc0.toByte()), 0, 5, 2, 1, two, 0, 8))
        assertArrayEquals(byteArrayOf(1, 2, 3, -1, 1, 2, 3, -1), two)
    }

    @Test
    fun validFramesFollowTheWireLimits() {
        assertTrue(LosslessTiles.validFrame(1920, 1080))
        assertFalse("odd", LosslessTiles.validFrame(1921, 1080))
        assertFalse("tiny", LosslessTiles.validFrame(14, 1080))
        assertFalse("u16", LosslessTiles.validFrame(65536, 16))
        assertEquals(0, LosslessCompositor(15, 15).count)
    }
}
