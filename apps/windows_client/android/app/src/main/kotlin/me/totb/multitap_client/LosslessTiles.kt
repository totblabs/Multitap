package me.totb.multitap_client

/**
 * ToTBSeamless lossless tiles (tiles-v1) on Android: the wire format, the TQ1
 * tile decoder and the client compositor's state, a line-for-line port of
 * native/windows_video/lossless_tiles.hpp (shared by the Host and the Windows
 * client; docs/lossless-tiles-design.md). The JVM tests decode payloads the
 * C++ encoder made (LosslessTilesTest), so both clients decode identically.
 *
 * A payload rides inside an ordinary non-key video packet, begins with
 * 'T''S''L''1' (H.264 Annex B begins with a zero byte) and is sent only after
 * this client opted in (its pings say `lt1-`). Little endian:
 *   0  'T''S''L''1'
 *   4  u16 width, u16 height     (source size; = the decoded size except in a scaled stream)
 *   8  u32 sequence              (1, 2, 3 ... per Host encoder)
 *   12 u64 dropPts               (drops apply to decoded video pts >= this)
 *   20 u16 dropCount, u16 addCount
 *   24 u16 drop indices, then adds: u16 index, u8 codec (1 = TQ1), u8 0, u32 size, size bytes
 * Tiles are 64x64, row-major, edge tiles partial.
 *
 * Android keeps the decoded pictures on the GPU (AndroidTileRenderer), so the
 * compositor here keeps no RGBA layer: every decoded add goes to a [TileSink]
 * (the renderer's tile texture) and [presentMask] says which tiles go over the
 * picture. A scaled source (tiles larger than the downscaled video) is drawn
 * as on Windows: the tiles at the source size over the video upscaled
 * bilinearly, by the GPU instead of rgba_upscale.hpp.
 */
internal object LosslessTiles {
    const val TILE = 64
    const val HEADER = 24
    const val ADD_HEADER = 8
    /** Both media envelopes (12-byte legacy, 28-byte tracks) stay <= 4 MiB. */
    const val MAX_PAYLOAD = 4 * 1024 * 1024 - 28
    const val CODEC_TQ1 = 1
    /** u64 max: no drop pending. Compared unsigned, as the C++ does. */
    const val NO_DROP = -1L

    fun columns(width: Int) = (width + TILE - 1) / TILE
    fun rows(height: Int) = (height + TILE - 1) / TILE

    /**
     * tiles-v1 frames: u16 dimensions, even (frameLayout), u16 tile indices
     * and the C++ frame-size bound for even NV12 frames.
     */
    fun validFrame(width: Int, height: Int): Boolean =
        width in 16..65535 && height in 16..65535 && width % 2 == 0 && height % 2 == 0 &&
            width.toLong() * height <= 0xFFFFFFFFL * 2 / 3 &&
            columns(width).toLong() * rows(height) <= 65536L

    fun isTilePayload(bytes: ByteArray, offset: Int, length: Int): Boolean =
        length >= 4 && offset >= 0 && offset + 4 <= bytes.size &&
            bytes[offset] == 'T'.code.toByte() && bytes[offset + 1] == 'S'.code.toByte() &&
            bytes[offset + 2] == 'L'.code.toByte() && bytes[offset + 3] == '1'.code.toByte()

    fun u16(bytes: ByteArray, at: Int): Int = (bytes[at].toInt() and 255) or ((bytes[at + 1].toInt() and 255) shl 8)
    fun u32(bytes: ByteArray, at: Int): Long = u16(bytes, at).toLong() or (u16(bytes, at + 2).toLong() shl 16)
    fun u64(bytes: ByteArray, at: Int): Long = u32(bytes, at) or (u32(bytes, at + 4) shl 32)

    private const val OP_INDEX = 0x00
    private const val OP_DIFF = 0x40
    private const val OP_LUMA = 0x80
    private const val OP_RGB = 0xfe
    private const val OP_ABOVE = 0xff
    private fun hash(px: Int) = ((px and 255) * 3 + ((px shr 8) and 255) * 5 + ((px shr 16) and 255) * 7 + 255 * 11) and 63

