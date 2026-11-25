package me.totb.multitap_client

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.random.Random

// The core receiver, case by case as apps/windows_host/src/udp_media_test.cpp checks
// the C++ one (its client-side cases; the Sender is the Host's). Pure, injected time.
internal const val TEST_CHALLENGE = 0x1122334455667788L

/** Video or audio message exactly as the Host's mediaPacket builds it, with the epoch set. */
internal fun mediaMessage(kind: Int, key: Boolean, epoch: Int, pts: Long, payload: Int, seed: Int = 1): ByteArray {
    val value = ByteArray(12 + payload)
    value[0] = kind.toByte(); value[1] = if (key) 1 else 0; value[2] = epoch.toByte(); value[3] = (epoch shr 8).toByte()
    for (n in 0 until 8) value[4 + n] = (pts ushr (8 * n)).toByte()
    var state = (seed * 2654435761L + 1).toInt()
    for (n in 12 until value.size) { state = state xor (state shl 13); state = state xor (state ushr 17); state = state xor (state shl 5); value[n] = state.toByte() }
    return value
}
internal fun videoMessage(pts: Long, payload: Int = 2000, key: Boolean = false, epoch: Int = 1, seed: Int = 1) = mediaMessage(1, key, epoch, pts, payload, seed)
internal fun pcmMessage(pts: Long, payload: Int = 1920, epoch: Int = 1) = mediaMessage(2, false, epoch, pts, payload, 7)
internal fun fragmentOf(cls: Int, id: Long, message: ByteArray, index: Int): ByteArray {
    val out = ByteArray(Udp.MAX_PLAINTEXT)
    return out.copyOf(Udp.writeFragment(out, cls, id, message, index))
}
internal fun split(cls: Int, id: Long, message: ByteArray) = List(Udp.fragmentCount(message.size)) { fragmentOf(cls, id, message, it) }
internal fun tailFor(id: Long, hostUs: Long, challenge: Long = TEST_CHALLENGE, rtt: Long = 0): ByteArray {
    val p = ByteArray(Udp.TAIL_BYTES)
    p[0] = Udp.KIND_TAIL.toByte(); Udp.put32(p, 2, id); Udp.put64(p, 6, hostUs); Udp.put64(p, 14, challenge); Udp.put32(p, 22, rtt)
    return p
}

class UdpReceiverTest {
    private val video = Udp.CLASS_VIDEO
    private val audio = Udp.CLASS_AUDIO

    /** A receiver with its output kept, and a clock. A given RTT arrives the only way it can: in a TAIL. */
    // `parity` false: the cases written before parity-v1 count controls and deadlines without ABILITIES.
    private class Client(rtt: Long = 0, epoch: Int = 1, parity: Boolean = false) {
        val receiver = UdpReceiver(epoch, 1000000, parity)
        val out = UdpReceiver.Output()
        var now = 1000000L
        init { if (rtt != 0L) feed(tailFor(0, 1, TEST_CHALLENGE, rtt)) }
        fun feed(plain: ByteArray) = receiver.datagram(plain, plain.size, now, out)
        fun feed(plains: List<ByteArray>) { for (p in plains) feed(p) }
        fun tick() = receiver.tick(now, out)
        fun at(us: Long) { now = us; tick() }
        fun controls(kind: Int) = out.control.count { it[0].toInt() == kind }
        fun taken(kind: Int) = out.control.filter { it[0].toInt() == kind }
        fun videos() = out.messages.count { it.video }
        fun clear() { out.messages.clear(); out.control.clear() }
    }

    @Test fun fragments() {
        val random = java.util.Random(11)
        val p = Udp.FRAGMENT_PAYLOAD
        for (size in listOf(13, 100, p - 1, p, p + 1, 2 * p, 2 * p + 1, 300000, Udp.MAX_MESSAGE_BYTES - 1, Udp.MAX_MESSAGE_BYTES)) {
            val message = videoMessage(1, size - 12, true)
            assertEquals((size + p - 1) / p, Udp.fragmentCount(size))
            val parts = split(video, 1, message).toMutableList()
            for (part in parts) assertTrue(part.size <= Udp.MAX_PLAINTEXT)
            for (order in 0 until 3) {
                if (order == 1) parts.reverse()
                if (order == 2) parts.shuffle(random)
                val c = Client()
                c.feed(parts); c.feed(parts)  // every fragment twice
                assertEquals(1, c.out.messages.size)
                assertArrayEquals("Reassembled bytes differ from the input", message, c.out.messages[0].bytes)
                assertTrue("Reassembly held memory", c.receiver.heldBytes() == 0L && c.receiver.gaps() <= parts.size)
            }
            for (audioSize in listOf(13, 1932, 4096)) {
                val sound = pcmMessage(1, audioSize - 12)
                val c = Client()
                c.feed(split(audio, 1, sound).reversed())
                assertTrue(c.out.messages.size == 1 && c.out.messages[0].bytes.contentEquals(sound) && !c.out.messages[0].video)
            }
        }
        assertTrue(Udp.fragmentCount(0) == 0 && Udp.fragmentCount(Udp.MAX_MESSAGE_BYTES + 1) == 0)
        assertTrue("The contract's limits moved", Udp.MAX_PLAINTEXT == 1167 && Udp.FRAGMENT_PAYLOAD == 1157 && Udp.MAX_MESSAGE_BYTES == 2369536)
    }

    @Test fun inOrderDelivery() {
        val c = Client()
        val one = videoMessage(1); val two = videoMessage(2); val three = videoMessage(3)
        c.feed(split(video, 1, one)); assertEquals(1, c.out.messages.size)
        c.feed(split(video, 3, three)); assertEquals("A later message overtook a gap", 1, c.out.messages.size)
        c.feed(split(video, 2, two))
        assertTrue(c.out.messages.size == 3 && c.out.messages[1].bytes.contentEquals(two) && c.out.messages[2].bytes.contentEquals(three))
        c.clear(); c.feed(split(video, 2, two)); assertTrue("An old message was delivered again", c.out.messages.isEmpty())
    }

