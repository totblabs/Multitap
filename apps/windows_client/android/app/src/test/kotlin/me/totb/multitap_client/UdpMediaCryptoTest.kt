package me.totb.multitap_client

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

// udp-media-v1 crypto against published vectors (RFC 7748, RFC 5869), the known
// answers of apps/windows_host/src/udp_media_test.cpp, and vectors printed by the
// Node QA leg (OpenSSL underneath) for fixed keys. JVM only: no device involved.
internal fun hex(text: String): ByteArray = ByteArray(text.length / 2) { text.substring(2 * it, 2 * it + 2).toInt(16).toByte() }

class UdpMediaCryptoTest {
    private val alice = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"
    private val alicePublic = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"
    private val bob = "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb"
    private val bobPublic = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"
    private val rfcShared = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"
    private val hostLeg = byteArrayOf(1, 2, 3, 4, 5, 6, 7, 8)
    private val clientLeg = byteArrayOf(9, 10, 11, 12, 13, 14, 15, 16)
    // Known answer from OpenSSL (node crypto.hkdfSync), as in the Host's udp_media_test.cpp.
    private val expectedOkm = hex("9cc448a5129c9f7cd41a4a8e69f17c4ae855055a968a2bfcf75b7ff6ecb1234534e07e518d53706d911384c7a0af96dad9f4d5074bb6796cd903de755e689dbacb08f0937879a79c")

    private fun derive(client: Boolean) = UdpCrypto.deriveKeys(hex(rfcShared), hex(alicePublic), hex(bobPublic), "session-1234".toByteArray(), client)!!

    private inner class Pair {
        val host = derive(false)
        val client = derive(true)
        val hostSeal = UdpSealer(host.send, hostLeg)
        val clientSeal = UdpSealer(client.send, clientLeg)
        val hostOpen = UdpOpener(host.receive)
        val clientOpen = UdpOpener(client.receive)
    }

    private fun setSequence(sealer: UdpSealer, next: Long) {
        UdpSealer::class.java.getDeclaredField("next").apply { isAccessible = true }.setLong(sealer, next)
    }

    @Test fun rfc7748Section61() {
        val a = UdpKeyPair(hex(alice))
        val b = UdpKeyPair(hex(bob))
        assertArrayEquals(hex(alicePublic), a.publicKey)
        assertArrayEquals(hex(bobPublic), b.publicKey)
        val one = ByteArray(32)
        val two = ByteArray(32)
        assertTrue(a.agree(hex(bobPublic), one) && b.agree(hex(alicePublic), two))
        assertArrayEquals(hex(rfcShared), one)
        assertArrayEquals(hex(rfcShared), two)
        // RFC 7748 masks bit 255 of a received u-coordinate.
        val masked = hex(bobPublic).also { it[31] = (it[31].toInt() or 0x80).toByte() }
        val three = ByteArray(32)
        assertTrue(a.agree(masked, three))
        assertArrayEquals(hex(rfcShared), three)
    }

    @Test fun rfc7748Section52() {
        // These u-coordinates lie on the twist. RFC 7748 gives an output, and so does this
        // port; Windows' CNG refuses them (udp_media_test.cpp). A Host key is never one.
        val vectors = listOf(
            Triple("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
                "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"),
            Triple("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
                "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"),
        )
        for ((scalar, u, output) in vectors) {
            val out = ByteArray(32)
            X25519.scalarMult(out, hex(scalar), hex(u))
            assertArrayEquals(hex(output), out)
        }
        // The iterated test: k = X25519(k, u), u = old k, from 9.
        var k = ByteArray(32).also { it[0] = 9 }
        var u = k.copyOf()
        for (n in 1..1000) {
            val next = ByteArray(32)
            X25519.scalarMult(next, k, u)
            u = k; k = next
            if (n == 1) assertArrayEquals(hex("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"), k)
        }
        assertArrayEquals(hex("684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51"), k)
    }

