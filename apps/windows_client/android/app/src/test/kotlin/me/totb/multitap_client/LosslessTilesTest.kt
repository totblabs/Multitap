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
    /** One compositor of the fixture: "frame" (unscaled) or "scaled" (Configure(..., true)) over [width] x [height] video. */
    private class Section(val width: Int, val height: Int, val scaled: Boolean, val steps: List<List<String>>)
    private class Vectors(val sections: List<Section>) {
        // The first, unscaled section: 144 x 80.
        val width get() = sections.first().width
        val height get() = sections.first().height
        val steps get() = sections.first().steps
    }

    private fun vectors(): Vectors {
        val text = javaClass.classLoader!!.getResourceAsStream("lossless_tiles_vectors.txt")!!.bufferedReader().readText()
        val lines = text.lines().filter { it.isNotBlank() && !it.startsWith("#") }.map { it.trim().split(' ') }
        val sections = ArrayList<Section>()
        for (line in lines) {
            if (line[0] == "frame" || line[0] == "scaled") sections.add(Section(line[1].toInt(), line[2].toInt(), line[0] == "scaled", ArrayList()))
            else (sections.last().steps as ArrayList).add(line)
        }
        return Vectors(sections)
    }

    private fun hex(text: String) = ByteArray(text.length / 2) { text.substring(it * 2, it * 2 + 2).toInt(16).toByte() }

    /** The C++ test's CPU picture: the tiles the sink got (source coordinates), over flat video where present. */
    private class Layer(val width: Int, val height: Int) : TileSink {
        val rgba = ByteArray(width * height * 4)
        override fun tile(index: Int, x: Int, y: Int, width: Int, height: Int, rgba: ByteArray) {
            for (row in 0 until height) System.arraycopy(rgba, row * width * 4, this.rgba, ((y + row) * this.width + x) * 4, width * 4)
        }
        /**
         * The published frame: the source size once tiles describe one (a
         * scaled source: flat video upscales to itself), else the video's.
         */
        fun compose(compositor: LosslessCompositor): ByteArray {
            val w = if (compositor.count > 0) compositor.width else compositor.videoWidth
            val h = if (compositor.count > 0) compositor.height else compositor.videoHeight
            val frame = ByteArray(w * h * 4)
            for (i in frame.indices step 4) { frame[i] = 0x10; frame[i + 1] = 0x20; frame[i + 2] = 0x30; frame[i + 3] = -1 }
            val columns = LosslessTiles.columns(w)
            for (y in 0 until h) for (x in 0 until w) {
                if (!compositor.present((y / 64) * columns + x / 64)) continue
                System.arraycopy(rgba, (y * width + x) * 4, frame, (y * w + x) * 4, 4)
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
        assertEquals(listOf("frame 144 80", "scaled 128 72", "scaled 144 80", "scaled 160 96"),
            vectors.sections.map { "${if (it.scaled) "scaled" else "frame"} ${it.width} ${it.height}" })
        var states = 0
        val poisoned = ArrayList<Boolean>()
        val scaled = ArrayList<Boolean>()
        for (section in vectors.sections) {
            val compositor = LosslessCompositor(section.width, section.height, allowScaledSource = section.scaled)
            // Every packet of the fixture carries 144 x 80 tiles.
            val layer = Layer(144, 80)
            for (step in section.steps) when (step[0]) {
                "packet" -> {
                    val payload = hex(step[1])
                    val expected = if (step[2] == "1") LosslessCompositor.Outcome.ACCEPTED else LosslessCompositor.Outcome.IGNORED
                    assertEquals("${section.width}x${section.height}", expected, compositor.accept(payload, 0, payload.size, layer))
                }
                "state" -> {
                    compositor.applyDecoded(step[1].toLong())
                    val present = if (compositor.count == 0) "-" else (0 until compositor.count).joinToString("") { if (compositor.present(it)) "1" else "0" }
                    val at = "${section.width}x${section.height} at pts ${step[1]}"
                    assertEquals("present tiles $at", step[2], present)
                    assertEquals("composed picture $at", step[3], fnv(layer.compose(compositor)))
                    states++
                }
            }
            poisoned.add(compositor.poisoned)
            scaled.add(compositor.scaled)
        }
        assertEquals(6 + 3 * 6, states)
        assertEquals("every sequence gap (and the larger video) poisoned", listOf(true, true, true, true), poisoned)
        assertEquals("only tiles over a smaller video are scaled", listOf(false, true, false, false), scaled)
    }

    // tiles-v2: a move, its rectangles and whatever arrives behind them apply on
    // their frame, in order, and the sink gets the tiles they changed
    // (lossless_moves_vectors.txt, the same generator with "moves").
    private fun moveSteps(): List<List<String>> {
        val text = javaClass.classLoader!!.getResourceAsStream("lossless_moves_vectors.txt")!!.bufferedReader().readText()
        val lines = text.lines().filter { it.isNotBlank() && !it.startsWith("#") }.map { it.trim().split(' ') }
        assertEquals(listOf("frame", "144", "80"), lines.first())
        return lines.drop(1)
    }

    @Test
    fun movesAndRectanglesWaitForTheirFrameLikeTheSharedCpp() {
        val compositor = LosslessCompositor(144, 80, moves = true)
        val layer = Layer(144, 80)
        var states = 0
        val waiting = ArrayList<Boolean>()
        for (step in moveSteps()) when (step[0]) {
            "packet" -> {
                val payload = hex(step[1])
                assertEquals(if (step[2] == "1") LosslessCompositor.Outcome.ACCEPTED else LosslessCompositor.Outcome.IGNORED, compositor.accept(payload, 0, payload.size, layer))
                waiting.add(compositor.waiting)
            }
            "state" -> {
                compositor.applyDecoded(step[1].toLong(), layer)
                val present = (0 until compositor.count).joinToString("") { if (compositor.present(it)) "1" else "0" }
                assertEquals("present tiles at pts ${step[1]}", step[2], present)
                assertEquals("composed picture at pts ${step[1]}", step[3], fnv(layer.compose(compositor)))
                states++
            }
        }
        assertEquals(9, states)
        // The plain first and third packets applied at once; the others waited.
        assertEquals(listOf(false, true, false, true, true, true), waiting)
        assertEquals(2L, compositor.movesApplied)
        assertTrue("a move over a tile the client lacks ends the tiles", compositor.poisoned)
        assertFalse(compositor.waiting)
    }

    @Test
    fun aDeviceOnWhichMovesAreSlowStopsAskingForThem() {
        // A quarter of a window of frames with moves took too long: too slow here.
        assertFalse(AndroidMedia.slowMoves(AndroidMedia.MOVE_WINDOW - 1, AndroidMedia.MOVE_WINDOW - 1))
        assertFalse(AndroidMedia.slowMoves(AndroidMedia.MOVE_WINDOW, AndroidMedia.MOVE_WINDOW / 4 - 1))
        assertTrue(AndroidMedia.slowMoves(AndroidMedia.MOVE_WINDOW, AndroidMedia.MOVE_WINDOW / 4))
        assertEquals(6_000_000L, AndroidMedia.MOVE_SLOW_NANOS)
    }

    @Test
    fun anEngineThatDidNotAskForMovesRefusesThem() {
        val compositor = LosslessCompositor(144, 80)
        val layer = Layer(144, 80)
        val packets = moveSteps().filter { it[0] == "packet" }.map { hex(it[1]) }
        assertEquals(LosslessCompositor.Outcome.ACCEPTED, compositor.accept(packets[0], 0, packets[0].size, layer))
        assertEquals(LosslessCompositor.Outcome.IGNORED, compositor.accept(packets[1], 0, packets[1].size, layer))
        assertTrue(compositor.poisoned)
        assertEquals(0, compositor.presentTiles)
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
    fun aScaledSourceNeedsTheScaledOptIn() {
        // The first packet declares 144 x 80 tiles over 128 x 72 video: a size
        // mismatch for a compositor of an output of tracks-v1.
        val payload = hex(vectors().steps.first { it[0] == "packet" }[1])
        val compositor = LosslessCompositor(128, 72)
        assertEquals(128, compositor.width)
        assertEquals(LosslessCompositor.Outcome.IGNORED, compositor.accept(payload, 0, payload.size) { _, _, _, _, _, _ -> })
        assertTrue(compositor.poisoned)
        assertFalse(compositor.scaled)
    }

    @Test
    fun aScaledSourceTakesItsSizeFromTheFirstPacketAndPlacesTilesInSourceCoordinates() {
        val packets = vectors().steps.filter { it[0] == "packet" }.map { hex(it[1]) }
        val compositor = LosslessCompositor(128, 72, allowScaledSource = true)
        assertEquals("no size before the first packet", 0, compositor.count)
        assertFalse(compositor.scaled)
        val placed = ArrayList<List<Int>>()
        assertEquals(LosslessCompositor.Outcome.ACCEPTED,
            compositor.accept(packets[0], 0, packets[0].size) { index, x, y, w, h, _ -> placed.add(listOf(index, x, y, w, h)) })
        assertTrue(compositor.scaled)
        assertEquals(144, compositor.width); assertEquals(80, compositor.height); assertEquals(6, compositor.count)
        assertEquals(128, compositor.videoWidth); assertEquals(72, compositor.videoHeight)
        // Tile 2 is the partial right column of the 144-wide source, tile 5 its corner.
        assertEquals(listOf(2, 128, 0, 16, 64), placed[2])
        assertEquals(listOf(5, 128, 64, 16, 16), placed[5])
        assertEquals(6, compositor.presentTiles)
        assertTrue(compositor.republishPending)
        // Drops are due by the shown frame's pts exactly as unscaled (ApplyDecodedScaled).
        assertEquals(LosslessCompositor.Outcome.ACCEPTED, compositor.accept(packets[1], 0, packets[1].size) { _, _, _, _, _, _ -> })
        compositor.applyDecoded(1999)
        assertEquals(6, compositor.presentTiles)
        compositor.applyDecoded(2000)
        assertEquals(4, compositor.presentTiles)
        assertFalse(compositor.republishPending)
    }

    @Test
    fun aScaledSourceKeepsItsFirstSizeAndRefusesOneSmallerThanTheVideo() {
        val packets = vectors().steps.filter { it[0] == "packet" }.map { hex(it[1]) }
        // A later packet of another source size: poisoned, the mask cleared.
        val resized = LosslessCompositor(128, 72, allowScaledSource = true)
        resized.accept(packets[0], 0, packets[0].size) { _, _, _, _, _, _ -> }
        val other = packets[1].copyOf().also { it[4] = 0x92.toByte() } // 146 wide
        assertEquals(LosslessCompositor.Outcome.IGNORED, resized.accept(other, 0, other.size) { _, _, _, _, _, _ -> })
        assertTrue(resized.poisoned)
        assertEquals(0, resized.presentTiles)
        assertTrue(resized.presentMask.all { it.toInt() == 0 })
        // Narrower than the video in one dimension, odd, or under the tiles-v1 minimum: refused before any size is taken.
        for ((label, video, edit) in listOf<Triple<String, Pair<Int, Int>, (ByteArray) -> Unit>>(
            Triple("narrower", 146 to 72, { _ -> }),
            Triple("shorter", 128 to 82, { _ -> }),
            Triple("odd", 128 to 72, { it[4] = 0x91.toByte() }),
        )) {
            val compositor = LosslessCompositor(video.first, video.second, allowScaledSource = true)
            val bytes = packets[0].copyOf().also(edit)
            var sunk = 0
            assertEquals(label, LosslessCompositor.Outcome.IGNORED, compositor.accept(bytes, 0, bytes.size) { _, _, _, _, _, _ -> sunk++ })
            assertTrue(label, compositor.poisoned)
            assertEquals(label, 0, compositor.count)
            assertEquals(label, 0, sunk)
        }
        // An invalid video size accepts nothing, scaled or not.
        assertEquals(LosslessCompositor.Outcome.IGNORED,
            LosslessCompositor(15, 15, allowScaledSource = true).accept(packets[0], 0, packets[0].size) { _, _, _, _, _, _ -> })
    }

    @Test
    fun aSinkThatCannotPlaceATilePoisons() {
        // AndroidMedia's renderer check: a tile of a source it does not draw throws in the sink.
        val payload = hex(vectors().steps.first { it[0] == "packet" }[1])
        val compositor = LosslessCompositor(128, 72, allowScaledSource = true)
        assertEquals(LosslessCompositor.Outcome.IGNORED,
            compositor.accept(payload, 0, payload.size) { _, _, _, _, _, _ -> throw IllegalStateException("tile source size") })
        assertTrue(compositor.poisoned)
        assertEquals(0, compositor.presentTiles)
        assertFalse(compositor.republishPending)
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
