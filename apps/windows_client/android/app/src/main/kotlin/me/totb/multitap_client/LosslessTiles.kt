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
    /** tiles-v2: rows of a rectangle moved by dy, and the exact pixels of a block (each side 1..64). */
    const val CODEC_MOVE = 2
    const val CODEC_RECT = 3
    const val NO_TILE = 0xffff
    const val MOVE_BYTES = 10
    const val RECT_HEADER = 8
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
 *  - a malformed packet, a size mismatch or a sequence gap clears the tiles.
 *    Plain video continues until a restarted source supplies packet 1.
 * With [allowScaledSource] (a single start only) the first tile packet may
 * declare a larger unscaled source, never smaller than the video: the tiles
 * then live in source coordinates ([width] x [height]) and the picture is
 * drawn at the source size, video upscaled where no tile is present
 * (AndroidTileRenderer). Until that packet [width], [height] and [count] are 0.
 *
 * tiles-v2 ([moves], an engine whose session asked with "lt1-m"): a packet with
 * a move or a rectangle waits for the decoded frame it belongs to, and so does
 * everything that arrives while it waits, in order (Compositor::queue). For
 * that the pixels of every tile are also kept here (RGBA, [width] x [height]):
 * a move shifts rows of that layer, and the tiles it touched go to the sink
 * again when their frame is shown ([applyDecoded]), one call per run of
 * neighbouring tiles in a row. Without [moves] such a packet is malformed.
 */
internal class LosslessCompositor(val videoWidth: Int, val videoHeight: Int, private val allowScaledSource: Boolean = false, private val moves: Boolean = false) {
    enum class Outcome { NOT_TILE, ACCEPTED, IGNORED }