    @Test fun nackTiming() {
        run {  // A long RTT: RTT/4, then 1.25 RTT.
            val c = Client(120000); val parts = split(video, 1, videoMessage(1, 5000))
            c.feed(parts[0]); c.feed(parts[1]); c.feed(parts[3]); val gap = c.now
            c.at(gap + 29999); assertEquals(0, c.controls(Udp.KIND_NACK))
            c.at(gap + 30000); assertEquals(1, c.controls(Udp.KIND_NACK))
            val n = c.taken(Udp.KIND_NACK)[0]
            assertTrue("Wrong NACK entry", n.size == 10 && n[1].toInt() == 1 && Udp.get32(n, 2) == 1L && Udp.get16(n, 6) == 2 && Udp.get16(n, 8) == 1)
            c.at(gap + 179999); assertEquals(1, c.controls(Udp.KIND_NACK))
            c.at(gap + 180000); assertEquals(2, c.controls(Udp.KIND_NACK))
        }
        run {  // A short RTT: the floors, 10 ms and then 20 ms.
            val c = Client(4000); val parts = split(video, 1, videoMessage(1, 5000))
            c.feed(parts[0]); c.feed(parts[2]); val gap = c.now
            c.at(gap + 9999); assertEquals(0, c.controls(Udp.KIND_NACK))
            c.at(gap + 10000); assertEquals(1, c.controls(Udp.KIND_NACK))
            c.at(gap + 29999); assertEquals(1, c.controls(Udp.KIND_NACK))
            c.at(gap + 30000); assertEquals(2, c.controls(Udp.KIND_NACK))
        }
        run {  // Three repeats and no more, while the fragment stays missing.
            val c = Client(20000); val parts = split(video, 1, videoMessage(1, 5000))
            c.feed(parts[0]); c.feed(parts[2]); val gap = c.now
            for (at in listOf(10000L, 35000L, 60000L, 85000L)) {
                val before = c.controls(Udp.KIND_NACK)
                c.at(gap + at - 1); assertEquals("A NACK came early", before, c.controls(Udp.KIND_NACK))
                c.at(gap + at); assertEquals("A NACK did not come on time", before + 1, c.controls(Udp.KIND_NACK))
            }
            c.at(gap + 89999); assertTrue(c.controls(Udp.KIND_NACK) == 4 && !c.receiver.awaitingKey())
        }
        run {  // The limit holds even if the RTT rises in mid-repair and leaves time for a fifth.
            val c = Client(4000); val parts = split(video, 1, videoMessage(1, 5000))
            c.feed(parts[0]); c.feed(parts[2]); val gap = c.now
            for (at in listOf(10000L, 30000L, 50000L)) c.at(gap + at)
            c.feed(tailFor(0, 1, TEST_CHALLENGE, 80000))
            c.at(gap + 70000); assertEquals(4, c.controls(Udp.KIND_NACK))
            var at = 75000L
            while (at < 300000) { c.at(gap + at); at += 5000 }
            assertEquals("A fragment was NACKed a fifth time", 4, c.controls(Udp.KIND_NACK))
        }
        run {  // A fragment that arrived is not asked for again, even behind an older gap still open.
            val c = Client(20000); val parts = split(video, 1, videoMessage(1, 5000))
            c.feed(parts[0]); c.feed(parts[2]); c.feed(parts[4]); val gap = c.now; c.at(gap + 10000); c.feed(parts[3]); c.clear(); c.at(gap + 35000)
            val n = c.taken(Udp.KIND_NACK)
            assertTrue(n.size == 1 && n[0][1].toInt() == 1 && Udp.get16(n[0], 6) == 1 && Udp.get16(n[0], 8) == 1)
        }
        run {  // A wholly lost message repeats too, until any of it arrives.
            val c = Client(20000); val parts = split(video, 1, videoMessage(1, 5000)); c.feed(split(video, 2, videoMessage(2))); val gap = c.now
            c.at(gap + 10000); assertEquals(1, c.controls(Udp.KIND_NACK)); c.feed(parts[1]); c.clear()
            c.at(gap + 35000); val n = c.taken(Udp.KIND_NACK)
            assertTrue("A message of known size was asked for whole", n.isNotEmpty() && Udp.get16(n[0], 8) != Udp.MAX_FRAGMENTS)
        }
        run {  // A repair in time cancels the NACK; a reorder inside the delay costs nothing.
            val c = Client(20000); val parts = split(video, 1, videoMessage(1, 5000))
            c.feed(parts[0]); c.feed(parts[2]); c.at(c.now + 5000); c.feed(parts[1])
            c.at(c.now + 100000); assertEquals(0, c.controls(Udp.KIND_NACK))
        }
        run {  // Runs and a wholly lost message.
            val c = Client(4000); val parts = split(video, 1, videoMessage(1, 8000)); val later = split(video, 3, videoMessage(3))
            c.feed(parts[0]); c.feed(parts[4]); c.feed(later)
            c.at(c.now + 10000); val n = c.taken(Udp.KIND_NACK); assertTrue(n.size == 1 && n[0][1].toInt() == 3)
            assertTrue("A run was not joined", Udp.get32(n[0], 2) == 1L && Udp.get16(n[0], 6) == 1 && Udp.get16(n[0], 8) == 3)
            assertTrue("The tail of a closed message was not NACKed", Udp.get32(n[0], 10) == 1L && Udp.get16(n[0], 14) == 5 && Udp.get16(n[0], 16) == parts.size - 5)
            assertTrue("A wholly lost message was not NACKed", Udp.get32(n[0], 18) == 2L && Udp.get16(n[0], 22) == 0 && Udp.get16(n[0], 24) == Udp.MAX_FRAGMENTS)
        }
        run {  // More gaps than one NACK holds: every NACK stays within 64 entries.
            val c = Client(4000); val parts = split(video, 1, videoMessage(1, 2000 * Udp.FRAGMENT_PAYLOAD))
            for (i in parts.indices step 2) c.feed(parts[i])
            c.at(c.now + 10000); val all = c.taken(Udp.KIND_NACK)
            assertEquals(Udp.MAX_NACKS_PER_TICK, all.size)
            for (n in all) assertTrue(n[1].toInt() == Udp.MAX_NACK_ENTRIES && n.size == 2 + Udp.MAX_NACK_ENTRIES * Udp.NACK_ENTRY_BYTES)
        }
    }

    @Test fun rttFromTail() {
        // The client's RTT is the Host's: 50 ms until one arrives, and 0 means unknown.
        val c = Client(); assertEquals(50000L, c.receiver.rttUs())
        c.feed(tailFor(0, 1)); assertEquals(50000L, c.receiver.rttUs())
        c.feed(tailFor(0, 1, TEST_CHALLENGE, 33000)); assertEquals(33000L, c.receiver.rttUs())
        c.feed(tailFor(0, 1)); assertEquals(33000L, c.receiver.rttUs())
        // The assumed 50 ms really drives the timers.
        val d = Client(); val parts = split(video, 1, videoMessage(1, 5000)); d.feed(parts[0]); d.feed(parts[2]); val gap = d.now
        d.at(gap + 12499); assertEquals(0, d.controls(Udp.KIND_NACK)); d.at(gap + 12500); assertEquals(1, d.controls(Udp.KIND_NACK))
        d.at(gap + 194999); assertFalse(d.receiver.awaitingKey()); d.at(gap + 195000); assertTrue(d.receiver.awaitingKey())
    }

    // The leg sleeps until nextTickUs(): it must name every timer, and never one in the
    // past that tick() would not move (the loop would spin at its shortest wait).
    @Test fun nextDeadline() {
        run {  // Idle: FEEDBACK only.
            val c = Client(); assertEquals(1100000L, c.receiver.nextTickUs())
            c.at(1100000); assertEquals(1200000L, c.receiver.nextTickUs())
        }
        run {  // A gap: its NACK, the three repeats, then the give-up.
            val c = Client(20000); val parts = split(video, 1, videoMessage(1, 5000)); c.feed(parts[0]); c.feed(parts[2]); val gap = c.now
            assertEquals(gap + 10000, c.receiver.nextTickUs())
            c.at(gap + 9999); assertEquals("A gap was NACKed before it was due", 0, c.controls(Udp.KIND_NACK))
            for ((at, next) in listOf(10000L to 35000L, 35000L to 60000L, 60000L to 85000L)) {
                c.at(gap + at); assertEquals(gap + next, c.receiver.nextTickUs())
            }
            c.at(gap + 85000); assertEquals("After the last repeat only the give-up is left", gap + 90000, c.receiver.nextTickUs())
            c.at(gap + 90000); assertTrue(c.receiver.awaitingKey() && c.controls(Udp.KIND_NACK) == 4 && c.controls(Udp.KIND_KEYFRAME) == 1)
            assertEquals(1100000L, c.receiver.nextTickUs())
            c.at(1100000); assertEquals(1200000L, c.receiver.nextTickUs())
            // After a loss the first repeat goes three round trips and 60 ms after the request.
            c.at(1200000); assertEquals("The KEYFRAME repeat is a deadline", 1210000L, c.receiver.nextTickUs())
            c.at(1210000); assertEquals(2, c.controls(Udp.KIND_KEYFRAME)); assertEquals(1300000L, c.receiver.nextTickUs())
        }
        run {  // A held repeat is no deadline, even when it is overdue: the key frame is arriving.
            val c = Client(20000); c.feed(split(video, 1, videoMessage(1, 100, true))); c.clear()
            c.receiver.queueFull(c.now)
            c.feed(split(video, 2, videoMessage(2, 5000, true))[0])
            c.now += 50000
            assertEquals(1100000L, c.receiver.nextTickUs())
            c.tick(); assertEquals(0, c.controls(Udp.KIND_KEYFRAME))
        }
    }

    @Test fun refusedKeyFramesBackOff() {
        run {  // The answer to a request is refused: the next request waits KEYFRAME_INTERVAL after the last.
            val c = Client(20000); c.feed(split(video, 1, videoMessage(1, 100, true))); c.clear()
            c.receiver.queueFull(c.now); c.tick(); assertEquals(1, c.controls(Udp.KIND_KEYFRAME)); val asked = c.now; c.clear()
            c.now = asked + 30000; c.feed(split(video, 2, videoMessage(2, 100, true))); assertTrue(c.videos() == 1 && !c.receiver.awaitingKey())
            c.receiver.refused(c.now); assertTrue(c.receiver.awaitingKey())
            c.at(asked + 499999); assertEquals("A refused key frame asked again within the interval", 0, c.controls(Udp.KIND_KEYFRAME))
            c.at(asked + 500000); assertEquals(1, c.controls(Udp.KIND_KEYFRAME))
        }
        run {  // A decoder that refuses everything: one request per interval, not one per answer.
            val c = Client(20000); c.feed(split(video, 1, videoMessage(1, 100, true))); c.clear()
            c.receiver.refused(c.now); c.tick(); assertEquals("Nothing asked before: at once", 1, c.controls(Udp.KIND_KEYFRAME))
            var id = 2L
            repeat(100) {  // 2 s, a key frame every 20 ms, each refused
                c.now += 20000
                c.feed(split(video, id, videoMessage(id, 100, true))); ++id
                if (c.out.messages.any { it.video && it.key }) c.receiver.refused(c.now)
                c.out.messages.clear(); c.tick()
            }
            assertEquals("Requests at 0, 0.5, 1, 1.5 and 2 s", 5, c.controls(Udp.KIND_KEYFRAME))
        }
        run {  // A P-frame lost to a full queue still asks at once, as on Windows.
            val c = Client(20000); c.feed(split(video, 1, videoMessage(1, 100, true))); c.clear()
            c.receiver.queueFull(c.now); c.tick(); c.feed(split(video, 2, videoMessage(2, 100, true))); c.clear()
            c.now += 30000; c.receiver.queueFull(c.now); c.tick(); assertEquals(1, c.controls(Udp.KIND_KEYFRAME))
        }
    }

