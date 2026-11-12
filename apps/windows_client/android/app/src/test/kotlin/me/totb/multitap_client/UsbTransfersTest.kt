package me.totb.multitap_client

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

// UsbPump over a fake connection that behaves as android.hardware.usb does: byte counts, never
// a status. The pump is not in the app (see UsbTransfers.kt); this is its state machine only.
// No USB device, UsbManager or Host was involved.
class UsbTransfersTest {
    private class FakeLink(override var maxRequestBytes: Int = 16384) : UsbLink {
        override var present = true
        /** Requests at the device, oldest first: (endpoint, buffer, length, tag). */
        val flying = ArrayList<Array<Any>>()
        val cancelled = ArrayList<Any>()
        val calls = ArrayList<String>()
        var halted = HashSet<Int>()
        var submitFails = false
        var controlAnswer: (Int, Int, Int, Int, ByteArray, Int, Int) -> Int = { _, _, _, _, _, length, _ -> length }
        var interfaceOk = true
        var clock = 0L

        override fun control(requestType: Int, request: Int, value: Int, index: Int, data: ByteArray, length: Int, timeoutMs: Int): Int {
            if (requestType == 0x82 && request == 0) {
                calls += "status:$index"
                if (!present) return -1
                data[0] = if (index in halted) 1 else 0
                data[1] = 0
                return 2
            }
            calls += "control:%02x:%d:%d:%d".format(requestType, request, value, index)
            return controlAnswer(requestType, request, value, index, data, length, timeoutMs)
        }

        override fun submit(endpoint: Int, data: ByteArray, length: Int, tag: Any): Boolean {
            if (submitFails) return false
            flying += arrayOf(endpoint, data, length, tag)
            return true
        }

        override fun cancel(tag: Any): Boolean { cancelled += tag; return true }
        override fun setConfiguration(value: Int): Boolean { calls += "config:$value"; return interfaceOk }
        override fun setInterface(number: Int, alternate: Int): Boolean { calls += "interface:$number:$alternate"; return interfaceOk }

        /** The oldest request on [endpoint] ends with [count] bytes ([fill] for an IN). */
        fun end(pump: UsbPump, endpoint: Int, count: Int, fill: Byte = 0) {
            val request = flying.first { it[0] == endpoint }
            flying.remove(request)
            val data = request[1] as ByteArray
            if (endpoint and 0x80 != 0) for (i in 0 until maxOf(count, 0)) data[i] = fill
            pump.completed(request[3], count)
        }

        fun asked(endpoint: Int) = flying.filter { it[0] == endpoint }.map { it[2] as Int }
    }

    private val bulkIn = UsbEndpointInfo(0x81, 2, 512, interfaceNumber = 0, alternate = 0)
    private val bulkOut = UsbEndpointInfo(0x02, 2, 512, interfaceNumber = 0, alternate = 0)
    private val interruptIn = UsbEndpointInfo(0x83, 3, 8, interfaceNumber = 1, alternate = 0)
    private val endpoints = listOf(bulkIn, bulkOut, interruptIn)

    private fun setup(type: Int, request: Int, value: Int, index: Int, length: Int) = byteArrayOf(
        type.toByte(), request.toByte(), value.toByte(), (value shr 8).toByte(), index.toByte(), (index shr 8).toByte(), length.toByte(), (length shr 8).toByte())

