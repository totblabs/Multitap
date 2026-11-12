package me.totb.multitap_client

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.view.Surface
import java.nio.ByteBuffer

/**
 * The device's H.264 decoders in the order AndroidMedia tries them
 * (orderAvcDecoders), and how one is configured (avcTunings). Only names and
 * the hardware flag are read here; size/capability queries are separate.
 */
internal object AvcDecoders {
    @Volatile private var listed: List<AvcDecoderCandidate>? = null
    /**
     * Started, then failed before its first frame although it had the
     * parameter sets in its format: once more with the plain format if it was
     * tuned, then tried last for the rest of this app run (AvcFailureMemory).
     * (A decoder that merely cannot be created or configured is skipped for
     * that start only; that can be a transient resource shortage.)
     * Refused or failed when started ahead of the key frame: gets SPS/PPS in
     * its format.
     */
    private val failures = AvcFailureMemory()

    /** Names in trial order; empty when the platform list cannot be read. */
    fun candidates(): List<String> = orderAvcDecoders(list(), failures.demoted())

    /** False when this decoder must get SPS/PPS in its format (it is started at the key frame). */
    private fun startsEarly(name: String) = failures.startsEarly(name)

    /** [tuningFlags]: what the failed decoder was configured with (avcTuningFlags). */
    fun failedBeforeFirstFrame(name: String, early: Boolean, tuningFlags: Int?) =
        failures.failedBeforeFirstFrame(name, early, tuningFlags)

    /** Stream stats `decoderFallbacks`: decoders moved to the plain format or to the end of the order, this app run. */
    val fallbacks: Int get() = failures.plainFallbacks + failures.demotions

    private fun list(): List<AvcDecoderCandidate> = listed ?: try {
        MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
            .filter { info ->
                !info.isEncoder && info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_AVC, true) } &&
                    !(Build.VERSION.SDK_INT >= 29 && info.isAlias)
            }
            .map { AvcDecoderCandidate(it.name, hardware(it)) }
    } catch (_: Exception) {
        emptyList()
    }.also { listed = it }

    private fun hardware(info: MediaCodecInfo): Boolean =
        if (Build.VERSION.SDK_INT >= 29) info.isHardwareAccelerated && !info.isSoftwareOnly
        else !avcSoftwareName(info.name)

    fun hardware(codec: MediaCodec): Boolean? = try {
        if (Build.VERSION.SDK_INT >= 29) codec.codecInfo.isHardwareAccelerated else !avcSoftwareName(codec.name)
    } catch (_: Exception) { null }

    private fun lowLatencyDeclared(codec: MediaCodec): Boolean = try {
        Build.VERSION.SDK_INT >= 30 && codec.codecInfo.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
            .isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)
    } catch (_: Exception) { false }

    /**
     * Creates, configures (asynchronous callbacks on [handler]) and starts a
     * decoder; null when none started. With it, what it was configured with.
     *
     * With [sps]/[pps] (at the key frame) every candidate is tried in order.
     * Without them (ahead of the key frame, parameter sets follow as
     * codec-config input) only the preferred decoder is: if it refuses, the
     * key frame starts it with them instead of another, slower decoder.
     */
    fun start(
        width: Int,
        height: Int,
        sps: ByteArray?,
        pps: ByteArray?,
        surface: Surface,
        callback: MediaCodec.Callback,
        handler: Handler,
        fps: Int,
        allowHardware: Boolean,
        memoryBudgetBytes: Long,
        exact: Boolean,
    ): AvcStarted? {
        val early = sps == null || pps == null
        val requested = StreamSizeRequest(listOf(StreamSizeTrack(width, height, fps)), allowHardware)
        val memory = streamSizeMemoryBytes(requested.tracks) ?: return null
        if (memory > memoryBudgetBytes || memoryBudgetBytes <= 0) return null
        val names = if (exact) {
            val profile = AvcStreamSizes.profile(sps) ?: return null
            AvcStreamSizes.candidates(allowHardware, profile).filter {
                validateStreamSizeRequest(requested, listOf(it), memoryBudgetBytes)["accepted"] == true
            }.map { it.name }
        } else {
            if (!legacyStreamSize(requested.tracks.single())) return null
            candidates().filter { name -> allowHardware || list().firstOrNull { it.name == name }?.hardware == false }
        }
        val attempts: List<String?> = when {
            // Only the old bounded ABI retains its actual default-decoder
            // attempt. It is never a v2 exact-capability success or large-frame
            // permission, and an explicit software choice stays software-only.
            !exact && names.isEmpty() && allowHardware -> listOf(null)
            names.isEmpty() -> emptyList()
            early -> if (startsEarly(names.first())) listOf(names.first()) else emptyList()
            else -> names
        }
        for (name in attempts) {
            val codec = try {
                if (name == null) MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
                else MediaCodec.createByCodecName(name)
            } catch (_: Exception) {
                if (name != null && early) failures.needsFormatParameterSets(name)
                continue
            }
            configureAndStart(codec, width, height, sps, pps, surface, callback, handler, fps)?.let { return it }
            try { codec.release() } catch (_: Exception) {}
            if (name != null && early) failures.needsFormatParameterSets(name)
        }
        return null
    }

    /** A tuning the decoder refuses at configure or at start (e.g. a rate it cannot meet) gives way to the next. */
    private fun configureAndStart(
        codec: MediaCodec,
        width: Int,
        height: Int,
        sps: ByteArray?,
        pps: ByteArray?,
        surface: Surface,
        callback: MediaCodec.Callback,
        handler: Handler,
        fps: Int,
    ): AvcStarted? {
        val name = codec.name
        val vendor = AvcVendorKeys.of(name)
        val declared = lowLatencyDeclared(codec)
        // A decoder that failed before its first frame with a tuned format gets the plain one.
        val plainOnly = failures.plainOnly(name)
        val tuning = avcConfigureFirst(avcTuningsFor(Build.VERSION.SDK_INT, declared, vendor != null, plainOnly)) { attempt, rung ->
            if (attempt > 0) codec.reset()
            codec.setCallback(callback, handler)
            codec.configure(format(width, height, sps, pps, fps, avcTuningKeys(rung, vendor?.keys.orEmpty(), fps)), surface, null, 0)
            codec.start()
        } ?: return null
        return AvcStarted(codec, avcTuningFlags(tuning, declared), vendor?.code ?: 0, plainOnly)
    }

    // Real time: decode each frame as it arrives (at the clocks a 60 fps
    // stream needs) rather than for battery-saving throughput ([tuning]).
    private fun format(width: Int, height: Int, sps: ByteArray?, pps: ByteArray?, fps: Int, tuning: Map<String, Int>) =
        MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            if (sps != null && pps != null) {
                setByteBuffer("csd-0", ByteBuffer.wrap(sps))
                setByteBuffer("csd-1", ByteBuffer.wrap(pps))
            }
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 4194304)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            for ((key, value) in tuning) setInteger(key, value)
        }

}

/**
 * A started decoder and what it was configured with: [lowLatency] is
 * avcTuningFlags, [vendorKeys] the AvcVendorKeys code its name matched (0 none),
 * [plainFallback] true when it got the plain format because it failed tuned.
 */
internal class AvcStarted(val codec: MediaCodec, val lowLatency: Int, val vendorKeys: Int, val plainFallback: Boolean = false)
