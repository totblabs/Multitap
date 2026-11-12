package me.totb.multitap_client

import android.app.ActivityManager
import android.content.Context
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build

/** Read-only metadata. No codec allocation/configure/start or display mutation. */
internal object AvcStreamSizes {
    fun memoryBudget(context: Context): Long {
        val runtime = Runtime.getRuntime()
        val heapFree = runtime.maxMemory() - (runtime.totalMemory() - runtime.freeMemory())
        // Keep the existing compressed queue budget available in the Java heap.
        if (heapFree < 16L * 1024 * 1024) return 0
        val manager = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager ?: return 0
        val memory = ActivityManager.MemoryInfo().also(manager::getMemoryInfo)
        if (memory.lowMemory || memory.availMem <= memory.threshold) return 0
        // A resource admission reserve, not a resolution/codec maximum. The
        // remainder stays available to the OS/other applications. Vendor codec
        // allocations can still fail; accepted metadata is never stream-ready.
        return (memory.availMem - memory.threshold) / 4
    }

    private fun hardware(info: MediaCodecInfo): Boolean =
        if (Build.VERSION.SDK_INT >= 29) info.isHardwareAccelerated && !info.isSoftwareOnly
        else !avcSoftwareName(info.name)

    fun profile(sps: ByteArray?): Int? = when {
        sps == null -> MediaCodecInfo.CodecProfileLevel.AVCProfileHigh
        sps.size < 6 -> null
        else -> when (sps[5].toInt() and 255) {
            66 -> MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline
            77 -> MediaCodecInfo.CodecProfileLevel.AVCProfileMain
            100 -> MediaCodecInfo.CodecProfileLevel.AVCProfileHigh
            else -> null // No 10-bit/new-profile fallback.
        }
    }

    fun candidates(allowHardware: Boolean, profile: Int = MediaCodecInfo.CodecProfileLevel.AVCProfileHigh): List<StreamSizeCandidate> {
        val infos = try { MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.toList() } catch (_: Exception) { return emptyList() }
        val order = AvcDecoders.candidates()
        return infos.filter { !it.isEncoder && it.supportedTypes.any { type -> type.equals(MediaFormat.MIMETYPE_VIDEO_AVC, true) } &&
            !(Build.VERSION.SDK_INT >= 29 && it.isAlias) && (allowHardware || !hardware(it)) }
            .sortedBy { val rank = order.indexOf(it.name); if (rank < 0) Int.MAX_VALUE else rank }
            .mapNotNull { info -> try {
                val capabilities = info.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
                val video = capabilities.videoCapabilities ?: return@mapNotNull null
                val maxWidth = video.supportedWidths.upper
                val maxHeight = video.supportedHeights.upper
                val pixels = Math.multiplyExact(maxWidth.toLong(), maxHeight.toLong())
                val rate = pixels.toDouble() * video.supportedFrameRates.upper
                if (maxWidth < 2 || maxHeight < 2 || pixels > 9007199254740991L || !rate.isFinite() || rate < 0 || rate > 9007199254740991.0) return@mapNotNull null
                if (!Regex("^[A-Za-z0-9_.-]{1,256}$").matches(info.name)) return@mapNotNull null
                StreamSizeCandidate(info.name, maxWidth, maxHeight, pixels, rate.toLong(), capabilities.maxSupportedInstances, hardware(info)) { track ->
                    when {
                        track.width % video.widthAlignment != 0 || track.height % video.heightAlignment != 0 -> "unsupported_size"
                        !video.isSizeSupported(track.width, track.height) -> "unsupported_size"
                        !video.areSizeAndRateSupported(track.width, track.height, track.fps.toDouble()) -> "unsupported_rate"
                        capabilities.profileLevels.none { it.profile == profile ||
                            (profile == MediaCodecInfo.CodecProfileLevel.AVCProfileHigh && it.profile == MediaCodecInfo.CodecProfileLevel.AVCProfileConstrainedHigh) ||
                            (profile == MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline && it.profile == MediaCodecInfo.CodecProfileLevel.AVCProfileConstrainedBaseline) } -> "unsupported_profile"
                        !capabilities.isFormatSupported(MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, track.width, track.height).apply {
                            setInteger(MediaFormat.KEY_FRAME_RATE, track.fps)
                            setInteger(MediaFormat.KEY_PROFILE, profile)
                        }) -> "unsupported_profile"
                        else -> null
                    }
                }
            } catch (_: Exception) { null } }
    }

    fun validate(request: StreamSizeRequest, context: Context): Map<String, Any?> =
        validateStreamSizeRequest(request, candidates(request.hardware), memoryBudget(context))

    fun informationalLimits(): Map<String, Any>? {
        val candidate = candidates(true).firstOrNull() ?: return null
        return mapOf("maxWidth" to candidate.maxWidth, "maxHeight" to candidate.maxHeight,
            "maxPixels" to candidate.maxPixels, "maxPixelRate" to candidate.maxPixelRate)
    }
}