    @Test fun smallOrderKeysFail() {
        for (point in listOf("0000000000000000000000000000000000000000000000000000000000000000", "0100000000000000000000000000000000000000000000000000000000000000",
            "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800", "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
            "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f", "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
            "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f")) {
            val pair = UdpKeyPair()
            val out = ByteArray(32) { 0x55 }
            assertFalse("A small-order public key gave a shared secret: $point", pair.agree(hex(point), out))
            assertTrue("A refused agreement left a secret behind", out.all { it.toInt() == 0 })
        }
        val client = UdpKeyPair()
        val host = UdpKeyPair()
        assertFalse(client.publicKey.contentEquals(host.publicKey))
        val one = ByteArray(32)
        val two = ByteArray(32)
        assertTrue(client.agree(host.publicKey, one) && host.agree(client.publicKey, two))
        assertArrayEquals(one, two)
        assertEquals(0, client.publicKey[31].toInt() and 0x80)
    }

    @Test fun rfc5869() {
        val vectors = listOf(
            listOf("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "000102030405060708090a0b0c", "f0f1f2f3f4f5f6f7f8f9",
                "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"),
            listOf("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f",
                "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
                "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
                "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87"),
            // Test case 3 has no salt: by RFC 5869 that is HashLen zero bytes.
            listOf("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "0000000000000000000000000000000000000000000000000000000000000000", "",
                "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"),
        )
        for ((ikm, salt, info, okm) in vectors) assertArrayEquals(hex(okm), UdpCrypto.hkdfSha256(hex(salt), hex(ikm), hex(info), hex(okm).size))
        assertEquals(null, UdpCrypto.hkdfSha256(ByteArray(32), ByteArray(32), ByteArray(0), 0))
        assertEquals(null, UdpCrypto.hkdfSha256(ByteArray(32), ByteArray(32), ByteArray(0), 255 * 32 + 1))
    }

    @Test fun keySchedule() {
        val client = derive(true)
        val host = derive(false)
        assertArrayEquals(expectedOkm.copyOfRange(0, 32), client.send.key)
        assertArrayEquals(expectedOkm.copyOfRange(0, 32), host.receive.key)
        assertArrayEquals(expectedOkm.copyOfRange(32, 64), host.send.key)
        assertArrayEquals(expectedOkm.copyOfRange(32, 64), client.receive.key)
        assertArrayEquals(expectedOkm.copyOfRange(64, 68), client.send.salt)
        assertArrayEquals(expectedOkm.copyOfRange(68, 72), client.receive.salt)
        // Consumed keys are gone and cannot start a second sealer.
        UdpSealer(host.send, hostLeg)
        assertTrue(host.send.key.all { it.toInt() == 0 } && host.send.salt.all { it.toInt() == 0 } && !host.send.ready)
        val out = ByteArray(Udp.MAX_DATAGRAM)
        assertEquals(0, UdpSealer(host.send, hostLeg).seal(byteArrayOf(1), 1, out))
        assertEquals(0, UdpOpener(host.send).open(out, out.size, ByteArray(Udp.MAX_PLAINTEXT)))
    }

