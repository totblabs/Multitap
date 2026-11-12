package me.totb.multitap_client

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

// GameMouse.kt without a device, a VM or real input: the cases of
// test/native/game_mouse_usb_test.cpp (relay frames, read-ahead and older
// Hosts, unlinks, pacing, joining) with Android's captured mouse in place of
// Raw Input, and the injected path's moveRelative, button and wheel messages.
class GameMouseTest {
    private val PRIMARY = 1
    private val SECONDARY = 2
    private val TERTIARY = 4
    private val BACK = 8
    private val FORWARD = 16

    private fun r(buttons: Int, dx: Int, dy: Int, wheel: Int = 0, pan: Int = 0) = GameMouseReports.make(buttons, dx, dy, wheel, pan)
    private fun submit(device: Int, seq: Int, endpoint: Int = GameMouseFrame.ENDPOINT, transfer: Int = GameMouseFrame.INTERRUPT, length: Int = 7) =
        GameMouseFrame(GameMouseFrame.SUBMIT, device, endpoint, transfer, seq, length).encode()
    private fun control(device: Int, seq: Int) =
        GameMouseFrame(GameMouseFrame.SUBMIT, device, 0x80, 0, seq, 255, 0, byteArrayOf(0x80.toByte(), 6, 2, 3, 9, 4, 255.toByte(), 0)).encode()
    private fun unlink(device: Int, seq: Int, target: Int) = GameMouseFrame(GameMouseFrame.UNLINK, device, seq = seq, length = target).encode()

    private class Sent {
        val frames = ArrayList<GameMouseFrame>()
        val send: (ByteArray) -> Unit = { frames += GameMouseFrame.decode(it)!! }
    }

    private fun assertReports(expected: List<ByteArray>, actual: List<ByteArray>) {
        assertEquals(expected.map { it.toList() }, actual.map { it.toList() })
    }

    @Test fun frames() {
        val frame = GameMouseFrame(GameMouseFrame.RETURN, 3, 0x81, 3, 0x01020304, 7, -104, ByteArray(8), byteArrayOf(1, 2, 3, 4, 5, 6, 7))
        val bytes = frame.encode()
        assertEquals(31, bytes.size)
        assertEquals(0x04.toByte(), bytes[4]); assertEquals(0x01.toByte(), bytes[7])
        assertEquals(0x98.toByte(), bytes[12]); assertEquals(0xff.toByte(), bytes[15])
        val back = GameMouseFrame.decode(bytes)!!
        assertEquals(0x01020304, back.seq); assertEquals(-104, back.status); assertEquals(0x81, back.endpoint)
        assertArrayEquals(frame.payload, back.payload)
        assertNull("a short frame decoded", GameMouseFrame.decode(bytes.copyOf(23)))
    }

    @Test fun reports() {
        assertArrayEquals(byteArrayOf(1, 3, 0, -2, -1, 0, 0), r(1, 3, -2))
        assertEquals(-2, GameMouseReports.y(r(0, 0, -2))); assertEquals(-32767, GameMouseReports.x(r(0, -32767, 0)))
        // Motion beyond a report is split, nothing lost.
        val big = GameMouseReports.split(0, 40000, -70000, 0, 0)
        assertEquals(3, big.size)
        assertEquals(40000, big.sumOf { GameMouseReports.x(it) }); assertEquals(-70000, big.sumOf { GameMouseReports.y(it) })
        // Joining: same buttons sum, a button change or an overflow never joins.
        assertArrayEquals(r(1, 3, 3, 1), GameMouseReports.join(r(1, 1, 1), r(1, 2, 2, 1)))
        assertNull(GameMouseReports.join(r(1, 1, 1), r(0, 1, 1)))
        assertNull(GameMouseReports.join(r(0, 32000, 0), r(0, 1000, 0)))
    }

