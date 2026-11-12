package me.totb.multitap_client

/** The Dart/native ownership contract. Retired calls never acquire a new lease. */
internal class AndroidMediaOwnership {
    @Volatile var current = 0L
        private set
    private var highest = 0L

    @Synchronized fun canBegin(next: Long): Boolean = next > highest && next > 0

    @Synchronized fun begin(next: Long): Boolean {
        if (!canBegin(next)) return false
        highest = next
        current = next
        return true
    }

    fun owns(expected: Long): Boolean = expected > 0 && current == expected

    @Synchronized fun end(expected: Long): Boolean {
        if (!owns(expected)) return false
        current = 0
        return true
    }

    @Synchronized fun clear() { current = 0 }
}

/**
 * StandardMethodCodec decodes Dart Uint8List as ByteArray inside this map.
 * The shared NativeRemoteMedia sends `{lease, bytes}`, and for an
 * input-latency-v1 watched video packet exactly `{lease, bytes, watch: true}`.
 */
internal data class AndroidMediaPacket(val lease: Long, val bytes: ByteArray, val watch: Boolean = false)

internal fun androidMediaPacket(arguments: Any?): AndroidMediaPacket? {
    val values = arguments as? Map<*, *> ?: return null
    val watched = values.containsKey("watch")
    // Only the literal `true` marks a watched packet; any other extra key or
    // value is outside the shared contract and stays a rejected envelope.
    if (watched && values["watch"] != true) return null
    if (values.size != (if (watched) 3 else 2) || !values.containsKey("lease") || !values.containsKey("bytes")) return null
    val lease = when (val raw = values["lease"]) {
        is Int -> raw.toLong()
        is Long -> raw
        else -> return null
    }
    val bytes = values["bytes"] as? ByteArray ?: return null
    if (lease <= 0 || bytes.size !in 13..4194304) return null
    // A watch applies to video (kind 1) only, as the Dart session sends it.
    if (watched && bytes[0].toInt() != 1) return null
    return AndroidMediaPacket(lease, bytes, watched)
}

/**
 * input-latency-v1 `inputFrames`: for each watched video packet, the time from
 * the engine accepting it until the first decoded frame at or after its PTS
 * was handed to the texture's pacing ([pts, microseconds]), as the Windows
 * engine measures queue entry -> published frame. Flutter's own wait for the
 * frame is reported separately (textureWaitTotalMs); neither is panel
 * scan-out. Bounded; one decoder at a time.
 */
internal class AndroidInputFrameWatch {
    private val pending = ArrayDeque<LongArray>()
    private val delivered = ArrayList<List<Long>>()

    fun watch(pts: Long, queuedNanos: Long) {
        if (pts < 0) return
        if (pending.size >= 16) pending.removeFirst()
        pending.addLast(longArrayOf(pts, queuedNanos))
    }

    fun rendered(pts: Long, renderedNanos: Long) {
        while (pending.isNotEmpty() && pending.first()[0] <= pts) {
            val item = pending.removeFirst()
            val us = (renderedNanos - item[1]) / 1000
            if (us in 0..60_000_000L && delivered.size < 64) delivered.add(listOf(item[0], us))
        }
    }

    /** The frames delivered since the previous statistics reading. */
    fun take(): List<List<Long>> {
        val out = ArrayList(delivered)
        delivered.clear()
        return out
    }

    fun clear() { pending.clear(); delivered.clear() }
}
