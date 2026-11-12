package me.totb.multitap_client

// audio-opus-v1 on Android, its pure parts (no Android types), so the JVM
// check android/test/AndroidOpusAudioCheck.kt runs this production code.
// AndroidOpusDecoder.kt applies them to MediaCodec "audio/opus". The Host's
// encoder is apps/windows_host/src/opus_audio.hpp, the Windows client's
// decoder windows/runner/opus_audio_decoder.h.

/**
 * A kind-2 media payload of audio-opus-v1: a u16 little-endian sequence
 * number, then one Opus packet of exactly 10 ms (480 samples per channel,
 * 48 kHz stereo; the Host encodes CELT only, 128 kbps VBR). The number counts
 * sent frames only: a gap is a packet lost on the way, and the VM's silent
 * pauses, during which the Host sends nothing, leave none.
 */
internal object AndroidOpus {
    const val MIME = "audio/opus"
    const val RATE = 48_000
    const val CHANNELS = 2
    const val FRAME_SAMPLES = 480
    const val FRAME_BYTES = FRAME_SAMPLES * CHANNELS * 2
    const val FRAME_US = 10_000L
    /** The largest Opus packet the payload may carry (as the Windows client). */
    const val MAX_PACKET = 1275
    /**
     * The Host encoder's lookahead (RESTRICTED_LOWDELAY: 2.5 ms), stated as
     * pre-skip / codec delay as an Ogg or WebM source would; each decoder
     * then trims it once from its first output.
     */
    const val PRE_SKIP = 120
    /** The usual 80 ms; it applies only after a flush, which this stream never does. */
    const val SEEK_PRE_ROLL_NS = 80_000_000L

    fun sequence(bytes: ByteArray, offset: Int): Int =
        (bytes[offset].toInt() and 0xff) or ((bytes[offset + 1].toInt() and 0xff) shl 8)

    /**
     * opus_packet_get_nb_samples at 48 kHz: the samples per channel the Opus
     * packet ([length] bytes at [offset]) holds, or -1 when it cannot be one.
     */
    fun samples(bytes: ByteArray, offset: Int, length: Int): Int {
        if (length < 1) return -1
        val toc = bytes[offset].toInt() and 0xff
        val frames = when (toc and 3) {
            0 -> 1
            1, 2 -> 2
            else -> if (length < 2) return -1 else bytes[offset + 1].toInt() and 0x3f
        }
        val size = (toc shr 3) and 3
        val perFrame = when {
            toc and 0x80 != 0 -> (RATE shl size) / 400                         // CELT
            toc and 0x60 == 0x60 -> if (toc and 0x08 != 0) RATE / 50 else RATE / 100  // hybrid
            size == 3 -> RATE * 60 / 1000                                       // SILK 60 ms
            else -> (RATE shl size) / 100                                       // SILK
        }
        val samples = frames * perFrame
        return if (samples * 25 > RATE * 3) -1 else samples
    }

    /** One audio-opus-v1 payload ([length] bytes at [offset]): a sequence number and one 10 ms Opus packet. */
    fun payloadValid(bytes: ByteArray, offset: Int, length: Int): Boolean =
        length in 3..2 + MAX_PACKET && samples(bytes, offset + 2, length - 2) == FRAME_SAMPLES

    /**
     * MediaFormat csd-0, csd-1 and csd-2 of MediaCodec's Opus decoder: the
     * RFC 7845 identification header (version 1, 2 channels, [PRE_SKIP],
     * 48 kHz, gain 0, mapping family 0), then the codec delay and the seek
     * pre-roll in nanoseconds as 64-bit integers in [littleEndian] order (the
     * platform's native order).
     */
    fun codecSpecificData(littleEndian: Boolean): List<ByteArray> {
        val head = ByteArray(19)
        "OpusHead".forEachIndexed { index, c -> head[index] = c.code.toByte() }
        head[8] = 1
        head[9] = CHANNELS.toByte()
        head[10] = PRE_SKIP.toByte(); head[11] = (PRE_SKIP shr 8).toByte()
        for (index in 0 until 4) head[12 + index] = (RATE shr (8 * index)).toByte()
        // [16..17] output gain 0, [18] mapping family 0.
        fun long(value: Long) = ByteArray(8) { index -> (value shr (8 * if (littleEndian) index else 7 - index)).toByte() }
        return listOf(head, long(PRE_SKIP * 1_000_000_000L / RATE), long(SEEK_PRE_ROLL_NS))
    }

