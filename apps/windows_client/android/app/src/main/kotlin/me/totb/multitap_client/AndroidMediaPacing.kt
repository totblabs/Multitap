package me.totb.multitap_client

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlin.math.roundToInt

// Pure policies of the Android media engine (no Android types), so the JVM
// check android/test/AndroidMediaPacingCheck.kt runs this production code.
// AndroidMedia.kt applies them to MediaCodec, AudioTrack and the texture.

/** Where the pacer sends a decoded output buffer: rendered to the texture or discarded. */
internal interface AndroidFrameSink {
    fun render(frame: Int)
    fun discard(frame: Int)
}

/**
 * Presentation pacing, the Android counterpart of the Windows client's
 * texture-pacing-v1. Flutter takes one frame from a SurfaceTexture per raster
 * frame, and only after the texture was marked dirty by a newly queued buffer.
 * A buffer rendered while an earlier one still waits can sit behind it (the
 * texture then shows a frame one or more behind the newest until enough newer
 * frames arrive), so at most one rendered frame waits for Flutter here:
 *
 * - a decoded frame is rendered at once when Flutter has taken the previous
 *   one;
 * - otherwise it is held in the codec; a newer decoded frame replaces the
 *   held one (the older is discarded and counted as skipped);
 * - when Flutter takes the waiting frame, the held (newest) frame is rendered.
 *
 * The newest frame is never discarded, so the last picture of a burst always
 * reaches the screen. If Flutter has taken no frame of this decoder within
 * [unconsumedLimitNanos] of the first one (no frame-consumed callbacks), the
 * pacer stops holding and renders every frame as it is decoded (the earlier
 * behaviour) instead of freezing the picture.
 *
 * In the [background] (the app is not visible, so Flutter takes nothing) no
 * frame is rendered at all, and the pacer never stops holding: every decoded
 * frame replaces the held one. [foreground] renders the held (newest) frame
 * at once, or, while a frame Flutter has not taken still waits, as soon as
 * that one is taken. A decoder made in the background (a resume or a resize)
 * therefore no longer renders every frame to a texture nobody draws.
 */
internal class AndroidFramePacer(private val unconsumedLimitNanos: Long = UNCONSUMED_LIMIT_NANOS) {
    /** A rendered frame Flutter has not taken yet. */
    private var waiting = false
    private var waitingDecodedAt = 0L
    private var firstRenderedAt = -1L
    private var held = -1
    private var heldDecodedAt = 0L

    /** False once the decoder fell back to rendering every frame at once. */
    var gated = true
        private set
    /** The app is not visible: nothing is rendered (see [background]). */
    var hidden = false
        private set
    /** Frames Flutter took from the texture (frame-consumed callbacks). */
    var taken = 0L
        private set
    var rendered = 0L
        private set
    /** Decoded frames that were never rendered (replaced by a newer one). */
    var skipped = 0L
        private set
    /** Decoded frames that had to wait behind a frame Flutter had not taken. */
    var queued = 0L
        private set
    /** Decoded -> taken by Flutter, summed over taken frames. */
    var waitTotalNanos = 0L
        private set
    var waitMaxNanos = 0L
        private set

    /** Known decoded buffers waiting for a texture take; unknown without callback gating. */
    val presentQueuePackets: Int?
        get() = if (gated) (if (waiting) 1 else 0) + (if (held >= 0) 1 else 0) else null

    /** Oldest decoded buffer's age, not physical presentation latency. No pacing changes. */
    fun presentQueueOldestNanos(now: Long): Long? {
        if (!gated) return null
        val oldest = when {
            waiting && held >= 0 -> minOf(waitingDecodedAt, heldDecodedAt)
            waiting -> waitingDecodedAt
            held >= 0 -> heldDecodedAt
            else -> return 0L
        }
        return (now - oldest).coerceAtLeast(0)
    }

    fun decoded(frame: Int, now: Long, sink: AndroidFrameSink) {
        if (!hidden && (!gated || !waiting)) {
            show(frame, now, now, sink)
            return
        }
        queued++
        val older = held
        held = frame
        heldDecodedAt = now
        if (older >= 0) {
            skipped++
            sink.discard(older)
        }
    }

    /** Flutter took the frame rendered last (its raster thread latched the texture). */
    fun consumed(now: Long, sink: AndroidFrameSink) {
        if (!waiting) return
        waiting = false
        taken++
        val wait = (now - waitingDecodedAt).coerceAtLeast(0)
        waitTotalNanos += wait
        if (wait > waitMaxNanos) waitMaxNanos = wait
        val next = held
        if (next >= 0 && !hidden) {
            held = -1
            show(next, heldDecodedAt, now, sink)
        }
    }

    /** The app is no longer visible: from now on decoded frames are held, never rendered. */
    fun background() { hidden = true }

    /** Visible again: the newest held frame is rendered now, or when the frame still waiting is taken. */
    fun foreground(now: Long, sink: AndroidFrameSink) {
        if (!hidden) return
        hidden = false
        // Flutter took nothing while hidden: its time to take a first frame starts again.
        if (firstRenderedAt >= 0) firstRenderedAt = now
        val next = held
        if (next >= 0 && (!gated || !waiting)) {
            held = -1
            show(next, heldDecodedAt, now, sink)
        }
    }

