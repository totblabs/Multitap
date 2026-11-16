package me.totb.multitap_client

/**
 * Which decoded frame the tile renderer latched, and whether a video frame is
 * still on its way (decoder worker only).
 *
 * Drops apply by presentation time, so the renderer must know the pts of the
 * picture it latched. Since Android 6 a frame released with
 * releaseOutputBuffer(index, true) carries its presentation time (in ns) as
 * the SurfaceTexture timestamp; [latched] checks that against the frames the
 * pacer actually rendered, in order. A timestamp that is none of them (a
 * device that does not keep it) returns null: the engine then stops showing
 * tiles (poison), never a tile on the wrong picture.
 */
internal class LosslessFrameOrder(private val limit: Int = 32) {
    private val decoded = HashMap<Int, Long>()
    private val rendered = ArrayDeque<Long>()
    private var newestReceived = Long.MIN_VALUE
    private var newestLatched = Long.MIN_VALUE

    /** The codec output buffer [index] holds the frame of [pts] (microseconds). */
    fun decoded(index: Int, pts: Long) { decoded[index] = pts }

    /** The pacer rendered buffer [index] to the renderer's surface. */
    fun rendered(index: Int) {
        val pts = decoded.remove(index) ?: return
        rendered.addLast(pts)
        while (rendered.size > limit) rendered.removeFirst()
    }

    fun discarded(index: Int) { decoded.remove(index) }

    /** The pts of the latched frame of [timestampNanos], or null when it was no rendered frame. */
    fun latched(timestampNanos: Long): Long? {
        val pts = timestampNanos / 1000
        if (timestampNanos % 1000 != 0L) return null
        while (rendered.isNotEmpty()) {
            val next = rendered.removeFirst()
            if (next == pts) { if (pts > newestLatched) newestLatched = pts; return pts }
        }
        return null
    }

    /** A video packet of [pts] went to the decoder. */
    fun videoReceived(pts: Long) { if (pts > newestReceived) newestReceived = pts }

    /** A received video frame has not been latched yet: it will show the tiles that came before it. */
    fun videoAhead(): Boolean = newestReceived > newestLatched

    fun clear() {
        decoded.clear(); rendered.clear()
        newestReceived = Long.MIN_VALUE; newestLatched = Long.MIN_VALUE
    }
}
