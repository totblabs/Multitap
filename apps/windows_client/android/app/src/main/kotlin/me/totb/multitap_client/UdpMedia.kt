package me.totb.multitap_client

/**
 * UDP media core (docs/remote-session-protocol.md, "UDP media"): the
 * client half of native/windows_video/udp_media.hpp, ported as written. Pure
 * and clock-injected: no I/O, no threads, no clock, no crypto
 * (UdpMediaCrypto.kt seals what this produces). Not thread safe. Custom
 * framing: it needs independent review before it carries real media.
 *
 * Wire integers are big endian except the 12-byte media envelope, which is
 * the WSS packet's own little-endian header. IDs are uint32 and times uint64
 * microseconds on the wire; here both are Long.
 */
internal object Udp {
    const val MAX_DATAGRAM = 1200
    const val HEADER_BYTES = 17
    const val TAG_BYTES = 16
    const val MAX_PLAINTEXT = MAX_DATAGRAM - HEADER_BYTES - TAG_BYTES
    const val FRAGMENT_HEADER_BYTES = 10
    const val FRAGMENT_PAYLOAD = MAX_PLAINTEXT - FRAGMENT_HEADER_BYTES
    const val MAX_FRAGMENTS = 2048
    const val MAX_MESSAGE_BYTES = MAX_FRAGMENTS * FRAGMENT_PAYLOAD
    const val KIND_FRAGMENT = 1
    const val KIND_FEEDBACK = 2
    const val KIND_NACK = 3
    const val KIND_KEYFRAME = 4
    const val KIND_TAIL = 5
    // parity-v1 (udp_media.hpp): PARITY from the Host, ABILITIES to it. This client sends
    // no LOSS, so it never gets a recovery frame, and takes parity of the video class only.
    const val KIND_PARITY = 7
    const val KIND_ABILITIES = 8
    const val ABILITIES_BYTES = 8
    const val MAX_PARITIES = 16
    const val ABILITY_PARITY = 1
    const val ABILITIES_INTERVAL_US = 1000000L
    const val REPAIRED_AFTER_US = 50000L
    const val MAX_REBUILT_WATCHED = 256
    // A key frame asked for after a loss and left without an answer is asked for again this
    // soon (udp_media.hpp askAgainDelay): the request itself can be lost.
    const val ANSWER_SLACK_US = 60000L
    const val MINIMUM_ASK_AGAIN_US = 100000L
    // quick-nack-v1 (udp_media.hpp): once the datagrams' sequences show what the path
    // delivers out of order, a NACK waits only half again that lateness and a millisecond.
    const val QUICK_NACK_DELAY_US = 2000L
    const val SEQUENCE_WINDOW_US = 15000000L
    const val SEQUENCE_LATE_UNKNOWN_US = 100000L
    const val SEQUENCE_KNOWN_AFTER = 500
    const val SEQUENCE_JUMPS = 32
    const val SEQUENCE_RUN_BUCKETS = 6
    const val CLASS_VIDEO = 0
    const val CLASS_AUDIO = 1
    const val FEEDBACK_BYTES = 34
    const val KEYFRAME_BYTES = 4
    const val TAIL_BYTES = 26
    const val NACK_ENTRY_BYTES = 8
    const val MAX_NACK_ENTRIES = 64
    const val FEEDBACK_INTERVAL_US = 100000L
    const val KEYFRAME_INTERVAL_US = 500000L
    const val ASSUMED_RTT_US = 50000L
    const val MINIMUM_NACK_DELAY_US = 10000L
    const val MINIMUM_NACK_REPEAT_US = 20000L
    const val MAX_NACK_REPEATS = 3
    const val GIVE_UP_SLACK_US = 20000L
    const val MINIMUM_GIVE_UP_US = 60000L
    const val MAXIMUM_GIVE_UP_US = 600000L
    /** The Host's limit for FEEDBACK that shows no progress (feedbackSilenceUs). */
    const val FEEDBACK_SILENCE_US = 1000000L
    // Memory bounds, the contract's own limits aside: messages behind the next one
    // to deliver, their bytes, recorded gaps, and NACK datagrams per tick.
    const val VIDEO_WINDOW = 64
    const val MAX_VIDEO_BYTES = 16L * 1024 * 1024
    const val MAX_GAPS = 4096
    const val MAX_NACKS_PER_TICK = 8
    // Audio: unfinished messages, and fragments of one (96 ms of PCM, real chunks take 2).
    const val AUDIO_PARTIALS = 8
    const val MAX_AUDIO_FRAGMENTS = 16

    const val PATH_WSS = 0
    const val PATH_UDP = 1

    fun get16(b: ByteArray, at: Int): Int = ((b[at].toInt() and 0xff) shl 8) or (b[at + 1].toInt() and 0xff)
    fun get32(b: ByteArray, at: Int): Long = (get16(b, at).toLong() shl 16) or get16(b, at + 2).toLong()
    fun get64(b: ByteArray, at: Int): Long = (get32(b, at) shl 32) or get32(b, at + 4)
    fun put16(b: ByteArray, at: Int, value: Int) { b[at] = (value ushr 8).toByte(); b[at + 1] = value.toByte() }
    fun put32(b: ByteArray, at: Int, value: Long) { put16(b, at, (value ushr 16).toInt()); put16(b, at + 2, value.toInt()) }
    fun put64(b: ByteArray, at: Int, value: Long) { put32(b, at, value ushr 32); put32(b, at + 4, value) }