    @Test fun nodeCrossCheck() {
        // Printed by the Node QA leg (udp-leg.mjs: keyPair/agree/sessionKeys/seal) for these fixed scalars.
        val client = UdpKeyPair(hex("a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf"))
        val host = UdpKeyPair(hex("c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedf"))
        assertArrayEquals(hex("605a725d2a4adfeeb1a29e17edd621c1b7593ee8cdbc44ac6c4ab6e2f805d23c"), client.publicKey)
        assertArrayEquals(hex("dc2cca31e8e43bbd91dff7e475cca3347eb478107d5bd765aba4ae4a30c35d44"), host.publicKey)
        val shared = ByteArray(32)
        assertTrue(client.agree(host.publicKey, shared))
        assertArrayEquals(hex("3b87c5035cce13678dd93d2dbc3a3d42039ebec86444a1ff82cd61b84c6fe968"), shared)
        val session = "loopback-session".toByteArray()
        val keys = UdpCrypto.deriveKeys(shared, client.publicKey, host.publicKey, session, true)!!
        assertArrayEquals(hex("f8286823ddb504d9c010e19a842b920a27d29e59c5c6984ac5ae5b1e9e2c3f78"), keys.send.key)
        assertArrayEquals(hex("c14abd2e"), keys.send.salt)
        assertArrayEquals(hex("0b03c9502194850e6e24db1d319037f5c1e870c5e2f35da3a46a8b41f69c682b"), keys.receive.key)
        assertArrayEquals(hex("48b5f961"), keys.receive.salt)
        val hostKeys = UdpCrypto.deriveKeys(shared, client.publicKey, host.publicKey, session, false)!!
        val hostSeal = UdpSealer(hostKeys.send, ByteArray(8) { 0x99.toByte() })
        setSequence(hostSeal, 5)
        val plainDown = hex("01000000000100000001010101002a00000000000000deadbeef")
        val down = ByteArray(Udp.MAX_DATAGRAM)
        val downSize = hostSeal.seal(plainDown, plainDown.size, down)
        assertArrayEquals(hex("0399999999999999990000000000000005756f6e584c60a6fb89e0ba929859f6707fe51ed5cc6e99fadc84e00acba183c7f10f5ef82e1f2470f5a3"), down.copyOf(downSize))
        val plain = ByteArray(Udp.MAX_PLAINTEXT)
        assertEquals(plainDown.size, UdpOpener(keys.receive).open(down, downSize, plain))
        assertArrayEquals(plainDown, plain.copyOf(plainDown.size))
        val plainUp = hex("02000000000000000007000000000000000000000000000000001122334455667788")
        val up = ByteArray(Udp.MAX_DATAGRAM)
        val upSize = UdpSealer(keys.send, hex("a1b2c3d4e5f60718")).seal(plainUp, plainUp.size, up)
        assertArrayEquals(hex("03a1b2c3d4e5f607180000000000000001c0c259e6f1a82f09a5e7252b0a5ccacf61a7c45f15c8dff665e8af764aa9b6f598a72c9c91891de03646ddd6f0e654326098"), up.copyOf(upSize))
        assertEquals(plainUp.size, UdpOpener(hostKeys.receive).open(up, upSize, plain))
    }

    @Test fun relayProofs() {
        // Printed by the relay's server/src/media-udp.mjs for this ticket and ID (udp_media_leg_test.cpp).
        val ticket = hex("030a11181f262d343b424950575e656c737a81888f969da4abb2b9c0c7ced5dc")
        val id = hex("a1b2c3d4e5f60718")
        assertArrayEquals(hex("01a1b2c3d4e5f60718ab888d6d37401b1e1a08612ffb35634f57bae04104bffdc1a5428a597a1645cf"), UdpCrypto.proof(1, "multitap-udp-bind-v1", id, ticket))
        assertArrayEquals(hex("02a1b2c3d4e5f6071819ea576916fe53cab5b247fcd507e39b51db70c504a8dae16905443ead3d41ad"), UdpCrypto.proof(2, "multitap-udp-ack-v1", id, ticket))
    }