    /**
     * The order Opus decoders are tried in: the platform's own software one
     * (libopus; every Android since 5.0 has it) first, then any other, never a
     * secure-only variant. A vendor offload decoder may buffer for battery.
     */
    fun decoderOrder(candidates: List<AvcDecoderCandidate>): List<String> {
        val usable = candidates.filter { !it.name.lowercase().endsWith(".secure") }.distinctBy { it.name }
        return usable.filter { !it.hardware }.map { it.name } + usable.filter { it.hardware }.map { it.name }
    }

    /** AudioFormat.ENCODING_PCM_16BIT / ENCODING_PCM_FLOAT (fixed platform values, so this stays a JVM object). */
    const val PCM_16 = 2
    const val PCM_FLOAT = 4

    /**
     * What a decoder's output format ([rate] Hz, [channels], [encoding]; null
     * when the format leaves it out) is to the track's 48 kHz s16 stereo: the
     * shape [toStereo16] turns into it, or null when it cannot (another rate,
     * more than two channels, another sample type). A decoder that does not
     * state its encoding gives 16-bit, as MediaCodec documents.
     */
    fun outputShape(rate: Int, channels: Int, encoding: Int?): PcmShape? {
        if (rate != RATE || channels !in 1..2) return null
        val float = when (encoding ?: PCM_16) { PCM_16 -> false; PCM_FLOAT -> true; else -> return null }
        return PcmShape(channels, float)
    }

    /**
     * [bytes] of decoder output in [from] (shape [shape]) as s16 stereo
     * little-endian in [to] from offset 0. Returns the bytes written, or -1
     * when the input is not whole samples or [to] is too small. Mono is played
     * on both sides; float is clipped to [-1, 1].
     */
    fun toStereo16(from: ByteArray, bytes: Int, shape: PcmShape, to: ByteArray): Int {
        val sampleBytes = if (shape.float) 4 else 2
        val frameBytes = sampleBytes * shape.channels
        if (bytes < 0 || bytes % frameBytes != 0 || bytes > from.size) return -1
        val frames = bytes / frameBytes
        if (frames * 4 > to.size) return -1
        if (!shape.float && shape.channels == CHANNELS) { System.arraycopy(from, 0, to, 0, bytes); return bytes }
        for (n in 0 until frames) {
            for (c in 0 until CHANNELS) {
                val source = n * frameBytes + (if (shape.channels == 1) 0 else c) * sampleBytes
                val value = if (shape.float) {
                    val bits = (from[source].toInt() and 0xff) or ((from[source + 1].toInt() and 0xff) shl 8) or
                        ((from[source + 2].toInt() and 0xff) shl 16) or (from[source + 3].toInt() shl 24)
                    val sample = java.lang.Float.intBitsToFloat(bits)
                    if (sample.isNaN()) 0 else Math.round(sample.coerceIn(-1f, 1f) * 32767f)
                } else (from[source].toInt() and 0xff) or (from[source + 1].toInt() shl 8)
                val at = (n * CHANNELS + c) * 2
                to[at] = value.toByte()
                to[at + 1] = (value shr 8).toByte()
            }
        }
        return frames * 4
    }
}

/** A decoder's PCM as [AndroidOpus.toStereo16] reads it: [channels] (1 or 2) of s16, or of float. */
internal data class PcmShape(val channels: Int, val float: Boolean)

/**
 * audio-opus-v1 on the media worker: one stream's Opus decoder [C], made off
 * the worker. Listing the platform's decoders and creating, configuring and
 * starting one took the start answer from ~70 ms (PCM) to 190-270 ms
 * (2026-11-03, the first session of an app run) when it ran on the worker
 * before the start answered and before the video decoder was made. Now the
 * worker only asks for one ([Codecs.open]) and goes on; the video never waits.
 *
 * Packets that come before the decoder is there are admitted to [stream] in
 * order and wait (at most [MAX_PENDING], the oldest dropped), as packets wait
 * for a free input buffer; input buffers the decoder offers before it was
 * handed over are kept for it. The first burst then decodes at once and the
 * track's start cut (AndroidAudioTrim) cuts it like any start, so the pause
 * and concealment rules of [AndroidOpusStream] are unchanged.
 *
 * A decoder that fails (an error, a refused input, none could be made) is
 * replaced at the next packet; one whose output format cannot be played
 * ([unusable]) is passed over for the next in trial order. After
 * [MAX_FAILURES] in a row without a decoded frame, or when the platform lists
 * no Opus decoder that was not passed over, [failed] is set and the stream
 * plays no sound (the video goes on; the app asks for PCM from the next
 * connection on, AndroidOpusDecoder.unusableThisRun).
 *
 * Worker thread only; [Codecs] makes and releases decoders elsewhere.
 */