    @Test fun aLargeReadGoesInPiecesTheApiTakesAndEndsAtAShortOne() {
        val link = FakeLink()
        val pump = UsbPump(link, endpoints)
        val results = ArrayList<UsbResult>()
        pump.submit(7, 0x81, 40000, null, results::add)
        assertEquals(listOf(16384), link.asked(0x81))
        link.end(pump, 0x81, 16384, fill = 1)
        assertEquals(listOf(16384), link.asked(0x81))
        link.end(pump, 0x81, 16384, fill = 2)
        // The rest of what was asked.
        assertEquals(listOf(40000 - 32768), link.asked(0x81))
        link.end(pump, 0x81, 1000, fill = 3)
        val result = results.single()
        assertEquals(usbStatusOk, result.status)
        assertEquals(33768, result.data.size)
        assertEquals(1, result.data[0].toInt())
        assertEquals(2, result.data[16384].toInt())
        assertEquals(3, result.data[33767].toInt())
        assertTrue(link.flying.isEmpty())
        assertEquals(1L, pump.urbs)
        assertEquals(33768L, pump.bytes)
        // From Android 9 one request takes it whole.
        val whole = FakeLink(maxRequestBytes = Int.MAX_VALUE)
        UsbPump(whole, endpoints).submit(1, 0x81, 262144, null) {}
        assertEquals(listOf(262144), whole.asked(0x81))
    }

    @Test fun aWriteGoesOutWholeInOrderOrSaysWhy() {
        val link = FakeLink()
        val pump = UsbPump(link, endpoints)
        val results = ArrayList<UsbResult>()
        val data = ByteArray(20000) { it.toByte() }
        pump.submit(1, 0x02, 20000, data, results::add)
        assertArrayEquals(data.copyOfRange(0, 16384), link.flying.single()[1] as ByteArray)
        link.end(pump, 0x02, 16384)
        assertArrayEquals(data.copyOfRange(16384, 20000), link.flying.single()[1] as ByteArray)
        link.end(pump, 0x02, 3616)
        assertEquals(usbStatusOk, results.single().status)
        assertEquals(0, results.single().data.size)

        // Less went out than asked and the endpoint says it is halted: a stall.
        link.halted.add(0x02)
        pump.submit(2, 0x02, 100, ByteArray(100), results::add)
        link.end(pump, 0x02, 0)
        assertEquals(usbStatusStall, results.last().status)
        assertEquals(1, pump.stalls)
        // Not halted, still there: a protocol error, never a success.
        link.halted.clear()
        pump.submit(3, 0x02, 100, ByteArray(100), results::add)
        link.end(pump, 0x02, 40)
        assertEquals(usbStatusProtocol, results.last().status)
    }

    @Test fun aReadOfNothingIsAskedAboutBecauseTheApiDoesNotSay() {
        val link = FakeLink()
        val pump = UsbPump(link, endpoints)
        val results = ArrayList<UsbResult>()
        // A healthy endpoint: a zero-length packet.
        pump.submit(1, 0x81, 512, null, results::add)
        link.end(pump, 0x81, 0)
        assertEquals(usbStatusOk, results.last().status)
        assertEquals(0, results.last().data.size)
        assertEquals("status:129", link.calls.last())
        // A halted one: the stall the desktop's driver must hear of.
        link.halted.add(0x81)
        pump.submit(2, 0x81, 512, null, results::add)
        link.end(pump, 0x81, -1)
        assertEquals(usbStatusStall, results.last().status)
        // The device left: gone, without asking it anything.
        link.present = false
        val asked = link.calls.size
        pump.submit(3, 0x81, 512, null, results::add)
        link.end(pump, 0x81, 0)
        assertEquals(usbStatusNoDevice, results.last().status)
        assertEquals(asked, link.calls.size)
    }

    @Test fun requestsOfOneEndpointRunInOrderAndEndpointsSideBySide() {
        val link = FakeLink()
        val pump = UsbPump(link, endpoints)
        val order = ArrayList<Int>()
        pump.submit(1, 0x83, 8, null) { order += 1 }
        pump.submit(2, 0x83, 8, null) { order += 2 }
        pump.submit(3, 0x81, 512, null) { order += 3 }
        // One at the device per endpoint; the second interrupt read waits.
        assertEquals(listOf(8), link.asked(0x83))
        assertEquals(listOf(512), link.asked(0x81))
        link.end(pump, 0x81, 512)
        assertEquals(listOf(3), order)
        link.end(pump, 0x83, 8)
        assertEquals(listOf(8), link.asked(0x83))
        link.end(pump, 0x83, 4)
        assertEquals(listOf(3, 1, 2), order)
    }

