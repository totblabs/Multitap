package me.totb.multitap_client

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.concurrent.atomic.AtomicBoolean

// The path rule and the gate, case by case as apps/windows_host/src/udp_media_test.cpp
// (pathRule) and apps/windows_client/test/native/udp_media_leg_test.cpp (pathRule)
// check the C++ ones. The engine here answers with AndroidMediaStatus bytes.
internal const val TEST_LEASE = 7L
internal const val TEST_LEG = 3L

internal fun mediaPacket(kind: Int, key: Boolean, epoch: Int, pts: Long, payload: Int = 32, fill: Int = 0x41): ByteArray {
    val packet = ByteArray(12 + payload) { fill.toByte() }
    packet[0] = kind.toByte(); packet[1] = if (key) 1 else 0; packet[2] = epoch.toByte(); packet[3] = (epoch shr 8).toByte()
    for (n in 0 until 8) packet[4 + n] = (pts ushr (8 * n)).toByte()
    return packet
}
internal fun tilePacket(epoch: Int, pts: Long) = mediaPacket(1, false, epoch, pts).also { it[12] = 'T'.code.toByte(); it[13] = 'S'.code.toByte(); it[14] = 'L'.code.toByte(); it[15] = '1'.code.toByte() }
internal fun onWire(packet: ByteArray) = packet.copyOf().also { it[2] = 0; it[3] = 0 }

/** Stands in for AndroidMedia's bounded queue. */
internal class FakeEngine {
    private val lock = Any()
    private val taken = ArrayList<ByteArray>()
    private var refuse = 0
    var watches = 0
        private set
    var afterStop = 0
        private set
    @Volatile var stopped = false
    @Volatile private var fullUntil = 0L
    /** Refused video offers, FULL or not. */
    var refusedVideo = 0
        private set
    val offer = UdpPathGate.Offer { message, watch ->
        synchronized(lock) {
            val video = message[0].toInt() == 1
            if (refuse > 0 || (video && System.nanoTime() < fullUntil)) { if (refuse > 0) --refuse; if (video) ++refusedVideo; return@Offer AndroidMediaStatus.QUEUE_FULL }
            if (stopped) { ++afterStop; if (video) ++refusedVideo; return@Offer AndroidMediaStatus.SESSION_REPLACED }
            taken.add(message.copyOf()); if (watch) ++watches
            AndroidMediaStatus.OK
        }
    }
    fun count() = synchronized(lock) { taken.size }
    fun copy() = synchronized(lock) { ArrayList(taken) }
    fun refuseNext(n: Int) = synchronized(lock) { refuse = n }
    /** A busy decoder: video answers FULL for [ms] from now. */
    fun fullFor(ms: Long) { fullUntil = System.nanoTime() + ms * 1000000 }
    fun full() = System.nanoTime() < fullUntil
}

/** A WSS push as AndroidMedia's media queue makes it: the first bytes, then the decoder's own admission. */
internal fun offerWss(gate: UdpPathGate, engine: FakeEngine, packet: ByteArray, lease: Long = TEST_LEASE, watch: Boolean = false): Byte =
    gate.offerWss(lease, packet.copyOf(minOf(packet.size, 16)), packet.size) { engine.offer.offer(packet, watch) }

internal fun offerUdp(gate: UdpPathGate, packet: ByteArray, leg: Long = TEST_LEG): UdpPathGate.Delivery {
    val envelope = UdpEnvelope()
    val video = packet[0].toInt() == 1
    Udp.parseEnvelope(packet, 0, packet.size, if (video) Udp.CLASS_VIDEO else Udp.CLASS_AUDIO, envelope)
    return gate.offerUdp(leg, packet.copyOf(), video, envelope.key, envelope.epoch, envelope.pts)
}

class UdpPathGateTest {
    private val ok = AndroidMediaStatus.OK
    private val accepted = UdpPathGate.Delivery.ACCEPTED
    private val dropped = UdpPathGate.Delivery.DROPPED
    private val full = UdpPathGate.Delivery.FULL

