package me.totb.multitap_client

import java.net.Inet4Address
import java.net.InetSocketAddress
import java.security.MessageDigest
import java.security.SecureRandom

/** udp-direct-v1, byte-identical to native/windows_video/udp_direct.hpp.
 *  Freshness as there (the wire is unchanged): an
 *  address learned from a ping takes at most MAX_LEARNED slots, leaves when it
 *  does not answer our challenge within LEARNED_TRIAL_US (or goes silent for
 *  LEARNED_IDLE_US), and a broker candidate displaces it; a ping whose nonce
 *  was already seen from another address is a replay, neither answered nor
 *  learned. Never probed: the any address, multicast, 0.0.0.0/8, 240/4 and
 *  broadcast, nor loopback unless the relay itself is loopback. */
internal class UdpDirect {
    private class Peer(val endpoint: InetSocketAddress, val learned: Boolean = false, val added: Long = 0L) {
        val nonce = ByteArray(16)
        var next = 0L; var sent = 0L; var seen = 0L; var rtt = 0L; var waiting = false
    }
    private class SeenPing(val nonce: ByteArray, val from: InetSocketAddress, val at: Long)
    private val random = SecureRandom()
    private var sendKey: ByteArray? = null
    private var receiveKey: ByteArray? = null
    private val peers = ArrayList<Peer>()
    private var relay: Peer? = null
    private var repliesAt = 0L
    private var replies = 0
    private val seenPings = arrayOfNulls<SeenPing>(REPLAY_RING)
    private var seenNext = 0