    /**
     * TQ1 into RGBA (alpha 255) at [out]/[outOffset] with [pitch] bytes per
     * row. Never writes outside width x height; false on any malformed,
     * truncated or over-long input (decodeTq1 in lossless_tiles.hpp).
     */
    fun decodeTq1(data: ByteArray, offset: Int, size: Int, width: Int, height: Int, out: ByteArray, outOffset: Int, pitch: Int): Boolean {
        if (width !in 1..TILE || height !in 1..TILE || pitch < width * 4 || offset < 0 || size < 0 || offset + size > data.size) return false
        if (outOffset < 0 || outOffset.toLong() + (height - 1).toLong() * pitch + width * 4L > out.size) return false
        val index = IntArray(64)
        val n = width * height
        var prev = 0
        var p = 0
        var x = 0
        var y = 0
        var at = offset
        val end = offset + size
        fun put(value: Int) {
            val target = outOffset + y * pitch + x * 4
            out[target] = value.toByte(); out[target + 1] = (value shr 8).toByte(); out[target + 2] = (value shr 16).toByte(); out[target + 3] = -1
            if (++x == width) { x = 0; ++y }
            ++p
        }
        while (p < n) {
            if (at >= end) return false
            val op = data[at++].toInt() and 255
            if (op == OP_RGB) {
                if (end - at < 3) return false
                prev = (data[at].toInt() and 255) or ((data[at + 1].toInt() and 255) shl 8) or ((data[at + 2].toInt() and 255) shl 16)
                at += 3
                put(prev)
            } else if (op == OP_ABOVE) {
                if (at >= end || y == 0) return false
                val count = (data[at++].toInt() and 255) + 1
                if (count > n - p) return false
                for (k in 0 until count) {
                    val source = outOffset + (y - 1) * pitch + x * 4
                    prev = (out[source].toInt() and 255) or ((out[source + 1].toInt() and 255) shl 8) or ((out[source + 2].toInt() and 255) shl 16)
                    put(prev)
                }
            } else if (op and 0xc0 == OP_INDEX) {
                prev = index[op and 63]; put(prev)
            } else if (op and 0xc0 == OP_DIFF) {
                val r = ((prev and 255) + ((op shr 4) and 3) - 2) and 255
                val g = (((prev shr 8) and 255) + ((op shr 2) and 3) - 2) and 255
                val b = (((prev shr 16) and 255) + (op and 3) - 2) and 255
                prev = r or (g shl 8) or (b shl 16); put(prev)
            } else if (op and 0xc0 == OP_LUMA) {
                if (at >= end) return false
                val second = data[at++].toInt() and 255
                val dg = (op and 63) - 32
                val drdg = (second shr 4) - 8
                val dbdg = (second and 15) - 8
                val r = ((prev and 255) + dg + drdg) and 255
                val g = (((prev shr 8) and 255) + dg) and 255
                val b = (((prev shr 16) and 255) + dg + dbdg) and 255
                prev = r or (g shl 8) or (b shl 16); put(prev)
            } else {
                val count = (op and 63) + 1
                if (count > n - p) return false
                for (k in 0 until count) put(prev)
            }
            index[hash(prev)] = prev
        }
        return at == end
    }
}

/** Receives each decoded add: tile [index] at [x],[y], [width] x [height] RGBA packed in [rgba] (width * 4 per row). */
internal fun interface TileSink {
    fun tile(index: Int, x: Int, y: Int, width: Int, height: Int, rgba: ByteArray)
}

