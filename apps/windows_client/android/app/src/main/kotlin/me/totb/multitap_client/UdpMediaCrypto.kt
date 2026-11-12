package me.totb.multitap_client

import java.security.MessageDigest
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * UDP media protection, Phase 3 contract: the client half of
 * native/windows_video/udp_media_crypto.hpp. AES-GCM and HMAC come from
 * javax.crypto; X25519 is RFC 7748 here, because Android has no X25519 key
 * agreement at API 26. An active Broker can substitute the public keys: this
 * is not end-to-end. Any failure here means the session stays on WSS. Custom
 * framing: it needs independent review.
 *
 * Our own copies of key material are wiped when they go. The key objects
 * javax.crypto builds from them keep a copy that cannot be wiped.
 */
internal object UdpCrypto {
    const val SEQUENCE_LIMIT = 1L shl 48
    const val LEG_ID_BYTES = 8
    const val KEY_BYTES = 32
    const val SALT_BYTES = 4
    const val NONCE_BYTES = 12
    const val REPLAY_SPAN = 2048
    // Above the 25-byte probe, so the two never mix.
    const val MINIMUM_DATAGRAM = Udp.HEADER_BYTES + 1 + Udp.TAG_BYTES
    private val keyLabel = "multitap-udp-media-v1".toByteArray(Charsets.US_ASCII)

    fun sha256(data: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(data)

    fun hmacSha256(key: ByteArray, data: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        return mac.doFinal(data)
    }

    /** RFC 5869. Null for an output size it cannot give. */
    fun hkdfSha256(salt: ByteArray, ikm: ByteArray, info: ByteArray, outSize: Int): ByteArray? {
        if (outSize <= 0 || outSize > 255 * 32) return null
        val prk = hmacSha256(salt, ikm)
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(prk, "HmacSHA256"))
        prk.fill(0)
        val out = ByteArray(outSize)
        var block = ByteArray(0)
        var done = 0
        var counter = 1
        while (done < outSize) {
            mac.update(block); mac.update(info); mac.update(counter.toByte())
            val next = mac.doFinal()
            block.fill(0); block = next
            val take = minOf(32, outSize - done)
            System.arraycopy(block, 0, out, done, take)
            done += take; ++counter
        }
        block.fill(0)
        return out
    }

    /** The contract's HKDF. Both sides pass the public keys in the same order. */
    fun deriveKeys(shared: ByteArray, client: ByteArray, host: ByteArray, sessionId: ByteArray, clientRole: Boolean): UdpSessionKeys? {
        val okm = hkdfSha256(sha256(sessionId), shared, keyLabel + client + host, 2 * KEY_BYTES + 2 * SALT_BYTES) ?: return null
        val clientToHost = UdpDirectionKey(okm.copyOfRange(0, KEY_BYTES), okm.copyOfRange(2 * KEY_BYTES, 2 * KEY_BYTES + SALT_BYTES))
        val hostToClient = UdpDirectionKey(okm.copyOfRange(KEY_BYTES, 2 * KEY_BYTES), okm.copyOfRange(2 * KEY_BYTES + SALT_BYTES, okm.size))
        okm.fill(0)
        return if (clientRole) UdpSessionKeys(clientToHost, hostToClient) else UdpSessionKeys(hostToClient, clientToHost)
    }

    /** A relay proof (BIND type 1, ACK type 2): type, leg ID, HMAC(ticket, label | leg ID). */
    fun proof(type: Int, label: String, id: ByteArray, ticket: ByteArray): ByteArray {
        val proof = ByteArray(1 + LEG_ID_BYTES + 32)
        proof[0] = type.toByte()
        System.arraycopy(id, 0, proof, 1, LEG_ID_BYTES)
        System.arraycopy(hmacSha256(ticket, label.toByteArray(Charsets.US_ASCII) + id), 0, proof, 1 + LEG_ID_BYTES, 32)
        return proof
    }

