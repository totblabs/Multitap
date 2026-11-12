package me.totb.multitap_client

// 화면 › 해상도 › 선명하게 (lib/android/android_sharp_picture.dart): the automatic size is this
// screen's logical size, which the panel then scales up, so text is soft. This choice asks for
// (up to) the panel's own pixels and raises Windows' scale by the same factor, so everything
// keeps its physical size and only gets sharper.
//
// The rule is decided from what this device says about itself, never from its name:
//  * the picture area in panel pixels and the automatic size for it (which already carries the
//    density and the viewing distance, android_screen.dart);
//  * the H.264 decoders the engine would start, in its order, each asked for exactly this size
//    at this frame rate (MediaCodecInfo.VideoCapabilities: supported size, size and rate
//    together, profile and level; AndroidStreamSizeNative.kt) and the memory budget;
//  * the ceiling an encoded picture has on this connection (the protocol's and the Host's).
// Windows has five scale steps (100-200 %): a panel with more than about twice the automatic
// size is asked for twice that size, not for more pixels that would make everything smaller.
// What the decoder does not take is tried smaller, towards the automatic size; when nothing
// above it is taken the plan is the automatic size and counts as a fallback.

internal data class SharpPictureRequest(
    /** The picture area in panel pixels. */
    val panelWidth: Int,
    val panelHeight: Int,
    /** The automatic size for that area (today's size). */
    val baseWidth: Int,
    val baseHeight: Int,
    val fps: Int,
    val hardware: Boolean = true,
    /** The ceiling of an encoded picture on this connection (0: none said). */
    val limitWidth: Int = 0,
    val limitHeight: Int = 0,
    val limitPixels: Long = 0,
)

/**
 * [scale]: the Windows scale (percent) that keeps things the size they have at [SharpPictureRequest.baseWidth]
 * and 100 %. [reason] null: the panel's own pixels. Otherwise why it is less: `scale_limit`,
 * `host_limit`, `decoder_size`, `decoder_rate`, `decoder_profile`, `memory_budget`,
 * `decoder_unknown`, `no_gain` (the automatic size already is the panel's), `invalid_request`.
 * [fallback]: nothing sharper than the automatic size could be had.
 */
internal data class SharpPicturePlan(val width: Int, val height: Int, val scale: Int, val reason: String?, val fallback: Boolean)

internal val sharpScaleSteps = intArrayOf(100, 125, 150, 175, 200)

/** The scale step nearest to [ratio] (1.0 = 100 %); between two steps the lower (never bigger than asked). */
internal fun sharpScale(ratio: Double): Int {
    if (!ratio.isFinite()) return 100
    var best = sharpScaleSteps[0]
    for (step in sharpScaleSteps) if (Math.abs(step - ratio * 100) < Math.abs(best - ratio * 100) - 1e-9) best = step
    return best
}

/** How many sizes between the panel's and the automatic one are tried before giving up. */
internal const val sharpPictureSteps = 12