    /** When [check] has something to decide, or -1. */
    fun checkAt(): Long =
        if (!hidden && gated && taken == 0L && held >= 0 && firstRenderedAt >= 0) firstRenderedAt + unconsumedLimitNanos else -1

    /** True when it stopped holding frames (Flutter never took one in time). */
    fun check(now: Long, sink: AndroidFrameSink): Boolean {
        val due = checkAt()
        if (due < 0 || now < due) return false
        gated = false
        val next = held
        held = -1
        if (next >= 0) show(next, heldDecodedAt, now, sink)
        return true
    }

    /** Forgets every frame (the codec was stopped; its buffer indices are gone). */
    fun reset() {
        waiting = false
        held = -1
        firstRenderedAt = -1
    }

    private fun show(frame: Int, decodedAt: Long, now: Long, sink: AndroidFrameSink) {
        if (firstRenderedAt < 0) firstRenderedAt = now
        waiting = true
        waitingDecodedAt = decodedAt
        rendered++
        sink.render(frame)
    }

    companion object {
        /** A first frame normally shows within ~100 ms (a resize switches textures then). */
        const val UNCONSUMED_LIMIT_NANOS = 1_500_000_000L
    }
}

/**
 * Decoder latency: queued to the codec -> its output buffer, by presentation
 * time; and decoder utilisation: [busyNanos] is the wall time during which at
 * least one input was inside the codec. The per-frame latencies of a
 * pipelined decoder overlap, so their sum is NOT a utilisation (it can pass
 * the elapsed time); the union in [busyNanos] cannot. Each idle-to-event gap
 * counts at most [MAX_GAP_NANOS] (an input a codec never outputs must not
 * look like busy time forever).
 */
internal class AndroidDecodeClock(capacity: Int = 32) {
    private val pts = LongArray(capacity)
    private val at = LongArray(capacity)
    private val arrived = LongArray(capacity)
    private var head = 0
    private var size = 0
    private var edge = 0L

    /** Wall time with at least one input inside the codec, since construction. */
    var busyNanos = 0L
        private set

    /**
     * When the packet of the frame [decoded] matched last reached the engine
     * (the [queued] arrival), -1 when it matched none: with the decode time,
     * how long a frame took from its arrival to its decoded picture, the wait
     * for a free decoder input included.
     */
    var lastArrival = -1L
        private set

    private fun advance(now: Long) {
        if (size > 0) busyNanos += (now - edge).coerceIn(0, MAX_GAP_NANOS)
        edge = now
    }

    /** [arrivedAt]: when the packet was accepted from the session or the UDP leg. */
    fun queued(presentationUs: Long, now: Long, arrivedAt: Long = now) {
        advance(now)
        if (size == pts.size) { head = (head + 1) % pts.size; size-- }
        val slot = (head + size) % pts.size
        pts[slot] = presentationUs
        at[slot] = now
        arrived[slot] = arrivedAt
        size++
    }

    /**
     * Nanoseconds since the matching input was queued, or -1. Inputs before it
     * that produced no output are forgotten.
     */
    fun decoded(presentationUs: Long, now: Long): Long {
        advance(now)
        lastArrival = -1
        while (size > 0) {
            val first = pts[head]
            if (first > presentationUs) return -1
            val queuedAt = at[head]
            val arrivedAt = arrived[head]
            head = (head + 1) % pts.size
            size--
            if (first == presentationUs) { lastArrival = arrivedAt; return (now - queuedAt).coerceAtLeast(0) }
        }
        return -1
    }

    fun clear() { head = 0; size = 0 }

    /** A new stream: utilisation counts from zero again (as the other totals). */
    fun resetBusy() { busyNanos = 0 }

    companion object {
        const val MAX_GAP_NANOS = 1_000_000_000L
    }
}

/**
 * Mean and maximum of what was added during the last [windowNanos] that had
 * any value, for statistics a person reads on the device: a lifetime mean
 * hides a change and a lifetime maximum never comes down. A window closes at
 * the first [add] or read after its end; a window without values (a still
 * picture sends no frames) keeps the one before it.
 */
internal class RecentWindow(private val windowNanos: Long = 1_000_000_000L) {
    private var start = -1L
    private var count = 0
    private var sum = 0L
    private var max = 0L
    private var lastCount = 0
    private var lastSum = 0L
    private var lastMax = 0L

    fun add(valueNanos: Long, now: Long) {
        roll(now)
        val value = valueNanos.coerceAtLeast(0)
        count++; sum += value
        if (value > max) max = value
    }

    /** Mean of the last window with values, in milliseconds; null before one closed. */
    fun meanMs(now: Long): Double? { roll(now); return if (lastCount == 0) null else lastSum / lastCount.toDouble() / 1e6 }

    /** Its largest value, in milliseconds. */
    fun maxMs(now: Long): Double? { roll(now); return if (lastCount == 0) null else lastMax / 1e6 }

    fun reset() { start = -1; count = 0; sum = 0; max = 0; lastCount = 0; lastSum = 0; lastMax = 0 }