    /**
     * This device's providers end to end, before any UDP goes out: two
     * throwaway key pairs, the HKDF, and a maximum-size datagram (1200 bytes)
     * sealed, refused once tampered, and opened, in each direction. The JVM
     * tests run on the JDK's providers; a phone runs Conscrypt.
     */
    fun selfCheck(): Boolean = try {
        val client = UdpKeyPair()
        val host = UdpKeyPair()
        val one = ByteArray(KEY_BYTES)
        val two = ByteArray(KEY_BYTES)
        val session = "multitap-udp-self-check".toByteArray(Charsets.US_ASCII)
        val agreed = client.agree(host.publicKey, one) && host.agree(client.publicKey, two) && one.contentEquals(two)
        val clientKeys = if (agreed) deriveKeys(one, client.publicKey, host.publicKey, session, true) else null
        val hostKeys = if (agreed) deriveKeys(two, client.publicKey, host.publicKey, session, false) else null
        one.fill(0); two.fill(0); client.wipe(); host.wipe()
        clientKeys != null && hostKeys != null &&
            roundTrip(UdpSealer(hostKeys.send, ByteArray(LEG_ID_BYTES)), UdpOpener(clientKeys.receive)) &&
            roundTrip(UdpSealer(clientKeys.send, ByteArray(LEG_ID_BYTES)), UdpOpener(hostKeys.receive))
    } catch (_: Throwable) { false }

    private fun roundTrip(sealer: UdpSealer, opener: UdpOpener): Boolean {
        val plain = ByteArray(Udp.MAX_PLAINTEXT) { (it * 7).toByte() }
        val datagram = ByteArray(Udp.MAX_DATAGRAM)
        val back = ByteArray(Udp.MAX_PLAINTEXT)
        if (sealer.seal(plain, plain.size, datagram) != Udp.MAX_DATAGRAM) return false
        val tampered = datagram.copyOf().also { it[Udp.MAX_DATAGRAM / 2] = (it[Udp.MAX_DATAGRAM / 2].toInt() xor 1).toByte() }
        return opener.open(tampered, tampered.size, back) == 0 && opener.open(datagram, datagram.size, back) == plain.size && back.contentEquals(plain)
    }
}

/** One direction's key and nonce salt. A sealer or opener consumes it. */
internal class UdpDirectionKey(val key: ByteArray, val salt: ByteArray) {
    var ready = true
        private set
    fun wipe() { key.fill(0); salt.fill(0); ready = false }
}

internal class UdpSessionKeys(val send: UdpDirectionKey, val receive: UdpDirectionKey)

/**
 * RFC 7748 X25519 on 16 limbs of 16 bits (the TweetNaCl layout): the ladder
 * swaps by mask, not by branch. Runs once per grant.
 */
internal object X25519 {
    private val base = ByteArray(32).also { it[0] = 9 }
    private val a24 = LongArray(16).also { it[0] = 0xDB41; it[1] = 1 }

    fun publicKey(secret: ByteArray): ByteArray = ByteArray(32).also { scalarMult(it, secret, base) }

    /** [out] = X25519([scalar], [u]). Bit 255 of [u] is masked, as RFC 7748 says. */
    fun scalarMult(out: ByteArray, scalar: ByteArray, u: ByteArray) {
        val z = scalar.copyOf(32)
        z[31] = ((z[31].toInt() and 127) or 64).toByte()
        z[0] = (z[0].toInt() and 248).toByte()
        val x = LongArray(16)
        unpack(x, u)
        val a = LongArray(16); val b = x.copyOf(); val c = LongArray(16); val d = LongArray(16)
        val e = LongArray(16); val f = LongArray(16)
        a[0] = 1; d[0] = 1
        for (i in 254 downTo 0) {
            val r = ((z[i ushr 3].toInt() and 0xff) ushr (i and 7)) and 1
            swap(a, b, r); swap(c, d, r)
            add(e, a, c); sub(a, a, c); add(c, b, d); sub(b, b, d)
            mul(d, e, e); mul(f, a, a); mul(a, c, a); mul(c, b, e)
            add(e, a, c); sub(a, a, c); mul(b, a, a); sub(c, d, f)
            mul(a, c, a24); add(a, a, d); mul(c, c, a); mul(a, d, f); mul(d, b, x); mul(b, e, e)
            swap(a, b, r); swap(c, d, r)
        }
        invert(c, c)
        mul(a, a, c)
        pack(out, a)
        z.fill(0)
        for (limbs in arrayOf(x, a, b, c, d, e, f)) limbs.fill(0)
    }