    @Test fun framingLayout() {
        val p = Pair()
        for (size in listOf(1, 16, 17, 1166, Udp.MAX_PLAINTEXT)) {
            val plain = ByteArray(size) { (it * 3).toByte() }
            val out = ByteArray(Udp.MAX_DATAGRAM)
            val back = ByteArray(Udp.MAX_PLAINTEXT)
            val sealed = p.hostSeal.seal(plain, size, out)
            assertEquals(size + Udp.HEADER_BYTES + Udp.TAG_BYTES, sealed)
            assertEquals(3, out[0].toInt())
            assertArrayEquals(hostLeg, out.copyOfRange(1, 9))
            // The exact contract: AAD bytes 0..16, nonce the direction's salt then the sequence, h2c key and salt.
            val aes = Cipher.getInstance("AES/GCM/NoPadding")
            aes.init(Cipher.DECRYPT_MODE, SecretKeySpec(expectedOkm.copyOfRange(32, 64), "AES"), GCMParameterSpec(128, expectedOkm.copyOfRange(68, 72) + out.copyOfRange(9, 17)))
            aes.updateAAD(out, 0, 17)
            assertArrayEquals(plain, aes.doFinal(out, 17, sealed - 17))
            assertEquals(size, p.clientOpen.open(out, sealed, back))
            assertArrayEquals(plain, back.copyOf(size))
        }
        val out = ByteArray(Udp.MAX_DATAGRAM + 16)
        assertEquals(0, p.hostSeal.seal(ByteArray(Udp.MAX_PLAINTEXT + 1), Udp.MAX_PLAINTEXT + 1, out))
        assertEquals(0, p.hostSeal.seal(ByteArray(1), 0, out))
        // A refused seal neither burns nor reuses a sequence: they count up from 1.
        val q = Pair()
        for (n in 1..2000) { assertTrue(q.clientSeal.seal(ByteArray(8), 8, out) != 0); assertEquals(n.toLong(), Udp.get64(out, 9)) }
    }

    @Test fun tamper() {
        val p = Pair()
        val plain = ByteArray(32) { it.toByte() }
        val datagram = ByteArray(Udp.MAX_DATAGRAM)
        val back = ByteArray(Udp.MAX_PLAINTEXT)
        val sealed = p.hostSeal.seal(plain, plain.size, datagram)
        for (byte in 0 until sealed) for (bit in 0 until 8) {
            val copy = datagram.copyOf()
            copy[byte] = (copy[byte].toInt() xor (1 shl bit)).toByte()
            val fresh = Pair()
            assertEquals("A tampered datagram opened", 0, fresh.clientOpen.open(copy, sealed, back))
            assertEquals("A tampered datagram disturbed the window", plain.size, fresh.clientOpen.open(datagram, sealed, back))
        }
        for (size in listOf(0, 1, 16, 25, 33, sealed - 1, sealed + 1)) assertEquals(0, p.clientOpen.open(datagram, size, back))
        assertEquals(0, p.clientOpen.open(ByteArray(Udp.MAX_DATAGRAM + 1).also { datagram.copyInto(it) }, Udp.MAX_DATAGRAM + 1, back))
        // The other direction and another grant.
        val reflected = ByteArray(Udp.MAX_DATAGRAM)
        val size = p.clientSeal.seal(plain, plain.size, reflected)
        assertEquals(0, p.clientOpen.open(reflected, size, back))
        assertEquals(0, p.hostOpen.open(datagram, sealed, back))
        assertEquals(plain.size, p.clientOpen.open(datagram, sealed, back))
        val x = UdpKeyPair()
        val y = UdpKeyPair()
        val sa = ByteArray(32)
        val sb = ByteArray(32)
        assertTrue(x.agree(y.publicKey, sa) && y.agree(x.publicKey, sb))
        val a = UdpCrypto.deriveKeys(sa, x.publicKey, y.publicKey, "other".toByteArray(), true)!!
        val b = UdpCrypto.deriveKeys(sb, x.publicKey, y.publicKey, "other".toByteArray(), false)!!
        val open = UdpOpener(a.receive)
        assertEquals(0, open.open(datagram, sealed, back))
        val own = ByteArray(Udp.MAX_DATAGRAM)
        val n = UdpSealer(b.send, hostLeg).seal(plain, plain.size, own)
        assertEquals(plain.size, open.open(own, n, back))
    }