    @Test fun mouseState() {
        // MotionEvent's button bits are the report's, in order.
        val state = GameMouseState()
        assertReports(listOf(r(1, 3, -2)), state.input(PRIMARY, 3f, -2f))
        assertReports(listOf(r(3, 0, 0)), state.input(PRIMARY or SECONDARY, 0f, 0f))
        assertReports(listOf(r(2, 0, 0)), state.input(SECONDARY, 0f, 0f))
        assertReports(listOf(r(2 or 4 or 8 or 16, 0, 0)), state.input(SECONDARY or TERTIARY or BACK or FORWARD, 0f, 0f))
        assertTrue("nothing changed but a report was made", state.input(SECONDARY or TERTIARY or BACK or FORWARD, 0f, 0f).isEmpty())
        // Stylus buttons are not mouse buttons.
        assertTrue(GameMouseState().input(32 or 64, 0f, 0f).isEmpty())
        // The click that handed the mouse over: held when capture began, never sent, its release neither.
        val handed = GameMouseState(PRIMARY)
        assertTrue("the hand-over press was sent", handed.input(PRIMARY, 0f, 0f).isEmpty())
        assertReports(listOf(r(0, 4, 0)), handed.input(PRIMARY, 4f, 0f))
        assertTrue("the hand-over release was sent", handed.input(0, 0f, 0f).isEmpty())
        assertReports(listOf(r(1, 0, 0)), handed.input(PRIMARY, 0f, 0f))
        // Fractional motion keeps its remainder.
        val fine = GameMouseState()
        assertTrue(fine.input(0, .5f, -.4f).isEmpty())
        assertReports(listOf(r(0, 1, 0)), fine.input(0, .5f, -.4f))
        assertReports(listOf(r(0, 0, -1)), fine.input(0, 0f, -.4f))
        // The wheel: 1.0 is one notch, finer wheels keep their remainder; the horizontal wheel the same.
        val wheel = GameMouseState()
        assertTrue("half a notch was sent", wheel.input(0, 0f, 0f, .5f).isEmpty())
        assertReports(listOf(r(0, 0, 0, 1)), wheel.input(0, 0f, 0f, .5f))
        assertReports(listOf(r(0, 0, 0, -2)), wheel.input(0, 0f, 0f, -2f))
        assertReports(listOf(r(0, 0, 0, 0, 1)), wheel.input(0, 0f, 0f, 0f, 1f))
        assertTrue(wheel.input(0, Float.NaN, 0f).isEmpty())
        // Release all lets go once.
        val held = GameMouseState()
        held.input(TERTIARY, 0f, 0f)
        assertArrayEquals(r(0, 0, 0), held.releaseAll())
        assertNull(held.releaseAll())
    }

    @Test fun readAheadHost() {
        val sent = Sent()
        val device = GameMouseDevice(7, sent.send)
        assertFalse(device.takesInput(0) || device.confirmed)
        // The Host's read-ahead: reads arrive before anything happens.
        for (seq in 1..8) assertEquals(GameMouseDevice.Seen.OK, device.frame(submit(7, seq), 1000))
        assertTrue(device.confirmed && device.takesInput(1000) && device.heldReads == 8 && sent.frames.isEmpty())
        // A report answers the oldest read at once.
        device.report(r(1, 5, -3), 2000)
        val first = sent.frames.single()
        assertEquals(GameMouseFrame.RETURN, first.type); assertEquals(1, first.seq); assertEquals(0, first.status)
        assertEquals(0x81, first.endpoint); assertEquals(3, first.transfer); assertEquals(7, first.length)
        assertArrayEquals(r(1, 5, -3), first.payload)
        // Within the pace it waits (4 ms until a round trip is known); motion with the same buttons joins, a button change stays apart.
        device.report(r(1, 1, 1), 2300)
        device.report(r(1, 2, 2), 2600)
        device.report(r(0, 0, 0), 2700)
        assertEquals(1, sent.frames.size); assertEquals(2, device.queuedReports)
        assertEquals(6000L, device.wake())
        device.tick(5999)
        assertEquals("a report went out before its time", 1, sent.frames.size)
        device.tick(6000)
        assertArrayEquals(r(1, 3, 3), sent.frames[1].payload); assertEquals(2, sent.frames[1].seq)
        device.tick(10000)
        assertArrayEquals(r(0, 0, 0), sent.frames[2].payload)
        assertNull(device.wake())
        // The Host cancels a read it no longer wants: both answers, in that order.
        sent.frames.clear()
        assertEquals(GameMouseDevice.Seen.OK, device.frame(unlink(7, 100, 5), 5000))
        assertEquals(2, sent.frames.size)
        assertEquals(GameMouseFrame.UNLINK_RETURN, sent.frames[0].type); assertEquals(100, sent.frames[0].seq); assertEquals(GameMouseFrame.RESET, sent.frames[0].status)
        assertEquals(GameMouseFrame.RETURN, sent.frames[1].type); assertEquals(5, sent.frames[1].seq); assertEquals(GameMouseFrame.RESET, sent.frames[1].status)
        assertEquals(0, sent.frames[1].payload.size)
        assertEquals(GameMouseDevice.Seen.OK, device.frame(unlink(7, 101, 5), 5000))
        assertEquals("a cancel of a read already gone was not answered 0", 0, sent.frames[2].status)
        // Every read gone: input still counts as taken for a second, then not.
        for (seq in listOf(4, 6, 7, 8)) device.frame(unlink(7, 200 + seq, seq), 6000)
        assertEquals(0, device.heldReads)
        assertTrue(device.takesInput(6000 + 900000L))
        assertFalse(device.takesInput(1000 + 1000000L + 1))
        // Reports keep their order without reads; a stale one gives its buttons only.
        val quiet = GameMouseDevice(8, sent.send)
        sent.frames.clear()
        quiet.frame(submit(8, 1), 0)
        quiet.report(r(0, 1, 0), 1000) // answered
        quiet.report(r(2, 9, 9), 2000)
        quiet.frame(submit(8, 2), 400000)
        assertEquals(2, sent.frames.size)
        assertArrayEquals("a stale report kept its motion", r(2, 0, 0), sent.frames[1].payload)
        // Malformed or foreign frames.
        assertEquals(GameMouseDevice.Seen.INVALID, quiet.frame(submit(9, 3), 0))
        assertEquals(GameMouseDevice.Seen.INVALID, quiet.frame(submit(8, 3).copyOf(10), 0))
        assertEquals(GameMouseDevice.Seen.OK, quiet.frame(submit(8, 4, length = 4), 0))
        assertEquals(GameMouseFrame.STALL, sent.frames.last().status); assertEquals(0, quiet.heldReads)
        assertEquals(GameMouseDevice.Seen.OK, quiet.frame(submit(8, 5, 0x82, 2, 64), 0))
        assertEquals(GameMouseFrame.STALL, sent.frames.last().status)
        assertFalse("stray requests made the Host look older", quiet.older)
    }