/**
 * One per decoder (the engine of a start, or one output's engine); a new start,
 * topology or size makes a new one, so everything is cleared. Not thread-safe:
 * the decoder worker owns it. [videoWidth] x [videoHeight] is the decoded
 * video (Compositor::Configure in lossless_tiles.hpp).
 *  - a tile packet records its drops (due when a decoded frame with
 *    pts >= dropPts is shown) and hands its adds to the sink;
 *  - every shown decoded frame first applies due drops ([applyDecoded]), then
 *    gets every present tile, so a tile is never shown on the frame carrying
 *    its change or later;
 *  - a malformed packet, a size mismatch or a sequence gap poisons it: no tile
 *    is shown again (plain video), never a stale tile.
 * With [allowScaledSource] (a single start only) the first tile packet may
 * declare a larger unscaled source, never smaller than the video: the tiles
 * then live in source coordinates ([width] x [height]) and the picture is
 * drawn at the source size, video upscaled where no tile is present
 * (AndroidTileRenderer). Until that packet [width], [height] and [count] are 0.
 */
internal class LosslessCompositor(val videoWidth: Int, val videoHeight: Int, private val allowScaledSource: Boolean = false) {
    enum class Outcome { NOT_TILE, ACCEPTED, IGNORED }

    private val valid = LosslessTiles.validFrame(videoWidth, videoHeight)
    /** The tiles' (source) size: the video's unless scaled; 0 until a scaled-capable compositor's first packet. */
    var width = 0
        private set
    var height = 0
        private set
    private var columns = 0
    var count = 0
        private set
    /** Per tile 0 (video) or 0xff (lossless), row-major: the renderer's mask texture as is. */
    var presentMask = ByteArray(0)
        private set
    /** Changes whenever [presentMask] does. */
    var maskVersion = 0
        private set
    private var dropAt = LongArray(0)
    private var sequence = 0L
    private val added = ArrayList<Int>()
    private val scratch = ByteArray(LosslessTiles.TILE * LosslessTiles.TILE * 4)

    var presentTiles = 0
        private set
    var poisoned = false
        private set
    var packets = 0L
        private set
    var addedTiles = 0L
        private set
    var droppedTiles = 0L
        private set
    var bytes = 0L
        private set

    init { if (valid && !allowScaledSource) establish(videoWidth, videoHeight) }

    /** Adds received since the last shown picture wait to be shown (a republish when no video frame follows). */
    val republishPending: Boolean get() = !poisoned && added.isNotEmpty()

    /** The tiles describe a source larger than the video (Compositor::scaled). */
    val scaled: Boolean get() = count > 0 && (width != videoWidth || height != videoHeight)

    fun present(index: Int): Boolean = index in 0 until count && presentMask[index].toInt() != 0

    /**
     * One media payload ([length] bytes at [offset]). Its adds reach [sink]
     * as they are decoded; a packet that turns out malformed poisons.
     */
    fun accept(payload: ByteArray, offset: Int, length: Int, sink: TileSink): Outcome {
        if (!LosslessTiles.isTilePayload(payload, offset, length)) return Outcome.NOT_TILE
        if (poisoned || (count == 0 && !allowScaledSource) || !valid) return Outcome.IGNORED
        val parsed = try { parse(payload, offset, length, sink) } catch (_: RuntimeException) { false }
        if (!parsed) { poison(); return Outcome.IGNORED }
        return Outcome.ACCEPTED
    }

    /**
     * A decoded frame of presentation time [pts] (microseconds) is shown now:
     * its due drops apply and the adds so far are part of it (ApplyDecoded;
     * ApplyDecodedScaled makes the same state changes for a scaled source).
     */
    fun applyDecoded(pts: Long) {
        added.clear()
        if (poisoned || presentTiles == 0) return
        var changed = false
        for (i in 0 until count) if (presentMask[i].toInt() != 0 && due(i, pts)) changed = true
        if (changed) maskVersion++
    }

    /** The shown picture was redrawn with every present tile (a republish). */
    fun clearAdded() = added.clear()

    /** No tile is shown again in this engine (e.g. the renderer cannot place one). */
    fun poison() {
        poisoned = true
        added.clear()
        if (presentTiles > 0) { presentMask.fill(0); presentTiles = 0; maskVersion++ }
    }