    /** The 12-byte header of a single-output WSS media message (mediaPacket). */
    fun parseEnvelope(message: ByteArray, offset: Int, size: Int, cls: Int, result: UdpEnvelope): Boolean {
        if (size < 13 || message[offset].toInt() != (if (cls == CLASS_VIDEO) 1 else 2)) return false
        val flags = message[offset + 1].toInt() and 0xff
        if (flags and (if (cls == CLASS_VIDEO) 1.inv() else 0.inv()) != 0) return false
        result.key = flags and 1 != 0
        result.epoch = (message[offset + 2].toInt() and 0xff) or ((message[offset + 3].toInt() and 0xff) shl 8)
        var pts = 0L
        for (n in 0 until 8) pts = pts or ((message[offset + 4 + n].toLong() and 0xff) shl (8 * n))
        result.pts = pts
        return true
    }

    fun fragmentCount(messageBytes: Int): Int =
        if (messageBytes in 1..MAX_MESSAGE_BYTES) (messageBytes + FRAGMENT_PAYLOAD - 1) / FRAGMENT_PAYLOAD else 0

    /** Writes fragment [index] of [message] into [out] and returns its plaintext size. */
    fun writeFragment(out: ByteArray, cls: Int, id: Long, message: ByteArray, index: Int): Int {
        val count = fragmentCount(message.size)
        val at = index * FRAGMENT_PAYLOAD
        val size = minOf(FRAGMENT_PAYLOAD, message.size - at)
        out[0] = KIND_FRAGMENT.toByte(); out[1] = cls.toByte(); put32(out, 2, id); put16(out, 6, index); put16(out, 8, count)
        System.arraycopy(message, at, out, FRAGMENT_HEADER_BYTES, size)
        return FRAGMENT_HEADER_BYTES + size
    }

    /**
     * Every fragment but the last carries exactly FRAGMENT_PAYLOAD bytes: a
     * slice's place follows from its index. Message IDs start at 1.
     */
    fun parseFragment(plain: ByteArray, size: Int, f: UdpFragment): Boolean {
        if (size <= FRAGMENT_HEADER_BYTES || size > MAX_PLAINTEXT || plain[0].toInt() != KIND_FRAGMENT ||
            (plain[1].toInt() and 0xff) > CLASS_AUDIO) return false
        f.cls = plain[1].toInt(); f.id = get32(plain, 2); f.index = get16(plain, 6); f.count = get16(plain, 8)
        f.data = plain; f.size = size - FRAGMENT_HEADER_BYTES
        return f.id != 0L && f.count != 0 && f.count <= MAX_FRAGMENTS && f.index < f.count &&
            (f.index + 1 == f.count || f.size == FRAGMENT_PAYLOAD)
    }

    const val PARITY_BYTES = FRAGMENT_HEADER_BYTES + FRAGMENT_PAYLOAD

    /** Parity datagrams that follow a message of [fragments] fragments. */
    fun parityCount(fragments: Int): Int = if (fragments > 0) minOf((fragments + 15) / 16, MAX_PARITIES) else 0

    /**
     * Writes parity [slot] of [message] into [out] as the Host does and returns its plaintext
     * size: the XOR of the fragments whose index is [slot] modulo [parities], each padded
     * with zeros to a full payload. Only tests send one.
     */
    fun writeParity(out: ByteArray, cls: Int, id: Long, message: ByteArray, slot: Int, parities: Int): Int {
        val count = fragmentCount(message.size)
        val lastSize = message.size - (count - 1) * FRAGMENT_PAYLOAD
        out[0] = KIND_PARITY.toByte(); out[1] = (cls or ((parities - 1) shl 4)).toByte(); put32(out, 2, id)
        put16(out, 6, (slot shl 11) or lastSize); put16(out, 8, count)
        java.util.Arrays.fill(out, FRAGMENT_HEADER_BYTES, PARITY_BYTES, 0)
        var index = slot
        while (index < count) {
            val at = index * FRAGMENT_PAYLOAD
            val size = minOf(FRAGMENT_PAYLOAD, message.size - at)
            for (n in 0 until size) out[FRAGMENT_HEADER_BYTES + n] = (out[FRAGMENT_HEADER_BYTES + n].toInt() xor message[at + n].toInt()).toByte()
            index += parities
        }
        return PARITY_BYTES
    }

    fun parseParity(plain: ByteArray, size: Int, p: UdpParity): Boolean {
        if (size != PARITY_BYTES || plain[0].toInt() != KIND_PARITY) return false
        val packed = get16(plain, 6)
        p.cls = plain[1].toInt() and 15; p.id = get32(plain, 2); p.count = get16(plain, 8); p.slot = packed ushr 11
        p.parities = ((plain[1].toInt() and 0xff) ushr 4) + 1; p.lastSize = packed and 2047; p.data = plain
        return p.cls == CLASS_VIDEO && p.id != 0L && p.count != 0 && p.count <= MAX_FRAGMENTS && p.parities == parityCount(p.count) &&
            p.slot < p.parities && p.lastSize != 0 && p.lastSize <= FRAGMENT_PAYLOAD
    }