    private fun roll(now: Long) {
        if (start < 0) { start = now; return }
        if (now - start < windowNanos) return
        if (count > 0) { lastCount = count; lastSum = sum; lastMax = max }
        count = 0; sum = 0; max = 0; start = now
    }
}

/**
 * Packet buffers reused across packets (a relay video packet is up to 4 MiB;
 * a fresh array per packet is large-object garbage at 100+ packets/s).
 * Power-of-two classes, bounded retained bytes; any thread.
 */
internal class AndroidBufferPool(
    private val maxRetainedBytes: Long = 8L * 1024 * 1024,
    private val maxPerClass: Int = 6,
) {
    private val classes = Array(MAX_CLASS + 1) { ArrayDeque<ByteArray>() }
    private var retained = 0L

    @Synchronized fun take(size: Int): ByteArray {
        require(size in 0..(1 shl MAX_CLASS))
        val index = sizeClass(size)
        val free = classes[index].removeLastOrNull() ?: return ByteArray(1 shl index)
        retained -= free.size
        return free
    }

    @Synchronized fun give(buffer: ByteArray) {
        val index = sizeClass(buffer.size)
        if (buffer.size != 1 shl index) return
        val free = classes[index]
        if (free.size >= maxPerClass || retained + buffer.size > maxRetainedBytes) return
        free.addLast(buffer)
        retained += buffer.size
    }

    @get:Synchronized val retainedBytes: Long get() = retained

    companion object {
        const val MIN_CLASS = 12   // 4 KiB (an audio packet is ~1.9 KiB)
        const val MAX_CLASS = 22   // 4 MiB, the largest relay media packet
        fun sizeClass(size: Int): Int {
            var index = MIN_CLASS
            while ((1 shl index) < size) index++
            return index
        }
    }
}

/**
 * media-wire-v1: one media packet from the Dart session on the binary channel
 * `multitap/remote/media` (answered off the main thread). Little endian:
 * [0] version 1, [1] flags (bit 0: input-latency-v1 watched video packet,
 * bit 1: udp-media-v1 keys are done, so the packet follows the path rule),
 * [2..7] zero, [8..15] lease, [16..] the media-v1 packet (13 .. 4 MiB).
 * The answer is one status byte ([AndroidMediaStatus]).
 */
internal class AndroidMediaWire(val lease: Long, val watch: Boolean, val offset: Int, val length: Int, val path: Boolean = false)

internal object AndroidMediaStatus {
    const val OK: Byte = 0
    const val QUEUE_FULL: Byte = 1
    const val SESSION_REPLACED: Byte = 2
    const val DECODER_FAILED: Byte = 3
    const val INVALID_PACKET: Byte = 4
}

internal const val MEDIA_WIRE_HEADER = 16

/**
 * The decoder's bounded admission (AndroidMedia.offer), any thread. Video has
 * the Windows engine's queue: [videoPackets] packets and [videoBytes] bytes,
 * those waiting for MediaCodec input included. Audio has its own [audioPackets],
 * as the Windows engine never queues audio behind video (remote_engine.cpp
 * Offer): 10 ms chunks over UDP must not take the slots a key frame needs.
 * A full share answers media_queue_full. The check and the count are separate
 * steps, so concurrent offers can pass the limit by one each.
 */
internal class AndroidMediaAdmission(private val videoPackets: Int, private val videoBytes: Long, private val audioPackets: Int) {
    private val video = AtomicInteger()
    private val audio = AtomicInteger()
    private val videoQueued = AtomicLong()
    private val audioQueued = AtomicLong()

    fun full(isVideo: Boolean, length: Int) =
        if (isVideo) video.get() >= videoPackets || videoQueued.get() + length > videoBytes else audio.get() >= audioPackets
    fun added(isVideo: Boolean, length: Int) {
        if (isVideo) { video.incrementAndGet(); videoQueued.addAndGet(length.toLong()) } else { audio.incrementAndGet(); audioQueued.addAndGet(length.toLong()) }
    }
    fun removed(isVideo: Boolean, length: Int) {
        if (isVideo) { video.decrementAndGet(); videoQueued.addAndGet(-length.toLong()) } else { audio.decrementAndGet(); audioQueued.addAndGet(-length.toLong()) }
    }
    /** Both kinds, for the statistics. */
    val packets get() = (video.get() + audio.get()).coerceAtLeast(0)
    val bytes get() = (videoQueued.get() + audioQueued.get()).coerceAtLeast(0)
}

internal fun androidMediaWire(message: ByteBuffer): AndroidMediaWire? {
    val base = message.position()
    val size = message.remaining()
    if (size < MEDIA_WIRE_HEADER + 13 || size > MEDIA_WIRE_HEADER + 4194304) return null
    val view = message.duplicate().order(ByteOrder.LITTLE_ENDIAN)
    if (view.get(base).toInt() != 1) return null
    val flags = view.get(base + 1).toInt()
    if (flags and 3.inv() != 0) return null
    for (index in 2..7) if (view.get(base + index).toInt() != 0) return null
    val lease = view.getLong(base + 8)
    if (lease <= 0) return null
    val watch = flags and 1 != 0
    // A watch applies to video (kind 1) only, as the Dart session sends it.
    if (watch && view.get(base + MEDIA_WIRE_HEADER).toInt() != 1) return null
    return AndroidMediaWire(lease, watch, base + MEDIA_WIRE_HEADER, size - MEDIA_WIRE_HEADER, flags and 2 != 0)
}

