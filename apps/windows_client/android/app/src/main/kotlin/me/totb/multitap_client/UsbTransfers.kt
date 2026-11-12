package me.totb.multitap_client

// usb-v1 from Android: NOT in the app. This file is the part that is built: how the desktop's
// USB requests (control, bulk, interrupt) would run on a device through Android's USB host API
// (UsbDeviceConnection, UsbRequest), portable and tested against a fake connection
// (UsbTransfersTest). Nothing creates a UsbPump outside tests; no device is enumerated, asked
// for or claimed; the manifest declares nothing. Why it stops here is docs/usb-redirection.md
// "Android": that API reports a byte count and never why a transfer failed, has no way to clear
// a halted endpoint on the host side, and cannot give a device back to Android's own drivers.
// What stands in for the missing status below is inference, good enough to test the state
// machine and not good enough to hand a person's USB stick to.
//
// What the API gives ([UsbLink]):
//  * controlTransfer: blocking, a byte count or a negative number;
//  * UsbRequest.queue / requestWait / cancel on bulk and interrupt endpoints: a byte count,
//    at most 16384 bytes a request before Android 9; no isochronous transfers at all;
//  * setConfiguration, setInterface, claimInterface(force).

internal const val usbStatusOk = 0
internal const val usbStatusIo = -5
internal const val usbStatusNoDevice = -19
internal const val usbStatusStall = -32
internal const val usbStatusProtocol = -71
internal const val usbStatusReset = -104
internal const val usbStatusTimeout = -110

/** A bulk (2) or interrupt (3) endpoint: its address (bit 7 = IN) and the interface setting it belongs to. */
internal class UsbEndpointInfo(val address: Int, val type: Int, val maxPacket: Int, val interfaceNumber: Int, val alternate: Int) {
    val input get() = address and 0x80 != 0
}

/** One device as android.hardware.usb gives it. Every call may block. */
internal interface UsbLink {
    /** Still on the bus (no ACTION_USB_DEVICE_DETACHED yet). */
    val present: Boolean
    /** The most one UsbRequest takes (16384 before Android 9). */
    val maxRequestBytes: Int
    /** UsbDeviceConnection.controlTransfer: bytes transferred, or negative. */
    fun control(requestType: Int, request: Int, value: Int, index: Int, data: ByteArray, length: Int, timeoutMs: Int): Int
    /** UsbRequest.queue of [length] bytes of [data] on [endpoint]; its end comes to [UsbPump.completed] with [tag]. */
    fun submit(endpoint: Int, data: ByteArray, length: Int, tag: Any): Boolean
    /** UsbRequest.cancel; the request still completes (with what it had). */
    fun cancel(tag: Any): Boolean
    fun setConfiguration(value: Int): Boolean
    fun setInterface(number: Int, alternate: Int): Boolean
}

internal class UsbResult(val status: Int, val data: ByteArray = ByteArray(0))

/**
 * The desktop's requests on one claimed device. Requests of one endpoint run one at a time in the
 * order given (the Host queues several), endpoints side by side. Thread-safe; [done] callbacks
 * run outside its lock.
 */