    private fun carry(o: LongArray) {
        for (i in 0 until 16) {
            o[i] += 1L shl 16
            val c = o[i] shr 16
            if (i < 15) o[i + 1] += c - 1 else o[0] += 38 * (c - 1)
            o[i] -= c shl 16
        }
    }
    private fun swap(p: LongArray, q: LongArray, bit: Int) {
        val mask = (bit - 1).toLong().inv()
        for (i in 0 until 16) { val t = mask and (p[i] xor q[i]); p[i] = p[i] xor t; q[i] = q[i] xor t }
    }
    private fun pack(o: ByteArray, n: LongArray) {
        val m = LongArray(16)
        val t = n.copyOf()
        carry(t); carry(t); carry(t)
        for (j in 0 until 2) {
            m[0] = t[0] - 0xffed
            for (i in 1 until 15) { m[i] = t[i] - 0xffff - ((m[i - 1] shr 16) and 1); m[i - 1] = m[i - 1] and 0xffff }
            m[15] = t[15] - 0x7fff - ((m[14] shr 16) and 1)
            val b = ((m[15] shr 16) and 1).toInt()
            m[14] = m[14] and 0xffff
            swap(t, m, 1 - b)
        }
        for (i in 0 until 16) { o[2 * i] = t[i].toByte(); o[2 * i + 1] = (t[i] shr 8).toByte() }
        m.fill(0); t.fill(0)
    }
    private fun unpack(o: LongArray, n: ByteArray) {
        for (i in 0 until 16) o[i] = (n[2 * i].toLong() and 0xff) + ((n[2 * i + 1].toLong() and 0xff) shl 8)
        o[15] = o[15] and 0x7fff
    }
    private fun add(o: LongArray, a: LongArray, b: LongArray) { for (i in 0 until 16) o[i] = a[i] + b[i] }
    private fun sub(o: LongArray, a: LongArray, b: LongArray) { for (i in 0 until 16) o[i] = a[i] - b[i] }
    private fun mul(o: LongArray, a: LongArray, b: LongArray) {
        val t = LongArray(31)
        for (i in 0 until 16) for (j in 0 until 16) t[i + j] += a[i] * b[j]
        for (i in 0 until 15) t[i] += 38 * t[i + 16]
        for (i in 0 until 16) o[i] = t[i]
        carry(o); carry(o)
    }
    private fun invert(o: LongArray, i: LongArray) {
        val c = i.copyOf()
        for (a in 253 downTo 0) { mul(c, c, c); if (a != 2 && a != 4) mul(c, c, i) }
        for (a in 0 until 16) o[a] = c[a]
    }
}

/** A fresh key pair per grant. An all-zero shared secret fails. */
internal class UdpKeyPair internal constructor(secret: ByteArray) {
    constructor() : this(ByteArray(UdpCrypto.KEY_BYTES).also { SecureRandom().nextBytes(it) })
    private val secret = secret.copyOf(UdpCrypto.KEY_BYTES).also { secret.fill(0) }
    val publicKey: ByteArray = X25519.publicKey(this.secret)

    /** Writes the RFC 7748 shared secret into [shared]; false, and all zero, for a small-order key. */
    fun agree(peer: ByteArray, shared: ByteArray): Boolean {
        if (peer.size != UdpCrypto.KEY_BYTES || shared.size != UdpCrypto.KEY_BYTES) return false
        X25519.scalarMult(shared, secret, peer)
        var any = 0
        for (b in shared) any = any or b.toInt()
        return any != 0
    }
    fun wipe() = secret.fill(0)
}

/**
 * One direction's sender. The sequence is private and only moves forward, and
 * no sealed datagram is ever kept, so a retransmission cannot reuse one.
 */
internal class UdpSealer(key: UdpDirectionKey, ownLegId: ByteArray) {
    private val salt = key.salt.copyOf()
    private val legId = ownLegId.copyOf(UdpCrypto.LEG_ID_BYTES)
    private val secret: SecretKeySpec?
    private val cipher: Cipher?
    private val nonce = ByteArray(UdpCrypto.NONCE_BYTES)
    private var next = 1L

    init {
        var spec: SecretKeySpec? = null
        var aes: Cipher? = null
        if (key.ready) try { spec = SecretKeySpec(key.key, "AES"); aes = Cipher.getInstance("AES/GCM/NoPadding") } catch (_: Exception) { spec = null; aes = null }
        secret = spec; cipher = aes
        key.wipe()
    }