    @Test fun olderHost() {
        // A released Host forwards the VM's control requests: the first one says it does not know the device.
        val sent = Sent()
        val device = GameMouseDevice(3, sent.send)
        assertEquals(GameMouseDevice.Seen.OLDER_HOST, device.frame(control(3, 41), 0))
        val stalled = sent.frames.single()
        assertEquals(41, stalled.seq); assertEquals(GameMouseFrame.STALL, stalled.status); assertEquals(0, stalled.transfer)
        assertEquals(GameMouseDevice.Seen.OLDER_HOST, device.frame(submit(3, 42), 0))
        assertEquals(GameMouseFrame.STALL, sent.frames.last().status)
        assertFalse(device.takesInput(0) || device.confirmed)
        device.report(r(1, 1, 1), 0)
        assertEquals("a report went to an older Host", 2, sent.frames.size)
        // A control request after the reads began is stalled, not a verdict.
        val later = GameMouseDevice(4, sent.send)
        later.frame(submit(4, 1), 0)
        assertEquals(GameMouseDevice.Seen.OK, later.frame(control(4, 2), 0))
        assertTrue(later.takesInput(0))
    }

    @Test fun pacing() {
        // The round trip is learned from the reads coming back after their answers; answers spread over it.
        val sent = Sent()
        val device = GameMouseDevice(1, sent.send)
        var seq = 0
        repeat(8) { device.frame(submit(1, ++seq), 0) }
        assertEquals(GameMouseDevice.FIRST_INTERVAL_US, device.interval())
        var now = 10000L
        repeat(40) {
            device.report(r(0, 1, 0), now)
            device.frame(submit(1, ++seq), now + 40000) // each answer returns as a new read 40 ms later
            now += 5000
        }
        assertTrue("40 ms over 8 reads did not pace at about 5 ms: ${device.interval()}", device.interval() in 4500..5500)
        // A short round trip paces at the one-millisecond floor, never faster.
        val fast = GameMouseDevice(2, sent.send)
        repeat(8) { fast.frame(submit(2, ++seq), 0) }
        var t = 10000L
        while (t < 30000) {
            fast.report(r(0, 1, 0), t)
            fast.frame(submit(2, ++seq), t + 2000)
            t += 125
        }
        assertEquals(GameMouseDevice.MIN_INTERVAL_US, fast.interval())
    }