    private class Op(val kind: Int, val notBefore: Long, val dropPts: Long, val index: Int, val x: Int, val y: Int, val w: Int, val h: Int, val dy: Int, val pixels: ByteArray?)
    private val ops = ArrayDeque<Op>()
    private var layer = ByteArray(0)
    private var touched = BooleanArray(0)
    private var run = ByteArray(0)
    /** Entries wait for their frame: nothing of them shows before it. */
    val waiting: Boolean get() = ops.isNotEmpty()
    var movesApplied = 0L
        private set

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
    var restarts = 0L
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
        val restarting = valid && (poisoned || sequence != 0L) && length in LosslessTiles.HEADER..LosslessTiles.MAX_PAYLOAD &&
            length <= payload.size - offset && LosslessTiles.u32(payload, offset + 8) == 1L
        if (restarting) {
            // The Host restarted its tile source. Queued drops, moves and mask
            // bits belong to the old source, including when the size is equal.
            poison()
            sequence = 0L
            width = 0; height = 0; columns = 0; count = 0
            presentMask = ByteArray(0); dropAt = LongArray(0)
            layer = ByteArray(0); touched = BooleanArray(0)
            if (!allowScaledSource) establish(videoWidth, videoHeight)
            poisoned = false
            maskVersion++
        }
        if (poisoned || (count == 0 && !allowScaledSource) || !valid) return Outcome.IGNORED
        val parsed = try { parse(payload, offset, length, sink) } catch (_: RuntimeException) { false }
        if (!parsed) { poison(); return Outcome.IGNORED }
        if (restarting) restarts++
        return Outcome.ACCEPTED
    }

    /**
     * A decoded frame of presentation time [pts] (microseconds) is shown now:
     * its due drops apply and the adds so far are part of it (ApplyDecoded;
     * ApplyDecodedScaled makes the same state changes for a scaled source).
     */
    fun applyDecoded(pts: Long, sink: TileSink? = null) {
        advance(pts, sink)
        added.clear()
        if (poisoned || presentTiles == 0) return
        var changed = false
        for (i in 0 until count) if (presentMask[i].toInt() != 0 && due(i, pts)) changed = true
        if (changed) maskVersion++
    }

    /** The shown picture was redrawn with every present tile (a republish). */
    fun clearAdded() = added.clear()

    /** Clear this source's tiles (e.g. the renderer cannot place one). */
    fun poison() {
        poisoned = true
        added.clear(); ops.clear()
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
        if (presentMask.size != count) {
            presentMask = ByteArray(count); dropAt = LongArray(count) { LosslessTiles.NO_DROP }
            // Without room for the layer the tiles still work; a move then ends them (plain video).
            if (moves) try { layer = ByteArray(width * height * 4); touched = BooleanArray(count); run = ByteArray(width * LosslessTiles.TILE * 4) } catch (_: OutOfMemoryError) { layer = ByteArray(0) }
        }
        // tiles-v2 entries wait for their frame, and everything that arrives while
        // any of them waits stays behind it, in order.
        var timed = ops.isNotEmpty()
        var scan = LosslessTiles.HEADER + drops * 2
        var scanned = 0
        while (scanned < adds && !timed) {
            if (size - scan < LosslessTiles.ADD_HEADER) return false
            val length = LosslessTiles.u32(p, offset + scan + 4)
            if (length > (size - scan - LosslessTiles.ADD_HEADER).toLong()) return false
            val codec = p[offset + scan + 2].toInt()
            timed = codec == LosslessTiles.CODEC_MOVE || codec == LosslessTiles.CODEC_RECT
            scan += LosslessTiles.ADD_HEADER + length.toInt(); scanned++
        }
        if (timed) return queue(p, offset, size, dropPts, drops, adds, next)
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
            if (layer.isNotEmpty()) for (row in 0 until h) System.arraycopy(scratch, row * w * 4, layer, ((y + row) * width + x) * 4, w * 4)
            // Newer content than any pending drop for this tile: it replaces it.
            presentMask[index] = -1; dropAt[index] = LosslessTiles.NO_DROP; presentTiles++; maskVersion++
            added.add(index); addedTiles++
            at += LosslessTiles.ADD_HEADER + tileBytes.toInt()
        }
        if (at != size) return false
        sequence = next; packets++; bytes += size
        return true
    }

    private fun queue(p: ByteArray, offset: Int, size: Int, dropPts: Long, drops: Int, adds: Int, next: Long): Boolean {
        if (layer.isEmpty()) return false
        val staged = ArrayList<Op>()
        var last = if (ops.isEmpty()) 0L else ops.last().notBefore
        // Never earlier than the entry queued before it.
        fun notBefore(own: Long): Long { if (java.lang.Long.compareUnsigned(own, last) > 0) last = own; return last }
        for (n in 0 until drops) {
            val index = LosslessTiles.u16(p, offset + LosslessTiles.HEADER + n * 2)
            if (index >= count) return false
            staged.add(Op(OP_DROP, notBefore(dropPts), dropPts, index, 0, 0, 0, 0, 0, null))
        }
        var at = LosslessTiles.HEADER + drops * 2
        for (n in 0 until adds) {
            if (size - at < LosslessTiles.ADD_HEADER) return false
            val index = LosslessTiles.u16(p, offset + at)
            val length = LosslessTiles.u32(p, offset + at + 4)
            val codec = p[offset + at + 2].toInt()
            if (p[offset + at + 3].toInt() != 0 || length > (size - at - LosslessTiles.ADD_HEADER).toLong()) return false
            val body = offset + at + LosslessTiles.ADD_HEADER
            when (codec) {
                LosslessTiles.CODEC_TQ1 -> {
                    if (index >= count) return false
                    val x = (index % columns) * LosslessTiles.TILE
                    val y = (index / columns) * LosslessTiles.TILE
                    val w = minOf(LosslessTiles.TILE, width - x)
                    val h = minOf(LosslessTiles.TILE, height - y)
                    val pixels = ByteArray(w * h * 4)
                    if (!LosslessTiles.decodeTq1(p, body, length.toInt(), w, h, pixels, 0, w * 4)) return false
                    staged.add(Op(OP_ADD, notBefore(0L), LosslessTiles.NO_DROP, index, x, y, w, h, 0, pixels))
                }
                LosslessTiles.CODEC_MOVE, LosslessTiles.CODEC_RECT -> {
                    // Without a frame to wait for there is no moment to apply it.
                    val least = if (codec == LosslessTiles.CODEC_MOVE) LosslessTiles.MOVE_BYTES else LosslessTiles.RECT_HEADER
                    if (index != LosslessTiles.NO_TILE || dropPts == LosslessTiles.NO_DROP || length < least) return false
                    val x = LosslessTiles.u16(p, body)
                    val y = LosslessTiles.u16(p, body + 2)
                    val w = LosslessTiles.u16(p, body + 4)
                    val h = LosslessTiles.u16(p, body + 6)
                    if (w == 0 || h == 0 || x + w > width || y + h > height) return false
                    if (codec == LosslessTiles.CODEC_MOVE) {
                        val dy = LosslessTiles.u16(p, body + 8).toShort().toInt()
                        if (length != LosslessTiles.MOVE_BYTES.toLong() || dy == 0 || kotlin.math.abs(dy) >= h) return false
                        staged.add(Op(OP_MOVE, notBefore(dropPts), dropPts, 0, x, y, w, h, dy, null))
                    } else {
                        if (w > LosslessTiles.TILE || h > LosslessTiles.TILE) return false
                        val pixels = ByteArray(w * h * 4)
                        if (!LosslessTiles.decodeTq1(p, body + LosslessTiles.RECT_HEADER, (length - LosslessTiles.RECT_HEADER).toInt(), w, h, pixels, 0, w * 4)) return false
                        staged.add(Op(OP_RECT, notBefore(dropPts), dropPts, 0, x, y, w, h, 0, pixels))
                    }
                }
                else -> return false
            }
            at += LosslessTiles.ADD_HEADER + length.toInt()
        }
        if (at != size) return false
        ops.addAll(staged)
        sequence = next; packets++; bytes += size
        return true
    }

    /** Applies, in order, every waiting entry whose frame has come; what they changed goes to [sink]. */
    private fun advance(pts: Long, sink: TileSink?) {
        if (poisoned || ops.isEmpty()) return
        var any = false
        while (ops.isNotEmpty() && java.lang.Long.compareUnsigned(ops.first().notBefore, pts) <= 0) {
            if (!apply(ops.removeFirst())) { poison(); return }
            any = true
        }
        if (!any) return
        // The tile texture follows the layer: one upload per run of touched tiles in a row.
        try {
            var index = 0
            while (index < count) {
                if (!touched[index]) { index++; continue }
                val row = index / columns
                var end = index
                while (end < count && end / columns == row && touched[end]) { touched[end] = false; end++ }
                val x = (index % columns) * LosslessTiles.TILE
                val y = row * LosslessTiles.TILE
                val w = minOf(width, ((end - 1) % columns + 1) * LosslessTiles.TILE) - x
                val h = minOf(LosslessTiles.TILE, height - y)
                for (line in 0 until h) System.arraycopy(layer, ((y + line) * width + x) * 4, run, line * w * 4, w * 4)
                (sink ?: throw IllegalStateException("no tile sink")).tile(index, x, y, w, h, run)
                index = end
            }
        } catch (_: RuntimeException) { poison() }
    }

    private fun touch(x: Int, y: Int, w: Int, h: Int) {
        for (r in y / LosslessTiles.TILE..(y + h - 1) / LosslessTiles.TILE) for (c in x / LosslessTiles.TILE..(x + w - 1) / LosslessTiles.TILE) touched[r * columns + c] = true
    }

    /** Every tile the rectangle touches is present: what the Host required before it sent the entry. */
    private fun covered(x: Int, y: Int, w: Int, h: Int): Boolean {
        for (r in y / LosslessTiles.TILE..(y + h - 1) / LosslessTiles.TILE) for (c in x / LosslessTiles.TILE..(x + w - 1) / LosslessTiles.TILE) if (presentMask[r * columns + c].toInt() == 0) return false
        return true
    }

    private fun apply(op: Op): Boolean {
        when (op.kind) {
            OP_DROP -> if (presentMask[op.index].toInt() != 0 && java.lang.Long.compareUnsigned(op.dropPts, dropAt[op.index]) < 0) dropAt[op.index] = op.dropPts
            OP_ADD -> {
                for (row in 0 until op.h) System.arraycopy(op.pixels!!, row * op.w * 4, layer, ((op.y + row) * width + op.x) * 4, op.w * 4)
                if (presentMask[op.index].toInt() == 0) { presentMask[op.index] = -1; presentTiles++; maskVersion++ }
                dropAt[op.index] = LosslessTiles.NO_DROP; addedTiles++
                touched[op.index] = true
            }
            OP_MOVE -> {
                if (!covered(op.x, op.y, op.w, op.h)) return false
                val rows = op.h - kotlin.math.abs(op.dy)
                val bytes = op.w * 4
                if (op.dy > 0) for (n in 0 until rows) { val y = op.y + n; System.arraycopy(layer, ((y + op.dy) * width + op.x) * 4, layer, (y * width + op.x) * 4, bytes) }
                else for (n in rows - 1 downTo 0) { val y = op.y - op.dy + n; System.arraycopy(layer, ((y + op.dy) * width + op.x) * 4, layer, (y * width + op.x) * 4, bytes) }
                movesApplied++
                touch(op.x, op.y, op.w, op.h)
            }
            else -> {
                if (!covered(op.x, op.y, op.w, op.h)) return false
                for (row in 0 until op.h) System.arraycopy(op.pixels!!, row * op.w * 4, layer, ((op.y + row) * width + op.x) * 4, op.w * 4)
                touch(op.x, op.y, op.w, op.h)
            }
        }
        return true
    }

    private companion object { const val OP_DROP = 0; const val OP_ADD = 1; const val OP_MOVE = 2; const val OP_RECT = 3 }
}
