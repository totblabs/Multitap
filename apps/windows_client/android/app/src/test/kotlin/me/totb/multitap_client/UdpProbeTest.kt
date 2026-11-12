package me.totb.multitap_client

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.TreeMap

// The probe schedule, 1 ms steps, case by case as apps/windows_client/test/native/
// udp_media_leg_test.cpp checks the C++ one. Pure, injected time.
internal val ticketBytes = hex("030a11181f262d343b424950575e656c737a81888f969da4abb2b9c0c7ced5dc")
internal val idBytes = hex("a1b2c3d4e5f60718")
// Printed by the relay's server/src/media-udp.mjs under Node for this ticket and ID.
internal val bindBytes = hex("01a1b2c3d4e5f60718ab888d6d37401b1e1a08612ffb35634f57bae04104bffdc1a5428a597a1645cf")
internal val ackBytes = hex("02a1b2c3d4e5f6071819ea576916fe53cab5b247fcd507e39b51db70c504a8dae16905443ead3d41ad")
internal val pingBytes = hex("03a1b2c3d4e5f6071801000000000000050000000000003039")

class UdpProbeTest {
    private class Sent(val ms: Long, val relay: Int, val bytes: ByteArray) {
        val ping get() = bytes[0].toInt() == 3
        val sequence get() = Udp.get32(bytes, 13)
    }

    private class ProbeSim(relays: Int = 1, private val base: Long = 0) {
        var now = base
        val ready = sortedSetOf<Int>()
        val failing = HashSet<Int>()
        val sent = ArrayList<Sent>()
        var onSend: ((Sent) -> Unit)? = null
        private val later = TreeMap<Long, ArrayDeque<() -> Unit>>()
        val probe = UdpProbe(relays, idBytes, bindBytes, ackBytes, base) { relay, data, size ->
            if (relay in failing) false
            else { val s = Sent((now - base) / 1000, relay, data.copyOf(size)); sent.add(s); onSend?.invoke(s); true }
        }
        init { for (relay in 0 until relays) ready.add(relay) }
        fun start() { for (relay in ready) probe.resolved(relay) }
        fun until(ms: Long) {
            while (true) {
                while (later.isNotEmpty() && later.firstKey() <= now) {
                    val entry = later.firstEntry()!!
                    val action = entry.value.removeFirst()
                    if (entry.value.isEmpty()) later.remove(entry.key)
                    action()
                }
                probe.tick(now)
                if (now >= base + ms * 1000) break
                now += 1000
            }
        }
        fun after(ms: Long, action: () -> Unit) { later.getOrPut(now + ms * 1000) { ArrayDeque() }.addLast(action) }
        fun ack(relay: Int = 0) = probe.received(relay, ackBytes, ackBytes.size, now)
        fun pong(ping: Sent): ByteArray = ping.bytes.copyOf().also { for (n in 1 until 9) it[n] = 0x99.toByte(); it[9] = 2 }
        fun deliverPong(ping: Sent) { val bytes = pong(ping); probe.received(ping.relay, bytes, bytes.size, now) }
        fun answer(delayMs: (Long) -> Int) {  // negative: never
            onSend = { s ->
                if (s.ping) {
                    val delay = delayMs(s.sequence)
                    if (delay >= 0) after(delay.toLong()) { deliverPong(s) }
                }
            }
        }
        fun pings() = sent.filter { it.ping }
        fun binds() = sent.filter { !it.ping }
        fun bounce(bytes: ByteArray) = probe.received(0, bytes, bytes.size, now)
    }