    /**
     * The path rule: a key frame from either path makes that path active,
     * P-frames count only from the active path, and the PTS must be newer than
     * the last delivered. PTS restarts with the encoder, so "newer" holds within
     * an epoch.
     */
    fun acceptVideo(state: UdpPathState, from: Int, epoch: Int, key: Boolean, pts: Long): Boolean {
        val current = state.delivered && epoch == state.epoch
        if (state.delivered && epoch < state.epoch) return false
        if (if (current) java.lang.Long.compareUnsigned(pts, state.lastPts) <= 0 || (!key && from != state.active) else !key) return false
        if (key) state.active = from
        state.epoch = epoch; state.lastPts = pts; state.delivered = true
        return true
    }
}

internal class UdpEnvelope { var key = false; var epoch = 0; var pts = 0L }

/** One parsed fragment; its slice is data[FRAGMENT_HEADER_BYTES, + size). */
internal class UdpFragment {
    var cls = 0
    var id = 0L
    var index = 0
    var count = 0
    var data = ByteArray(0)
    var size = 0
}

/** One parsed parity; its mixed payload is data[FRAGMENT_HEADER_BYTES, + FRAGMENT_PAYLOAD). */
internal class UdpParity {
    var cls = 0
    var id = 0L
    var count = 0
    var slot = 0
    var parities = 0
    var lastSize = 0
    var data = ByteArray(0)
}

internal class UdpPathState(var active: Int = Udp.PATH_WSS, var epoch: Int = 0, var lastPts: Long = 0, var delivered: Boolean = false) {
    fun copy() = UdpPathState(active, epoch, lastPts, delivered)
}

/** A message being rebuilt; its bytes end up identical to the input. */
internal open class UdpPartial {
    private var count = 0
    private var have = 0
    private var size = 0
    private var got = BooleanArray(0)
    private var bytes = ByteArray(0)

    fun started() = count != 0
    fun whole() = count != 0 && have == count
    fun count() = count
    fun missing() = count - have
    fun has(index: Int) = index < got.size && got[index]
    fun reserved() = count.toLong() * Udp.FRAGMENT_PAYLOAD
    fun start(count: Int) { this.count = count; have = 0; size = 0; got = BooleanArray(count); bytes = ByteArray(count * Udp.FRAGMENT_PAYLOAD) }
    fun add(f: UdpFragment): Boolean {
        if (got[f.index]) return false
        got[f.index] = true; ++have
        System.arraycopy(f.data, Udp.FRAGMENT_HEADER_BYTES, bytes, f.index * Udp.FRAGMENT_PAYLOAD, f.size)
        if (f.index + 1 == count) size = f.index * Udp.FRAGMENT_PAYLOAD + f.size
        return true
    }
    /** XORs fragment [index] as the Host mixed it (padded with zeros to a full payload) into [to] from [at]. */
    fun mix(index: Int, to: ByteArray, at: Int) {
        val from = index * Udp.FRAGMENT_PAYLOAD
        for (n in 0 until Udp.FRAGMENT_PAYLOAD) to[at + n] = (to[at + n].toInt() xor bytes[from + n].toInt()).toByte()
    }
    fun take(): ByteArray { val result = if (size == bytes.size) bytes else bytes.copyOf(size); bytes = ByteArray(0); return result }
    fun release() { count = 0; have = 0; size = 0; got = BooleanArray(0); bytes = ByteArray(0) }
}

/**
 * What the path did to the datagrams, from their sequences (quick-nack-v1, SequenceTrack
 * in udp_media.hpp). Pure; the caller passes each authenticated datagram's sequence as
 * it arrives, repairs and all.
 */
internal class UdpSequenceTrack {
    /** Stretches of sequences that were skipped, by length: 1, 2, 3-4, 5-16, 17-64, more. */
    val runs = LongArray(Udp.SEQUENCE_RUN_BUCKETS)
    /** Datagrams that came after a later one (each was counted in a run before). */
    var late = 0L
        private set
    /** The longest any came after the datagram that skipped it. */
    var lateMaxUs = 0L
        private set
    private var highest = 0L
    private var windowAt = 0L
    private var lateNow = 0L
    private var lateBefore = 0L
    private var inOrder = 0
    private val jumpFirst = LongArray(Udp.SEQUENCE_JUMPS) { 1 }
    private val jumpLast = LongArray(Udp.SEQUENCE_JUMPS)
    private val jumpTime = LongArray(Udp.SEQUENCE_JUMPS)
    private var jumpAt = 0

    /** Returns how late this datagram came after one sent later (0: in order). */
    fun arrived(sequence: Long, nowUs: Long): Long {
        roll(nowUs)
        if (highest == 0L) { highest = sequence; return 0 }
        if (sequence > highest) {
            val missing = sequence - highest - 1
            if (missing != 0L) {
                ++runs[bucket(missing)]
                val at = jumpAt++ % Udp.SEQUENCE_JUMPS
                jumpFirst[at] = highest + 1; jumpLast[at] = sequence - 1; jumpTime[at] = nowUs
            }
            highest = sequence; if (inOrder < Int.MAX_VALUE) ++inOrder
            return 0
        }
        // The jump that skipped it is forgotten: later than anything a NACK should wait for.
        var lateness = Udp.SEQUENCE_LATE_UNKNOWN_US
        for (n in 0 until Udp.SEQUENCE_JUMPS) if (sequence >= jumpFirst[n] && sequence <= jumpLast[n]) { lateness = maxOf(nowUs - jumpTime[n], 1); break }
        ++late; lateMaxUs = maxOf(lateMaxUs, lateness); lateNow = maxOf(lateNow, lateness)
        return lateness
    }
    /** Enough came in order to say what this path reorders. */
    fun known() = inOrder >= Udp.SEQUENCE_KNOWN_AFTER
    /** The longest lateness of this window and the one before. */
    fun lateUs(nowUs: Long): Long { roll(nowUs); return maxOf(lateNow, lateBefore) }