/**
 * The media-v1 packet rules the decoder relies on: kind 1 (H.264, flags bit
 * 0 = key frame) or 2 (audio, flags 0), reserved bytes zero, a non-negative
 * little-endian PTS. Audio is 48 kHz s16 stereo PCM in whole frames, or with
 * [opus] (a stream granted audio-opus-v1 whose stream.info said "opus") one
 * sequence-numbered 10 ms Opus packet (AndroidOpus.payloadValid).
 */
internal fun androidMediaPacketValid(bytes: ByteArray, length: Int, opus: Boolean = false): Boolean {
    if (length < 13 || length > bytes.size) return false
    if (bytes[2].toInt() != 0 || bytes[3].toInt() != 0) return false
    if (bytes[11].toInt() < 0) return false   // PTS sign bit
    return when (bytes[0].toInt()) {
        1 -> bytes[1].toInt() and 1.inv() == 0
        2 -> bytes[1].toInt() == 0 &&
            (if (opus) AndroidOpus.payloadValid(bytes, 12, length - 12) else (length - 12) % 4 == 0)
        else -> false
    }
}

internal fun androidMediaPts(bytes: ByteArray): Long =
    ByteBuffer.wrap(bytes, 4, 8).order(ByteOrder.LITTLE_ENDIAN).long

/** The NAL units of bytes[from until] (Annex B), each with a 4-byte start code. */
internal fun annexBUnits(bytes: ByteArray, from: Int, until: Int = bytes.size): List<ByteArray> {
    val end = minOf(until, bytes.size)
    val starts = mutableListOf<Pair<Int, Int>>()
    var index = from
    while (index + 3 < end) {
        if (bytes[index].toInt() == 0 && bytes[index + 1].toInt() == 0) {
            if (bytes[index + 2].toInt() == 1) { starts.add(index to 3); index += 3; continue }
            if (bytes[index + 2].toInt() == 0 && bytes[index + 3].toInt() == 1) { starts.add(index to 4); index += 4; continue }
        }
        index++
    }
    return starts.mapIndexed { at, pair ->
        val stop = if (at + 1 < starts.size) starts[at + 1].first else end
        byteArrayOf(0, 0, 0, 1) + bytes.copyOfRange(pair.first + pair.second, stop)
    }
}

/**
 * Vendor decoder keys that ask for output in decode order without delay (the
 * Host sends no B-frames), by decoder name. Keys, values and name prefixes are
 * the ones Moonlight-Android sets (MediaCodecHelper.setDecoderLowLatencyOptions
 * and its *DecoderPrefixes); MediaTek's is not a vendor extension but an
 * option of MediaTek's own ACodec (OMX), which Moonlight sets on every decoder.
 * [code] is the stream stats `decoderVendorKeys` value.
 */
internal enum class AvcVendorKeys(val code: Int, private val prefixes: List<String>, val keys: Map<String, Int>) {
    QUALCOMM(1, listOf("c2.qti.", "omx.qcom."),
        mapOf("vendor.qti-ext-dec-picture-order.enable" to 1, "vendor.qti-ext-dec-low-latency.enable" to 1)),
    EXYNOS(2, listOf("c2.exynos.", "omx.exynos."), mapOf("vendor.rtc-ext-dec-low-latency.enable" to 1)),
    KIRIN(3, listOf("c2.hisi.", "omx.hisi."), mapOf(
        "vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-req" to 1,
        "vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-rdy" to -1)),
    AMLOGIC(4, listOf("c2.amlogic.", "omx.amlogic."), mapOf("vendor.low-latency.enable" to 1)),
    MEDIATEK(5, listOf("c2.mtk.", "omx.mtk."), mapOf("vdec-lowlatency" to 1));

    companion object {
        fun of(decoder: String): AvcVendorKeys? {
            val name = decoder.lowercase()
            return values().firstOrNull { vendor -> vendor.prefixes.any { name.startsWith(it) } }
        }
    }
}

internal fun avcVendorLowLatencyKeys(decoder: String): Map<String, Int> = AvcVendorKeys.of(decoder)?.keys ?: emptyMap()

/**
 * True when the access unit's first slice is a non-reference picture
 * (nal_ref_idc 0): no later frame depends on it, so it may be dropped while
 * the decoder is behind. The Host's x264 stream normally has none; IDR and
 * reference slices are never dropped. Scans at most [limit] bytes.
 */
internal fun h264NonReference(bytes: ByteArray, from: Int, until: Int, limit: Int = 4096): Boolean {
    val end = minOf(until, bytes.size, from + limit)
    var index = from
    while (index + 3 < end) {
        if (bytes[index].toInt() == 0 && bytes[index + 1].toInt() == 0 && bytes[index + 2].toInt() == 1) {
            val header = bytes[index + 3].toInt()
            val type = header and 31
            if (type == 5) return false
            if (type in 1..4) return header shr 5 and 3 == 0
            index += 3
            continue
        }
        index++
    }
    return false
}