    private class Rig(keyed: Boolean = true, epoch: Int = 1) {
        val engine = FakeEngine()
        val gate = UdpPathGate()
        init { gate.attach(TEST_LEASE, engine.offer); gate.disengage(TEST_LEG); gate.stream(epoch, 1); gate.engage(keyed) }
        fun wss(packet: ByteArray) = offerWss(gate, engine, onWire(packet))
        fun over(packet: ByteArray) = offerUdp(gate, packet)
    }

    @Test fun acceptVideo() {
        val s = UdpPathState()
        val wss = Udp.PATH_WSS
        val udp = Udp.PATH_UDP
        assertFalse(Udp.acceptVideo(s, wss, 1, false, 10))
        assertFalse(Udp.acceptVideo(s, udp, 1, false, 10))
        assertTrue(Udp.acceptVideo(s, wss, 1, true, 10) && s.active == wss)
        assertTrue(Udp.acceptVideo(s, wss, 1, false, 20))
        assertFalse(Udp.acceptVideo(s, udp, 1, false, 30))
        assertTrue(!Udp.acceptVideo(s, wss, 1, false, 20) && !Udp.acceptVideo(s, wss, 1, false, 19))
        assertTrue(!Udp.acceptVideo(s, udp, 1, true, 20) && s.active == wss)
        assertTrue(Udp.acceptVideo(s, udp, 1, true, 40) && s.active == udp)
        assertFalse("A straggler on the old path passed", Udp.acceptVideo(s, wss, 1, false, 50))
        assertTrue(Udp.acceptVideo(s, udp, 1, false, 50))
        assertFalse(Udp.acceptVideo(s, wss, 1, true, 45))
        assertTrue(Udp.acceptVideo(s, wss, 1, true, 60) && s.active == wss)
        // A new epoch restarts PTS, and needs a key frame.
        assertFalse(Udp.acceptVideo(s, wss, 2, false, 70))
        assertTrue(Udp.acceptVideo(s, wss, 2, true, 1) && s.lastPts == 1L && s.epoch == 2)
        assertFalse(Udp.acceptVideo(s, wss, 1, true, 1000))
        // PTS is unsigned on the wire.
        assertTrue(Udp.acceptVideo(s, wss, 2, false, Long.MIN_VALUE))
        assertFalse(Udp.acceptVideo(s, wss, 2, false, 5))
    }

    @Test fun contractRuleInBothDirections() {
        val r = Rig()
        assertEquals("A P-frame counted from a path that is not active", dropped, r.over(mediaPacket(1, false, 1, 10)))
        assertTrue(r.wss(mediaPacket(1, false, 1, 10)) == ok && r.engine.count() == 1)
        assertTrue("A UDP key frame did not make UDP active", r.over(mediaPacket(1, true, 1, 20)) == accepted && r.gate.view().active == 1)
        assertTrue("A WSS P-frame passed while UDP is active", r.wss(mediaPacket(1, false, 1, 30)) == ok && r.engine.count() == 2)
        assertTrue(r.over(mediaPacket(1, false, 1, 30)) == accepted && r.engine.count() == 3)
        assertTrue("A WSS key frame did not make WSS active", r.wss(mediaPacket(1, true, 1, 40)) == ok && r.engine.count() == 4 && r.gate.view().active == 0)
        assertEquals(1L, r.gate.view().fallbacks)
        assertTrue(r.over(mediaPacket(1, false, 1, 50)) == dropped && r.engine.count() == 4)
        assertTrue(r.wss(mediaPacket(1, false, 1, 50)) == ok && r.engine.count() == 5)
    }

    @Test fun newerPtsOnEitherPath() {
        val r = Rig()
        assertEquals(accepted, r.over(mediaPacket(1, true, 1, 100)))
        assertEquals(dropped, r.over(mediaPacket(1, true, 1, 100)))
        assertEquals(dropped, r.over(mediaPacket(1, true, 1, 99)))
        r.wss(mediaPacket(1, true, 1, 99))
        assertTrue("An older WSS key frame took over", r.engine.count() == 1 && r.gate.view().active == 1 && r.gate.wssKeys() == 0L)
    }