    private fun roll(nowUs: Long) {
        if (nowUs - windowAt < Udp.SEQUENCE_WINDOW_US) return
        lateBefore = if (nowUs - windowAt < 2 * Udp.SEQUENCE_WINDOW_US) lateNow else 0
        lateNow = 0; windowAt = nowUs
    }

    companion object {
        fun bucket(length: Long): Int = when { length <= 1 -> 0; length == 2L -> 1; length <= 4 -> 2; length <= 16 -> 3; length <= 64 -> 4; else -> 5 }
    }
}

/**
 * Client side. The caller opens each datagram and passes the plaintext; [tick]
 * runs the timers, every few milliseconds whether or not media arrives. It
 * gets messages to hand to the decoder and control plaintexts to seal and
 * send. Its RTT is the Host's, carried by TAIL. Malformed input only ever
 * drops data.
 */
internal class UdpReceiver(private var epoch: Int, nowUs: Long, parity: Boolean = true) {
    class Message(val video: Boolean, val key: Boolean, val epoch: Int, val pts: Long, val bytes: ByteArray)
    class Output { val messages = ArrayList<Message>(); val control = ArrayList<ByteArray>() }

    // Indices below frontier are passed. parity: the parities that came and are not used
    // up, by slot, each a whole PARITY plaintext.
    private class Entry : UdpPartial() {
        var frontier = 0; var known = false; var skip = false; val envelope = UdpEnvelope()
        var lastSize = 0; var parity: Array<ByteArray?>? = null
    }
    // A rebuilt fragment whose own datagram may still come.
    private class Made(val id: Long, val index: Int, val at: Long)
    // Oldest first. `whole`: no fragment of that message arrived, so its size is unknown.
    private class Gap(val id: Long, val index: Int, var nacks: Int, val whole: Boolean, val at: Long, var due: Long)
    private class Audio : UdpPartial() { var id = 0L }

    private var rtt = Udp.ASSUMED_RTT_US
    private var feedbackAt = nowUs + Udp.FEEDBACK_INTERVAL_US
    private var keyAt = 0L
    private var keySentAt = Long.MIN_VALUE / 2  // the last KEYFRAME request
    private var echoUs = 0L
    private var echoAt = 0L
    private var challenge = 0L
    private var next = 1L    // next ID to deliver
    private var closed = 0L  // the last known sent in full
    private var awaitKey = false
    private var answerLost = false
    private var held = 0L
    private var received = 0
    private var lost = 0
    private var highestWhole = 0L
    private val window = ArrayDeque<Entry>()  // window[0] is message `next`
    private val gaps = ArrayDeque<Gap>()
    private val audio = Array(Udp.AUDIO_PARTIALS) { Audio() }
    private val fragment = UdpFragment()
    private val parityIn = UdpParity()
    private val rebuiltFragment = UdpFragment()
    // parity-v1. [parity] false: it never tells the Host that it takes parity (tests).
    private var abilitiesAt = if (parity) nowUs else Long.MAX_VALUE
    private val made = ArrayDeque<Made>()
    // The wait began with a loss given up: its request is repeated sooner (askAgainDelay).
    private var lossWait = false
    private var asked = 0  // requests sent in this wait, counted to two
    // quick-nack-v1: the path's measured reordering, once the caller knows it.
    private var reorderKnown = false
    private var reorderUs = 0L
    private val nackPlain = ByteArray(2 + Udp.MAX_NACK_ENTRIES * Udp.NACK_ENTRY_BYTES)  // every tick builds in here
    // The earliest a recorded gap may be NACKed: up to 4096 gaps are walked only then.
    // It may be early (a gap filled since), never late.
    private var nackDue = Long.MAX_VALUE

    // Diagnostics only: the Host leaves UDP when FEEDBACK shows
    // nothing newer made whole for 1 s (hostAction 271 reason 3, cause 3). [watch]
    // runs the same test here and records which wait it was; nothing sent changes.
    private var finished = 0L  // the newest message the Host finished, from TAIL
    private var progressWhole = 0L
    private var progressAt = nowUs
    private var stalled = false
    private var awaitCause = 0
    /** KEYFRAME requests sent. */
    var keyRequests = 0L
        private set
    /** Stretches of [Udp.FEEDBACK_SILENCE_US] without progress while UDP was the active path. */
    var stalls = 0L
        private set
    /** The last stall's wait: one of the STALL_ codes. */
    var stallCause = 0
        private set
    /** NACK datagrams sent. */
    var nacks = 0L
        private set
    /** Parity datagrams that came. */
    var parities = 0L
        private set
    /** Fragments rebuilt from parity, and those of them whose own datagram stayed away (lost ones). */
    var rebuilt = 0L
        private set
    var repaired = 0L
        private set