/**
 * Packets wait for a decoder input buffer only while the decoder is behind.
 * Then queued non-reference video is dropped (oldest first, never the newest)
 * instead of decoding pictures that are already stale. Returns the dropped
 * count; [drop] receives each removed packet.
 */
internal inline fun <T> dropStaleNonReference(
    queue: ArrayDeque<T>,
    now: Long,
    accepted: (T) -> Long,
    nonReference: (T) -> Boolean,
    drop: (T) -> Unit,
    depth: Int = 3,
    ageNanos: Long = 50_000_000L,
): Int {
    if (queue.size < 2) return 0
    if (queue.size < depth && now - accepted(queue.first()) < ageNanos) return 0
    var dropped = 0
    var index = 0
    while (index < queue.size - 1) {
        val packet = queue[index]
        if (nonReference(packet)) {
            queue.removeAt(index)
            drop(packet)
            dropped++
        } else {
            index++
        }
    }
    return dropped
}

/**
 * Bounded, adaptive audio latency. The Host sends 10 ms PCM packets through the
 * relay; a burst after a network stall leaves extra audio queued for good unless
 * some is dropped (and clock drift between the VM and the phone adds up the
 * same way). Over each [windowNanos] the lowest queued level is the standing
 * latency, the rest is jitter cushion: when even that lowest level stays above
 * the current target, one packet is dropped. Above the current cap packets are
 * dropped outright.
 *
 * The target is the jitter cushion and adapts: it starts at [targetFrames],
 * grows by [growFrames] whenever the track reports a new underrun (a gap in
 * the sound is worse than a little delay) up to [maxTargetFrames], and shrinks
 * by [shrinkFrames] after each [shrinkAfterNanos] without an underrun, back to
 * [targetFrames]. The cap follows the target by the same headroom as the
 * initial cap and never exceeds [capacityFrames] (the track's buffer). With
 * [maxTargetFrames] equal to [targetFrames] the thresholds are fixed.
 *
 * The track also underruns when the VM goes silent (the Host's WASAPI
 * loopback then sends nothing). Given the packet's PTS, an underrun whose
 * next packet does not go on from where the sound ended (more than
 * [gapMicros] later) is a pause of the sound: counted in [silences], and the
 * target stays (audio-stats-v1, as the Windows client's AudioJitterBuffer).
 *
 * The start of each stream goes through [start] first (audio-start-cut-v1):
 * the audio that waited while the stream was set up arrives at once, and
 * written as it came it filled the track to the cap with its oldest part and
 * the rest was refused packet by packet (6-14 overruns and 60-140 ms lost at
 * every Android start, 2026-11-02), leaving the stale part queued for ~8 s of
 * one-packet trims. [start] holds packets until one arrives on time (at least
 * half a packet after the one before) with [target] frames held, keeping only
 * the newest [target]: the older backlog is dropped in one cut, counted as
 * one overrun as on Windows. That packet then goes through [accept] on top of
 * the held audio, so the standing level starts at the target.
 */