    @Test fun replay() {
        val w = UdpReplayWindow()
        fun take(s: Long): Boolean { val f = w.fresh(s); if (f) w.accept(s); return f }
        assertTrue(take(1) && !take(1))
        assertTrue(take(3) && take(2) && !take(2) && !take(3))
        assertTrue(take(3000))
        assertTrue(!take(3000 - 2048) && take(3000 - 2047) && !take(3000 - 2047))
        assertTrue(take(3000 + 2048) && take(3001) && !take(3000))
        for (s in 10000L until 20000L) assertTrue(take(s))
        for (s in 19999L downTo 19999L - 2047) assertFalse(take(s))
        assertFalse(take(19999L - 2048))
        val lone = UdpReplayWindow().also { it.accept(5000) }
        for (back in 2048L until 5000L) assertFalse(lone.fresh(5000 - back))
        val ring = UdpReplayWindow()
        for (s in 1L until 10000L step 2) { assertTrue(ring.fresh(s)); ring.accept(s) }
        assertTrue(ring.fresh(9998))

        val p = Pair()
        val plain = ByteArray(8).also { it[0] = 1 }
        val one = ByteArray(Udp.MAX_DATAGRAM)
        val two = ByteArray(Udp.MAX_DATAGRAM)
        val three = ByteArray(Udp.MAX_DATAGRAM)
        val back = ByteArray(Udp.MAX_PLAINTEXT)
        val a = p.hostSeal.seal(plain, 8, one)
        val b = p.hostSeal.seal(plain, 8, two)
        val c = p.hostSeal.seal(plain, 8, three)
        assertTrue(p.clientOpen.open(two, b, back) == 8 && p.clientOpen.open(two, b, back) == 0)
        assertTrue(p.clientOpen.open(one, a, back) == 8 && p.clientOpen.open(one, a, back) == 0)
        // A forgery with a huge sequence must not move the window.
        val forged = three.copyOf().also { Udp.put64(it, 9, 1000000) }
        assertTrue(p.clientOpen.open(forged, c, back) == 0 && p.clientOpen.open(three, c, back) == 8)
        // Limits. Sequence 0 and sequences at 2^48 or above never open; the last valid one does.
        val q = Pair()
        val out = ByteArray(Udp.MAX_DATAGRAM)
        setSequence(q.hostSeal, UdpCrypto.SEQUENCE_LIMIT - 2)
        assertFalse(q.hostSeal.exhausted())
        val last = q.hostSeal.seal(plain, 8, out)
        assertTrue(last != 0 && Udp.get64(out, 9) == UdpCrypto.SEQUENCE_LIMIT - 2)
        assertEquals(8, q.clientOpen.open(out, last, back))
        val top = q.hostSeal.seal(plain, 8, out)
        assertTrue(top != 0 && Udp.get64(out, 9) == UdpCrypto.SEQUENCE_LIMIT - 1 && q.clientOpen.open(out, top, back) == 8)
        assertTrue(q.hostSeal.exhausted() && q.hostSeal.seal(plain, 8, out) == 0)
        val bad = one.copyOf()
        for (sequence in listOf(0L, UdpCrypto.SEQUENCE_LIMIT, Long.MIN_VALUE, -1L)) { Udp.put64(bad, 9, sequence); assertEquals(0, p.clientOpen.open(bad, a, back)) }
        val r = Pair()
        setSequence(r.hostSeal, UdpCrypto.SEQUENCE_LIMIT)
        assertEquals(0, r.hostSeal.seal(plain, 8, out))
    }

    @Test fun retransmissionTakesNewSequence() {
        val p = Pair()
        val message = ByteArray(40) { it.toByte() }
        val out = ByteArray(Udp.MAX_DATAGRAM)
        val first = p.hostSeal.seal(message, message.size, out).let { out.copyOf(it) }
        val again = p.hostSeal.seal(message, message.size, out).let { out.copyOf(it) }
        assertEquals(1L, Udp.get64(first, 9))
        assertEquals(2L, Udp.get64(again, 9))
        assertFalse(first.contentEquals(again))
    }

    // The leg runs this before any UDP; here on the JDK's providers (a phone runs Conscrypt).
    @Test fun selfCheck() {
        assertTrue(UdpCrypto.selfCheck())
    }
}
