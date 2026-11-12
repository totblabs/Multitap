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
    fun take(): ByteArray { val result = if (size == bytes.size) bytes else bytes.copyOf(size); bytes = ByteArray(0); return result }
    fun release() { count = 0; have = 0; size = 0; got = BooleanArray(0); bytes = ByteArray(0) }
}

/**
 * Client side. The caller opens each datagram and passes the plaintext; [tick]
 * runs the timers, every few milliseconds whether or not media arrives. It
 * gets messages to hand to the decoder and control plaintexts to seal and
 * send. Its RTT is the Host's, carried by TAIL. Malformed input only ever
 * drops data.
 */
internal class UdpReceiver(private var epoch: Int, nowUs: Long) {
    class Message(val video: Boolean, val key: Boolean, val epoch: Int, val pts: Long, val bytes: ByteArray)
    class Output { val messages = ArrayList<Message>(); val control = ArrayList<ByteArray>() }

    // Indices below frontier are passed.
    private class Entry : UdpPartial() { var frontier = 0; var known = false; var skip = false; val envelope = UdpEnvelope() }
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
    private var held = 0L
    private var received = 0
    private var lost = 0
    private var highestWhole = 0L
    private val window = ArrayDeque<Entry>()  // window[0] is message `next`
    private val gaps = ArrayDeque<Gap>()
    private val audio = Array(Udp.AUDIO_PARTIALS) { Audio() }
    private val fragment = UdpFragment()
    private val nackPlain = ByteArray(2 + Udp.MAX_NACK_ENTRIES * Udp.NACK_ENTRY_BYTES)  // every tick builds in here
    // The earliest a recorded gap may be NACKed: up to 4096 gaps are walked only then.
    // It may be early (a gap filled since), never late.
    private var nackDue = Long.MAX_VALUE

    // Diagnostics only (phone, 2026-11-02): the Host leaves UDP when FEEDBACK shows
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

    fun rttUs() = rtt
    /** Announced on WSS. What carries another epoch is dropped. */
    fun setEpoch(epoch: Int) { this.epoch = epoch }

    fun datagram(plain: ByteArray, size: Int, nowUs: Long, out: Output) {
        val f = fragment
        if (Udp.parseFragment(plain, size, f)) { ++received; if (f.cls == Udp.CLASS_AUDIO) audio(f, out) else video(f, nowUs, out) }
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
        if (awaitKey && nowUs >= keyAt && !keyArriving()) {
            val plain = ByteArray(Udp.KEYFRAME_BYTES); plain[0] = Udp.KIND_KEYFRAME.toByte(); Udp.put16(plain, 2, epoch)
            out.control.add(plain); keyAt = nowUs + Udp.KEYFRAME_INTERVAL_US; keySentAt = nowUs; ++keyRequests
        }
        if (nowUs >= feedbackAt) { feedback(nowUs, out); feedbackAt = nowUs + Udp.FEEDBACK_INTERVAL_US }
    }

    /**
     * When [tick] next has work: FEEDBACK, a NACK, a give-up or a KEYFRAME
     * repeat. A held repeat waits for the key frame that is arriving, whose
     * datagrams or give-up come first. The caller may tick sooner.
     */
    fun nextTickUs(): Long {
        var next = minOf(feedbackAt, nackDue)
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
        awaitKey = false; gaps.clear(); nackDue = Long.MAX_VALUE
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
    private fun nackDelay() = maxOf(rtt / 4, Udp.MINIMUM_NACK_DELAY_US)
    private fun nackRepeat() = maxOf(rtt * 5 / 4, Udp.MINIMUM_NACK_REPEAT_US)
    private fun addGap(id: Long, index: Int, whole: Boolean, nowUs: Long) {
        val due = nowUs + nackDelay()
        gaps.addLast(Gap(id, index, 0, whole, nowUs, due))
        nackDue = minOf(nackDue, due)
    }
    private fun dropUntilKey(nowUs: Long, cause: Int) { if (!awaitKey) { awaitKey = true; keyAt = nowUs; awaitCause = cause } }
    private fun abandonBefore(id: Long) {
        while (next < id && window.isNotEmpty()) {
            val m = window.first()
            if (!m.skip) lost += if (m.started()) m.missing() else 1
            held -= m.reserved(); window.removeFirst(); ++next
        }
        next = maxOf(next, id); closed = maxOf(closed, next - 1)
    }
    private fun giveUp(id: Long, nowUs: Long) { dropUntilKey(nowUs, STALL_LOSS); abandonBefore(id + 1) }
    private fun deliver(m: Entry, out: Output) {
        if (m.envelope.epoch != epoch) return
        if (m.envelope.key) awaitKey = false
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
        for (e in window) if (e.known && !e.skip && e.envelope.key) return true
        return false
    }
    // Fragment 0 names the message. A message of no use stops costing memory and NACKs.
    // A P-frame stays even while waiting for a key: an older message may be that key,
    // and drain sorts it out.
    private fun inspect(id: Long, m: Entry, f: UdpFragment, nowUs: Long) {
        val valid = Udp.parseEnvelope(f.data, Udp.FRAGMENT_HEADER_BYTES, f.size, Udp.CLASS_VIDEO, m.envelope)
        m.known = true
        if (valid && m.envelope.epoch == epoch) { if (m.envelope.key) abandonBefore(oldestNeeded(id)); return }
        if (!valid || m.envelope.epoch > epoch) dropUntilKey(nowUs, STALL_EPOCH)
        m.skip = true; held -= m.reserved(); m.release()
    }
    private fun video(f: UdpFragment, nowUs: Long, out: Output) {
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
            if (size > 2) { plain[1] = ((size - 2) / Udp.NACK_ENTRY_BYTES).toByte(); out.control.add(plain.copyOf(size)); size = 2; ++sent }
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