internal class AndroidAudioTrim(
    private val targetFrames: Long = 48L * 40,
    private val capFrames: Long = 48L * 80,
    private val windowNanos: Long = 2_000_000_000L,
    private val maxTargetFrames: Long = targetFrames,
    private val capacityFrames: Long = Long.MAX_VALUE,
    private val growFrames: Long = 48L * 20,
    private val shrinkFrames: Long = 48L * 10,
    private val shrinkAfterNanos: Long = 8_000_000_000L,
    private val gapMicros: Long = 50_000L,
) {
    private var windowStart = -1L
    private var lowest = Long.MAX_VALUE
    private var lastUnderruns = -1L
    private var calmSince = -1L
    private var endPts = -1L
    var trimmed = 0L
        private set
    /** audio-stats-v1: frames the standing-level trim dropped. */
    var trimmedFrames = 0L
        private set
    /** Packets refused above the cap, and underruns the sound went on through / paused at. */
    var overruns = 0L
        private set
    var underruns = 0L
        private set
    var silences = 0L
        private set
    /** Current jitter cushion in frames (between the initial and the maximum target). */
    var target = targetFrames
        private set
    /** The cap the target asks for: the target plus the initial headroom, within the track's capacity. */
    val wantedCap get() = minOf(target + (capFrames - targetFrames), maxOf(capacityFrames, capFrames))
    /**
     * The level the track needs queued before it plays (its start threshold,
     * or before API 31 its whole effective buffer), as the track reported it.
     * The cap never stays below it: a track that waits for more than may be
     * queued never starts (2026-11-11, audio-path-v1).
     */
    var startLevel = 0L
    val cap get() = maxOf(wantedCap, startLevel)
    var grown = 0L
        private set

    /** True until [start] has let the stream's sound begin; [reset] starts over. */
    var starting = true
        private set
    /** The held start of the stream; [start] says how many bytes of it to write. */
    var held = ByteArray(0)
        private set
    private var heldBytes = 0
    private var heldAt = -1L
    private var heldLastFrames = 0
    private var cutThisStart = false
    /** Frames the start cuts dropped (each cut is also one of [overruns]). */
    var cutFrames = 0L
        private set

    /**
     * One packet's PCM ([pcm] from [offset], [length] bytes of whole frames)
     * that arrived at [now] while [starting]. -1: the packet is held (write
     * nothing). Otherwise the start is over: write that many bytes of [held]
     * first (the newest [target] frames before this packet), then hand this
     * packet to [accept] as usual.
     */
    fun start(pcm: ByteArray, offset: Int, length: Int, now: Long): Int {
        check(starting)
        val keep = (target * 4).toInt()
        val onTime = heldAt >= 0 && now - heldAt >= heldLastFrames * NANOS_PER_FRAME / 2
        if (onTime && heldBytes >= keep) {
            starting = false
            heldAt = -1
            return heldBytes.also { heldBytes = 0 }
        }
        if (held.size < keep) held = held.copyOf(keep)
        if (length >= keep) {
            cut(heldBytes + length - keep)
            System.arraycopy(pcm, offset + length - keep, held, 0, keep)
            heldBytes = keep
        } else {
            val excess = heldBytes + length - keep
            if (excess > 0) {
                System.arraycopy(held, excess, held, 0, heldBytes - excess)
                heldBytes -= excess
                cut(excess)
            }
            System.arraycopy(pcm, offset, held, heldBytes, length)
            heldBytes += length
        }
        heldAt = now
        heldLastFrames = length / 4
        return -1
    }

    private fun cut(bytes: Int) {
        if (bytes <= 0) return
        cutFrames += bytes / 4
        if (!cutThisStart) { cutThisStart = true; overruns++ }
    }

    /**
     * [underruns] is the track's cumulative underrun count (0 where unknown);
     * [ptsMicros] the packet's Host capture-clock PTS (-1 where unknown: every
     * underrun then counts).
     */
    fun accept(queuedFrames: Long, packetFrames: Int, now: Long, underruns: Long = 0, ptsMicros: Long = -1): Boolean {
        adapt(underruns, now, ptsMicros)
        if (ptsMicros >= 0) endPts = ptsMicros + packetFrames * 1000L / 48
        val queued = queuedFrames.coerceAtLeast(0)
        if (queued + packetFrames > cap) { overruns++; return false }
        if (windowStart < 0) windowStart = now
        if (queued < lowest) lowest = queued
        if (now - windowStart < windowNanos) return true
        val standing = lowest
        windowStart = now
        lowest = Long.MAX_VALUE
        if (standing > target) {
            trimmed++
            trimmedFrames += packetFrames
            return false
        }
        return true
    }

    private fun adapt(underruns: Long, now: Long, ptsMicros: Long) {
        if (calmSince < 0) calmSince = now
        if (lastUnderruns < 0) { lastUnderruns = underruns; return }
        if (underruns > lastUnderruns) {
            lastUnderruns = underruns
            if (ptsMicros >= 0 && endPts >= 0 && ptsMicros > endPts + gapMicros) { silences++; return }
            this.underruns++
            calmSince = now
            if (target < maxTargetFrames) { target = minOf(maxTargetFrames, target + growFrames); grown++ }
            return
        }
        if (target > targetFrames && now - calmSince >= shrinkAfterNanos) {
            target = maxOf(targetFrames, target - shrinkFrames)
            calmSince = now
        }
    }

    fun reset() {
        windowStart = -1; lowest = Long.MAX_VALUE; lastUnderruns = -1; calmSince = -1; endPts = -1; target = targetFrames
        starting = true; heldBytes = 0; heldAt = -1; heldLastFrames = 0; cutThisStart = false
    }

    companion object {
        /** 48 kHz. */
        const val NANOS_PER_FRAME = 1_000_000_000L / 48_000
    }
}

/**
 * Whether the AudioTrack's effective buffer (setBufferSizeInFrames) follows
 * [AndroidAudioTrim.wantedCap] while the stream plays, instead of being the
 * track's whole capacity.
 *
 * Before API 31 a streaming track that is not on the fast mixer path starts,
 * and starts again after it ran dry, only once that whole effective buffer is
 * filled (AudioFlinger Track::isReady; AudioTrack.play() documents the
 * "minimum level", and API 31 added setStartThresholdInFrames to set it apart
 * from the buffer). The buffer must then be what the trim lets be queued, or
 * the track has nothing to start it. The same holds on API 31 and later when
 * the start threshold could not be set.
 *
 * A buffer set to the trim's initial cap (80 ms) once, at the track's start,
 * is not enough. The trim's target grows after underruns up to that same
 * 80 ms, so on Android 8-11 the headroom between target and cap would go to
 * nothing: every burst of packets above it is dropped (a 10 ms hole each),
 * the track runs dry and waits for a full buffer again. So the buffer follows
 * the cap as the target grows and shrinks, and the cap keeps its headroom.
 */
internal fun audioBufferFollowsCap(api: Int, startThresholdApplied: Boolean): Boolean =
    api < 31 || !startThresholdApplied