    @Test fun fullQueueLeavesTheRuleAlone() {
        val r = Rig()
        r.engine.refuseNext(1)
        assertEquals(AndroidMediaStatus.QUEUE_FULL, r.wss(mediaPacket(1, true, 1, 10)))
        assertTrue("The same packet could not be offered again", r.wss(mediaPacket(1, true, 1, 10)) == ok && r.engine.count() == 1)
        r.engine.refuseNext(1)
        assertTrue("A refused UDP key frame changed the active path", r.over(mediaPacket(1, true, 1, 20)) == full && r.gate.view().active == 0)
    }

    @Test fun tilesFollowThePath() {
        val r = Rig()
        r.wss(mediaPacket(1, true, 1, 100))
        assertTrue(r.wss(tilePacket(1, 1000)) == ok && r.engine.count() == 2)
        assertTrue("A tile ahead of the encoder's PTS swallowed the next frame", r.wss(mediaPacket(1, false, 1, 200)) == ok && r.engine.count() == 3)
        assertTrue("A tile passed from the wrong path", r.over(tilePacket(1, 201)) == dropped && r.engine.count() == 3)
    }

    @Test fun epochGate() {
        val r = Rig()
        r.over(mediaPacket(1, true, 1, 10))
        assertTrue(r.over(mediaPacket(1, true, 0, 20)) == dropped && r.over(mediaPacket(2, false, 0, 20)) == dropped)
        assertTrue(r.over(mediaPacket(1, true, 2, 20)) == dropped && r.over(mediaPacket(2, false, 2, 20)) == dropped)
        r.gate.stream(2, 2)
        assertEquals(2, r.gate.epoch())
        assertEquals(dropped, r.over(mediaPacket(1, true, 1, 30)))
        assertEquals("A P-frame opened a new epoch", dropped, r.over(mediaPacket(1, false, 2, 5)))
        assertEquals("PTS restarts with the epoch", accepted, r.over(mediaPacket(1, true, 2, 5)))
        assertEquals("A key frame over UDP did not name its stream", 2L, r.gate.view().keyedStream)
    }

    @Test fun keyGateAndRestartedRuns() {
        val r = Rig(false)
        assertTrue(r.over(mediaPacket(1, false, 1, 10)) == dropped && r.over(mediaPacket(2, false, 1, 10)) == dropped)
        assertEquals(ok, r.wss(mediaPacket(1, true, 1, 20)))
        assertEquals("Audio stopped after the key frame", accepted, r.over(mediaPacket(2, false, 1, 30)))
        r.gate.attach(TEST_LEASE, r.engine.offer)
        assertTrue("A new decoder took media before a key frame", r.over(mediaPacket(1, false, 1, 40)) == dropped && r.over(mediaPacket(2, false, 1, 40)) == dropped)
        assertEquals(accepted, r.over(mediaPacket(1, true, 1, 40)))
        r.gate.stream(1, 2)
        assertEquals("A restarted run's PTS stayed behind the old one", accepted, r.over(mediaPacket(1, false, 1, 7)))
    }

    @Test fun wssAudioAndTheLegsCue() {
        val r = Rig()
        r.over(mediaPacket(1, true, 1, 10))
        val before = r.engine.count()
        assertTrue("WSS audio was dropped in flight", r.wss(mediaPacket(2, false, 1, 11)) == ok && r.engine.count() == before + 1)
        assertEquals(0L, r.gate.wssKeys())
        r.wss(mediaPacket(1, true, 1, 12)); assertEquals(1L, r.gate.wssKeys())
    }

    @Test fun detachedAndOtherLeases() {
        val r = Rig()
        r.gate.attach(0, null)
        assertTrue(r.over(mediaPacket(1, true, 1, 10)) == dropped && !r.gate.attached())
        // Android: with no decoder attached a WSS push takes the decoder's own answer
        // (AndroidMedia answers session_replaced for a lease it no longer owns).
        var asked = 0
        assertEquals(AndroidMediaStatus.SESSION_REPLACED, r.gate.offerWss(TEST_LEASE, ByteArray(16), 44) { ++asked; AndroidMediaStatus.SESSION_REPLACED })
        assertEquals(1, asked)
        // So does a push for another lease: the rule belongs to the attached decoder.
        val s = Rig()
        s.over(mediaPacket(1, true, 1, 10))
        val other = onWire(mediaPacket(1, false, 1, 5))
        assertEquals(ok, offerWss(s.gate, s.engine, other, lease = TEST_LEASE + 1))
        assertTrue("The rule judged another lease's packet", s.engine.count() == 2 && s.gate.view().active == 1)
    }

