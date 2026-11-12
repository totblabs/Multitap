package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

// `multitap/udp` as lib/data/udp_probe.dart NativeUdpLeg calls it and reads its answer,
// keyed as windows/runner/udp_media_channel.h writes it; and media-wire-v1's path bit.
class UdpChannelTest {
    private fun args(vararg changes: Pair<String, Any?>): Map<String, Any?> {
        val value = hashMapOf<String, Any?>("leg" to 1, "relays" to arrayListOf(hashMapOf("host" to "relay.example", "port" to 443)),
            "id" to ByteArray(8), "ticket" to ByteArray(32), "sessionId" to "s-1", "clockUs" to 5L)
        for ((key, change) in changes) if (change == null) value.remove(key) else value[key] = change
        return value
    }

    @Test fun grant() {
        val grant = udpGrant(args())!!
        assertEquals(listOf("relay.example" to 443), grant.relays)
        assertEquals("s-1", grant.sessionId)
        assertNotNull(udpGrant(args("relays" to arrayListOf(hashMapOf("host" to "a", "port" to 1L), hashMapOf("host" to "b", "port" to 65535)))))
        for (bad in listOf("relays" to null, "relays" to arrayListOf<Any>(), "relays" to arrayListOf(hashMapOf("host" to "a", "port" to 0)),
            "relays" to arrayListOf(hashMapOf("host" to "a", "port" to 65536)), "relays" to arrayListOf(hashMapOf("host" to "a", "port" to 443.0)),
            "relays" to arrayListOf(hashMapOf("host" to 1, "port" to 443)), "relays" to arrayListOf("a:443"),
            "id" to ByteArray(7), "id" to null, "ticket" to ByteArray(31), "ticket" to arrayListOf(1), "sessionId" to "", "sessionId" to null)) {
            assertNull("accepted ${bad.first}=${bad.second}", udpGrant(args(bad)))
        }
        assertEquals(5L, udpInteger(5)); assertEquals(5L, udpInteger(5L)); assertNull(udpInteger(5.0)); assertNull(udpInteger("5"))
    }

    @Test fun pollAnswer() {
        val open = udpReadingValue(UdpReading(UdpProbe.Report(), true, false, null, emptyList(), 0, 0, UdpPathGate.View(0, 0, 0)))
        assertEquals(emptyMap<String, Long>(), open["report"])
        assertEquals(setOf("report", "running", "arrivals", "audioBytes", "keyedStream"), open.keys)
        val report = UdpProbe.Report(UdpProbe.PROBE_REACHABLE, 1, 12, 30, 4, 3, 1)
        val value = udpReadingValue(UdpReading(report, false, true, ByteArray(32) { 7 }, listOf(100L, 5000000L, 4000L), 1920, 2, UdpPathGate.View(1, 3, 9)))
        assertEquals(mapOf("udpProbe" to 1L, "udpRelayIndex" to 1L, "udpRttP50Ms" to 12L, "udpRttP95Ms" to 30L, "udpLossPct" to 4L, "udpKeepSent" to 3L,
            "udpKeepLost" to 1L, "udpActivePath" to 1L, "udpFallbacks" to 3L, "udpGaps" to 2L, "udpKeyRequests" to 0L, "udpStalls" to 0L), value["report"])
        // The receiver's stall watch: the cause only once a stall was seen.
        val stalled = udpReadingValue(UdpReading(report, true, true, null, emptyList(), 0, 0, UdpPathGate.View(1, 1, 0), keyRequests = 4, stalls = 2, stallCause = 3))
        val fields = stalled["report"] as Map<*, *>
        assertTrue(fields["udpKeyRequests"] == 4L && fields["udpStalls"] == 2L && fields["udpStallCause"] == 3L)
        assertEquals(false, value["running"])
        // A plain list of ints, never a typed array.
        assertTrue(value["arrivals"] is ArrayList<*> && value["arrivals"] == listOf(100L, 5000000L, 4000L))
        assertEquals(1920L, value["audioBytes"]); assertEquals(9L, value["keyedStream"])
        assertTrue((value["publicKey"] as ByteArray).all { it.toInt() == 7 })
        // No PONG: a loss figure without RTTs; no relay before the ACK.
        val noPong = udpReadingValue(UdpReading(UdpProbe.Report(UdpProbe.PROBE_BIND_TIMEOUT), true, false, null, emptyList(), 0, 0, UdpPathGate.View(0, 0, 0)))
        assertEquals(mapOf("udpProbe" to 4L), noPong["report"])
    }

    @Test fun wirePathFlag() {
        fun wire(flags: Int, kind: Int): ByteBuffer {
            val buffer = ByteBuffer.allocate(16 + 20).order(ByteOrder.LITTLE_ENDIAN)
            buffer.put(1).put(flags.toByte()).put(ByteArray(6)).putLong(7).put(kind.toByte()).put(ByteArray(19))
            return buffer.position(0) as ByteBuffer
        }
        assertTrue(androidMediaWire(wire(0, 1))!!.let { !it.path && !it.watch })
        assertTrue(androidMediaWire(wire(2, 1))!!.let { it.path && !it.watch })
        assertTrue(androidMediaWire(wire(3, 1))!!.let { it.path && it.watch })
        assertTrue(androidMediaWire(wire(2, 2))!!.path)
        assertNull(androidMediaWire(wire(4, 1)))
        assertNull(androidMediaWire(wire(3, 2)))
    }
}