/**
 * A track that is written to and does not play: its played position stays
 * the same for [stuckNanos] while as much is queued as the trim allows
 * ([full]). AndroidMedia then makes the track again with its buffer
 * following the cap (2026-11-11): whatever level the platform waits for, the
 * new track can reach it. A silent VM writes nothing and is never seen here.
 */
internal class AudioStartWatch(private val stuckNanos: Long = 1_000_000_000L) {
    private var played = -1L
    private var since = -1L

    fun reset() { played = -1; since = -1 }

    /** One write at [now]: the track's played position and whether the queue is at the cap. */
    fun stuck(now: Long, played: Long, full: Boolean): Boolean {
        if (played != this.played || !full || since < 0) { this.played = played; since = now; return false }
        return now - since >= stuckNanos
    }
}

/** One H.264 decoder of this device as the ordering policy sees it. */
internal class AvcDecoderCandidate(val name: String, val hardware: Boolean)

/** Software decoder names, for releases without MediaCodecInfo.isHardwareAccelerated (API < 29). */
internal fun avcSoftwareName(name: String): Boolean {
    val lower = name.lowercase()
    return lower.startsWith("omx.google.") || lower.startsWith("c2.android.") ||
        lower.startsWith("omx.ffmpeg.") || lower.startsWith("c2.ffmpeg.") || lower.contains(".sw.")
}

/**
 * The order H.264 decoders are tried in: hardware first, each group in the
 * platform's own order; never a secure-only variant (it needs a protected
 * surface); a decoder that failed earlier in this app run goes last, so a
 * device whose hardware decoder fails falls back to the next one (software
 * last of all) on the next start.
 */
internal fun orderAvcDecoders(candidates: List<AvcDecoderCandidate>, failed: Set<String>): List<String> {
    val usable = candidates.filter { !it.name.lowercase().endsWith(".secure") }
        .distinctBy { it.name }
    return usable.filter { it.hardware && it.name !in failed }.map { it.name } +
        usable.filter { !it.hardware && it.name !in failed }.map { it.name } +
        usable.filter { it.name in failed }.map { it.name }
}

/**
 * One decoder configuration attempt: KEY_LOW_LATENCY, real-time priority
 * (KEY_PRIORITY 0), an operating rate, and the vendor low-latency keys.
 */
internal data class AvcTuning(
    val lowLatency: Boolean,
    val priority: Boolean,
    val operatingRate: Boolean,
    val vendor: Boolean,
)

/**
 * Configuration attempts, most latency-reducing first. KEY_LOW_LATENCY is
 * asked for on API 30+ even where the decoder does not declare the feature.
 * A decoder that refuses an attempt (configure throws) is configured again
 * with the next: without the vendor keys, then the released preview.8
 * settings (KEY_LOW_LATENCY only where declared, priority 0), then plain.
 */
internal fun avcTunings(api: Int, lowLatencyDeclared: Boolean, vendorKeys: Boolean): List<AvcTuning> {
    val lowLatency = api >= 30
    return listOf(
        AvcTuning(lowLatency, priority = true, operatingRate = true, vendor = vendorKeys),
        AvcTuning(lowLatency, priority = true, operatingRate = true, vendor = false),
        AvcTuning(lowLatency && lowLatencyDeclared, priority = true, operatingRate = false, vendor = false),
        AvcTuning(lowLatency = false, priority = false, operatingRate = false, vendor = false),
    ).distinct()
}

/**
 * The integer format keys one attempt adds to size, parameter sets, input size
 * and frame rate (MediaFormat.KEY_PRIORITY 0 = real time, KEY_OPERATING_RATE
 * at the stream's own rate as ExoPlayer/Media3 sets it, KEY_LOW_LATENCY).
 */
internal fun avcTuningKeys(tuning: AvcTuning, vendorKeys: Map<String, Int>, fps: Int): Map<String, Int> {
    val keys = LinkedHashMap<String, Int>()
    if (tuning.priority) keys["priority"] = 0
    if (tuning.operatingRate) keys["operating-rate"] = fps
    if (tuning.lowLatency) keys["low-latency"] = 1
    if (tuning.vendor) keys.putAll(vendorKeys)
    return keys
}

/**
 * Stream stats `decoderLowLatency`: what the started decoder was configured
 * with. Bits: 1 KEY_LOW_LATENCY, 2 real-time priority, 4 operating rate,
 * 8 vendor low-latency keys, 16 the decoder declares FEATURE_LowLatency.
 */
internal fun avcTuningFlags(tuning: AvcTuning, lowLatencyDeclared: Boolean): Int =
    (if (tuning.lowLatency) 1 else 0) or (if (tuning.priority) 2 else 0) or (if (tuning.operatingRate) 4 else 0) or
        (if (tuning.vendor) 8 else 0) or (if (lowLatencyDeclared) 16 else 0)

/**
 * Configures with the first tuning that [attempt] (configure and start) does
 * not throw for: a key the decoder refuses costs one attempt, never the
 * decoder. The tuning used, or null when every attempt threw.
 */