internal class AndroidOpusPipe<C : Any>(
    private val codecs: Codecs<C>,
    private val emit: (ByteArray, Int, Long, Long) -> Unit,
) {
    interface Codecs<C : Any> {
        /** Makes a decoder for [attempt] off the worker, then [opened] on the worker. False: it cannot (shutting down). */
        fun open(attempt: Attempt<C>): Boolean
        /** One packet into input [index] of [codec]; throws when it refuses it. */
        fun input(codec: C, index: Int, bytes: ByteArray, offset: Int, length: Int, pts: Long)
        /** Stops and releases [codec], off the worker. */
        fun retire(codec: C)
    }

    /**
     * One decoder asked for; its callbacks name it. [codec] is null until it
     * was handed over. [skip]: decoders this stream already found unusable,
     * which the maker passes over.
     */
    class Attempt<C : Any>(val skip: Set<String> = emptySet()) {
        internal var codec: C? = null
    }

    private class Pending(val bytes: ByteArray, val length: Int, val pts: Long)

    val stream = AndroidOpusStream()
    private var current: Attempt<C>? = null
    private val inputs = ArrayDeque<Int>()
    private val pending = ArrayDeque<Pending>()
    private var failures = 0
    private val rejected = LinkedHashSet<String>()

    /** No decoder could be made (or kept): this stream has no sound. */
    var failed = false
        private set
    /** The decoder in use ("" before one was handed over). */
    var name = ""
        private set

    /** [attempt]'s decoder is the one in use (handed over, not replaced). */
    fun current(attempt: Attempt<C>) = attempt === current && attempt.codec != null

    /** At the stream's start: a decoder is asked for before the first packet. */
    fun prepare() {
        if (current == null && !failed && open()) stream.reset()
    }

    /** One audio-opus-v1 payload ([length] bytes at [offset], already valid), its PTS and arrival. */
    fun packet(bytes: ByteArray, offset: Int, length: Int, pts: Long, accepted: Long) {
        if (failed) return
        if (current == null) {
            if (!open()) return
            stream.reset()
        }
        when (stream.admit(AndroidOpus.sequence(bytes, offset), pts, accepted)) {
            AndroidOpusStream.LATE -> return
            AndroidOpusStream.RESTART -> if (!open()) return
        }
        queue(bytes, offset + 2, length - 2, pts)
    }

    /** The decoder asked for by [attempt]: [codec] named [name], or null when none could be made ([none]: the platform lists no Opus decoder). */
    fun opened(attempt: Attempt<C>, codec: C?, name: String, none: Boolean) {
        if (attempt !== current) { codec?.let(codecs::retire); return }
        if (codec == null) {
            broken()
            if (none) failed = true
            return
        }
        attempt.codec = codec
        this.name = name
        drain()
    }

    /** [attempt]'s decoder freed input [index] (also before it was handed over). */
    fun inputAvailable(attempt: Attempt<C>, index: Int) {
        if (attempt !== current) return
        inputs.addLast(index)
        drain()
    }

    /** [attempt]'s decoder put out [size] bytes of PCM in [pcm] for presentation time [pts]. */
    fun decoded(attempt: Attempt<C>, pts: Long, pcm: ByteArray, size: Int) {
        if (!current(attempt)) return
        // [emit] reports its own track failures; nothing escapes into the codec's callback.
        try {
            if (size > 0 && stream.decoded(pts, pcm, size, emit)) failures = 0
        } catch (_: Exception) {}
    }

    /** [attempt]'s decoder failed: the next packet asks for a new one. */
    fun codecFailed(attempt: Attempt<C>) {
        if (attempt === current) broken()
    }

    /**
     * [attempt]'s output cannot become the track's 48 kHz s16 stereo
     * (AndroidOpus.outputShape): that decoder is passed over from now on and
     * the next packet asks for the next one in trial order. Only when none is
     * left does the stream go without sound (no noise is ever played). Before
     * 2026-11-11 the first such decoder ended the sound for the whole stream.
     */
    fun unusable(attempt: Attempt<C>) {
        if (attempt !== current) return
        if (name.isNotEmpty()) rejected.add(name)
        close()
    }

    /** Decoders this stream passed over (their output format could not be used). */
    val passedOver: Set<String> get() = rejected

    /** The decoder in use or being made is given up (one being made is released when it comes). */
    fun close() {
        val old = current
        current = null
        inputs.clear()
        pending.clear()
        old?.codec?.let(codecs::retire)
    }

    private fun open(): Boolean {
        close()
        val attempt = Attempt<C>(rejected.toSet())
        current = attempt
        if (codecs.open(attempt)) return true
        current = null
        broken()
        return false
    }

    /** The decoder failed: the next packet asks for a new one, up to [MAX_FAILURES] in a row. */
    private fun broken() {
        close()
        if (++failures > MAX_FAILURES) failed = true
    }

    private fun queue(bytes: ByteArray, offset: Int, length: Int, pts: Long) {
        val codec = current?.codec
        val index = if (codec == null) null else inputs.removeFirstOrNull()
        if (codec == null || index == null) {
            // Not made yet, or no input buffer free (rare: the decoder holds
            // several). Its admission entry is skipped if it never gets in.
            if (pending.size >= MAX_PENDING) pending.removeFirst()
            pending.addLast(Pending(bytes.copyOfRange(offset, offset + length), length, pts))
            return
        }
        input(codec, index, bytes, offset, length, pts)
    }

    private fun drain() {
        while (pending.isNotEmpty() && inputs.isNotEmpty()) {
            val codec = current?.codec ?: return
            val waiting = pending.removeFirst()
            input(codec, inputs.removeFirst(), waiting.bytes, 0, waiting.length, waiting.pts)
        }
    }

    private fun input(codec: C, index: Int, bytes: ByteArray, offset: Int, length: Int, pts: Long) {
        try { codecs.input(codec, index, bytes, offset, length, pts) } catch (_: Exception) { broken() }
    }

    companion object {
        /** Decoders replaced in a row without a decoded frame before the stream gives up its sound. */
        const val MAX_FAILURES = 3
        /** 160 ms of packets waiting for the decoder or an input buffer at most. */
        const val MAX_PENDING = 16
    }
}