internal class UsbPump(
    private val link: UsbLink,
    endpoints: List<UsbEndpointInfo>,
    private val now: () -> Long = { System.nanoTime() / 1_000_000 },
    private val controlTimeoutMs: Int = 5000,
) {
    private class Urb(val token: Int, val endpoint: UsbEndpointInfo, val length: Int, val out: ByteArray?, val done: (UsbResult) -> Unit) {
        val data = if (out == null) ByteArray(length) else out
        var transferred = 0
        var chunk = ByteArray(0)
        var asked = 0
        var cancelled = false
        var finished = false
    }

    private val known = endpoints.associateBy { it.address }
    private val queues = HashMap<Int, ArrayDeque<Urb>>()
    private val lock = Any()
    private var gone = false
    var urbs = 0L; private set
    var bytes = 0L; private set
    var stalls = 0; private set
    var failed = 0; private set
    var timeouts = 0; private set

    /** A bulk or interrupt request ([out] null: IN of [length] bytes). */
    fun submit(token: Int, endpoint: Int, length: Int, out: ByteArray?, done: (UsbResult) -> Unit) {
        val info = known[endpoint]
        if (info == null || info.input != (out == null) || length < 0 || (out != null && out.size < length)) {
            done(UsbResult(usbStatusProtocol)); return
        }
        val urb = Urb(token, info, length, out, done)
        val first = synchronized(lock) {
            if (gone) null else queues.getOrPut(endpoint) { ArrayDeque() }.let { it.addLast(urb); it.size == 1 }
        }
        when (first) {
            null -> finish(urb, usbStatusNoDevice)
            true -> advance(urb)
            else -> Unit
        }
    }

    /** The next piece of [urb] goes to the device, or it ends. */
    private fun advance(urb: Urb) {
        val left = urb.length - urb.transferred
        val whole = link.maxRequestBytes - link.maxRequestBytes % maxOf(urb.endpoint.maxPacket, 1)
        val size = minOf(left, maxOf(whole, urb.endpoint.maxPacket))
        urb.asked = size
        urb.chunk = if (urb.out == null) ByteArray(size) else urb.out.copyOfRange(urb.transferred, urb.transferred + size)
        if (!link.submit(urb.endpoint.address, urb.chunk, size, urb)) {
            finish(urb, if (link.present) usbStatusIo else usbStatusNoDevice)
        }
    }

    /** The reaper: the request queued with [tag] ended with [count] bytes (negative: none). */
    fun completed(tag: Any, count: Int) {
        val urb = tag as? Urb ?: return
        if (synchronized(lock) { urb.finished }) return
        val moved = count.coerceIn(0, urb.asked)
        if (urb.out == null) System.arraycopy(urb.chunk, 0, urb.data, urb.transferred, moved)
        urb.transferred += moved
        when {
            urb.cancelled -> finish(urb, usbStatusReset)
            moved == urb.asked && urb.transferred < urb.length -> advance(urb)
            moved == urb.asked -> finish(urb, usbStatusOk)
            // Less than asked. An IN that got something ended with a short packet. Anything else
            // has no status to read: the endpoint and the bus are asked what happened.
            urb.out == null && moved > 0 -> finish(urb, usbStatusOk)
            else -> finish(urb, inferred(urb))
        }
    }

    private fun inferred(urb: Urb): Int = when {
        !link.present -> usbStatusNoDevice
        halted(urb.endpoint.address) == true -> usbStatusStall
        // An IN of nothing from a healthy endpoint is a zero-length packet.
        urb.out == null -> usbStatusOk
        else -> usbStatusProtocol
    }

    /** GET_STATUS of the endpoint: its halt bit, or null when the device does not answer. */
    private fun halted(endpoint: Int): Boolean? {
        val status = ByteArray(2)
        return if (link.control(0x82, 0, 0, endpoint, status, 2, 250) == 2) status[0].toInt() and 1 != 0 else null
    }

    private fun finish(urb: Urb, status: Int) {
        val next = synchronized(lock) {
            if (urb.finished) return
            urb.finished = true
            urbs++
            when (status) {
                usbStatusOk -> bytes += urb.transferred
                usbStatusStall -> stalls++
                usbStatusTimeout -> timeouts++
                usbStatusReset -> Unit
                else -> failed++
            }
            val queue = queues[urb.endpoint.address]
            if (queue != null && queue.firstOrNull() === urb) { queue.removeFirst(); queue.firstOrNull() } else { queue?.remove(urb); null }
        }
        urb.done(UsbResult(status, if (status == usbStatusOk && urb.out == null) urb.data.copyOf(urb.transferred) else ByteArray(0)))
        if (next != null) {
            if (synchronized(lock) { gone }) finish(next, usbStatusNoDevice) else if (next.cancelled) finish(next, usbStatusReset) else advance(next)
        }
    }

    /** The desktop gave up on [token]. True when it was pending here. */
    fun cancel(token: Int): Boolean {
        val (urb, flying) = synchronized(lock) {
            for (queue in queues.values) for ((index, item) in queue.withIndex()) {
                if (item.token == token && !item.finished) { item.cancelled = true; return@synchronized item to (index == 0) }
            }
            null to false
        }
        if (urb == null) return false
        // Not at the device yet: it never gets there. At the device: it ends with what it had.
        if (!flying) finish(urb, usbStatusReset) else link.cancel(urb)
        return true
    }

    /** The device left the bus: everything pending ends, nothing more starts. */
    fun gone() {
        val pending = synchronized(lock) { gone = true; queues.values.flatMap { it.toList() } }
        for (urb in pending) finish(urb, usbStatusNoDevice)
    }

    private fun busy(interfaceNumber: Int, except: Int) = synchronized(lock) {
        queues.any { (address, queue) -> address != except && queue.isNotEmpty() && known[address]?.interfaceNumber == interfaceNumber }
    }

    /**
     * A control request ([setup]: the 8 setup bytes), blocking. Three standard requests change
     * state the kernel keeps too and go through the API instead of the wire: SET_CONFIGURATION,
     * SET_INTERFACE and CLEAR_FEATURE(ENDPOINT_HALT). The last has no API: selecting the
     * endpoint's interface setting again resets both sides (data toggles included), which is done
     * only while nothing else runs on that interface; otherwise only the device is told.
     */
    fun control(setup: ByteArray, out: ByteArray = ByteArray(0)): UsbResult {
        if (setup.size != 8) return UsbResult(usbStatusProtocol)
        if (synchronized(lock) { gone } || !link.present) return UsbResult(usbStatusNoDevice)
        val type = setup[0].toInt() and 0xff
        val request = setup[1].toInt() and 0xff
        val value = (setup[2].toInt() and 0xff) or ((setup[3].toInt() and 0xff) shl 8)
        val index = (setup[4].toInt() and 0xff) or ((setup[5].toInt() and 0xff) shl 8)
        val length = (setup[6].toInt() and 0xff) or ((setup[7].toInt() and 0xff) shl 8)
        fun api(ok: Boolean) = UsbResult(if (ok) usbStatusOk else if (link.present) usbStatusStall else usbStatusNoDevice)
        if (type == 0x00 && request == 9) return api(link.setConfiguration(value))
        if (type == 0x01 && request == 11) return api(link.setInterface(index, value))
        if (type == 0x02 && request == 1 && value == 0) {
            val info = known[index and 0xff]
            if (info != null && !busy(info.interfaceNumber, info.address) && link.setInterface(info.interfaceNumber, info.alternate)) {
                return UsbResult(usbStatusOk)
            }
        }
        val input = type and 0x80 != 0
        if (!input && out.size < length) return UsbResult(usbStatusProtocol)
        val data = if (input) ByteArray(length) else out
        val started = now()
        val count = link.control(type, request, value, index, data, length, controlTimeoutMs)
        if (count >= 0) {
            synchronized(lock) { urbs++; bytes += count }
            return UsbResult(usbStatusOk, if (input) data.copyOf(minOf(count, length)) else ByteArray(0))
        }
        // No status: a failure that took the whole timeout was one, a device that is gone is
        // gone, and what is left is the device refusing the request (a stall).
        val status = when {
            !link.present -> usbStatusNoDevice
            now() - started >= controlTimeoutMs - 50 -> usbStatusTimeout
            else -> usbStatusStall
        }
        synchronized(lock) { urbs++; when (status) { usbStatusStall -> stalls++; usbStatusTimeout -> timeouts++; else -> failed++ } }
        return UsbResult(status)
    }
}