    @Test fun cancellingStopsWhatWaitsAndEndsWhatIsAtTheDevice() {
        val link = FakeLink()
        val pump = UsbPump(link, endpoints)
        val results = HashMap<Int, UsbResult>()
        pump.submit(1, 0x83, 8, null) { results[1] = it }
        pump.submit(2, 0x83, 8, null) { results[2] = it }
        // Waiting: it never reaches the device.
        assertTrue(pump.cancel(2))
        assertEquals(usbStatusReset, results[2]!!.status)
        assertTrue(link.cancelled.isEmpty())
        // At the device: cancelled there, and its end is the answer.
        assertTrue(pump.cancel(1))
        assertEquals(1, link.cancelled.size)
        assertFalse(results.containsKey(1))
        link.end(pump, 0x83, 0)
        assertEquals(usbStatusReset, results[1]!!.status)
        assertEquals(0, results[1]!!.data.size)
        assertTrue(link.flying.isEmpty())
        // Unknown or already answered: nothing to cancel.
        assertFalse(pump.cancel(1))
        assertFalse(pump.cancel(99))
        // A long read cancelled between two pieces asks for no further piece.
        pump.submit(5, 0x81, 40000, null) { results[5] = it }
        assertTrue(pump.cancel(5))
        link.end(pump, 0x81, 16384)
        assertEquals(usbStatusReset, results[5]!!.status)
        assertTrue(link.flying.isEmpty())
        // The next request of that endpoint runs.
        pump.submit(6, 0x81, 512, null) { results[6] = it }
        link.end(pump, 0x81, 512)
        assertEquals(usbStatusOk, results[6]!!.status)
    }

    @Test fun aDeviceThatLeavesEndsEverythingAndStartsNothing() {
        val link = FakeLink()
        val pump = UsbPump(link, endpoints)
        val statuses = ArrayList<Int>()
        pump.submit(1, 0x81, 512, null) { statuses += it.status }
        pump.submit(2, 0x81, 512, null) { statuses += it.status }
        pump.submit(3, 0x02, 4, ByteArray(4)) { statuses += it.status }
        link.present = false
        pump.gone()
        assertEquals(listOf(usbStatusNoDevice, usbStatusNoDevice, usbStatusNoDevice), statuses)
        pump.submit(4, 0x81, 512, null) { statuses += it.status }
        assertEquals(usbStatusNoDevice, statuses.last())
        assertEquals(usbStatusNoDevice, pump.control(setup(0x80, 6, 0x100, 0, 18)).status)
        // A late end of what was at the device changes nothing.
        link.end(pump, 0x81, 512)
        assertEquals(4, statuses.size)
    }

    @Test fun whatIsNotAnEndpointOfTheDeviceNeverReachesIt() {
        val link = FakeLink()
        val pump = UsbPump(link, endpoints)
        val statuses = ArrayList<Int>()
        pump.submit(1, 0x85, 8, null) { statuses += it.status }       // no such endpoint
        pump.submit(2, 0x81, 8, ByteArray(8)) { statuses += it.status } // a write to an IN endpoint
        pump.submit(3, 0x02, 8, null) { statuses += it.status }       // a read from an OUT endpoint
        pump.submit(4, 0x02, 8, ByteArray(4)) { statuses += it.status } // fewer bytes than said
        assertEquals(List(4) { usbStatusProtocol }, statuses)
        assertTrue(link.flying.isEmpty())
        link.submitFails = true
        pump.submit(5, 0x81, 8, null) { statuses += it.status }
        assertEquals(usbStatusIo, statuses.last())
    }