/**
 * audio-opus-v1's sequence numbers and the concealment of lost packets, for a
 * decoder that answers each packet later (MediaCodec in asynchronous mode).
 *
 * - [admit] takes a received packet's number and PTS before it goes to the
 *   decoder. A gap before it (a packet lost on UDP, or dropped by a full Host
 *   queue) of at most [MAX_CONCEALED] packets is concealed, unless the
 *   packet's PTS is later than the gap explains by more than [PAUSE_US]: the
 *   VM went silent among the missing packets, and repeating sound from before
 *   that pause would be wrong. A longer gap is left unfilled (the sound
 *   resumes). A packet up to [LATE_WINDOW] behind the one expected
 *   (reordered or repeated) is late and dropped; further back the Host's
 *   encoder started over (a new capture under the same stream): [RESTART], a
 *   new decoder, and this packet starts the sequence again.
 * - [decoded] takes the decoder's PCM for one packet and gives [decoded]'s
 *   `emit` what plays, in order: the concealment of the packets lost before
 *   it, then that PCM. MediaCodec has no packet-loss concealment (Opus PLC),
 *   so a lost packet is the last decoded 10 ms repeated, fading linearly to
 *   silence by the end of [MAX_CONCEALED] frames (60 ms); the next decoded
 *   frame fades in from there over [FADE_IN_SAMPLES].
 *
 * Concealed frames carry the PTS right before the packet that showed the gap,
 * so AndroidAudioTrim's pause rule measures from real sound. Not thread-safe
 * (the media worker).
 */
internal class AndroidOpusStream {
    companion object {
        const val LATE = 0
        const val DECODE = 1
        const val RESTART = 2
        const val MAX_CONCEALED = 6
        const val LATE_WINDOW = 64
        /** The Host stamps frames from its loopback reads, which a ~16 ms timer can delay. */
        const val PAUSE_US = 25_000L
        const val FADE_IN_SAMPLES = 120
        /** Packets in the decoder without their output yet; a working decoder holds one or two. */
        const val MAX_WAITING = 32
    }

    var packets = 0L
        private set
    /** Frames concealed (repeated and faded). */
    var concealed = 0L
        private set
    /** Lost frames left unfilled (a long gap, or one across a pause). */
    var unfilled = 0L
        private set
    var late = 0L
        private set
    var restarts = 0L
        private set
    /** Packets that went to the decoder and never came out of it. */
    var lost = 0L
        private set

    private var started = false
    private var expected = 0
    private var nextPts = 0L
    private val waitingPts = LongArray(MAX_WAITING)
    private val waitingAccepted = LongArray(MAX_WAITING)
    private val waitingConceal = IntArray(MAX_WAITING)
    private var head = 0
    private var count = 0
    private val last = ByteArray(AndroidOpus.FRAME_BYTES)
    private var haveLast = false
    private val frame = ByteArray(AndroidOpus.FRAME_BYTES)