    @Test fun probeBytesMatchTheRelay() {
        val sent = ArrayList<ByteArray>()
        val probe = UdpProbe(1, idBytes, bindBytes, ackBytes, 0) { _, data, size -> sent.add(data.copyOf(size)); true }
        probe.resolved(0); probe.tick(0)
        assertArrayEquals("BIND differs from the relay's reference", bindBytes, sent.last())
        // The reference is PING 5 stamped 12345 us. Here the stamp moves with the clock, so
        // PING 0 carries it and PING 5 the rest.
        probe.received(0, ackBytes, ackBytes.size, 12345)
        assertArrayEquals(pingBytes.copyOfRange(17, 25), sent.last().copyOfRange(17, 25))
        for (n in 1..5) probe.tick(12345L + n * 200000)
        val ping = sent.last()
        pingBytes.copyInto(ping, 17, 17, 25)
        assertArrayEquals("PING differs from the relay's reference", pingBytes, ping)
    }

    @Test fun bindSchedule() {
        run {
            val sim = ProbeSim(); assertEquals(0L, sim.probe.nextUs()); sim.start(); sim.until(4999)
            assertEquals("One relay: one BIND every 200 ms", 25, sim.sent.size)
            for (n in sim.sent.indices) assertEquals(n * 200L, sim.sent[n].ms)
            assertEquals(0, sim.probe.report().code)
            // The leg sleeps until nextUs(): here the end of the BIND budget comes before the next BIND.
            assertEquals(5000000L, sim.probe.nextUs())
            sim.until(5000); assertTrue(sim.probe.report().code == UdpProbe.PROBE_BIND_TIMEOUT && sim.probe.over())
            assertEquals(Long.MAX_VALUE, sim.probe.nextUs())
        }
        run {  // After the burst the next deadline is the keepalive, 20 s apart.
            val sim = ProbeSim(); sim.start(); sim.until(0); sim.answer { 10 }; sim.ack()
            assertEquals(200000L, sim.probe.nextUs())
            sim.until(7000); assertEquals(24800000L, sim.probe.nextUs())
        }
        run {
            val sim = ProbeSim(2); sim.start(); sim.until(2500)
            val expected = intArrayOf(0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0)
            assertEquals(13, sim.sent.size)
            for (n in 0 until 13) assertTrue("Relays in order, three each, then again", sim.sent[n].relay == expected[n] && sim.sent[n].ms == n * 200L)
        }
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0)
            sim.bounce(ackBytes.copyOf(ackBytes.size - 1))
            sim.bounce(ackBytes.copyOf(ackBytes.size + 1))
            for (at in listOf(0, 1, 9, 40)) sim.bounce(ackBytes.copyOf().also { it[at] = (it[at].toInt() xor 1).toByte() })
            sim.bounce(bindBytes); sim.bounce(pingBytes); sim.bounce(ByteArray(0))
            sim.until(1000); assertTrue("A bad ACK started the burst", sim.pings().isEmpty())
            sim.ack(); assertEquals("The real ACK did not bind", 1, sim.pings().size)
        }
        run {
            val sim = ProbeSim(2); sim.start(); sim.until(700); sim.ack(1); sim.until(6000)
            for (ping in sim.pings()) assertEquals(1, ping.relay)
            assertTrue("No BIND after the ACK; the relay that answered is kept",
                sim.binds().last().ms == 600L && sim.probe.report().relay == 1 && sim.probe.report().code == UdpProbe.PROBE_NO_PONG)
        }
        run { val sim = ProbeSim(2); sim.start(); sim.until(700); sim.ack(0); sim.until(6000); assertEquals(0, sim.probe.report().relay) }
        run {
            val sim = ProbeSim(2); sim.ready.remove(0); sim.start(); sim.until(700)
            assertTrue("An open name holds its turn, then the next relay goes", sim.sent.size == 1 && sim.sent[0].relay == 1)
            sim.probe.resolved(0); sim.until(1300); assertEquals("A name that resolves late joins in", 0, sim.sent.last().relay)
        }
        run {
            val sim = ProbeSim(); sim.until(100); assertTrue(sim.sent.isEmpty())
            sim.probe.resolved(0); assertTrue("BIND at once, on its own turn", sim.sent.size == 1 && sim.sent[0].ms == 100L)
        }
    }

    @Test fun failureCodes() {
        run { val sim = ProbeSim(2); sim.ready.clear(); sim.start(); sim.until(5000); assertTrue(sim.probe.report().code == UdpProbe.PROBE_LOOKUP_FAILED && sim.sent.isEmpty()) }
        run { val sim = ProbeSim(); sim.failing.add(0); sim.start(); sim.until(5000); assertEquals(UdpProbe.PROBE_SOCKET_ERROR, sim.probe.report().code) }
        run { val sim = ProbeSim(2); sim.ready.remove(0); sim.start(); sim.until(5000); assertEquals(UdpProbe.PROBE_BIND_TIMEOUT, sim.probe.report().code) }
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0); sim.ack(); sim.until(2999)
            assertEquals("No outcome before 3 s", 0, sim.probe.report().code)
            sim.until(3000)
            val r = sim.probe.report()
            assertTrue("5 when the ACK came and no PONG did", r.code == UdpProbe.PROBE_NO_PONG && r.relay == 0 && r.lossPct == 100 && r.keepSent == 0L && r.rttP50Ms < 0)
        }
    }

    @Test fun pingSchedule() {
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0); sim.answer { 10 }; sim.ack(); sim.until(90000)
            val pings = sim.pings()
            assertEquals("25 and 4 keepalives", 29, pings.size)
            for (n in 0 until 25) assertEquals(n * 200L, pings[n].ms)
            val later = longArrayOf(24800, 44800, 64800, 84800)
            for (n in 0 until 4) assertEquals(later[n], pings[25 + n].ms)
            for (n in 0 until 29) assertEquals(n.toLong(), pings[n].sequence)
            assertEquals(4L, sim.probe.report().keepSent)
        }
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0)
            sim.answer { sequence -> if (sequence < 25) sequence.toInt() + 1 else 900 }
            sim.ack(); sim.until(30000)
            val r = sim.probe.report()
            assertTrue("Percentiles come from the burst only", r.code == UdpProbe.PROBE_REACHABLE && r.rttP50Ms == 13 && r.rttP95Ms == 24 && r.lossPct == 0 && r.keepSent == 1L && r.keepLost == 0L)
        }
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0)
            sim.answer { sequence -> when (sequence) { 3L -> 1001; 4L -> 1000; 5L -> -1; else -> 10 } }
            sim.ack(); sim.until(6000)
            val r = sim.probe.report()
            assertTrue("Loss counts PINGs without a PONG within 1 s", r.lossPct == 8 && r.code == UdpProbe.PROBE_REACHABLE && r.rttP95Ms == 10)
        }
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0)
            sim.onSend = { s -> if (s.ping && s.sequence == 0L) sim.after(5) { repeat(30) { sim.deliverPong(s) } } }
            sim.ack(); sim.until(6000); assertEquals("A duplicate PONG hid a lost PING", 96, sim.probe.report().lossPct)
        }
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0); sim.ack()
            val pong = sim.pong(sim.pings().first())
            sim.bounce(pong.copyOf(pong.size - 1))
            sim.bounce(pong.copyOf(pong.size + 1))
            for ((at, value) in listOf(0 to 2, 9 to 1, 10 to 1, 12 to 1, 16 to 7, 24 to (pong[24].toInt() xor 1))) sim.bounce(pong.copyOf().also { it[at] = value.toByte() })
            sim.until(6000)
            assertTrue("A PONG must match its PING exactly", sim.probe.report().code == UdpProbe.PROBE_NO_PONG && sim.probe.report().lossPct == 100)
        }
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0)
            sim.answer { sequence -> when (sequence) { 26L -> -1; 27L -> 1500; else -> 100 } }
            sim.ack(); sim.until(70000)
            val r = sim.probe.report()
            assertTrue("Keepalives count sent and unanswered", r.keepSent == 3L && r.keepLost == 2L && r.code == UdpProbe.PROBE_REACHABLE)
        }
    }

    @Test fun waitingForTheHost() {
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0); sim.answer { 450 }; sim.ack(); sim.until(7000)
            assertEquals("A first PONG for sequence 0 loses nothing", 25, sim.pings().size)
            val r = sim.probe.report()
            assertTrue("A slow path is not a late Host", r.code == UdpProbe.PROBE_REACHABLE && r.lossPct == 0 && r.rttP50Ms == 450)
        }
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0)
            sim.onSend = { s -> if (s.ping && s.ms >= 650) sim.after(20) { sim.deliverPong(s) } }
            sim.ack(); sim.until(7000)
            val pings = sim.pings()
            assertTrue("A first PONG for a later sequence restarts from 0",
                pings.size == 30 && pings[4].sequence == 4L && pings[5].sequence == 0L && pings[5].ms == 820L && pings.last().ms == 820L + 24 * 200)
            val r = sim.probe.report()
            assertTrue("Nothing is lost to a Host that binds late", r.code == UdpProbe.PROBE_REACHABLE && r.rttP50Ms == 20 && r.rttP95Ms == 20 && r.lossPct == 0)
        }
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0)
            val old = HashMap<Long, Sent>()
            sim.onSend = { s ->
                if (s.ping) {
                    if (s.ms < 650) old[s.sequence] = s
                    else if (s.ms < 820 || s.sequence != 2L) sim.after(20) { sim.deliverPong(s) }
                }
            }
            sim.ack(); sim.until(900)
            for (sequence in listOf(1L, 2L, 3L)) sim.deliverPong(old.getValue(sequence))
            sim.until(1300); sim.deliverPong(old.getValue(2)); sim.until(7000)
            assertTrue("A late PONG of the old numbering counts for nothing", sim.probe.report().lossPct == 4 && sim.probe.report().rttP95Ms == 20)
        }
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0)
            sim.onSend = { s -> if (s.ping && s.ms >= 2800) sim.after(150) { sim.deliverPong(s) } }
            sim.ack(); sim.until(9000)
            assertTrue("The last waiting PING can still start the burst",
                sim.pings().size == 15 + 25 && sim.probe.report().code == UdpProbe.PROBE_REACHABLE && sim.probe.report().lossPct == 0)
        }
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0); sim.ack(); sim.until(2999)
            assertTrue("Fifteen waiting PINGs", sim.pings().size == 15 && sim.probe.report().code == 0)
            sim.until(3000)
            val last = sim.pings().last()
            sim.deliverPong(last); sim.until(48000)
            val pings = sim.pings()
            assertTrue("No PONG in 3 s: keepalives go on, numbered after a burst",
                pings.size == 17 && pings[15].ms == 22800L && pings[15].sequence == 25L && pings[16].ms == 42800L && pings[16].sequence == 26L)
            val r = sim.probe.report()
            assertTrue("A late PONG restarts nothing", r.code == UdpProbe.PROBE_NO_PONG && r.keepSent == 2L && r.keepLost == 2L)
        }
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0); sim.answer { sequence -> if (sequence < 25) -1 else 40 }; sim.ack(); sim.until(30000)
            val r = sim.probe.report()
            assertTrue("A keepalive answered later leaves the outcome alone",
                sim.pings().size == 16 && r.code == UdpProbe.PROBE_NO_PONG && r.lossPct == 100 && r.keepSent == 1L && r.keepLost == 0L)
        }
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0); sim.ack(); sim.until(1000); sim.probe.socketLost()
            val r = sim.probe.report()
            assertTrue("A socket lost during the burst", sim.probe.over() && r.code == UdpProbe.PROBE_SOCKET_ERROR && r.relay == 0 && r.lossPct < 0)
        }
        run {
            val sim = ProbeSim(); sim.start(); sim.until(0); sim.answer { sequence -> if (sequence < 25) 10 else -1 }; sim.ack(); sim.until(24900); sim.probe.socketLost()
            val r = sim.probe.report()
            assertTrue("A socket lost later keeps what was measured", r.code == UdpProbe.PROBE_REACHABLE && r.keepSent == 1L && r.keepLost == 1L)
        }
    }
}
