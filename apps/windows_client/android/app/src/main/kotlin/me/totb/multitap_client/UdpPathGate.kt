package me.totb.multitap_client

import java.util.concurrent.atomic.AtomicLong

/**
 * The contract's path rule, the epoch filter and the key gate for both media
 * paths (udp-media-v1, Phase 3): windows/runner/udp_path_gate.h. Pure:
 * nothing here touches a decoder or a clock.
 */
internal class UdpPathRule {
    var engaged = false
        private set
    var epoch = 0
        private set
    private var state = UdpPathState()
    var fallbacks = 0L
        private set
    val active get() = state.active
    val awaitingKey get() = !state.delivered

    /** A stream run announced on WSS: PTS restarts with it, so "newer" starts over. */
    fun stream(epoch: Int) { this.epoch = epoch; state.lastPts = 0 }
    /** A new decoder takes nothing before a key frame. */
    fun decoderStarted() { state.delivered = false; state.lastPts = 0 }
    /**
     * From here on both paths follow the rule. [keyed]: WSS already fed this
     * decoder a key frame, so its P-frames go on.
     */
    fun engage(keyed: Boolean) { engaged = true; state = UdpPathState(Udp.PATH_WSS, epoch, 0, keyed) }
    fun disengage() { engaged = false; state = UdpPathState(); fallbacks = 0 }

    /**
     * The state this video message would leave, for [commit] once the decoder
     * took it, or null: a full queue must leave the rule as it was, because WSS
     * offers the same packet again.
     */
    fun admit(from: Int, epoch: Int, key: Boolean, pts: Long, tile: Boolean): UdpPathState? {
        if (epoch != this.epoch) return null
        // Tiles are stamped from the tick, ahead of the encoder's PTS, and are no frame:
        // they follow the active path and leave "newer" to the video.
        if (tile) return if (state.delivered && epoch == state.epoch && from == state.active) state.copy() else null
        val next = state.copy()
        return if (Udp.acceptVideo(next, from, epoch, key, pts)) next else null
    }
    fun commit(next: UdpPathState) {
        if (state.active == Udp.PATH_UDP && next.active == Udp.PATH_WSS) ++fallbacks
        state = next
    }
    /** Audio has no path: it needs only a decoder that took this epoch's key frame. */
    fun admitAudio(epoch: Int) = epoch == this.epoch && state.delivered
}

/**
 * Where both paths meet the decoder. The lock covers the rule and the
 * decoder's admission together, so frames cannot pass each other, and
 * [attach] cannot return while an offer is inside the decoder queue. Until the
 * Host's key arrives this is a straight pass, and WSS bytes reach the decoder
 * as they always did.
 *
 * Android differences from the Windows gate: the leg does not join its thread
 * on stop, so each UDP offer names its leg and only the current one passes;
 * and a WSS packet for another lease, or with no decoder attached, takes the
 * decoder's own answer (session_replaced) instead of reading as taken.
 */
internal class UdpPathGate {
    /** The decoder's admission of one whole message: an [AndroidMediaStatus] byte. */
    fun interface Offer { fun offer(message: ByteArray, watch: Boolean): Byte }
    enum class Delivery { ACCEPTED, DROPPED, FULL }
    class View(val active: Int, val fallbacks: Long, val keyedStream: Long)

    private val lock = Any()
    private var offer: Offer? = null
    private var owner = 0L
    private var leg = 0L
    private val rule = UdpPathRule()
    private var generation = 0L
    private var keyedStream = 0L
    @Volatile private var epoch = 0
    private val wssKeys = AtomicLong()

    /** Main thread: the decoder of media lease [owner]; null detaches. */
    fun attach(owner: Long, offer: Offer?) {
        synchronized(lock) {
            this.offer = offer
            this.owner = if (offer == null) 0 else owner
            if (offer != null) rule.decoderStarted()
        }
    }
    fun attached() = synchronized(lock) { offer != null }
    fun stream(epoch: Int, generation: Long) {
        synchronized(lock) { rule.stream(epoch); this.generation = generation; this.epoch = epoch }
    }
    fun engage(keyed: Boolean) { synchronized(lock) { rule.engage(keyed) } }
    /** The rule is off; from now on only leg [leg]'s messages may pass (0: none). */
    fun disengage(leg: Long = 0) { synchronized(lock) { rule.disengage(); this.leg = leg } }

    /**
     * Media queue, WSS, a packet whose first bytes are [head] ([length] in all):
     * [offer] admits it. A packet the rule drops reads as taken, a dropped frame
     * is no failure. The epoch is the announced one: Dart has already refused any other.
     */
    fun offerWss(lease: Long, head: ByteArray, length: Int, offer: () -> Byte): Byte {
        synchronized(lock) {
            val envelope = UdpEnvelope()
            if (this.offer == null || lease != owner || !rule.engaged || !Udp.parseEnvelope(head, 0, length, Udp.CLASS_VIDEO, envelope)) return offer()
            val next = rule.admit(Udp.PATH_WSS, rule.epoch, envelope.key, envelope.pts, tile(head, length)) ?: return AndroidMediaStatus.OK
            val result = offer()
            if (result == AndroidMediaStatus.OK) { rule.commit(next); if (envelope.key) wssKeys.incrementAndGet() }
            return result
        }
    }

    /**
     * Leg thread, a whole UDP message of leg [leg]. The decoder's stable input
     * has reserved zero where the epoch travels. Every video message is watched:
     * a frame's input tag comes on WSS, possibly after the frame.
     */
    fun offerUdp(leg: Long, message: ByteArray, video: Boolean, key: Boolean, epoch: Int, pts: Long): Delivery {
        synchronized(lock) {
            val offer = this.offer
            if (offer == null || leg != this.leg) return Delivery.DROPPED
            var next: UdpPathState? = null
            if (video) next = rule.admit(Udp.PATH_UDP, epoch, key, pts, tile(message, message.size)) ?: return Delivery.DROPPED
            else if (!rule.admitAudio(epoch)) return Delivery.DROPPED
            message[2] = 0; message[3] = 0
            val result = offer.offer(message, video)
            if (result == AndroidMediaStatus.QUEUE_FULL) return Delivery.FULL
            if (result != AndroidMediaStatus.OK) return Delivery.DROPPED
            if (next != null) { rule.commit(next); if (key) keyedStream = generation }
            return Delivery.ACCEPTED
        }
    }

    fun epoch() = epoch
    /** Moves with every WSS key frame the rule accepted: the leg's cue that UDP lost a switch. */
    fun wssKeys() = wssKeys.get()
    fun view(): View = synchronized(lock) { View(if (rule.active == Udp.PATH_UDP) 1 else 0, rule.fallbacks, keyedStream) }
    /** Video counts from UDP now (the leg's stall watch). */
    fun udpActive() = synchronized(lock) { rule.engaged && rule.active == Udp.PATH_UDP }

    private fun tile(message: ByteArray, size: Int) = size > 12 && size - 12 >= 4 && message.size >= 16 &&
        message[12] == 'T'.code.toByte() && message[13] == 'S'.code.toByte() && message[14] == 'L'.code.toByte() && message[15] == '1'.code.toByte()
}