    /** Packets admitted whose output has not come. */
    val waiting get() = count

    /**
     * One received packet ([sequence], [pts] in µs, [accepted] its arrival):
     * [LATE] (drop it), [DECODE] (to the decoder now) or [RESTART] (to a new
     * decoder). [DECODE] and [RESTART] packets must reach the decoder in this
     * order, with [pts] as their presentation time.
     */
    fun admit(sequence: Int, pts: Long, accepted: Long): Int {
        var action = DECODE
        var conceal = 0
        if (started) {
            val ahead = (sequence - expected) and 0xffff
            if (ahead >= 0x8000) {
                if (0x10000 - ahead <= LATE_WINDOW) { late++; return LATE }
                restarts++
                reset()
                action = RESTART
            } else if (ahead > 0) {
                if (ahead <= MAX_CONCEALED && pts <= nextPts + ahead * AndroidOpus.FRAME_US + PAUSE_US) conceal = ahead
                else unfilled += ahead
            }
        }
        // A decoder that keeps packets without output: the oldest is gone.
        if (count == MAX_WAITING) { head = (head + 1) % MAX_WAITING; count--; lost++ }
        val slot = (head + count) % MAX_WAITING
        waitingPts[slot] = pts
        waitingAccepted[slot] = accepted
        waitingConceal[slot] = conceal
        count++
        started = true
        expected = (sequence + 1) and 0xffff
        nextPts = pts + AndroidOpus.FRAME_US
        return action
    }

    /**
     * The decoder's output ([length] bytes of s16 stereo in [pcm], which may
     * be faded in place) for the packet it was given with presentation time
     * [pts] (a decoder may shift it by less than a packet). [emit] gets each
     * piece in play order: (pcm, length, the packet's PTS, its arrival). False
     * when the output belongs to no admitted packet or is not whole frames.
     */
    fun decoded(pts: Long, pcm: ByteArray, length: Int, emit: (ByteArray, Int, Long, Long) -> Unit): Boolean {
        // Outputs come in input order: admitted packets before this one that
        // produced nothing are gone.
        while (count > 1 && waitingPts[(head + 1) % MAX_WAITING] <= pts) { pop(); lost++ }
        if (count == 0 || waitingPts[head] > pts) return false
        val packetPts = waitingPts[head]
        val accepted = waitingAccepted[head]
        val conceal = waitingConceal[head]
        pop()
        if (length <= 0 || length % 4 != 0 || length > pcm.size) { lost++; return false }
        var gain = 1f
        if (conceal > 0 && haveLast) {
            for (k in 0 until conceal) {
                val from = 1f - k.toFloat() / MAX_CONCEALED
                val to = 1f - (k + 1).toFloat() / MAX_CONCEALED
                for (n in 0 until AndroidOpus.FRAME_SAMPLES) {
                    val g = from + (to - from) * (n + 1) / AndroidOpus.FRAME_SAMPLES
                    scale(last, frame, n, g)
                }
                concealed++
                emit(frame, AndroidOpus.FRAME_BYTES, (packetPts - (conceal - k) * AndroidOpus.FRAME_US).coerceAtLeast(0), accepted)
                gain = to
            }
        } else if (conceal > 0) unfilled += conceal
        // The repeated frame is the decoded one, before any fade-in.
        if (length == AndroidOpus.FRAME_BYTES) { System.arraycopy(pcm, 0, last, 0, length); haveLast = true }
        if (gain < 1f) {
            val ramp = minOf(FADE_IN_SAMPLES, length / 4)
            for (n in 0 until ramp) scale(pcm, pcm, n, gain + (1f - gain) * n / FADE_IN_SAMPLES)
        }
        packets++
        emit(pcm, length, packetPts, accepted)
        return true
    }

    /** A new decoder: nothing waits in it, and nothing was decoded to repeat. */
    fun reset() {
        started = false
        head = 0
        count = 0
        haveLast = false
    }

    private fun pop() { head = (head + 1) % MAX_WAITING; count-- }

    /** Stereo s16 little-endian sample [n] of [from], times [gain], into [to]. */
    private fun scale(from: ByteArray, to: ByteArray, n: Int, gain: Float) {
        for (c in 0 until AndroidOpus.CHANNELS) {
            val at = (n * AndroidOpus.CHANNELS + c) * 2
            val sample = (from[at].toInt() and 0xff) or (from[at + 1].toInt() shl 8)
            val value = Math.round(sample * gain).coerceIn(-32768, 32767)
            to[at] = value.toByte()
            to[at + 1] = (value shr 8).toByte()
        }
    }
}