    @Test fun deltas() {
        val deltas = GameMouseDeltas()
        assertNull(deltas.take(0))
        deltas.add(3, -2)
        assertEquals(3 to -2, deltas.take(1000))
        deltas.add(1, 0)
        assertNull("a second message within 4 ms", deltas.take(4999))
        assertEquals(5000L, deltas.wake())
        assertEquals(1 to 0, deltas.take(5000))
        // Each axis within +-16384; the rest stays for the next.
        deltas.add(20000, -40000)
        assertEquals(16384 to -16384, deltas.take(9000))
        assertEquals(3616 to -16384, deltas.take(9001, force = true))
        assertEquals(0 to -7232, deltas.take(13001))
        assertNull(deltas.wake())
    }

    private class Injected {
        val events = ArrayList<Map<String, Any>>()
        val inject: (Map<String, Any>) -> Unit = { events += it }
        fun short() = events.map {
            when (it["event"]) {
                "moveRelative" -> "m${it["dx"]},${it["dy"]}"
                "button" -> "${it["button"]}${if (it["down"] == true) "v" else "^"}"
                "wheel" -> "w${it["delta"]}"
                else -> "?"
            }
        }
    }

    @Test fun injectedPath() {
        val out = Injected()
        val capture = GameMouseCapture(null, PRIMARY, out.inject)
        assertFalse(capture.usb)
        // The hand-over click is still held: its release never reaches the VM.
        capture.input(PRIMARY, 5f, 0f, 0f, 0f, 1000)
        capture.input(PRIMARY, 2f, 1f, 0f, 0f, 2000) // within 4 ms: waits
        assertEquals(listOf("m5,0"), out.short())
        assertEquals(5000L, capture.wake())
        capture.input(0, 0f, 0f, 0f, 0f, 3000)
        assertEquals(listOf("m5,0"), out.short())
        // A click goes after the movement before it, whatever the pace.
        capture.input(SECONDARY, 1f, 0f, 0f, 0f, 3500)
        assertEquals(listOf("m5,0", "m3,1", "right" + "v"), out.short())
        // The wheel: one notch is 120; the horizontal wheel has no injected form.
        capture.input(SECONDARY, 0f, 0f, -1f, 1f, 9000)
        assertEquals("w-120", out.short().last())
        // Movement after a quiet moment goes at once; within 4 ms it leaves on the timer.
        capture.input(SECONDARY, 4f, 4f, 0f, 0f, 9100)
        capture.input(SECONDARY, 1f, 1f, 0f, 0f, 9200)
        assertEquals("m4,4", out.short().last())
        assertEquals(13100L, capture.wake())
        assertTrue(capture.tick(13099))
        assertEquals("m4,4", out.short().last())
        assertTrue(capture.tick(13100))
        assertEquals("m1,1", out.short().last())
        // The end: pending movement, then every button up.
        capture.input(SECONDARY or BACK, 0f, 0f, 0f, 0f, 13200)
        capture.input(SECONDARY or BACK, 7f, 0f, 0f, 0f, 13300)
        capture.end(13400)
        assertEquals(listOf("x1v", "m7,0", "right^", "x1^"), out.short().takeLast(4))
        // A wheel spin beyond the relay's bound goes in parts.
        val spin = Injected()
        GameMouseCapture(null, 0, spin.inject).input(0, 0f, 0f, 127f, 0f, 0)
        assertEquals(listOf("w12000", "w3240"), spin.short())
    }

    @Test fun usbPath() {
        val sent = Sent()
        val device = GameMouseDevice(5, sent.send)
        for (seq in 1..8) device.frame(submit(5, seq), 0)
        val out = Injected()
        val capture = GameMouseCapture(device, 0, out.inject)
        assertTrue(capture.usb)
        // Everything goes to the virtual mouse; nothing is injected.
        capture.input(PRIMARY, 3f, -1f, 1f, 0f, 1000)
        assertTrue(out.events.isEmpty())
        assertArrayEquals(r(1, 3, -1, 1), sent.frames.single().payload)
        assertTrue(capture.tick(1000))
        // The end lets every button go on the device too.
        capture.end(9000)
        assertArrayEquals(r(0, 0, 0), sent.frames.last().payload)
        // The VM stopped reading for over a second: the tick ends capture.
        for (seq in 1..7) device.frame(unlink(5, 100 + seq, seq + 2), 9000)
        assertFalse(capture.tick(9000 + GameMouseDevice.READING_US))
    }
}