    @Test fun onlyTheCurrentLegPasses() {
        val r = Rig()
        assertEquals(dropped, offerUdp(r.gate, mediaPacket(1, true, 1, 10), leg = TEST_LEG + 1))
        assertEquals(0, r.engine.count())
        r.gate.disengage(TEST_LEG + 1); r.gate.engage(true)
        assertEquals("A stopped leg's message passed", dropped, offerUdp(r.gate, mediaPacket(1, true, 1, 10)))
        assertEquals(accepted, offerUdp(r.gate, mediaPacket(1, true, 1, 10), leg = TEST_LEG + 1))
        r.gate.disengage()
        assertEquals(dropped, offerUdp(r.gate, mediaPacket(1, true, 1, 20), leg = TEST_LEG + 1))
    }

    @Test fun decoderInputIsTheWssInput() {
        // Only video is watched on UDP, and the epoch never reaches the decoder.
        val r = Rig()
        r.over(mediaPacket(1, true, 1, 10, 32, 0x51)); r.over(mediaPacket(2, false, 1, 11))
        assertEquals(1, r.engine.watches)
        assertArrayEquals(onWire(mediaPacket(1, true, 1, 10, 32, 0x51)), r.engine.copy()[0])
        // A decoder refusal other than full is a drop.
        val gate = UdpPathGate()
        gate.attach(TEST_LEASE) { _, _ -> AndroidMediaStatus.INVALID_PACKET }; gate.disengage(TEST_LEG); gate.stream(1, 1); gate.engage(true)
        assertEquals(dropped, offerUdp(gate, mediaPacket(1, true, 1, 10)))
        assertEquals(0, gate.view().active)
    }

    @Test fun disengagedIsAStraightPass() {
        val engine = FakeEngine()
        val gate = UdpPathGate()
        gate.attach(TEST_LEASE, engine.offer); gate.stream(1, 1)
        val packets = listOf(onWire(mediaPacket(1, false, 1, 50)), onWire(mediaPacket(1, false, 1, 50)), onWire(mediaPacket(1, false, 1, 10)), onWire(mediaPacket(2, false, 1, 5)), byteArrayOf(1, 2, 3))
        for (packet in packets) assertEquals(ok, offerWss(gate, engine, packet, watch = true))
        val taken = engine.copy()
        assertEquals(packets.size, taken.size)
        for (n in packets.indices) assertArrayEquals(packets[n], taken[n])
        assertEquals(5, engine.watches)
        // Off again, the rule is gone.
        val r = Rig()
        r.over(mediaPacket(1, true, 1, 10)); r.gate.disengage(TEST_LEG)
        assertTrue(r.wss(mediaPacket(1, false, 1, 1)) == ok && r.engine.count() == 2 && r.gate.view().active == 0 && r.gate.view().fallbacks == 0L)
    }

    @Test fun attachWaitsForAnOfferInsideTheDecoder() {
        val gate = UdpPathGate()
        val entered = AtomicBoolean(false)
        val release = AtomicBoolean(false)
        val detached = AtomicBoolean(false)
        gate.attach(TEST_LEASE) { _, _ -> entered.set(true); while (!release.get()) Thread.sleep(1); AndroidMediaStatus.OK }
        gate.disengage(TEST_LEG); gate.stream(1, 1); gate.engage(false)
        val sender = Thread { offerUdp(gate, mediaPacket(1, true, 1, 10)) }.apply { start() }
        while (!entered.get()) Thread.sleep(1)
        val stopper = Thread { gate.attach(0, null); detached.set(true) }.apply { start() }
        Thread.sleep(50)
        val early = detached.get()
        release.set(true); sender.join(); stopper.join()
        assertTrue("attach returned while an offer was inside the decoder", !early && detached.get())
    }
}