    @Test fun giveUpDeadline() {
        for ((rtt, deadline) in listOf(5000L to 60000L, 11000L to 60000L, 20000L to 90000L, 33000L to 135500L, 60000L to 230000L, 80000L to 300000L,
            150000L to 545000L, 166000L to 600000L, 300000L to 600000L)) {
            val c = Client(rtt)
            c.feed(split(video, 1, videoMessage(1, 10, true)))  // nothing yet at stake
            c.clear()
            val second = split(video, 2, videoMessage(2, 5000))
            c.feed(second[0]); c.feed(second[2])
            val gap = c.now
            c.at(gap + deadline - 1); assertFalse("Given up before the deadline, RTT $rtt", c.receiver.awaitingKey())
            c.at(gap + deadline); assertTrue("Not given up at the deadline, RTT $rtt", c.receiver.awaitingKey())
            assertEquals(1, c.controls(Udp.KIND_KEYFRAME))
        }
        run {  // A paced IDR longer than the deadline is not lost: only a gap starts the clock.
            val c = Client(5000); val parts = split(video, 1, videoMessage(1, 300000, true))
            for (part in parts) { c.now += 370; c.feed(part); c.tick() }
            assertTrue(c.videos() == 1 && !c.receiver.awaitingKey() && c.out.control.size < 5)
        }
        run {  // A gap repaired in time resets nothing later: the message delivers.
            val c = Client(20000); val parts = split(video, 1, videoMessage(1, 5000, true))
            c.feed(parts[0]); c.feed(parts[2]); c.feed(parts[3]); c.feed(parts[4]); c.at(c.now + 59000); c.feed(parts[1])
            c.at(c.now + 200000); assertTrue(c.videos() == 1 && !c.receiver.awaitingKey())
        }
        run {  // A whole P-frame waiting behind the lost one dies with it: its reference is gone.
            val c = Client(20000); c.feed(split(video, 1, videoMessage(1, 100, true))); c.clear()
            val lost = split(video, 2, videoMessage(2, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.feed(split(video, 3, videoMessage(3))); c.feed(split(video, 4, videoMessage(4)))
            assertTrue(c.out.messages.isEmpty())
            c.at(c.now + 90000); assertTrue(c.out.messages.isEmpty() && c.receiver.awaitingKey() && c.receiver.heldBytes() == 0L)
        }
        run {  // While waiting, P-frames behind a key frame still under repair are kept: they follow it.
            val c = Client(20000); c.feed(split(video, 1, videoMessage(1, 100, true))); c.clear()
            val lost = split(video, 2, videoMessage(2, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.at(c.now + 90000); assertTrue(c.receiver.awaitingKey()); c.clear()
            val key = split(video, 3, videoMessage(3, 5000, true)); c.feed(key[0]); c.feed(key[2]); c.feed(key[3]); c.feed(key[4])
            c.feed(split(video, 4, videoMessage(4))); c.feed(split(video, 5, videoMessage(5)))
            c.feed(key[1]); assertTrue(c.videos() == 3 && c.out.messages[0].key && !c.receiver.awaitingKey())
        }
        run {  // The same when the key frame has not named itself yet: fragment 0 was the one lost.
            val c = Client(20000); c.feed(split(video, 1, videoMessage(1, 100, true))); c.clear()
            val lost = split(video, 2, videoMessage(2, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.at(c.now + 90000); c.clear()
            val key = split(video, 3, videoMessage(3, 5000, true)); c.feed(key[1]); c.feed(key[2]); c.feed(key[3]); c.feed(key[4])
            c.feed(split(video, 4, videoMessage(4))); c.feed(key[0])
            assertTrue(c.videos() == 2 && c.out.messages[0].key && c.out.messages[1].pts == 4L)
        }
        run {  // A repair after the give-up is dropped and delivers nothing.
            val c = Client(20000); val first = split(video, 1, videoMessage(1, 5000)); val next = split(video, 2, videoMessage(2))
            c.feed(first[0]); c.feed(first[2]); c.at(c.now + 90000); c.feed(first[1]); c.feed(next); c.tick()
            assertTrue(c.videos() == 0 && c.receiver.awaitingKey())
        }
    }

    @Test fun dropUntilKey() {
        val c = Client(20000); val first = split(video, 1, videoMessage(1, 5000, true))
        c.feed(first); c.clear()
        val lost = split(video, 2, videoMessage(2, 5000)); c.feed(lost[0]); c.feed(lost[2])
        c.at(c.now + 90000); assertTrue(c.receiver.awaitingKey())
        val k = c.taken(Udp.KIND_KEYFRAME)
        assertTrue("Wrong KEYFRAME", k.size == 1 && k[0].size == Udp.KEYFRAME_BYTES && k[0][1].toInt() == 0 && Udp.get16(k[0], 2) == 1)
        c.clear()
        c.feed(split(video, 3, videoMessage(3))); c.feed(split(video, 4, videoMessage(4)))
        assertTrue(c.out.messages.isEmpty() && c.receiver.heldBytes() == 0L)
        // The request may be lost: asked again after three round trips and 60 ms, then a
        // quarter second on, then every interval.
        c.at(c.now + 119999); assertEquals(0, c.controls(Udp.KIND_KEYFRAME))
        c.at(c.now + 1); assertEquals("The first repeat after a loss", 1, c.controls(Udp.KIND_KEYFRAME))
        c.at(c.now + 249999); assertEquals(1, c.controls(Udp.KIND_KEYFRAME))
        c.at(c.now + 1); assertEquals("The second repeat after a loss", 2, c.controls(Udp.KIND_KEYFRAME))
        c.at(c.now + 499999); assertEquals(2, c.controls(Udp.KIND_KEYFRAME))
        c.at(c.now + 1); assertEquals("Later repeats keep the interval", 3, c.controls(Udp.KIND_KEYFRAME))
        c.clear()
        // No gap, no NACK for frames it will drop.
        c.at(c.now + 100000); assertEquals(0, c.controls(Udp.KIND_NACK))
        // A key frame lost at the start of its message: it still counts when it is whole.
        val key = videoMessage(9, 5000, true); val parts = split(video, 5, key)
        c.feed(parts[1]); c.feed(parts[2]); assertTrue(c.receiver.awaitingKey())
        c.feed(parts[0]); c.feed(parts[3]); c.feed(parts[4])
        assertTrue(c.videos() == 1 && c.out.messages[0].bytes.contentEquals(key) && c.out.messages[0].key && !c.receiver.awaitingKey())
        c.clear(); c.feed(split(video, 6, videoMessage(10)))
        assertEquals(1, c.videos())
        c.at(c.now + 2000000); assertEquals(0, c.controls(Udp.KIND_KEYFRAME))
    }

    @Test fun keyFrameRules() {
        run {
            val c = Client(20000)
            c.feed(split(video, 1, videoMessage(1, 100, true))); c.clear()
            c.receiver.queueFull(c.now); c.at(c.now); c.clear()
            val answer = split(video, 2, videoMessage(2, 5000, true))
            c.feed(answer[0]); c.feed(answer[2])
            val start = c.now
            c.at(start + 89999); assertEquals(0, c.controls(Udp.KIND_KEYFRAME))
            c.at(start + 90000); assertEquals(1, c.controls(Udp.KIND_KEYFRAME))
            c.clear(); c.at(start + 190000); assertEquals(0, c.controls(Udp.KIND_KEYFRAME))
        }
        run {  // The repeat waits while a key frame is visibly arriving, however long that takes.
            val c = Client(20000); c.feed(split(video, 1, videoMessage(1, 100, true))); c.clear()
            val lost = split(video, 2, videoMessage(2, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.at(c.now + 90000); assertEquals(1, c.controls(Udp.KIND_KEYFRAME)); c.clear()
            val key = split(video, 3, videoMessage(3, 5000, true)); c.feed(key[0]); c.feed(key[1])
            c.at(c.now + 500000); c.at(c.now + 1000000); assertEquals(0, c.controls(Udp.KIND_KEYFRAME))
            // It stalls: TAIL closes it, and the held repeat goes at the give-up, not 500 ms later.
            c.feed(tailFor(3, 1)); val closed = c.now
            c.at(closed + 89999); assertEquals(0, c.controls(Udp.KIND_KEYFRAME))
            c.at(closed + 90000); assertTrue(c.controls(Udp.KIND_KEYFRAME) == 1 && c.receiver.awaitingKey())
            // The second request of this wait: the next goes a quarter second on.
            c.clear(); c.at(closed + 90000 + 249999); assertEquals(0, c.controls(Udp.KIND_KEYFRAME)); c.at(closed + 90000 + 250000); assertEquals(1, c.controls(Udp.KIND_KEYFRAME))
        }
        run {  // It finishes: no repeat at all.
            val c = Client(20000); c.feed(split(video, 1, videoMessage(1, 100, true))); c.clear()
            val lost = split(video, 2, videoMessage(2, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.at(c.now + 90000); c.clear()
            val key = split(video, 3, videoMessage(3, 5000, true)); c.feed(key[0]); c.at(c.now + 700000); c.feed(key[1]); c.feed(key[2]); c.feed(key[3]); c.feed(key[4]); c.at(c.now + 2000000)
            assertTrue(c.videos() == 1 && c.controls(Udp.KIND_KEYFRAME) == 0)
        }
        // Only a usable key frame holds it, not a P-frame or an old epoch's key frame, even behind a gap.
        for (waiting in listOf(videoMessage(4), videoMessage(4, 100, true, 0))) {
            val c = Client(20000); c.feed(split(video, 1, videoMessage(1, 100, true))); c.clear()
            val lost = split(video, 2, videoMessage(2, 5000)); c.feed(lost[0]); c.feed(lost[2]); c.at(c.now + 90000); val asked = c.now; c.clear()
            c.now = asked + 115000; c.feed(split(video, 4, waiting)); assertEquals("Fixture", 2, c.receiver.windowMessages())
            c.at(asked + 120000); assertEquals(1, c.controls(Udp.KIND_KEYFRAME))
        }
        run {  // A newer key frame leaves an older one under repair alone: killing it is how IDRs cascade.
            val c = Client(20000); val first = split(video, 1, videoMessage(1, 5000, true)); val second = split(video, 3, videoMessage(3, 5000, true))
            c.feed(first[0]); c.feed(first[2]); c.feed(first[3]); c.feed(first[4]); c.feed(split(video, 2, videoMessage(2))); c.feed(second[0]); c.feed(second[1])
            c.feed(first[1]); assertTrue(c.videos() == 2 && c.out.messages[0].key && c.out.messages[0].pts == 1L && c.out.messages[1].pts == 2L)
            c.feed(second[2]); c.feed(second[3]); c.feed(second[4]); assertTrue(c.videos() == 3 && c.out.messages[2].key)
        }
        run {  // If the older one cannot be repaired, the newer one still ends the wait.
            val c = Client(20000); val first = split(video, 1, videoMessage(1, 5000, true)); val second = split(video, 2, videoMessage(2, 5000, true))
            c.feed(first[0]); c.feed(first[2]); c.feed(second[0]); c.feed(second[1]); c.at(c.now + 90000); assertTrue(c.receiver.awaitingKey() && c.videos() == 0)
            c.feed(second[2]); c.feed(second[3]); c.feed(second[4]); assertTrue(c.videos() == 1 && c.out.messages[0].pts == 2L && !c.receiver.awaitingKey())
        }
        run {  // Only the nearest older key frame stays, so a run of them cannot pile up.
            val c = Client(20000); val a = split(video, 1, videoMessage(1, 5000, true)); val b = split(video, 2, videoMessage(2, 5000, true)); val d = split(video, 3, videoMessage(3, 5000, true))
            c.feed(a[0]); c.feed(b[0]); c.feed(d[0]); assertEquals(2, c.receiver.windowMessages()); c.feed(a[1]); assertEquals(2, c.receiver.windowMessages())
        }
        run {  // A key frame in progress makes older unfinished frames pointless, without any IDR request.
            val c = Client(20000); val old = split(video, 1, videoMessage(1, 5000)); val key = split(video, 2, videoMessage(2, 5000, true))
            c.feed(old[0]); c.feed(old[2]); c.feed(key[0]); c.feed(key[1])
            c.at(c.now + 200000)
            assertTrue(!c.receiver.awaitingKey() && c.controls(Udp.KIND_KEYFRAME) == 0)
            c.feed(key[2]); c.feed(key[3]); c.feed(key[4])
            assertTrue(c.videos() == 1 && c.out.messages[0].key)
        }
        run {  // A whole key frame on the other path stops the repeat and clears the gaps.
            val c = Client(20000); val lost = split(video, 1, videoMessage(1, 5000)); c.feed(lost[0]); c.feed(lost[2])
            c.at(c.now + 90000); assertTrue(c.receiver.awaitingKey()); c.clear()
            c.receiver.keyFrameOtherPath(); c.at(c.now + 2000000)
            assertTrue(!c.receiver.awaitingKey() && c.controls(Udp.KIND_KEYFRAME) == 0 && c.controls(Udp.KIND_NACK) == 0)
        }
        run {  // WSS ended it, and a UDP key frame already under way still finishes.
            val c = Client(20000); val key = split(video, 1, videoMessage(1, 5000, true))
            c.feed(key[0]); c.feed(key[1]); c.receiver.keyFrameOtherPath(); c.feed(key[2]); c.feed(key[3]); c.feed(key[4])
            c.at(c.now + 50000); assertTrue(c.videos() == 0 && c.controls(Udp.KIND_NACK) == 1)
            c.feed(key[0]); c.feed(key[1]); assertEquals(1, c.videos())
        }
        run {  // A full decoder queue skips to the next key frame.
            val c = Client(20000); c.feed(split(video, 1, videoMessage(1, 100, true))); c.clear()
            c.receiver.queueFull(c.now); c.tick(); assertEquals(1, c.controls(Udp.KIND_KEYFRAME))
            c.clear(); c.feed(split(video, 2, videoMessage(2))); assertTrue(c.out.messages.isEmpty())
            c.feed(split(video, 3, videoMessage(3, 100, true))); assertEquals(1, c.videos())
        }
    }

    @Test fun epochs() {
        val c = Client(20000, 5)
        c.feed(split(video, 1, videoMessage(1, 100, true, 5))); assertEquals(1, c.videos()); c.clear()
        c.feed(split(video, 2, videoMessage(2, 100, false, 4))); c.feed(split(audio, 1, pcmMessage(2, 1920, 4)))
        assertTrue("A stale epoch was delivered or broke the chain", c.out.messages.isEmpty() && !c.receiver.awaitingKey())
        c.feed(split(video, 3, videoMessage(3, 100, false, 6))); assertTrue(c.out.messages.isEmpty() && c.receiver.awaitingKey())
        c.tick(); assertEquals("KEYFRAME does not carry the client's epoch", 5, Udp.get16(c.taken(Udp.KIND_KEYFRAME)[0], 2))
        c.clear(); c.receiver.setEpoch(6)
        c.feed(split(video, 4, videoMessage(4, 100, true, 6))); assertTrue(c.videos() == 1 && !c.receiver.awaitingKey())
        // A message whole and waiting when the epoch moves on is not delivered.
        c.clear(); val a = split(video, 5, videoMessage(5, 5000, false, 6)); val b = split(video, 6, videoMessage(6, 100, false, 6))
        c.feed(a[0]); c.feed(a[2]); c.feed(b); c.receiver.setEpoch(7); c.feed(a[1]); c.feed(a[3]); c.feed(a[4])
        assertTrue(c.out.messages.isEmpty())
    }

    @Test fun tailHandling() {
        run {  // The last fragments of an idle screen's frame.
            val c = Client(4000); val parts = split(video, 1, videoMessage(1, 5000)); c.feed(parts[0]); c.feed(parts[1])
            c.at(c.now + 100000); assertEquals(0, c.controls(Udp.KIND_NACK))
            c.clear(); c.feed(tailFor(1, 123456)); c.at(c.now + 10000)
            val n = c.taken(Udp.KIND_NACK); assertTrue("TAIL did not reveal the lost end", n.size == 1 && Udp.get16(n[0], 6) == 2 && Udp.get16(n[0], 8) == parts.size - 2)
        }
        run {  // A whole message lost on an idle screen.
            val c = Client(4000); c.feed(split(video, 1, videoMessage(1))); c.clear(); c.feed(tailFor(2, 5)); c.at(c.now + 10000)
            val n = c.taken(Udp.KIND_NACK); assertTrue(n.size == 1 && Udp.get32(n[0], 2) == 2L && Udp.get16(n[0], 8) == Udp.MAX_FRAGMENTS)
        }
        run {  // Nothing lost: TAIL changes nothing, repeats change nothing.
            val c = Client(4000); c.feed(split(video, 1, videoMessage(1))); c.feed(tailFor(1, 5)); c.feed(tailFor(1, 6)); c.at(c.now + 200000)
            assertTrue(c.controls(Udp.KIND_NACK) == 0 && !c.receiver.awaitingKey())
        }
        run {  // Echo and hold, once.
            val c = Client(4000); val start = c.now; c.feed(tailFor(0, 777777, 0xabcdef)); c.at(start + 37000); assertEquals(0, c.controls(Udp.KIND_FEEDBACK))
            c.at(start + 100000); var f = c.taken(Udp.KIND_FEEDBACK); assertTrue(f.size == 1 && Udp.get64(f[0], 14) == 777777L && Udp.get32(f[0], 22) == 100000L)
            assertEquals(0xabcdefL, Udp.get64(f[0], 26))
            c.clear(); c.at(start + 200000); f = c.taken(Udp.KIND_FEEDBACK); assertTrue(f.size == 1 && Udp.get64(f[0], 14) == 0L && Udp.get32(f[0], 22) == 0L)
            assertEquals(0xabcdefL, Udp.get64(f[0], 26))
            c.feed(tailFor(0, 5, 0x1234)); c.clear(); c.at(start + 300000); assertEquals(0x1234L, Udp.get64(c.taken(Udp.KIND_FEEDBACK)[0], 26))
        }
        run {  // Nothing reached it: FEEDBACK echoes nothing.
            val c = Client(); c.at(c.now + 100000); assertEquals(0L, Udp.get64(c.taken(Udp.KIND_FEEDBACK)[0], 26))
        }
    }

    @Test fun feedbackTiming() {
        val c = Client(20000); val start = c.now
        var t = start
        while (t < start + 1000000) { c.at(t); t += 1000 }
        assertEquals("FEEDBACK does not run every 100 ms on an idle stream", 9, c.controls(Udp.KIND_FEEDBACK))
        c.clear(); c.at(start + 1100000); var f = c.taken(Udp.KIND_FEEDBACK)
        assertTrue(f.size == 1 && f[0].size == Udp.FEEDBACK_BYTES && f[0][1].toInt() == 0)
        val first = split(video, 1, videoMessage(1, 100, true)); val second = split(video, 2, videoMessage(2, 5000)); val third = split(video, 3, videoMessage(3, 100))
        c.at(c.now + 50000)
        c.feed(first); c.feed(second[0]); c.feed(second[2]); c.feed(third); c.feed(split(audio, 1, pcmMessage(1))); c.feed(tailFor(3, 9))
        c.clear(); c.at(c.now + 50000); f = c.taken(Udp.KIND_FEEDBACK)
        // Message 3 is whole but waits behind message 2: only what is handed on is progress.
        assertTrue(f.size == 1 && Udp.get32(f[0], 2) == 1L && Udp.get32(f[0], 6) == (1 + first.size + 2 + third.size + 2 + 1).toLong())  // 1: the TAIL that set the RTT
        assertEquals("Datagrams counted lost before the give-up", 0L, Udp.get32(f[0], 10))
        c.clear(); c.at(c.now + 300000); f = c.taken(Udp.KIND_FEEDBACK); assertTrue(f.isNotEmpty() && Udp.get32(f.last(), 10) == (second.size - 2).toLong())
    }

    @Test fun audioRules() {
        run {  // Never waits for video, and arrives out of order as it comes.
            val c = Client(20000); val lost = split(video, 1, videoMessage(1, 5000)); c.feed(lost[0]); c.feed(lost[2])
            val one = pcmMessage(1); val two = pcmMessage(2); val a = split(audio, 1, one); val b = split(audio, 2, two)
            assertEquals("10 ms of PCM is not two fragments", 2, a.size)
            c.feed(b); assertTrue(c.out.messages.size == 1 && c.out.messages[0].bytes.contentEquals(two))
            c.feed(a); assertTrue(c.out.messages.size == 2 && c.out.messages[1].bytes.contentEquals(one))
        }
        run {  // A lost audio fragment causes nothing at all: no NACK, no give-up.
            val c = Client(4000); c.feed(split(audio, 1, pcmMessage(1))[0]); c.feed(split(audio, 3, pcmMessage(3)))
            c.feed(split(audio, 4, pcmMessage(4))[1]); c.feed(tailFor(0, 1))
            repeat(300) { c.at(c.now + 1000) }
            assertTrue(c.controls(Udp.KIND_NACK) == 0 && c.controls(Udp.KIND_KEYFRAME) == 0 && !c.receiver.awaitingKey())
        }
        run {  // Unfinished audio is bounded and counts as lost when it goes.
            val c = Client(20000)
            for (id in 1L..100L) { c.feed(split(audio, id, pcmMessage(id))[0]); assertTrue(c.receiver.heldBytes() <= Udp.AUDIO_PARTIALS * 2L * Udp.FRAGMENT_PAYLOAD) }
            val parts = split(audio, 200, pcmMessage(200, 5000)); assertTrue(parts.size > 2); c.feed(parts); assertEquals(1, c.out.messages.size)
            c.clear(); c.at(c.now + 100000)
            // Eight slots: 92 halves pushed out by newer ones, and one more by message 200.
            assertEquals("Unfinished audio was not counted lost", 93L, Udp.get32(c.taken(Udp.KIND_FEEDBACK).last(), 10))
        }
    }

    @Test fun maliciousPlaintext() {
        // Each one is whole and authenticated by assumption, and must deliver nothing.
        val malformed = listOf(
            videoMessage(1).also { it[0] = 2 }, videoMessage(1).also { it[0] = 3 }, videoMessage(1).also { it[0] = 0 },
            videoMessage(1).also { it[1] = 2 }, ByteArray(12) { 1 })  // the last: the envelope alone
        for (message in malformed) {
            val c = Client(); c.feed(split(video, 1, message))
            c.feed(tailFor(1, 1)); c.at(c.now + 1000000)
            assertTrue("A malformed message was delivered", c.videos() == 0 && c.receiver.awaitingKey())
        }
        for (message in listOf(pcmMessage(1).also { it[1] = 1 }, pcmMessage(1).also { it[0] = 1 }, ByteArray(12) { 2 })) {
            val c = Client(); c.feed(split(audio, 1, message)); assertTrue("Malformed audio was delivered", c.out.messages.isEmpty())
        }
        run {
            val message = videoMessage(1, 5000); val parts = split(video, 1, message)
            fun expect(bad: ByteArray, reason: String) { val c = Client(); c.feed(bad); assertTrue(reason, c.receiver.windowMessages() == 0 && c.receiver.heldBytes() == 0L) }
            expect(parts[0].copyOf().also { it[1] = 2 }, "A bad class was accepted")
            expect(parts[0].copyOf().also { Udp.put32(it, 2, 0) }, "Message ID 0 was accepted")
            expect(parts[0].copyOf().also { Udp.put16(it, 8, 0) }, "Count 0 was accepted")
            expect(parts[0].copyOf().also { Udp.put16(it, 8, 2049) }, "Count 2049 was accepted")
            expect(parts[0].copyOf().also { Udp.put16(it, 6, Udp.get16(it, 8)) }, "An index past the count was accepted")
            expect(parts[0].copyOf(parts[0].size - 1), "A short middle fragment was accepted")
            expect(parts[0].copyOf(parts[0].size + 1), "A long fragment was accepted")
            expect(parts[0].copyOf(Udp.FRAGMENT_HEADER_BYTES), "An empty fragment was accepted")
            expect(ByteArray(0), "An empty plaintext was accepted")
            // A count that changes inside a message is ignored; the message still finishes.
            val c = Client(); val changed = parts[1].copyOf().also { Udp.put16(it, 8, parts.size + 1) }; c.feed(parts[0]); c.feed(changed); c.feed(parts[1])
            for (i in 2 until parts.size) c.feed(parts[i])
            assertTrue("A changed count disturbed a message", c.out.messages.size == 1 && c.out.messages[0].bytes.contentEquals(message))
        }
        run {  // TAIL and sizes.
            val c = Client()
            c.feed(tailFor(1, 1).also { it[1] = 1 }); c.feed(tailFor(1, 1).copyOf(Udp.TAIL_BYTES - 1)); c.feed(tailFor(1, 1).copyOf(Udp.TAIL_BYTES + 1))
            c.at(c.now + 200000); assertEquals("A malformed TAIL counted", 0L, Udp.get32(c.taken(Udp.KIND_FEEDBACK)[0], 6))
            // Huge IDs cannot make it spin or grow.
            c.feed(tailFor(0xffffffffL, 1)); c.feed(tailFor(0xfffffff0L, 2)); assertTrue(c.receiver.windowMessages() <= Udp.VIDEO_WINDOW)
            c.at(c.now + 2000000)
            c.feed(fragmentOf(video, 0xffffffffL, videoMessage(1, 5000, true), 0)); assertTrue(c.receiver.windowMessages() <= Udp.VIDEO_WINDOW)
        }
    }

    // The Host's 271 cause 3 (FEEDBACK heard, nothing made whole for 1 s)
    // watched from this side: when it happens, and which wait it was.
    @Test fun stallWatch() {
        fun stall(cause: Int, onUdp: Boolean = true, setup: (Client) -> Unit): Client {
            val c = Client()
            c.feed(split(video, 1, videoMessage(1, key = true))); c.feed(tailFor(1, 1))
            val start = c.now
            c.receiver.watch(start, onUdp)
            setup(c)
            c.receiver.watch(start + 999999, onUdp); assertEquals("A stall came early", 0L, c.receiver.stalls)
            c.receiver.watch(start + 1000000, onUdp)
            if (!onUdp) { assertEquals("A stall counted off the active path", 0L, c.receiver.stalls); return c }
            assertEquals(1L, c.receiver.stalls); assertEquals(cause, c.receiver.stallCause)
            c.receiver.watch(start + 3000000, onUdp); assertEquals("One stretch counted twice", 1L, c.receiver.stalls)
            return c
        }
        val p2 = { split(video, 2, videoMessage(2)) }
        stall(UdpReceiver.STALL_QUEUE_FULL) { it.receiver.queueFull(it.now); it.feed(p2()); it.feed(tailFor(2, 2)) }
        stall(UdpReceiver.STALL_REFUSED) { it.receiver.refused(it.now); it.feed(p2()); it.feed(tailFor(2, 2)) }
        stall(UdpReceiver.STALL_LOSS) { val parts = p2(); it.feed(parts[0]); it.feed(tailFor(2, 2)); it.at(it.now + 500000) }
        stall(UdpReceiver.STALL_EPOCH) { it.feed(split(video, 2, videoMessage(2, epoch = 2))); it.feed(tailFor(2, 2)) }
        stall(UdpReceiver.STALL_REPAIR) { it.feed(p2()[0]); it.feed(tailFor(2, 2)) }
        stall(UdpReceiver.STALL_NOTHING) { it.feed(tailFor(2, 2)) }
        stall(0, onUdp = false) { it.receiver.queueFull(it.now); it.feed(p2()); it.feed(tailFor(2, 2)) }
        // Progress ends a stretch: a key frame made whole, and the next stall needs its own second.
        val c = stall(UdpReceiver.STALL_QUEUE_FULL) { it.receiver.queueFull(it.now); it.feed(p2()); it.feed(tailFor(2, 2)) }
        c.at(4000000); assertEquals("The wait asked for no key frame", 1L, c.receiver.keyRequests)
        c.feed(split(video, 3, videoMessage(3, key = true))); c.feed(tailFor(4, 3))
        c.receiver.watch(4100000, true); c.receiver.watch(5099999, true); assertEquals(1L, c.receiver.stalls)
        c.receiver.watch(5100000, true); assertEquals(2L, c.receiver.stalls)
        assertEquals("Message 4 is missing outright", UdpReceiver.STALL_NOTHING, c.receiver.stallCause)
        // Caught up (nothing newer than the Host finished) is progress, however long.
        val idle = Client(); idle.feed(split(video, 1, videoMessage(1, key = true))); idle.feed(tailFor(1, 1))
        for (at in 0L..5000000L step 100000) idle.receiver.watch(1000000 + at, true)
        assertEquals(0L, idle.receiver.stalls)
    }

    @Test fun windowBounds() {
        val c = Client(20000); c.feed(split(video, 1, videoMessage(1, 5000, true))[0])
        for (id in 3L until 1000L step 7) {
            c.feed(fragmentOf(video, id, videoMessage(id, 5000), 1))
            assertTrue(c.receiver.windowMessages() <= Udp.VIDEO_WINDOW && c.receiver.heldBytes() <= Udp.MAX_VIDEO_BYTES && c.receiver.gaps() <= Udp.MAX_GAPS)
        }
        // One enormous message then many more: the byte bound holds, and delivery recovers.
        val d = Client(20000)
        var id = 1L
        while (id <= 40) { d.feed(fragmentOf(video, id, videoMessage(id, Udp.MAX_MESSAGE_BYTES - 12, true), 1)); assertTrue(d.receiver.heldBytes() <= Udp.MAX_VIDEO_BYTES); ++id }
        d.at(d.now + 2000000); d.clear()
        d.feed(split(video, id + 200, videoMessage(9000, 2000, true))); assertEquals("Delivery did not recover", 1, d.videos())
        // Gaps are capped too.
        val g = Client(20000); val big = videoMessage(1, 2000 * Udp.FRAGMENT_PAYLOAD); val count = Udp.fragmentCount(big.size)
        for (m in 1L..8L) {
            g.feed(fragmentOf(video, m, big, count - 1)); assertTrue(g.receiver.gaps() <= Udp.MAX_GAPS)
            g.feed(tailFor(m, 1)); assertTrue(g.receiver.gaps() <= Udp.MAX_GAPS)
        }
    }

    @Test fun fuzz() {
        val random = Random(0x75647031)
        run {  // Random plaintext: bounds hold, nothing delivered.
            val c = Client(); var delivered = 0L
            for (n in 0 until 500000) {
                val size = random.nextInt(64) + if (n % 16 == 0) random.nextInt(Udp.MAX_PLAINTEXT) else 0
                val plain = random.nextBytes(size)
                if (size != 0 && n % 2 == 1) plain[0] = (1 + random.nextInt(8)).toByte()
                c.feed(plain)
                c.now += 50; if (n % 8 == 0) c.tick()
                delivered += c.out.messages.size; c.clear()
                if (n % 1024 == 0) assertTrue(c.receiver.windowMessages() <= Udp.VIDEO_WINDOW && c.receiver.gaps() <= Udp.MAX_GAPS &&
                    c.receiver.heldBytes() <= Udp.MAX_VIDEO_BYTES + Udp.AUDIO_PARTIALS.toLong() * Udp.MAX_AUDIO_FRAGMENTS * Udp.FRAGMENT_PAYLOAD)
            }
            assertEquals("Random plaintext produced a delivered message", 0L, delivered)
        }
        run {  // Real traffic with junk, duplicates, loss and reordering mixed in: what comes out is exactly what went in.
            val c = Client(4000); val sent = ArrayList<ByteArray>(); val wire = ArrayList<ByteArray>()
            var nextId = 1L; var pts = 1L; var delivered = 0; var lastPts = 0L
            for (n in 0 until 30000) {
                if (wire.size < 8) {
                    val key = random.nextInt(50) == 0
                    val m = videoMessage(pts, random.nextInt(6000) + 10, key, 1, pts.toInt()); ++pts
                    sent.add(m); wire.addAll(split(video, nextId, m)); ++nextId
                }
                val at = random.nextInt(minOf(wire.size, 6)); val f = wire[at]
                if (random.nextInt(20) != 0) wire.removeAt(at)
                if (random.nextInt(10) != 0) c.feed(f)
                val plain = random.nextBytes(random.nextInt(40))
                if (plain.isNotEmpty()) { if (plain[0].toInt() == Udp.KIND_FRAGMENT || plain[0].toInt() == Udp.KIND_TAIL) plain[0] = 0; c.feed(plain) }
                c.now += 700; c.tick()
                for (m in c.out.messages) {
                    assertTrue("A delivered message is not an input message, or is out of order",
                        m.video && m.pts > lastPts && m.pts <= sent.size && m.bytes.contentEquals(sent[(m.pts - 1).toInt()]))
                    lastPts = m.pts; ++delivered
                }
                c.clear()
            }
            assertTrue("The fixture delivered too little: $delivered", delivered > 100)
        }
    }

    private fun parityOf(id: Long, message: ByteArray, slot: Int): ByteArray {
        val out = ByteArray(Udp.MAX_PLAINTEXT)
        return out.copyOf(Udp.writeParity(out, video, id, message, slot, Udp.parityCount(Udp.fragmentCount(message.size))))
    }
    private fun paritiesOf(id: Long, message: ByteArray) = List(Udp.parityCount(Udp.fragmentCount(message.size))) { parityOf(id, message, it) }
    private fun fnv(bytes: ByteArray): ULong { var h = 1469598103934665603UL; for (b in bytes) { h = h xor (b.toLong() and 0xff).toULong(); h *= 1099511628211UL }; return h }

    // The parity datagrams are the Host's, byte for byte: these are the FNV-1a hashes of what
    // native/windows_video/udp_media.hpp writeParity writes for the same messages (ID 9).
    @Test fun parityWire() {
        val expected = mapOf(
            100 to listOf(0xcac03a0db11d27fbUL),
            5000 to listOf(0xc64db82303dafef5UL),
            40000 to listOf(0x8f9a4833150ed410UL, 0x1b9bd7f244c40c22UL, 0x6ba8cb5d4e42b5a9UL),
            300000 to listOf(0xb3b03d3c45f8960bUL, 0x98c52c0b466f59e9UL, 0xe975a0902c9a11a1UL, 0x7b3c5b200cc6f4f8UL, 0xd35a51fc86c3a131UL, 0xc10c99f40c57342aUL,
                0x3b355be8e00e4c7cUL, 0x85d1b4054a63c24bUL, 0xad54285c25871e10UL, 0xf4dae30ced2a6309UL, 0xafe009688a463420UL, 0xd19f714b1d8fbfa8UL,
                0x7f6a026827df3806UL, 0xadb57ba96e30a87aUL, 0x7e185ae6deb30e36UL, 0x976114b4027c81b6UL))
        for ((payload, hashes) in expected) {
            val m = mediaMessage(1, true, 1, 7, payload, 3)
            assertEquals("Parity count for $payload", hashes.size, Udp.parityCount(Udp.fragmentCount(m.size)))
            assertEquals("Parity bytes for $payload differ from the Host's", hashes, paritiesOf(9, m).map(::fnv))
        }
        assertTrue(Udp.parityCount(0) == 0 && Udp.parityCount(1) == 1 && Udp.parityCount(16) == 1 && Udp.parityCount(17) == 2 &&
            Udp.parityCount(256) == 16 && Udp.parityCount(2048) == 16)
        assertEquals(Udp.MAX_PLAINTEXT, Udp.PARITY_BYTES)
    }

    // parity-v1, as apps/windows_host/src/udp_media_test.cpp (parityRules) checks the C++ receiver.
    @Test fun parityRules() {
        for (lost in listOf(0, 2, 4)) {  // Any one lost fragment comes back at once: the first, a middle one, the short last one.
            val c = Client(20000, parity = true); val m = videoMessage(1, 5000, true); val parts = split(video, 1, m)
            assertEquals("Fixture", 5, parts.size)
            for (i in parts.indices) if (i != lost) c.feed(parts[i])
            assertEquals(0, c.videos())
            c.feed(paritiesOf(1, m))
            assertTrue("Fragment $lost was not rebuilt", c.videos() == 1 && c.out.messages[0].bytes.contentEquals(m) && c.out.messages[0].key)
            c.at(c.now + 300000)
            assertTrue("A rebuilt fragment was asked for again", c.controls(Udp.KIND_NACK) == 0 && !c.receiver.awaitingKey())
            assertTrue(c.receiver.rebuilt == 1L && c.receiver.repaired == 1L && c.receiver.parities == 1L && c.receiver.heldBytes() == 0L)
        }
        run {  // A run of lost fragments as long as the parity count comes back whole.
            val c = Client(20000, parity = true); val m = videoMessage(1, 40000); val parts = split(video, 1, m)
            assertTrue("Fixture", parts.size == 35 && Udp.parityCount(parts.size) == 3)
            for (i in parts.indices) if (i !in 10..12) c.feed(parts[i])
            c.feed(paritiesOf(1, m))
            assertTrue(c.videos() == 1 && c.out.messages[0].bytes.contentEquals(m) && c.receiver.rebuilt == 3L)
        }
        run {  // Two lost in one class: parity cannot tell them apart. The NACK goes, and one repair lets parity bring the other.
            val c = Client(20000, parity = true); val m = videoMessage(1, 40000); val parts = split(video, 1, m)
            for (i in parts.indices) if (i != 4 && i != 7) c.feed(parts[i])
            c.feed(paritiesOf(1, m)); assertEquals(0, c.videos())
            c.at(c.now + 10000); val n = c.taken(Udp.KIND_NACK)
            assertTrue("Both were not asked for", n.size == 1 && n[0][1].toInt() == 2 && Udp.get16(n[0], 6) == 4 && Udp.get16(n[0], 14) == 7)
            c.feed(parts[7])
            assertTrue("The waiting parity did not rebuild the other", c.videos() == 1 && c.out.messages[0].bytes.contentEquals(m) && c.receiver.rebuilt == 1L)
        }
        run {  // A message of one datagram that was lost: its parity alone is the message.
            val c = Client(20000, parity = true); val m = videoMessage(1, 300, true)
            c.feed(paritiesOf(1, m))
            assertTrue(c.videos() == 1 && c.out.messages[0].bytes.contentEquals(m) && c.out.messages[0].key)
        }
        run {  // A parity that overtook its fragment rebuilds it too, and that is no loss: not counted as repaired.
            val c = Client(20000, parity = true); val m = videoMessage(1, 5000); val parts = split(video, 1, m)
            for (i in parts.indices) if (i != 3) c.feed(parts[i])
            c.feed(paritiesOf(1, m)); assertEquals(1, c.videos())
            c.now += 20000; c.feed(parts[3]); c.at(c.now + 200000)
            assertTrue(c.receiver.rebuilt == 1L && c.receiver.repaired == 0L)
        }
        run {  // A parity for a message whose fragments are still on their way: it waits, and no gap is closed twice.
            val c = Client(20000, parity = true); val m = videoMessage(1, 5000); val parts = split(video, 1, m)
            c.feed(paritiesOf(1, m)); assertEquals(0, c.videos())
            for (i in 0 until 4) c.feed(parts[i])
            assertTrue("The last fragment was not rebuilt from the waiting parity", c.videos() == 1 && c.out.messages[0].bytes.contentEquals(m))
        }
        run {  // ABILITIES: at once, then every second, with the bit and the count of repaired fragments.
            val c = Client(20000, parity = true); c.tick()
            var a = c.taken(Udp.KIND_ABILITIES)
            assertTrue(a.size == 1 && a[0].size == Udp.ABILITIES_BYTES && a[0][1].toInt() == 0 && Udp.get16(a[0], 2) == Udp.ABILITY_PARITY && Udp.get32(a[0], 4) == 0L)
            val m = videoMessage(1, 5000, true); val parts = split(video, 1, m)
            for (i in parts.indices) if (i != 1) c.feed(parts[i])
            c.feed(paritiesOf(1, m)); c.clear()
            c.at(c.now + 999999); assertEquals(0, c.controls(Udp.KIND_ABILITIES))
            assertEquals("ABILITIES is a deadline", 2000000L, c.receiver.nextTickUs())
            c.at(c.now + 1); a = c.taken(Udp.KIND_ABILITIES)
            assertTrue("The repaired count did not go to the Host", a.size == 1 && Udp.get32(a[0], 4) == 1L)
            val old = Client(20000); old.tick(); old.at(old.now + 3000000)
            assertEquals("A receiver that takes no parity said it does", 0, old.controls(Udp.KIND_ABILITIES))
        }
        run {  // Malformed parity only ever drops data.
            val m = videoMessage(1, 40000); val parts = split(video, 1, m); val good = paritiesOf(1, m)
            val bad = ArrayList<ByteArray>()
            bad.add(good[0].copyOf().also { it[1] = (video or (5 shl 4)).toByte() })                 // a parity count that is not the message's
            bad.add(good[0].copyOf().also { Udp.put16(it, 6, (3 shl 11) or (Udp.get16(it, 6) and 2047)) })  // a slot past the count
            bad.add(good[0].copyOf().also { Udp.put16(it, 6, 0) })                                    // a last fragment of no bytes
            bad.add(good[0].copyOf().also { Udp.put16(it, 6, Udp.FRAGMENT_PAYLOAD + 1) })             // or of too many
            bad.add(good[0].copyOf().also { it[1] = (Udp.CLASS_AUDIO or (2 shl 4)).toByte() })        // sound has no parity
            bad.add(good[0].copyOf().also { it[1] = (2 or (2 shl 4)).toByte() })                      // a recovery frame is never sent to this client
            bad.add(good[0].copyOf().also { Udp.put32(it, 2, 0) })                                    // no message 0
            bad.add(good[0].copyOf(good[0].size - 1))                                                 // short
            bad.add(good[0].copyOf().also { Udp.put16(it, 8, 0) })                                    // no fragments
            val c = Client(20000, parity = true)
            for (i in parts.indices) if (i != 5) c.feed(parts[i])
            for (p in bad) c.feed(p)
            assertTrue("A malformed parity was used", c.videos() == 0 && c.receiver.rebuilt == 0L)
            // A parity whose fragment count is not the started message's is ignored as well.
            val other = videoMessage(1, 30000); c.feed(paritiesOf(1, other)); assertEquals(0, c.videos())
            c.feed(good); assertTrue(c.videos() == 1 && c.out.messages[0].bytes.contentEquals(m))
        }
    }

    // Loss, reordering and duplicates with parity behind every message: what comes out is
    // exactly what went in, in order, and parity saves requests.
    @Test fun parityFuzz() {
        val random = Random(0x70617231)
        val c = Client(4000, parity = true); val sent = ArrayList<ByteArray>(); val wire = ArrayList<ByteArray>()
        var nextId = 1L; var pts = 1L; var delivered = 0; var lastPts = 0L
        for (n in 0 until 40000) {
            if (wire.size < 8) {
                val key = random.nextInt(50) == 0
                val m = videoMessage(pts, random.nextInt(30000) + 10, key, 1, pts.toInt()); ++pts
                sent.add(m); wire.addAll(split(video, nextId, m)); wire.addAll(paritiesOf(nextId, m)); ++nextId
            }
            val at = random.nextInt(minOf(wire.size, 4)); val f = wire[at]
            if (random.nextInt(30) != 0) wire.removeAt(at)
            if (random.nextInt(25) != 0) c.feed(f)
            c.now += 300; c.tick()
            for (m in c.out.messages) {
                assertTrue("A delivered message is not an input message, or is out of order",
                    m.video && m.pts > lastPts && m.pts <= sent.size && m.bytes.contentEquals(sent[(m.pts - 1).toInt()]))
                lastPts = m.pts; ++delivered
            }
            c.clear()
            if (n % 512 == 0) assertTrue(c.receiver.windowMessages() <= Udp.VIDEO_WINDOW && c.receiver.heldBytes() <= Udp.MAX_VIDEO_BYTES)
        }
        assertTrue("The fixture delivered too little: $delivered", delivered > 300)
        assertTrue("Parity rebuilt nothing: ${c.receiver.rebuilt}", c.receiver.rebuilt > 100)
    }

    // quick-nack-v1, as apps/windows_host/src/udp_media_test.cpp (quickNack) checks the C++ one.
    @Test fun quickNack() {
        run {  // The buckets of a run's length.
            val lengths = longArrayOf(1, 2, 3, 4, 5, 16, 17, 64, 65, 100000); val buckets = intArrayOf(0, 1, 2, 2, 3, 3, 4, 4, 5, 5)
            for (n in lengths.indices) assertEquals("A run's bucket", buckets[n], UdpSequenceTrack.bucket(lengths[n]))
        }
        run {  // In order: nothing counted. A jump is a run; what comes after all is late by the time since the jump.
            val t = UdpSequenceTrack(); var now = 1000000L
            for (q in 40L until 50L) { now += 100; assertEquals(0L, t.arrived(q, now)) }
            assertFalse("Ten datagrams were enough to know the path", t.known())
            now += 100; assertTrue("A run of three was not counted", t.arrived(53, now) == 0L && t.runs[2] == 1L)
            val jump = now
            assertTrue("A datagram that came after all was not late by the time since its jump", t.arrived(51, jump + 3000) == 3000L && t.late == 1L && t.lateMaxUs == 3000L)
            assertEquals(3000L, t.lateUs(jump + 3000))
            assertTrue("A run of one was not counted", t.arrived(54, jump + 3100) == 0L && t.arrived(56, jump + 3200) == 0L && t.runs[0] == 1L)
            assertTrue("The longest lateness was not kept", t.arrived(55, jump + 9000) == 5800L && t.lateMaxUs == 5800L && t.lateUs(jump + 9000) == 5800L)
            assertEquals("The window before was forgotten at once", 5800L, t.lateUs(1000000 + Udp.SEQUENCE_WINDOW_US + 5))
            assertEquals("An old lateness was kept for good", 0L, t.lateUs(1000000 + 2 * Udp.SEQUENCE_WINDOW_US + 10))
            assertTrue("The counts are not the leg's lifetime", t.lateMaxUs == 5800L && t.late == 2L)
        }
        run {  // A jump older than the ones remembered: later than any NACK should wait for.
            val t = UdpSequenceTrack(); var now = 1000000L; var q = 10L; t.arrived(q, now)
            for (n in 0..Udp.SEQUENCE_JUMPS) { q += 2; now += 100; t.arrived(q, now) }
            assertEquals("A datagram of a forgotten jump was given a short lateness", Udp.SEQUENCE_LATE_UNKNOWN_US, t.arrived(11, now + 100))
            assertEquals("The newest jump was forgotten", 200L, t.arrived(q - 1, now + 200))
        }
        run {  // Known after SEQUENCE_KNOWN_AFTER datagrams in order.
            val t = UdpSequenceTrack(); for (q in 1L..Udp.SEQUENCE_KNOWN_AFTER) t.arrived(q, 1000000 + q)
            assertFalse("Known one datagram early", t.known()); t.arrived(Udp.SEQUENCE_KNOWN_AFTER + 1L, 2000000); assertTrue(t.known())
        }
        fun firstNack(c: Client): Long {  // microseconds from a gap to its NACK
            val parts = split(video, 1, videoMessage(1, 5000)); c.feed(parts[0]); c.feed(parts[2]); val gap = c.now
            var at = 0L
            while (at <= 40000) { c.at(gap + at); if (c.controls(Udp.KIND_NACK) != 0) return at; at += 100 }
            return -1
        }
        run { val c = Client(4000); assertTrue("A receiver that was told nothing did not wait 10 ms", firstNack(c) == 10000L && c.receiver.nackDelayUs() == 10000L) }
        run { val c = Client(4000); c.receiver.setReorder(0); assertEquals("A path that reorders nothing did not ask after 2 ms", 2000L, firstNack(c)) }
        run { val c = Client(4000); c.receiver.setReorder(3000); assertEquals("Half again the lateness and a millisecond", 5500L, firstNack(c)) }
        run { val c = Client(4000); c.receiver.setReorder(400); assertEquals("The quick delay has no floor", 2000L, firstNack(c)) }
        run { val c = Client(4000); c.receiver.setReorder(50000); assertEquals("A path that reorders badly waited longer than before", 10000L, firstNack(c)) }
        run { val c = Client(120000); c.receiver.setReorder(50000); assertEquals("A long path waited longer than a quarter of its round trip", 30000L, firstNack(c)) }
        run { val c = Client(120000); c.receiver.setReorder(0); assertEquals(2000L, firstNack(c)) }
        run {  // The deadline the leg sleeps to follows the quick wait.
            val c = Client(4000); c.receiver.setReorder(0); val parts = split(video, 1, videoMessage(1, 5000)); c.feed(parts[0]); c.feed(parts[2])
            assertEquals(c.now + 2000, c.receiver.nextTickUs())
        }
        run {  // Asked for, then rebuilt from a parity that came after the NACK: it was lost, and the repair that follows does not undo the count.
            val c = Client(4000, parity = true); c.receiver.setReorder(0); val m = videoMessage(1, 5000, true); val parts = split(video, 1, m)
            c.feed(parts[0]); c.feed(parts[2]); c.feed(parts[3]); c.at(c.now + 2000); assertEquals("Fixture: no NACK", 1, c.controls(Udp.KIND_NACK))
            c.feed(parts[4]); c.feed(paritiesOf(1, m)); assertTrue(c.videos() == 1 && c.out.messages[0].bytes.contentEquals(m))
            assertEquals("A fragment asked for and then rebuilt was not counted as repaired at once", 1L, c.receiver.repaired)
            c.now += 8000; c.feed(parts[1]); c.at(c.now + 200000); assertTrue("Its repair changed the count", c.receiver.repaired == 1L && c.receiver.rebuilt == 1L)
        }
        run {  // Not asked for: its own datagram within 50 ms still means it was only out of order.
            val c = Client(4000, parity = true); c.receiver.setReorder(0); val m = videoMessage(1, 5000, true); val parts = split(video, 1, m)
            c.feed(parts[0]); c.feed(parts[2]); c.feed(parts[3]); c.feed(parts[4]); c.feed(paritiesOf(1, m)); assertEquals(1, c.videos())
            c.now += 1000; c.feed(parts[1]); c.at(c.now + 200000); assertTrue(c.receiver.repaired == 0L && c.controls(Udp.KIND_NACK) == 0)
        }
    }
}