    fun configure(keys: UdpSessionKeys) {
        val label = "multitap-udp-direct-v1".toByteArray(Charsets.US_ASCII)
        sendKey = UdpCrypto.hmacSha256(keys.send.key, label)
        receiveKey = UdpCrypto.hmacSha256(keys.receive.key, label)
    }
    fun relay(endpoint: InetSocketAddress) { if (relay?.endpoint != endpoint) relay = Peer(endpoint) }
    fun relayRtt(now: Long=0) = relay?.let { if(now>0&&(it.seen==0L||now-it.seen>=ALIVE_US))0L else it.rtt } ?: 0L
    /** An address this path may probe. */
    fun routable(endpoint: InetSocketAddress): Boolean {
        val address = endpoint.address ?: return false
        if (endpoint.port !in 1..65535 || address.isAnyLocalAddress || address.isMulticastAddress) return false
        if (address is Inet4Address) {
            val first = address.address[0].toInt() and 0xff
            if (first == 0 || first >= 224) return false
        }
        return !address.isLoopbackAddress || relay?.endpoint?.address?.isLoopbackAddress == true
    }
    /** A candidate the broker supplied. */
    fun candidate(endpoint: InetSocketAddress) { add(endpoint, false, 0L) }
    fun learned() = peers.count { it.learned }
    fun candidates() = peers.size
    private fun add(endpoint: InetSocketAddress, fromPing: Boolean, now: Long): Boolean {
        if (!routable(endpoint) || endpoint == relay?.endpoint || find(endpoint) != null) return false
        if (fromPing && learned() >= MAX_LEARNED) return false
        if (peers.size >= MAX_CANDIDATES) {
            if (fromPing) return false
            // A broker candidate displaces a learned address, an unconfirmed one first.
            val victim = peers.firstOrNull { it.learned && it.seen == 0L } ?: peers.firstOrNull { it.learned } ?: return false
            peers.remove(victim)
        }
        peers.add(Peer(endpoint, fromPing, now))
        return true
    }
    /** The peer repeats one nonce until answered, always from the same address;
     *  the same nonce from another address is a captured ping played back. */
    private fun freshPing(nonce: ByteArray, from: InetSocketAddress, now: Long): Boolean {
        for (seen in seenPings) {
            if (seen != null && now - seen.at in 0 until REPLAY_WINDOW_US && MessageDigest.isEqual(seen.nonce, nonce)) return seen.from == from
        }
        seenPings[seenNext++ % REPLAY_RING] = SeenPing(nonce.copyOf(), from, now)
        return true
    }
    fun tick(now: Long, send: (InetSocketAddress, ByteArray) -> Unit) {
        if (sendKey == null) return
        // Learned addresses that never answered our challenge, or stopped.
        peers.removeAll { it.learned && (if (it.seen > 0L) now - it.seen >= LEARNED_IDLE_US else now - it.added >= LEARNED_TRIAL_US) }
        fun probe(peer: Peer) {
            if (now < peer.next) return
            peer.next = now + PROBE_EVERY_US
            if (!peer.waiting || now - peer.sent >= ALIVE_US) { random.nextBytes(peer.nonce); peer.sent = now; peer.waiting = true }
            send(peer.endpoint, seal(1, peer.nonce))
        }
        relay?.let(::probe); peers.forEach(::probe)
    }
    fun received(bytes: ByteArray, size: Int, from: InetSocketAddress, now: Long, send: (InetSocketAddress, ByteArray) -> Unit): Boolean {
        val key = receiveKey ?: return false
        if (size != PACKET_BYTES || bytes[0] != 84.toByte() || bytes[1] != 86.toByte() || bytes[2] != 68.toByte() || bytes[3] != 1.toByte() || bytes[4].toInt() !in 1..2) return false
        if (!MessageDigest.isEqual(UdpCrypto.hmacSha256(key, bytes.copyOfRange(0, 21)), bytes.copyOfRange(21, PACKET_BYTES))) return false
        if (bytes[4] == 1.toByte()) {
            if (now - repliesAt >= 1000000) { repliesAt = now; replies = 0 }
            if (++replies > 32) return true
            val nonce = bytes.copyOfRange(5, 21)
            if (!freshPing(nonce, from, now)) return true  // a replay from elsewhere
            add(from, true, now)
            send(from, seal(2, nonce))
        } else {
            val peer = find(from)
            if (peer != null && peer.waiting && now - peer.sent < ALIVE_US && MessageDigest.isEqual(peer.nonce, bytes.copyOfRange(5, 21))) {
                peer.rtt = maxOf(1, now - peer.sent); peer.seen = now; peer.waiting = false
            }
        }
        return true
    }
    fun target(now: Long): InetSocketAddress? {
        val relayRtt = relayRtt(now)
        return peers.filter { it.seen > 0 && now - it.seen < ALIVE_US && (relayRtt == 0L || it.rtt <= relayRtt + 2000) }.minByOrNull { it.rtt }?.endpoint
    }
    fun accepts(from: InetSocketAddress, now: Long) = peers.any { it.endpoint == from && it.seen > 0 && now - it.seen < ALIVE_US }
    fun rtt(endpoint: InetSocketAddress) = find(endpoint)?.rtt ?: 0
    fun wipe() { sendKey?.fill(0); receiveKey?.fill(0); sendKey = null; receiveKey = null }
    private fun find(endpoint: InetSocketAddress) = relay?.takeIf { it.endpoint == endpoint } ?: peers.find { it.endpoint == endpoint }
    private fun seal(kind: Int, nonce: ByteArray): ByteArray {
        val packet = ByteArray(PACKET_BYTES)
        packet[0] = 84; packet[1] = 86; packet[2] = 68; packet[3] = 1; packet[4] = kind.toByte()
        nonce.copyInto(packet, 5)
        UdpCrypto.hmacSha256(sendKey!!, packet.copyOfRange(0, 21)).copyInto(packet, 21)
        return packet
    }
    companion object {
        const val PACKET_BYTES = 53
        const val MAX_CANDIDATES = 5
        const val MAX_LEARNED = 2
        const val REPLAY_RING = 64
        const val PROBE_EVERY_US = 250000L
        const val ALIVE_US = 1500000L
        const val LEARNED_TRIAL_US = 3000000L
        const val LEARNED_IDLE_US = 10000000L
        const val REPLAY_WINDOW_US = 10000000L
    }
}