    fun rttUs() = rtt
    /**
     * quick-nack-v1: the longest this path delivered a datagram out of order lately
     * (UdpSequenceTrack.lateUs), once the caller knows it. Until then a NACK waits as before.
     */
    fun setReorder(lateUs: Long) { reorderKnown = true; reorderUs = lateUs }
    fun nackDelayUs() = nackDelay()
    /** Announced on WSS. What carries another epoch is dropped. */
    fun setEpoch(epoch: Int) { this.epoch = epoch }

    fun datagram(plain: ByteArray, size: Int, nowUs: Long, out: Output) {
        val f = fragment
        if (Udp.parseFragment(plain, size, f)) { ++received; if (f.cls == Udp.CLASS_AUDIO) audio(f, out) else video(f, nowUs, out) }
        else if (Udp.parseParity(plain, size, parityIn)) { ++received; ++parities; parity(parityIn, nowUs, out) }
        else if (size == Udp.TAIL_BYTES && plain[0].toInt() == Udp.KIND_TAIL && plain[1].toInt() == 0) {
            ++received; challenge = Udp.get64(plain, 14)
            val rtt = Udp.get32(plain, 22)
            if (rtt != 0L) this.rtt = rtt
            tail(Udp.get32(plain, 2), Udp.get64(plain, 6), nowUs, out)
        }
    }

    fun tick(nowUs: Long, out: Output) {
        val limit = giveUpDelay()
        while (gaps.isNotEmpty()) {
            val gap = gaps.first()
            if (filled(gap)) { gaps.removeFirst(); continue }
            if (nowUs - gap.at < limit) break
            giveUp(gap.id, nowUs)
        }
        drain(out)
        if (nowUs >= nackDue) nack(nowUs, out)
        if (answerLost && awaitKey) keyAt = minOf(keyAt, nowUs)
        answerLost = false
        if (awaitKey && nowUs >= keyAt && !keyArriving()) {
            val plain = ByteArray(Udp.KEYFRAME_BYTES); plain[0] = Udp.KIND_KEYFRAME.toByte(); Udp.put16(plain, 2, epoch)
            // The request itself can be lost, and then nothing is on its way: after a loss the
            // first repeat need only outlast an answer's way here. The Host merges a request
            // that comes while its key frame is on its way, and acts on one in a quarter second.
            out.control.add(plain); keyAt = nowUs + (if (lossWait) askAgainDelay() else Udp.KEYFRAME_INTERVAL_US); keySentAt = nowUs; ++keyRequests
            if (asked < 2) ++asked
        }
        if (nowUs >= feedbackAt) { feedback(nowUs, out); feedbackAt = nowUs + Udp.FEEDBACK_INTERVAL_US }
        while (made.isNotEmpty() && nowUs - made.first().at >= Udp.REPAIRED_AFTER_US) { made.removeFirst(); ++repaired }
        if (nowUs >= abilitiesAt) {
            val plain = ByteArray(Udp.ABILITIES_BYTES); plain[0] = Udp.KIND_ABILITIES.toByte()
            Udp.put16(plain, 2, Udp.ABILITY_PARITY); Udp.put32(plain, 4, repaired)
            out.control.add(plain); abilitiesAt = nowUs + Udp.ABILITIES_INTERVAL_US
        }
    }

    /**
     * When [tick] next has work: FEEDBACK, a NACK, a give-up or a KEYFRAME
     * repeat. A held repeat waits for the key frame that is arriving, whose
     * datagrams or give-up come first. The caller may tick sooner.
     */
    fun nextTickUs(): Long {
        var next = minOf(feedbackAt, nackDue, abilitiesAt)
        if (gaps.isNotEmpty()) next = minOf(next, gaps.first().at + giveUpDelay())
        if (awaitKey && !keyArriving()) next = minOf(next, keyAt)
        return next
    }

    /**
     * A key frame took the other path: that path is active, and what was in
     * flight here is dead. Only the newest message may still finish, restarted
     * empty, because a switch back opens with a key frame that may just be arriving.
     */
    fun keyFrameOtherPath() {
        awaitKey = false; answerLost = false; gaps.clear(); nackDue = Long.MAX_VALUE
        if (window.isEmpty()) return
        abandonBefore(next + window.size - 1)
        held -= window.first().reserved(); window[0] = Entry(); closed = next - 1
    }
    /** The decoder queue refused a message: skip to the next key frame. */
    fun queueFull(nowUs: Long) = dropUntilKey(nowUs, STALL_QUEUE_FULL)
    /**
     * The decoder refused a key frame, or refuses whatever comes (it failed): as
     * [queueFull], but the request goes no sooner than KEYFRAME_INTERVAL after the
     * last one. Otherwise every refused answer asks again at once, and a decoder
     * that cannot take them gets a new IDR every round trip.
     */
    fun refused(nowUs: Long) {
        if (awaitKey) return
        dropUntilKey(nowUs, STALL_REFUSED)
        keyAt = maxOf(nowUs, keySentAt + Udp.KEYFRAME_INTERVAL_US)
    }

