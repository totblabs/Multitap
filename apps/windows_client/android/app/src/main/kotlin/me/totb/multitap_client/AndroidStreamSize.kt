package me.totb.multitap_client

/** Actual channel parser/admission policy, portable for memory-only tests. */
internal data class StreamSizeTrack(val width: Int, val height: Int, val fps: Int, val sourceWidth: Int? = null, val sourceHeight: Int? = null)
internal data class StreamSizeRequest(val tracks: List<StreamSizeTrack>, val hardware: Boolean)
internal fun legacyStreamSize(track: StreamSizeTrack): Boolean =
    track.width in 2..3840 && track.height in 2..3840 && track.width % 2 == 0 && track.height % 2 == 0 &&
        track.width.toLong() * track.height <= 8294400L
internal data class StreamSizeCandidate(
    val name: String,
    val maxWidth: Int,
    val maxHeight: Int,
    val maxPixels: Long,
    val maxPixelRate: Long,
    val instances: Int,
    val accelerated: Boolean = false,
    val accepts: (StreamSizeTrack) -> String?,
)

internal fun streamSizeRequest(value: Any?): StreamSizeRequest? {
    val map = value as? Map<*, *> ?: return null
    if (map.keys != setOf("schema", "tracks", "hardware") || map["schema"] != 1 || map["hardware"] !is Boolean) return null
    val tracks = map["tracks"] as? List<*> ?: return null
    if (tracks.size !in 1..2) return null
    fun integer(value: Any?): Int? = when (value) {
        is Int -> value
        is Long -> if (value in 1..Int.MAX_VALUE.toLong()) value.toInt() else null
        else -> null
    }
    val parsed = ArrayList<StreamSizeTrack>(tracks.size)
    for (raw in tracks) {
        val track = raw as? Map<*, *> ?: return null
        val source = track.containsKey("sourceWidth") || track.containsKey("sourceHeight")
        if (track.keys != (if (source) setOf("width", "height", "fps", "sourceWidth", "sourceHeight") else setOf("width", "height", "fps"))) return null
        val w = integer(track["width"]) ?: return null
        val h = integer(track["height"]) ?: return null
        val fps = integer(track["fps"]) ?: return null
        if (w < 2 || h < 2 || w % 2 != 0 || h % 2 != 0 || fps !in 1..60) return null
        val sw = if (source) integer(track["sourceWidth"]) ?: return null else null
        val sh = if (source) integer(track["sourceHeight"]) ?: return null else null
        if (source) {
            val sourceWidth = sw ?: return null
            val sourceHeight = sh ?: return null
            if (sourceWidth < w || sourceHeight < h || sourceWidth % 2 != 0 || sourceHeight % 2 != 0) return null
        }
        parsed.add(StreamSizeTrack(w, h, fps, sw, sh))
    }
    return StreamSizeRequest(parsed, map["hardware"] as Boolean)
}

/**
 * Admission estimate, not a claim about opaque vendor decoder allocations:
 * AVC may retain sixteen NV12 references plus three RGBA presentation images.
 * The caller supplies fresh system/heap headroom; no resolution maximum exists.
 */
internal fun streamSizeMemoryBytes(tracks: List<StreamSizeTrack>): Long? = try {
    var bytes = 0L
    for (track in tracks) {
        val blocks = Math.multiplyExact((track.width.toLong() + 15) / 16, (track.height.toLong() + 15) / 16)
        val pixels = Math.multiplyExact(blocks, 256L)
        bytes = Math.addExact(bytes, Math.multiplyExact(pixels, 36L))
    }
    bytes
} catch (_: ArithmeticException) { null }

internal fun streamSizeReply(
    tracks: List<StreamSizeTrack>, reason: String?, candidate: StreamSizeCandidate? = null,
): Map<String, Any?> = mapOf(
    "schema" to 1, "accepted" to (reason == null), "reason" to reason,
    "tracks" to tracks.map { mapOf("width" to it.width, "height" to it.height, "fps" to it.fps, "accepted" to (reason == null), "reason" to reason) +
        (if (it.sourceWidth != null && it.sourceHeight != null) mapOf("sourceWidth" to it.sourceWidth, "sourceHeight" to it.sourceHeight) else emptyMap()) },
    "validation" to "exact-candidate", "decoder" to (candidate?.name ?: "unknown"),
    // Informational envelope only. It is never used as proof of another shape.
    "maxWidth" to (candidate?.maxWidth ?: 0), "maxHeight" to (candidate?.maxHeight ?: 0),
    "maxPixels" to (candidate?.maxPixels ?: 0L), "maxPixelRate" to (candidate?.maxPixelRate ?: 0L),
)

internal fun validateStreamSizeRequest(
    request: StreamSizeRequest, candidates: List<StreamSizeCandidate>, memoryBudgetBytes: Long,
): Map<String, Any?> {
    val memory = streamSizeMemoryBytes(request.tracks)
        ?: return streamSizeReply(request.tracks, "memory_budget", candidates.firstOrNull())
    if (memoryBudgetBytes <= 0 || memory > memoryBudgetBytes) return streamSizeReply(request.tracks, "memory_budget", candidates.firstOrNull())
    var reason = "capability_unknown"
    for (candidate in candidates) {
        if (!request.hardware && candidate.accelerated) continue
        if (candidate.instances < request.tracks.size) { reason = "insufficient_instances"; continue }
        val refusals = request.tracks.map { track ->
            try { candidate.accepts(track) } catch (_: Exception) { "capability_unknown" }
        }
        val failed = refusals.firstOrNull { it != null }
        if (failed == null) return streamSizeReply(request.tracks, null, candidate)
        reason = if (failed in setOf("unsupported_size", "unsupported_rate", "unsupported_profile", "capability_unknown")) failed else "capability_unknown"
    }
    return streamSizeReply(request.tracks, reason, candidates.firstOrNull())
}

/** Late read-only metadata results never become permission to start an owner. */
internal class StreamSizeObservation {
    private var generation = 0L
    @Synchronized fun snapshot(): Long = generation
    @Synchronized fun changed() { generation++ }
    @Synchronized fun current(expected: Long): Boolean = expected == generation
}