internal fun sharpPicturePlan(
    request: SharpPictureRequest,
    candidates: List<StreamSizeCandidate>,
    memoryBudgetBytes: Long,
): SharpPicturePlan {
    val baseWidth = request.baseWidth
    val baseHeight = request.baseHeight
    fun base(reason: String, fallback: Boolean) = SharpPicturePlan(baseWidth, baseHeight, 100, reason, fallback)
    if (baseWidth < 2 || baseHeight < 2 || baseWidth % 2 != 0 || baseHeight % 2 != 0 || request.fps !in 1..60 ||
        request.panelWidth < 2 || request.panelHeight < 2 || request.panelWidth > 32768 || request.panelHeight > 32768) {
        return base("invalid_request", fallback = true)
    }
    fun even(value: Double) = (value.toInt() / 2) * 2
    // The same shape as the automatic size, within the panel.
    val most = minOf(request.panelWidth.toDouble() / baseWidth, request.panelHeight.toDouble() / baseHeight)
    if (even(baseWidth * most) <= baseWidth || even(baseHeight * most) <= baseHeight) return base("no_gain", fallback = false)
    var reason: String? = null
    // Above 200 % (and half a step) more pixels would only make everything smaller.
    val top = sharpScaleSteps.last() / 100.0
    val wanted = if (most > top + .125 / 2) { reason = "scale_limit"; top } else most
    for (step in 0 until sharpPictureSteps) {
        val factor = wanted - (wanted - 1) * step / sharpPictureSteps
        val width = if (step == 0 && reason == null) even(request.panelWidth.toDouble().coerceAtMost(baseWidth * most + 1)) else even(baseWidth * factor)
        val height = if (step == 0 && reason == null) even(request.panelHeight.toDouble().coerceAtMost(baseHeight * most + 1)) else even(baseHeight * factor)
        if (width <= baseWidth || height <= baseHeight) break
        if ((request.limitWidth > 0 && width > request.limitWidth) || (request.limitHeight > 0 && height > request.limitHeight) ||
            (request.limitPixels > 0 && width.toLong() * height > request.limitPixels)) {
            if (reason == null || reason == "scale_limit") reason = "host_limit"
            continue
        }
        val check = validateStreamSizeRequest(
            StreamSizeRequest(listOf(StreamSizeTrack(width, height, request.fps)), request.hardware), candidates, memoryBudgetBytes)
        if (check["accepted"] == true) return SharpPicturePlan(width, height, sharpScale(width.toDouble() / baseWidth), reason, fallback = false)
        val refused = when (check["reason"]) {
            "unsupported_size" -> "decoder_size"
            "unsupported_rate" -> "decoder_rate"
            "unsupported_profile" -> "decoder_profile"
            "memory_budget" -> "memory_budget"
            else -> "decoder_unknown"
        }
        if (reason == null || reason == "scale_limit" || reason == "host_limit") reason = refused
    }
    return base(reason ?: "decoder_unknown", fallback = true)
}

internal fun sharpPictureRequest(value: Any?): SharpPictureRequest? {
    val map = value as? Map<*, *> ?: return null
    fun number(name: String): Long? = when (val item = map[name]) {
        is Int -> item.toLong()
        is Long -> item
        else -> null
    }
    fun int(name: String): Int? = number(name)?.takeIf { it in 0..Int.MAX_VALUE.toLong() }?.toInt()
    return SharpPictureRequest(
        panelWidth = int("panelWidth") ?: return null,
        panelHeight = int("panelHeight") ?: return null,
        baseWidth = int("baseWidth") ?: return null,
        baseHeight = int("baseHeight") ?: return null,
        fps = int("fps") ?: return null,
        hardware = map["hardware"] != false,
        limitWidth = int("limitWidth") ?: 0,
        limitHeight = int("limitHeight") ?: 0,
        limitPixels = number("limitPixels")?.coerceAtLeast(0) ?: 0,
    )
}

/**
 * What the statistics say about 선명하게 (the `stats` answer's sharp* fields): the last plan and how
 * often the automatic size had to be used instead, by the decoder's word here or by the Host's
 * (reported from Dart: a refused resize, a picture encoded smaller than asked).
 */
internal class SharpPictureStats {
    private var requested = false
    private var last: SharpPicturePlan? = null
    private var fallbacks = 0
    private var fallbackReason: String? = null

    @Synchronized fun planned(plan: SharpPicturePlan) {
        requested = true
        last = plan
        if (plan.fallback) { fallbacks++; fallbackReason = plan.reason }
    }

    /** The Host did not give the planned picture ([reason]: `host_refused`, `host_reduced`). */
    @Synchronized fun hostFallback(reason: String) {
        if (reason != "host_refused" && reason != "host_reduced") return
        fallbacks++
        fallbackReason = reason
    }

    /** The choice was turned off. */
    @Synchronized fun off() { requested = false; last = null }

    @Synchronized fun values(): Map<String, Any?> = mapOf(
        "sharpRequested" to requested,
        "sharpWidth" to (last?.width ?: 0),
        "sharpHeight" to (last?.height ?: 0),
        "sharpScale" to (last?.scale ?: 0),
        "sharpReason" to last?.reason,
        "sharpFallbacks" to fallbacks,
        "sharpFallbackReason" to fallbackReason,
    )

}

/** The `sharpPicture` answer. */
internal fun sharpPictureReply(plan: SharpPicturePlan): Map<String, Any?> = mapOf(
    "width" to plan.width, "height" to plan.height, "scale" to plan.scale, "reason" to plan.reason, "fallback" to plan.fallback,
)

/** One count per app run, whichever engine instance planned. */
internal object SharpPictures {
    val stats = SharpPictureStats()
}