internal inline fun avcConfigureFirst(tunings: List<AvcTuning>, attempt: (index: Int, tuning: AvcTuning) -> Unit): AvcTuning? {
    for ((index, tuning) in tunings.withIndex()) {
        try {
            attempt(index, tuning)
            return tuning
        } catch (_: Exception) {}
    }
    return null
}

internal class AndroidDisplayMode(val id: Int, val width: Int, val height: Int, val hz: Float)

/**
 * The display mode to ask for while a stream is shown, or null to keep the
 * current one: same resolution, the highest refresh up to 120 Hz that is a
 * whole multiple of 60 (an even cadence for 30/60 fps streams), above the
 * current refresh. A 120 Hz panel left at 60 Hz latches each decoded frame up
 * to 16.7 ms later and delivers touches half as often.
 */
internal fun androidStreamDisplayMode(current: AndroidDisplayMode, modes: List<AndroidDisplayMode>): Int? {
    fun cadence(hz: Float) = (hz in 59f..61f) || (hz in 119f..121f)
    val best = modes.filter { it.width == current.width && it.height == current.height && cadence(it.hz) }
        .maxByOrNull { it.hz } ?: return null
    return if (best.hz > current.hz + 1f) best.id else null
}

/**
 * The mode to hold while streaming, which unlike [androidStreamDisplayMode]
 * also names the mode the display is already in when that is the best one: a
 * panel with adaptive refresh (Samsung) then keeps it instead of stepping down
 * to 30 Hz when it takes the picture for static content. Null when no 60/120 Hz
 * mode exists at the current resolution.
 */
internal fun androidStreamHoldMode(current: AndroidDisplayMode, modes: List<AndroidDisplayMode>): Int? {
    androidStreamDisplayMode(current, modes)?.let { return it }
    fun cadence(hz: Float) = (hz in 59f..61f) || (hz in 119f..121f)
    val best = modes.filter { it.width == current.width && it.height == current.height && cadence(it.hz) }
        .maxByOrNull { it.hz } ?: return null
    return if (best.hz >= current.hz - 1f) best.id else null
}

/**
 * The frame rate the stream's surface declares (Surface.setFrameRate with
 * FRAME_RATE_COMPATIBILITY_FIXED_SOURCE): the Host streams at most 60 fps, so
 * a panel that offers 60 Hz or more is told 60 and lets its adaptive logic
 * choose among the multiples (60, 120) but never below. Null where the panel
 * has no such mode.
 */
internal fun androidStreamFrameRate(modes: List<AndroidDisplayMode>, current: AndroidDisplayMode): Float? =
    if (modes.any { it.width == current.width && it.height == current.height && it.hz >= 59f }) 60f else null

/**
 * Whether the display request must be applied again: the display mode in
 * effect or the surface changed since it was applied (fold, unfold, rotation,
 * a new Flutter surface), or the system moved the preference.
 */
internal fun androidRefreshNeedsReapply(
    appliedMode: Int?, appliedSurface: Int?,
    wantedMode: Int?, currentSurface: Int?, preferredMode: Int,
): Boolean =
    (wantedMode != null && (appliedMode != wantedMode || preferredMode != wantedMode)) ||
        (currentSurface != null && appliedSurface != currentSurface)

/**
 * This app's own CPU cost, from /proc/self/stat (utime + stime in clock
 * ticks). Android hides /proc/stat from apps, so only this process is
 * measured; the whole-device figure stays unknown. Result in tenths of a
 * percent of the whole device (all cores = 100%), or -1 when unusable.
 */
internal fun parseSelfStatTicks(stat: String): Long? {
    // "pid (comm) state ppid ..." - comm may hold spaces and parentheses, so
    // fields are counted from the last ')'. utime/stime are fields 14 and 15.
    val end = stat.lastIndexOf(')')
    if (end < 0) return null
    val fields = stat.substring(end + 1).trim().split(' ').filter { it.isNotEmpty() }
    if (fields.size < 13) return null
    val user = fields[11].toLongOrNull() ?: return null
    val system = fields[12].toLongOrNull() ?: return null
    return if (user < 0 || system < 0) null else user + system
}

internal class AppCpuMeter(private val cores: Int, private val ticksPerSecond: Long) {
    private var lastTicks = -1L
    private var lastNanos = 0L
    /** Latest result; held while readings arrive closer than half a second. */
    var tenths = -1
        private set

    fun sample(ticks: Long?, nowNanos: Long): Int {
        if (ticks == null || cores < 1 || ticksPerSecond < 1) { tenths = -1; lastTicks = -1; return tenths }
        if (lastTicks < 0) { lastTicks = ticks; lastNanos = nowNanos; return tenths }
        val elapsed = nowNanos - lastNanos
        if (elapsed < 500_000_000L) return tenths
        tenths = if (ticks < lastTicks) -1 else {
            val cpuSeconds = (ticks - lastTicks).toDouble() / ticksPerSecond
            val share = cpuSeconds / (elapsed / 1e9 * cores) * 1000.0
            share.roundToInt().coerceIn(0, 1000)
        }
        lastTicks = ticks
        lastNanos = nowNanos
        return tenths
    }

    fun reset() { lastTicks = -1; tenths = -1 }
}