    private fun establish(width: Int, height: Int) {
        this.width = width; this.height = height
        columns = LosslessTiles.columns(width)
        count = columns * LosslessTiles.rows(height)
    }

    private fun due(i: Int, pts: Long): Boolean {
        val at = dropAt[i]
        if (at == LosslessTiles.NO_DROP || java.lang.Long.compareUnsigned(pts, at) < 0) return false
        presentMask[i] = 0; dropAt[i] = LosslessTiles.NO_DROP; presentTiles--; droppedTiles++
        return true
    }

    private fun parse(p: ByteArray, offset: Int, size: Int, sink: TileSink): Boolean {
        if (size < LosslessTiles.HEADER || size > LosslessTiles.MAX_PAYLOAD || offset + size > p.size) return false
        if (count == 0) {
            // First packet of a scaled-capable compositor: the unscaled source
            // size, never smaller than the video. The wire limits replace an
            // arbitrary magnification ratio (the C++ upscaler's own checks
            // pass for every valid frame at least the video's size).
            val w = LosslessTiles.u16(p, offset + 4)
            val h = LosslessTiles.u16(p, offset + 6)
            if (!LosslessTiles.validFrame(w, h) || w < videoWidth || h < videoHeight) return false
            establish(w, h)
        }
        if (LosslessTiles.u16(p, offset + 4) != width || LosslessTiles.u16(p, offset + 6) != height) return false
        val next = LosslessTiles.u32(p, offset + 8)
        if (next != ((sequence + 1) and 0xFFFFFFFFL)) return false
        val dropPts = LosslessTiles.u64(p, offset + 12)
        val drops = LosslessTiles.u16(p, offset + 20)
        val adds = LosslessTiles.u16(p, offset + 22)
        if (size < LosslessTiles.HEADER + drops * 2) return false
        // Allocated on the first tile packet: sessions with released Hosts pay nothing.
        if (presentMask.size != count) { presentMask = ByteArray(count); dropAt = LongArray(count) { LosslessTiles.NO_DROP } }
        for (n in 0 until drops) {
            val index = LosslessTiles.u16(p, offset + LosslessTiles.HEADER + n * 2)
            if (index >= count) return false
            if (presentMask[index].toInt() != 0 && java.lang.Long.compareUnsigned(dropPts, dropAt[index]) < 0) dropAt[index] = dropPts
        }
        var at = LosslessTiles.HEADER + drops * 2
        for (n in 0 until adds) {
            if (size - at < LosslessTiles.ADD_HEADER) return false
            val index = LosslessTiles.u16(p, offset + at)
            val tileBytes = LosslessTiles.u32(p, offset + at + 4)
            if (index >= count || p[offset + at + 2].toInt() != LosslessTiles.CODEC_TQ1 || p[offset + at + 3].toInt() != 0 ||
                tileBytes > (size - at - LosslessTiles.ADD_HEADER).toLong()) return false
            val x = (index % columns) * LosslessTiles.TILE
            val y = (index / columns) * LosslessTiles.TILE
            val w = minOf(LosslessTiles.TILE, width - x)
            val h = minOf(LosslessTiles.TILE, height - y)
            if (presentMask[index].toInt() != 0) { presentMask[index] = 0; presentTiles--; maskVersion++ }
            if (!LosslessTiles.decodeTq1(p, offset + at + LosslessTiles.ADD_HEADER, tileBytes.toInt(), w, h, scratch, 0, w * 4)) return false
            sink.tile(index, x, y, w, h, scratch)
            // Newer content than any pending drop for this tile: it replaces it.
            presentMask[index] = -1; dropAt[index] = LosslessTiles.NO_DROP; presentTiles++; maskVersion++
            added.add(index); addedTiles++
            at += LosslessTiles.ADD_HEADER + tileBytes.toInt()
        }
        if (at != size) return false
        sequence = next; packets++; bytes += size
        return true
    }
}