    @Test fun controlRequestsAnswerWithDataOrAnInferredReason() {
        val link = FakeLink()
        var time = 0L
        val pump = UsbPump(link, endpoints, now = { time }, controlTimeoutMs = 5000)
        link.controlAnswer = { _, _, _, _, data, _, _ -> data[0] = 18; data[1] = 1; 18 }
        val descriptor = pump.control(setup(0x80, 6, 0x0100, 0, 64))
        assertEquals(usbStatusOk, descriptor.status)
        assertEquals(18, descriptor.data.size)
        assertEquals(18, descriptor.data[0].toInt())
        assertEquals("control:80:6:256:0", link.calls.last())
        // Out data goes as given.
        var sent = ByteArray(0)
        link.controlAnswer = { _, _, _, _, data, length, _ -> sent = data.copyOf(length); length }
        assertEquals(usbStatusOk, pump.control(setup(0x21, 9, 0x200, 0, 3), byteArrayOf(1, 2, 3)).status)
        assertArrayEquals(byteArrayOf(1, 2, 3), sent)
        assertEquals(usbStatusProtocol, pump.control(setup(0x21, 9, 0x200, 0, 3), byteArrayOf(1)).status)
        // A failure at once is the device refusing: a stall.
        link.controlAnswer = { _, _, _, _, _, _, _ -> -1 }
        assertEquals(usbStatusStall, pump.control(setup(0x80, 6, 0x0600, 0, 10)).status)
        // A failure after the whole timeout is a timeout.
        link.controlAnswer = { _, _, _, _, _, _, timeout -> time += timeout; -1 }
        assertEquals(usbStatusTimeout, pump.control(setup(0x80, 6, 0x0100, 0, 18)).status)
        // A device that is gone is gone.
        link.controlAnswer = { _, _, _, _, _, _, _ -> link.present = false; -1 }
        assertEquals(usbStatusNoDevice, pump.control(setup(0x80, 6, 0x0100, 0, 18)).status)
        assertEquals(1, pump.stalls)
        assertEquals(1, pump.timeouts)
        assertEquals(usbStatusProtocol, pump.control(ByteArray(7)).status)
    }

    @Test fun configurationAndInterfaceGoThroughTheApiNotTheWire() {
        val link = FakeLink()
        val pump = UsbPump(link, endpoints)
        assertEquals(usbStatusOk, pump.control(setup(0x00, 9, 1, 0, 0)).status)
        assertEquals("config:1", link.calls.single())
        assertEquals(usbStatusOk, pump.control(setup(0x01, 11, 2, 1, 0)).status)
        assertEquals("interface:1:2", link.calls.last())
        assertFalse(link.calls.any { it.startsWith("control:") })
        link.interfaceOk = false
        assertEquals(usbStatusStall, pump.control(setup(0x00, 9, 3, 0, 0)).status)
        assertEquals(usbStatusStall, pump.control(setup(0x01, 11, 0, 1, 0)).status)
    }

    @Test fun clearingAHaltResetsBothSidesWhenNothingElseRunsOnThatInterface() {
        val link = FakeLink()
        val pump = UsbPump(link, endpoints)
        // CLEAR_FEATURE(ENDPOINT_HALT) of the bulk IN endpoint, interface 0 idle.
        assertEquals(usbStatusOk, pump.control(setup(0x02, 1, 0, 0x81, 0)).status)
        assertEquals(listOf("interface:0:0"), link.calls)
        // Another endpoint of that interface is busy: its request must not be cut off, so only
        // the device is told (the host side keeps its data toggle: the gap this file states).
        link.calls.clear()
        pump.submit(1, 0x02, 4, ByteArray(4)) {}
        assertEquals(usbStatusOk, pump.control(setup(0x02, 1, 0, 0x81, 0)).status)
        assertEquals(listOf("control:02:1:0:129"), link.calls)
        // A request on another interface does not matter.
        link.end(pump, 0x02, 4)
        link.calls.clear()
        pump.submit(2, 0x83, 8, null) {}
        assertEquals(usbStatusOk, pump.control(setup(0x02, 1, 0, 0x81, 0)).status)
        assertEquals(listOf("interface:0:0"), link.calls)
        // The API refuses: the device is told at least.
        link.calls.clear()
        link.interfaceOk = false
        assertEquals(usbStatusOk, pump.control(setup(0x02, 1, 0, 0x02, 0)).status)
        assertEquals(listOf("interface:0:0", "control:02:1:0:2"), link.calls)
    }
}