    /**
     * Leg thread, each pass. [onUdp]: the gate counts video from this path,
     * the only time the Host judges it. Progress is what the Host counts: a
     * newer message made whole, or none newer than it finished.
     */
    fun watch(nowUs: Long, onUdp: Boolean) {
        if (!onUdp || highestWhole != progressWhole || finished <= highestWhole) {
            progressWhole = highestWhole; progressAt = nowUs; stalled = false
            return
        }
        if (stalled || nowUs - progressAt < Udp.FEEDBACK_SILENCE_US) return
        stalled = true; ++stalls
        val head = window.firstOrNull()
        stallCause = when {
            awaitKey -> awaitCause
            head != null && head.started() && !head.whole() -> STALL_REPAIR
            else -> STALL_NOTHING
        }
    }

    fun awaitingKey() = awaitKey
    fun heldBytes(): Long { var bytes = held; for (a in audio) bytes += a.reserved(); return bytes }
    fun gaps() = gaps.size
    fun windowMessages() = window.size

    private fun entry(id: Long): Entry? = if (id >= next && id - next < window.size) window[(id - next).toInt()] else null
    private fun filled(gap: Gap): Boolean {
        val m = entry(gap.id)
        return m == null || m.skip || m.whole() || (!gap.whole && m.has(gap.index))
    }
    // Counted from gap detection: a paced IDR can take longer than this with no loss at all.
    private fun giveUpDelay() = (rtt * 7 / 2 + Udp.GIVE_UP_SLACK_US).coerceIn(Udp.MINIMUM_GIVE_UP_US, Udp.MAXIMUM_GIVE_UP_US)
    private fun nackDelay(): Long {
        val slow = maxOf(rtt / 4, Udp.MINIMUM_NACK_DELAY_US)
        return if (reorderKnown) (if (reorderUs != 0L) reorderUs * 3 / 2 + 1000 else 0L).coerceIn(Udp.QUICK_NACK_DELAY_US, slow) else slow
    }
    private fun nackRepeat() = maxOf(rtt * 5 / 4, Udp.MINIMUM_NACK_REPEAT_US)
    private fun addGap(id: Long, index: Int, whole: Boolean, nowUs: Long) {
        val due = nowUs + nackDelay()
        gaps.addLast(Gap(id, index, 0, whole, nowUs, due))
        nackDue = minOf(nackDue, due)
    }
    // A wait already running keeps its request times; only a wait that began with a loss
    // and saw no other cause repeats its request sooner.
    private fun dropUntilKey(nowUs: Long, cause: Int) {
        if (!awaitKey) { awaitKey = true; asked = 0; keyAt = nowUs; awaitCause = cause; lossWait = cause == STALL_LOSS }
        else if (cause != STALL_LOSS) lossWait = false
    }
    // How long after a request the next one goes while no answer shows: three round trips
    // and the Host's time to code a frame, then a quarter second, then the interval.
    private fun askAgainDelay(): Long {
        val first = (rtt * 3 + Udp.ANSWER_SLACK_US).coerceIn(Udp.MINIMUM_ASK_AGAIN_US, Udp.KEYFRAME_INTERVAL_US)
        return when (asked) { 0 -> first; 1 -> maxOf(first, Udp.KEYFRAME_INTERVAL_US / 2); else -> Udp.KEYFRAME_INTERVAL_US }
    }
    private fun abandonBefore(id: Long) {
        while (next < id && window.isNotEmpty()) {
            val m = window.first()
            if (awaitKey && answers(m)) answerLost = true
            if (!m.skip) lost += if (m.started()) m.missing() else 1
            held -= m.reserved(); window.removeFirst(); ++next
        }
        next = maxOf(next, id); closed = maxOf(closed, next - 1)
    }
    private fun giveUp(id: Long, nowUs: Long) { dropUntilKey(nowUs, STALL_LOSS); abandonBefore(id + 1) }
    private fun deliver(m: Entry, out: Output) {
        if (m.envelope.epoch != epoch) return
        if (m.envelope.key) { awaitKey = false; answerLost = false }
        // Whole and handed on: a whole frame that is dropped must not read as progress.
        highestWhole = next and 0xffffffffL
        out.messages.add(Message(true, m.envelope.key, m.envelope.epoch, m.envelope.pts, m.take()))
    }
    // Video is delivered in order only.
    private fun drain(out: Output) {
        while (window.isNotEmpty()) {
            val m = window.first()
            val useless = m.skip || (awaitKey && m.known && !m.envelope.key)
            if (!useless && !m.whole()) return
            if (!useless) deliver(m, out)
            held -= m.reserved(); window.removeFirst(); ++next
        }
    }
    // Everything through `last` went out in full: what is missing there is lost.
    private fun close(last: Long, nowUs: Long) {
        if (last < next || last <= closed) return
        if (last - next >= Udp.VIDEO_WINDOW) { dropUntilKey(nowUs, STALL_LOSS); abandonBefore(last - Udp.VIDEO_WINDOW + 1) }
        while (next + window.size <= last) window.addLast(Entry())
        var id = maxOf(closed + 1, next)
        while (id <= last) {
            val m = window[(id - next).toInt()]
            if (!m.skip) {
                if (!m.started()) addGap(id, 0, true, nowUs)
                else {
                    for (i in m.frontier until m.count()) if (!m.has(i)) addGap(id, i, false, nowUs)
                    m.frontier = m.count()
                }
            }
            ++id
        }
        closed = last
    }
    // What a key frame at `id` makes pointless: everything older, back to an older key
    // frame. That one may be nearly repaired, and dropping it for one just starting is
    // how IDRs cascade: every new request would kill the last answer.
    private fun oldestNeeded(id: Long): Long {
        var at = id
        while (at-- > next) { val e = window[(at - next).toInt()]; if (e.known && !e.skip && e.envelope.key) return at }
        return id
    }
    // Another request now would only send a second IDR after this one. If this one
    // stalls, TAIL closes it, it is given up, and the repeat that was held goes at once.
    private fun keyArriving(): Boolean {
        return window.any(::answers)
    }
    private fun answers(e: Entry) = e.known && !e.skip && e.envelope.key
    // Fragment 0 names the message. A message of no use stops costing memory and NACKs.
    // A P-frame stays even while waiting for a key: an older message may be that key,
    // and drain sorts it out.
    private fun inspect(id: Long, m: Entry, f: UdpFragment, nowUs: Long) {
        val valid = Udp.parseEnvelope(f.data, Udp.FRAGMENT_HEADER_BYTES, f.size, Udp.CLASS_VIDEO, m.envelope)
        m.known = true
        if (valid && m.envelope.epoch == epoch) { if (m.envelope.key) abandonBefore(oldestNeeded(id)); return }
        if (!valid || m.envelope.epoch > epoch) dropUntilKey(nowUs, STALL_EPOCH)
        m.skip = true; held -= m.reserved(); m.release(); m.parity = null
    }
    // parity-v1: the one fragment still missing of a parity's class is that parity XOR the
    // others of the class. It is taken as if it had just arrived.
    private fun rebuild(id: Long, m: Entry, slot: Int, nowUs: Long) {
        val parity = m.parity ?: return
        val mixed = parity.getOrNull(slot) ?: return
        val step = parity.size
        var missing = -1
        var index = slot
        while (index < m.count()) { if (!m.has(index)) { if (missing >= 0) return; missing = index }; index += step }
        parity[slot] = null
        if (missing < 0) return
        index = slot
        while (index < m.count()) { if (index != missing) m.mix(index, mixed, Udp.FRAGMENT_HEADER_BYTES); index += step }
        val f = rebuiltFragment
        f.cls = Udp.CLASS_VIDEO; f.id = id; f.index = missing; f.count = m.count(); f.data = mixed
        f.size = if (missing + 1 == m.count()) m.lastSize else Udp.FRAGMENT_PAYLOAD
        if (!m.add(f)) return
        ++rebuilt
        // Already asked for: it was lost, and the repair now on its way must not read as its own
        // datagram coming after all (a NACK may go before a long message's parity arrives).
        if (gaps.any { it.id == id && it.nacks != 0 && (if (it.whole) it.nacks <= Udp.MAX_NACK_REPEATS else it.index == missing) }) ++repaired
        else { made.addLast(Made(id, missing, nowUs)); if (made.size > Udp.MAX_REBUILT_WATCHED) { made.removeFirst(); ++repaired } }
        if (missing == 0) inspect(id, m, f, nowUs)
    }
    // A parity datagram. Its message's fragments have all left the Host by now.
    private fun parity(p: UdpParity, nowUs: Long, out: Output) {
        if (p.id < next) return
        if (p.id - next >= Udp.VIDEO_WINDOW) { dropUntilKey(nowUs, STALL_LOSS); abandonBefore(p.id - Udp.VIDEO_WINDOW + 1) }
        close(p.id - 1, nowUs)
        while (next + window.size <= p.id) window.addLast(Entry())
        val m = window[(p.id - next).toInt()]
        if (m.skip) return
        if (!m.started()) {
            if (held + p.count.toLong() * Udp.FRAGMENT_PAYLOAD > Udp.MAX_VIDEO_BYTES) return
            m.start(p.count); held += m.reserved()
        }
        if (m.count() != p.count) return
        val parity = m.parity ?: arrayOfNulls<ByteArray>(p.parities).also { m.parity = it }
        m.lastSize = p.lastSize
        if (!m.whole() && parity[p.slot] == null) parity[p.slot] = p.data.copyOf(Udp.PARITY_BYTES)
        rebuild(p.id, m, p.slot, nowUs)
        if (m.skip) return drain(out)
        // What is still missing is lost; a later parity or a repair may bring it.
        for (i in m.frontier until m.count()) if (!m.has(i)) addGap(p.id, i, false, nowUs)
        m.frontier = m.count()
        limitGaps(nowUs); drain(out)
    }
    private fun video(f: UdpFragment, nowUs: Long, out: Output) {
        // The fragment itself, after parity rebuilt it: it was not lost.
        if (made.isNotEmpty()) { val at = made.indexOfFirst { it.id == f.id && it.index == f.index }; if (at >= 0) made.removeAt(at) }
        if (f.id < next) return
        if (f.id - next >= Udp.VIDEO_WINDOW) { dropUntilKey(nowUs, STALL_LOSS); abandonBefore(f.id - Udp.VIDEO_WINDOW + 1) }
        close(f.id - 1, nowUs)
        while (next + window.size <= f.id) window.addLast(Entry())
        val m = window[(f.id - next).toInt()]
        if (m.skip) return
        if (!m.started()) {
            if (held + f.count.toLong() * Udp.FRAGMENT_PAYLOAD > Udp.MAX_VIDEO_BYTES) return  // stays a gap: deadline, then key frame
            m.start(f.count); held += m.reserved()
        }
        if (m.count() != f.count || !m.add(f)) return
        if (f.index == 0) { inspect(f.id, m, f, nowUs); if (m.skip) return drain(out) }
        // A fragment that came late may be the last one a waiting parity needed.
        val parity = m.parity
        if (parity != null) { rebuild(f.id, m, f.index % parity.size, nowUs); if (m.skip) return drain(out) }
        val end = if (f.id <= closed) m.count() else f.index
        for (i in m.frontier until end) if (!m.has(i)) addGap(f.id, i, false, nowUs)
        m.frontier = maxOf(m.frontier, end)
        limitGaps(nowUs); drain(out)
    }
    private fun limitGaps(nowUs: Long) {
        if (gaps.size <= Udp.MAX_GAPS) return
        gaps.removeAll { filled(it) }
        while (gaps.size > Udp.MAX_GAPS) { giveUp(gaps.first().id, nowUs); gaps.removeFirst() }
    }
    private fun tail(last: Long, hostUs: Long, nowUs: Long, out: Output) {
        echoUs = hostUs; echoAt = nowUs; finished = last
        close(last, nowUs); limitGaps(nowUs); drain(out)
    }
    // Audio is delivered as soon as it is whole and has no gaps: an unfinished
    // message just makes room for a newer one.
    private fun audio(f: UdpFragment, out: Output) {
        if (f.count > Udp.MAX_AUDIO_FRAGMENTS) return
        var slot: Audio? = null
        for (a in audio) if (a.started() && a.id == f.id) slot = a
        if (slot == null) {
            var oldest = audio[0]
            for (a in audio) if (!a.started() || a.id < oldest.id) { oldest = a; if (!a.started()) break }
            if (oldest.started()) lost += oldest.missing()
            oldest.release(); oldest.start(f.count); oldest.id = f.id
            slot = oldest
        }
        if (slot.count() != f.count || !slot.add(f) || !slot.whole()) return
        val bytes = slot.take(); slot.release()
        val e = UdpEnvelope()
        if (Udp.parseEnvelope(bytes, 0, bytes.size, Udp.CLASS_AUDIO, e) && e.epoch == epoch) out.messages.add(Message(false, false, e.epoch, e.pts, bytes))
    }
    private fun nack(nowUs: Long, out: Output) {
        val plain = nackPlain
        plain[0] = Udp.KIND_NACK.toByte()
        var size = 2
        var lastId = 0L
        var lastEnd = 0
        var runAt = 0
        var sent = 0
        fun flush() {
            if (size > 2) { plain[1] = ((size - 2) / Udp.NACK_ENTRY_BYTES).toByte(); out.control.add(plain.copyOf(size)); size = 2; ++sent; ++nacks }
        }
        var due = Long.MAX_VALUE
        for (gap in gaps) {
            if (gap.nacks > Udp.MAX_NACK_REPEATS || filled(gap)) continue
            if (gap.due > nowUs) { due = minOf(due, gap.due); continue }
            if (gap.whole && entry(gap.id)!!.started()) { gap.nacks = Udp.MAX_NACK_REPEATS + 1; continue }  // its fragments have gaps of their own
            if (size > 2 && !gap.whole && gap.id == lastId && gap.index == lastEnd) Udp.put16(plain, runAt, ++lastEnd - Udp.get16(plain, runAt - 2))
            else {
                if (size == 2 + Udp.MAX_NACK_ENTRIES * Udp.NACK_ENTRY_BYTES) { flush(); if (sent == Udp.MAX_NACKS_PER_TICK) { nackDue = nowUs; return } }
                val at = size; size += Udp.NACK_ENTRY_BYTES
                // A wholly lost message: its size is unknown, the Host clamps the run.
                Udp.put32(plain, at, gap.id); Udp.put16(plain, at + 4, gap.index); Udp.put16(plain, at + 6, if (gap.whole) Udp.MAX_FRAGMENTS else 1)
                lastId = gap.id; lastEnd = gap.index + 1; runAt = at + 6
            }
            ++gap.nacks; gap.due = nowUs + nackRepeat()
            if (gap.nacks <= Udp.MAX_NACK_REPEATS) due = minOf(due, gap.due)
        }
        flush()
        nackDue = due
    }
    private fun feedback(nowUs: Long, out: Output) {
        val plain = ByteArray(Udp.FEEDBACK_BYTES); plain[0] = Udp.KIND_FEEDBACK.toByte()
        Udp.put32(plain, 2, highestWhole); Udp.put32(plain, 6, received.toLong()); Udp.put32(plain, 10, lost.toLong())
        if (echoUs != 0L) { Udp.put64(plain, 14, echoUs); Udp.put32(plain, 22, nowUs - echoAt); echoUs = 0 }
        Udp.put64(plain, 26, challenge)
        out.control.add(plain)
    }

    companion object {
        // udpStallCause: waiting for a key frame after the decoder queue was full, after a
        // refusal (the decoder or the path rule), after a loss given up, or after a message
        // of another epoch; waiting for the repair of the next message; nothing to deliver.
        const val STALL_QUEUE_FULL = 1
        const val STALL_REFUSED = 2
        const val STALL_LOSS = 3
        const val STALL_EPOCH = 4
        const val STALL_REPAIR = 5
        const val STALL_NOTHING = 6
    }
}