    /**
     * Writes the datagram into [out] and returns its size, or 0 for nothing
     * sent. At the sequence limit it refuses for good, and the Host returns to WSS.
     */
    fun seal(plain: ByteArray, size: Int, out: ByteArray): Int {
        val aes = cipher ?: return 0
        if (size <= 0 || size > Udp.MAX_PLAINTEXT || exhausted()) return 0
        out[0] = 3; System.arraycopy(legId, 0, out, 1, UdpCrypto.LEG_ID_BYTES); Udp.put64(out, 9, next)
        System.arraycopy(salt, 0, nonce, 0, UdpCrypto.SALT_BYTES); System.arraycopy(out, 9, nonce, UdpCrypto.SALT_BYTES, 8)
        return try {
            aes.init(Cipher.ENCRYPT_MODE, secret, GCMParameterSpec(Udp.TAG_BYTES * 8, nonce))
            aes.updateAAD(out, 0, Udp.HEADER_BYTES)
            val written = aes.doFinal(plain, 0, size, out, Udp.HEADER_BYTES)
            ++next
            if (written == size + Udp.TAG_BYTES) Udp.HEADER_BYTES + written else 0
        } catch (_: Exception) {
            // javax.crypto refuses a nonce twice in a row, so a failure costs its sequence.
            ++next
            0
        }
    }
    fun exhausted() = next >= UdpCrypto.SEQUENCE_LIMIT
}

/** The last 2,048 sequences: the newest seen and those just before it. */
internal class UdpReplayWindow {
    private var highest = 0L
    private val bits = LongArray(UdpCrypto.REPLAY_SPAN / 64)

    fun fresh(sequence: Long): Boolean {
        if (sequence > highest) return true
        return highest - sequence < UdpCrypto.REPLAY_SPAN && bits[word(sequence)] and mask(sequence) == 0L
    }
    fun accept(sequence: Long) {
        if (sequence > highest) {
            if (sequence - highest >= UdpCrypto.REPLAY_SPAN) bits.fill(0)
            else { var n = highest + 1; while (n <= sequence) { bits[word(n)] = bits[word(n)] and mask(n).inv(); ++n } }
            highest = sequence
        }
        bits[word(sequence)] = bits[word(sequence)] or mask(sequence)
    }
    private fun word(sequence: Long) = (sequence / 64 % (UdpCrypto.REPLAY_SPAN / 64)).toInt()
    private fun mask(sequence: Long) = 1L shl (sequence % 64).toInt()
}

/** One direction's receiver. Anything that fails is dropped silently. */
internal class UdpOpener(key: UdpDirectionKey) {
    private val salt = key.salt.copyOf()
    private val secret: SecretKeySpec?
    private val cipher: Cipher?
    private val nonce = ByteArray(UdpCrypto.NONCE_BYTES)
    private val replay = UdpReplayWindow()

    init {
        var spec: SecretKeySpec? = null
        var aes: Cipher? = null
        if (key.ready) try { spec = SecretKeySpec(key.key, "AES"); aes = Cipher.getInstance("AES/GCM/NoPadding") } catch (_: Exception) { spec = null; aes = null }
        secret = spec; cipher = aes
        key.wipe()
    }

    /**
     * Writes the plaintext (MAX_PLAINTEXT bytes at most) and returns its size,
     * or 0. The replay window moves only after authentication, so a forgery
     * cannot move it.
     */
    fun open(datagram: ByteArray, size: Int, plain: ByteArray): Int {
        val aes = cipher ?: return 0
        if (size < UdpCrypto.MINIMUM_DATAGRAM || size > Udp.MAX_DATAGRAM || size > datagram.size || datagram[0].toInt() != 3) return 0
        val sequence = Udp.get64(datagram, 9)
        // Unsigned on the wire: a negative Long is above the limit too.
        if (sequence <= 0 || sequence >= UdpCrypto.SEQUENCE_LIMIT || !replay.fresh(sequence)) return 0
        System.arraycopy(salt, 0, nonce, 0, UdpCrypto.SALT_BYTES); System.arraycopy(datagram, 9, nonce, UdpCrypto.SALT_BYTES, 8)
        val plainSize = size - Udp.HEADER_BYTES - Udp.TAG_BYTES
        val opened = try {
            aes.init(Cipher.DECRYPT_MODE, secret, GCMParameterSpec(Udp.TAG_BYTES * 8, nonce))
            aes.updateAAD(datagram, 0, Udp.HEADER_BYTES)
            aes.doFinal(datagram, Udp.HEADER_BYTES, plainSize + Udp.TAG_BYTES, plain, 0) == plainSize
        } catch (_: Exception) { false }
        if (!opened) { plain.fill(0, 0, minOf(plainSize, plain.size)); return 0 }
        replay.accept(sequence)
        return plainSize
    }
}
