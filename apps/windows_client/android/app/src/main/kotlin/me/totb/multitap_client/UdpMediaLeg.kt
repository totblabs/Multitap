package me.totb.multitap_client

import android.os.Process
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.PortUnreachableException
import java.net.StandardProtocolFamily
import java.net.StandardSocketOptions
import java.nio.ByteBuffer
import java.nio.channels.DatagramChannel
import java.nio.channels.SelectionKey
import java.nio.channels.Selector
import java.util.concurrent.atomic.AtomicIntegerArray
import java.util.concurrent.atomic.AtomicReferenceArray

/** One leg's `udp.grant`. [ticket] is a secret: never logged, wiped once the proofs exist. */
internal class UdpGrant(val relays: List<Pair<String, Int>>, val id: ByteArray, val ticket: ByteArray, val sessionId: String, val direct: Boolean = false, val stun: List<Pair<String,Int>> = emptyList())

/** One reading of the leg (windows/runner/udp_media_leg.h Reading). */
internal class UdpReading(
    val report: UdpProbe.Report,
    val running: Boolean,
    val media: Boolean,
    val publicKey: ByteArray?,
    /** Since the last reading, delivered to the decoder: pts, arrival us, bytes. */
    val arrivals: List<Long>,
    val audioBytes: Long,
    /** Datagrams given up as lost, this leg's lifetime. */
    val gaps: Long,
    val path: UdpPathGate.View,
    /** The crypto self-check failed: this leg never sent anything. */
    val failed: Boolean = false,
    /** The receiver's diagnostics (UdpReceiver.keyRequests, stalls, stallCause). */
    val keyRequests: Long = 0,
    val stalls: Long = 0,
    val stallCause: Int = 0,
    val local: InetSocketAddress? = null,
    val directPath: Int = 0,
    val directRttMs: Long = -1,
    val relayRttMs: Long = -1,
    val reflexive: InetSocketAddress? = null,
)

/**
 * udp-media-v1 on the client (windows/runner/udp_media_leg.cpp): one thread
 * owns the socket from the first BIND, because the relay pins that socket's
 * address. It runs the probe, the key agreement and the core receiver, and
 * hands whole messages to the gate. Nothing here can end a session: when this
 * leg stops or fails, the Host hears silence and goes back to WSS.
 *
 * [token] marks this leg at the gate, which passes only the current leg's
 * messages. [clockUs] is the session clock now, so arrivals read on the WSS
 * path's time base. [lookup] resolves an IPv4 name; it may never return, so it
 * runs off the leg's thread. [selfCheck] tries the crypto first (UdpCrypto).
 */
internal class UdpMediaLeg(
    private val grant: UdpGrant,
    private val gate: UdpPathGate,
    private val token: Long,
    clockUs: Long,
    private val lookup: (String) -> InetAddress? = ::udpResolveIPv4,
    private val selfCheck: () -> Boolean = UdpCrypto::selfCheck,
) {
    private val clockBase = steadyUs() - clockUs
    @Volatile private var stopped = false
    private var channel: DatagramChannel? = null
    @Volatile private var selector: Selector? = null
    private var relays = arrayOfNulls<InetSocketAddress>(0)
    private var chosen = 0
    private val direct = UdpDirect()
    private var candidates = emptyList<InetSocketAddress>()
    private var local: InetSocketAddress? = null
    private var directPath = 0
    private var directRttMs = -1L
    private var relayRttMs = -1L
    private var localReported = false
    private val stun = UdpStun()
    private var reflexive: InetSocketAddress? = null
    private data class InputItem(val sequence: Long,val epoch: Int,val bytes: ByteArray)
    private val inputs=ArrayDeque<InputItem>()
    fun input(sequence: Long,epoch: Int,bytes: ByteArray){
        synchronized(lock){if(!grant.direct||!media||directPath!=2||inputs.size>=64||sequence<1||sequence>0xffffffffL||bytes.isEmpty()||bytes.size>Udp.MAX_PLAINTEXT-9)return;inputs.addLast(InputItem(sequence,epoch,bytes.copyOf()))};wake()
    }
    fun peer(value: List<InetSocketAddress>) { if(value.size <= UdpDirect.MAX_CANDIDATES) synchronized(lock) { candidates = value.toList() }; wake() }

    // Leg thread only.
    private var ownKey: UdpKeyPair? = null
    private var sealer: UdpSealer? = null
    private var opener: UdpOpener? = null
    private var receiver: UdpReceiver? = null
    private val output = UdpReceiver.Output()
    private val handoff = UdpHandoff { gate.offerUdp(token, it.bytes, it.video, it.key, it.epoch, it.pts) }
    private val opened = ByteArray(Udp.MAX_PLAINTEXT)
    private val sealed = ByteArray(Udp.MAX_DATAGRAM)
    private var epoch = 0
    private var wssKeys = 0L
    private var keyFailed = false
    @Volatile private var passes = 0L

    private val lock = Any()  // guards the fields below
    private var report = UdpProbe.Report()
    private var running = true
    private var media = false
    private var publicKey: ByteArray? = null
    private val arrivals = ArrayList<Long>()
    private var audioBytes = 0L
    private var gaps = 0L
    private var keyRequests = 0L
    private var stalls = 0L
    private var stallCause = 0
    private var hostKey: ByteArray? = null
    private var failed = false

    private val thread = Thread(::run, "Multitap UDP").apply { isDaemon = true; start() }

    fun hostKey(key: ByteArray) { synchronized(lock) { hostKey = key.copyOf() }; wake() }

    fun read(): UdpReading {
        val path = gate.view()
        synchronized(lock) {
            return UdpReading(report.copy(), running, media, publicKey?.copyOf(), ArrayList(arrivals), audioBytes, gaps, path, failed,
                keyRequests, stalls, stallCause, local, directPath, directRttMs, relayRttMs, reflexive).also { arrivals.clear(); audioBytes = 0 }
        }
    }

    /**
     * Repeatable, and it never waits: the thread ends within one pass of its
     * loop, and a name lookup is left behind. The gate refuses this leg's
     * messages once another leg or none is current.
     */
    fun stop() {
        stopped = true
        wake()
    }

    // Ends the loop's wait: it sleeps until its next deadline, so news from other threads wakes it.
    private fun wake() { try { selector?.wakeup() } catch (_: Exception) {} }

    /** Tests: waits for the thread to end. */
    fun join(millis: Long) = thread.join(millis)

    /** Tests: passes of the loop so far, each one wake-up of the thread. */
    fun passes() = passes

    private fun run() {
        // Media arrives here: as urgent as the decoder's thread (AndroidMedia). Never fatal.
        try { Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY) } catch (_: Throwable) {}
        // Anything thrown here would end the app: every failure ends this leg only.
        try { loop() } catch (_: Throwable) { finish(UdpProbe.PROBE_SOCKET_ERROR) }
        try { channel?.close() } catch (_: Throwable) {}
        try { selector?.close() } catch (_: Throwable) {}
        ownKey?.wipe()
        direct.wipe()
        synchronized(lock) { running = false; hostKey?.fill(0) }
    }

    private fun finish(code: Int) { synchronized(lock) { if (report.code == 0) report = report.copy(code = code) } }

    private fun loop() {
        val bindProof = UdpCrypto.proof(1, "multitap-udp-bind-v1", grant.id, grant.ticket)
        val ackProof = UdpCrypto.proof(2, "multitap-udp-ack-v1", grant.id, grant.ticket)
        grant.ticket.fill(0)
        // A provider that cannot do this would drop every datagram, silently: say so, and stay on WSS.
        if (!selfCheck()) { synchronized(lock) { failed = true }; return }
        if (!openSocket()) return finish(UdpProbe.PROBE_SOCKET_ERROR)
        val count = grant.relays.size + grant.stun.size
        relays = arrayOfNulls(grant.relays.size)
        val states = AtomicIntegerArray(count)  // 0 open, 1 resolved, 2 failed
        val addresses = AtomicReferenceArray<InetAddress>(count)
        startLookups(states, addresses)
        val looked = BooleanArray(count)
        val probe = UdpProbe(grant.relays.size, grant.id, bindProof, ackProof, steadyUs()) { relay, data, size -> send(relay, data, size) }
        val buffer = ByteBuffer.allocate(Udp.MAX_DATAGRAM + 1)
        val channel = channel!!
        val selector = selector!!
        while (!stopped) {
            ++passes
            for (relay in 0 until count) {
                val state = states.get(relay)
                if (looked[relay] || state == 0) continue
                looked[relay] = true
                if (state != 1) continue
                if(relay>=grant.relays.size){if(grant.direct)stun.server(InetSocketAddress(addresses.get(relay),grant.stun[relay-grant.relays.size].second));continue}
                relays[relay] = InetSocketAddress(addresses.get(relay), grant.relays[relay].second)
                probe.resolved(relay)
            }
            try { selector.select(waitMs(probe)); selector.selectedKeys().clear() } catch (_: Exception) { probe.socketLost(); publish(probe); return }
            for (n in 0 until RECEIVE_BURST) {
                buffer.clear()
                val from = try { channel.receive(buffer) } catch (_: PortUnreachableException) { continue } catch (_: Exception) { probe.socketLost(); publish(probe); return }
                if (from !is InetSocketAddress) break
                datagram(probe, from, buffer.array(), buffer.position())
            }
            val now = steadyUs()
            probe.tick(now)
            publish(probe)
            if (probe.over()) return
            keys(probe, now)
            directTick(now)
            inputs(now)
            if(grant.direct)stun.tick(now,::directSend)
            if (receiver != null) media(now)
        }
    }

    private fun startLookups(states: AtomicIntegerArray, addresses: AtomicReferenceArray<InetAddress>) {
        for (relay in 0 until grant.relays.size+grant.stun.size) {
            val host = if(relay<grant.relays.size)grant.relays[relay].first else grant.stun[relay-grant.relays.size].first
            val resolve = lookup
            try {
                Thread({
                    val found = try { resolve(host) } catch (_: Throwable) { null }
                    if (found != null) addresses.set(relay, found)
                    states.set(relay, if (found != null) 1 else 2)
                    wake()
                }, "Multitap UDP lookup").apply { isDaemon = true; start() }
            } catch (_: Throwable) { states.set(relay, 2) }
        }
    }

    private fun openSocket(): Boolean = try {
        val opened = DatagramChannel.open(StandardProtocolFamily.INET)
        channel = opened
        opened.configureBlocking(false)
        try { opened.setOption(StandardSocketOptions.SO_RCVBUF, RECEIVE_BUFFER) } catch (_: Exception) {}
        opened.bind(InetSocketAddress(InetAddress.getByAddress(ByteArray(4)), 0))
        selector = Selector.open().also { opened.register(it, SelectionKey.OP_READ) }
        true
    } catch (_: Exception) { false }

    private fun send(relay: Int, data: ByteArray, size: Int): Boolean {
        val target = relays.getOrNull(relay) ?: return false
        return try { channel?.send(ByteBuffer.wrap(data, 0, size), target) == size } catch (_: Exception) { false }
    }

    private fun relayOf(from: InetSocketAddress): Int {
        for (n in relays.indices) { val relay = relays[n]; if (relay != null && relay.port == from.port && relay.address == from.address) return n }
        return -1
    }
    private fun directSend(target: InetSocketAddress, bytes: ByteArray) {
        val wrapped = if(bytes.size == UdpDirect.PACKET_BYTES && bytes[0] == 84.toByte() && bytes[1] == 86.toByte() && bytes[2] == 68.toByte() && relayOf(target) >= 0) byteArrayOf(3) + grant.id + bytes else bytes
        try { channel?.send(ByteBuffer.wrap(wrapped), target) } catch (_: Exception) {}
    }
    private fun directTick(now: Long) {
        if(!grant.direct || receiver == null)return
        val relay = relays.getOrNull(chosen) ?: return
        direct.relay(relay)
        synchronized(lock) { candidates.forEach(direct::candidate); candidates = emptyList() }
        if(!localReported) {
            localReported = true
            try {
                val own = channel!!.localAddress as InetSocketAddress
                DatagramChannel.open(StandardProtocolFamily.INET).use { query ->
                    query.connect(relay)
                    val route = query.localAddress as InetSocketAddress
                    synchronized(lock) { local = InetSocketAddress(route.address, own.port) }
                }
            } catch (_: Exception) {}
        }
        direct.tick(now, ::directSend)
        val selected = direct.target(now)
        synchronized(lock) { if(directPath==0)directPath=1; directRttMs = if(selected != null)(direct.rtt(selected)+999)/1000 else -1; relayRttMs = direct.relayRtt(now).let { if(it>0)(it+999)/1000 else -1 } }
    }

    private fun datagram(probe: UdpProbe, from: InetSocketAddress, data: ByteArray, size: Int) {
        if(grant.direct)stun.received(data,size,from,steadyUs())?.let { synchronized(lock){reflexive=it};return }
        val relay = relayOf(from)
        val now = steadyUs()
        if(grant.direct && receiver != null) {
            if(direct.received(data,size,from,now,::directSend))return
            if(relay >= 0 && size == 9 + UdpDirect.PACKET_BYTES && data[0] == 3.toByte() && direct.received(data.copyOfRange(9,size),size-9,from,now,::directSend))return
        }
        if(relay < 0 && (!grant.direct || !direct.accepts(from,now)))return
        if (!probe.bound() || size == UdpProbe.PROBE_BYTES) return probe.received(relay, data, size, now)
        val opener = opener
        if ((relay >= 0 && relay != probe.chosen()) || opener == null) return
        val plain = opener.open(data, size, opened)
        if (plain > 0) { if(grant.direct)synchronized(lock){directPath=if(relay<0)2 else 1};sync(); receiver!!.datagram(opened, plain, now, output); deliver(now) }
    }

    private fun keys(probe: UdpProbe, now: Long) {
        if (keyFailed || receiver != null || probe.report().code != UdpProbe.PROBE_REACHABLE) return
        val own = ownKey ?: try {
            UdpKeyPair().also { ownKey = it; synchronized(lock) { publicKey = it.publicKey.copyOf() } }
        } catch (_: Exception) { keyFailed = true; return }
        val host = synchronized(lock) { hostKey } ?: return
        val shared = ByteArray(UdpCrypto.KEY_BYTES)
        val keys = if (own.agree(host, shared)) UdpCrypto.deriveKeys(shared, own.publicKey, host, grant.sessionId.toByteArray(Charsets.UTF_8), true) else null
        shared.fill(0); own.wipe()
        if (keys == null) { keyFailed = true; return }
        if(grant.direct)direct.configure(keys)
        sealer = UdpSealer(keys.send, grant.id)
        opener = UdpOpener(keys.receive)
        epoch = gate.epoch()
        wssKeys = gate.wssKeys()
        receiver = UdpReceiver(epoch, now)
        synchronized(lock) { media = true }
    }
    private fun inputs(now: Long){
        val pending=synchronized(lock){inputs.toList().also { inputs.clear() }}
        val sealer=sealer?:return;val target=if(grant.direct)direct.target(now) else null;if(target==null)return
        for(item in pending){
            val plain=ByteArray(9+item.bytes.size);plain[0]=0x30;Udp.put32(plain,1,item.sequence);Udp.put16(plain,5,item.epoch);Udp.put16(plain,7,item.bytes.size);item.bytes.copyInto(plain,9)
            val size=sealer.seal(plain,plain.size,sealed);if(size>0)directSend(target,sealed.copyOf(size))
        }
    }

    // Before any datagram: what WSS announced may be what this one carries.
    private fun sync() {
        val receiver = receiver!!
        val announced = gate.epoch()
        if (announced != epoch) { epoch = announced; receiver.setEpoch(announced) }
        val keys = gate.wssKeys()
        if (keys != wssKeys) { wssKeys = keys; receiver.keyFrameOtherPath() }
    }

    private fun media(now: Long) {
        sync()
        val receiver = receiver!!
        receiver.tick(now, output)
        deliver(now)
        receiver.watch(now, gate.udpActive())
    }

    private fun deliver(now: Long) {
        val receiver = receiver!!
        // What the decoder could not take, or the rule refused, leaves a hole: skip to the
        // next IDR. A refused switch key frame would otherwise end the picture for good.
        when (handoff.pass(output.messages, now) { message ->
            val size = message.bytes.size.toLong()
            synchronized(lock) {
                if (!message.video) audioBytes += size
                else {
                    if (arrivals.size >= MAX_ARRIVALS) repeat(3) { arrivals.removeAt(0) }
                    arrivals.add(message.pts); arrivals.add(now - clockBase); arrivals.add(size)
                }
            }
        }) {
            UdpHandoff.Hole.FULL -> receiver.queueFull(now)
            UdpHandoff.Hole.REFUSED -> receiver.refused(now)
            UdpHandoff.Hole.NONE -> {}
        }
        val sealer = sealer!!
        for (plain in output.control) {
            if (plain.size == Udp.FEEDBACK_BYTES && plain[0].toInt() == Udp.KIND_FEEDBACK) synchronized(lock) {
                gaps = Udp.get32(plain, 10)
                keyRequests = receiver.keyRequests; stalls = receiver.stalls; stallCause = receiver.stallCause
            }
            val size = sealer.seal(plain, plain.size, sealed)
            if (size > 0) { val target = if(grant.direct)direct.target(now) else null; if(target != null)directSend(target,sealed.copyOf(size)) else send(chosen, sealed, size) }
        }
        output.control.clear()
    }

    // Until the next deadline of the probe or the receiver, at most the cap: no fixed
    // 2 ms polling while probing, on keepalives or on an idle screen.
    private fun waitMs(probe: UdpProbe): Long {
        if (handoff.holding) return MIN_WAIT_MS
        val receiver = receiver
        var next = probe.nextUs()
        if (receiver != null) next = minOf(next, receiver.nextTickUs())
        return udpWaitMs(next - steadyUs(), if (receiver != null) MEDIA_WAIT_MS else IDLE_WAIT_MS)
    }

    private fun publish(probe: UdpProbe) {
        chosen = if (probe.bound()) probe.chosen() else 0
        val copy = probe.report().copy()
        synchronized(lock) { report = copy }
    }

    companion object {
        // A timer's resolution, the longest wait with media (WSS cues are read on waking), and without.
        const val MIN_WAIT_MS = 2L
        const val MEDIA_WAIT_MS = 20L
        const val IDLE_WAIT_MS = 250L
        private const val RECEIVE_BUFFER = 2 * 1024 * 1024
        private const val RECEIVE_BURST = 256  // datagrams per pass, so timers keep running
        private const val MAX_ARRIVALS = 256 * 3
    }
}

private fun steadyUs() = System.nanoTime() / 1000

/**
 * The leg's hand-off to the gate, in order. A busy decoder queue (a decoder
 * starting up, a burst) answers FULL: a video message is offered again on the
 * next passes for up to RETRY_US, as the WSS path waits and offers the same
 * packet again, and later video waits behind it. Only then, or when the decoder
 * or the rule refuses it, is it lost. Audio never waits: a full audio share
 * drops that chunk.
 */
internal class UdpHandoff(private val offer: (UdpReceiver.Message) -> UdpPathGate.Delivery) {
    /** What the receiver must hear: nothing, a P-frame lost to a full queue, or a refusal that backs off. */
    enum class Hole { NONE, FULL, REFUSED }
    private var waiting = ArrayList<UdpReceiver.Message>()
    private var fullSince = -1L

    /** A video message waits for the decoder: the leg comes back within MIN_WAIT_MS. */
    val holding get() = fullSince >= 0

    /** Offers [messages] after those still waiting (and empties it); [taken] sees each one the decoder took. */
    fun pass(messages: MutableList<UdpReceiver.Message>, nowUs: Long, taken: (UdpReceiver.Message) -> Unit): Hole {
        if (waiting.isEmpty() && messages.isEmpty()) return Hole.NONE
        waiting.addAll(messages); messages.clear()
        val keep = ArrayList<UdpReceiver.Message>()
        var blocked = false
        var skipping = false
        var hole = Hole.NONE
        for (message in waiting) {
            if (message.video && blocked) { keep.add(message); continue }
            if (message.video && skipping && !message.key) continue
            val result = offer(message)
            if (message.video && result == UdpPathGate.Delivery.FULL) {
                if (fullSince < 0) fullSince = nowUs
                if (nowUs - fullSince < RETRY_US && waiting.size <= MAX_WAITING) { blocked = true; keep.add(message); continue }
            }
            if (message.video) fullSince = -1
            if (result == UdpPathGate.Delivery.ACCEPTED) {
                taken(message)
                // A key frame taken after the hole ends it: no request, and its P-frames go on.
                if (message.video && message.key) { skipping = false; hole = Hole.NONE }
            } else if (message.video) {
                skipping = true
                // A refused key frame, or a decoder that refuses (it failed): the next request backs off.
                if (result == UdpPathGate.Delivery.FULL && !message.key) { if (hole == Hole.NONE) hole = Hole.FULL } else hole = Hole.REFUSED
            }
        }
        waiting = keep
        return hole
    }

    companion object {
        const val RETRY_US = 50000L
        const val MAX_WAITING = 32
    }
}

/** Milliseconds to wait for a deadline [untilUs] away: rounded up, at least MIN_WAIT_MS, at most [capMs]. */
internal fun udpWaitMs(untilUs: Long, capMs: Long): Long =
    if (untilUs >= capMs * 1000) capMs else maxOf(UdpMediaLeg.MIN_WAIT_MS, (untilUs + 999) / 1000)

internal fun udpResolveIPv4(host: String): InetAddress? = InetAddress.getAllByName(host).firstOrNull { it is Inet4Address }

/** An int32 or int64 of the standard codec, nothing else. */
internal fun udpInteger(value: Any?): Long? = when (value) { is Int -> value.toLong(); is Long -> value; else -> null }

/** `start`'s grant, exactly as windows/runner/udp_media_channel.h ParseGrant takes it, or null. */
internal fun udpGrant(args: Map<*, *>): UdpGrant? {
    val relays = args["relays"] as? List<*>
    val session = args["sessionId"] as? String
    val id = args["id"] as? ByteArray
    val ticket = args["ticket"] as? ByteArray
    if (relays == null || relays.isEmpty() || session == null || session.isEmpty() || id == null || id.size != 8 || ticket == null || ticket.size != 32) return null
    val parsed = ArrayList<Pair<String, Int>>()
    for (value in relays) {
        val relay = value as? Map<*, *> ?: return null
        val host = relay["host"] as? String ?: return null
        val port = udpInteger(relay["port"]) ?: return null
        if (port < 1 || port > 65535) return null
        parsed.add(host to port.toInt())
    }
    val stun=ArrayList<Pair<String,Int>>()
    if(args["direct"]==true){
        val servers=args["stun"] as? List<*>?:emptyList<Any>();if(servers.size>2)return null
        for(value in servers){val row=value as? Map<*,*>?:return null;val host=row["host"] as? String?:return null;val port=udpInteger(row["port"])?:return null;if(port<1||port>65535)return null;stun.add(host to port.toInt())}
    }
    return UdpGrant(parsed, id, ticket, session, args["direct"] == true,stun)
}

/** `poll`'s answer, keyed exactly as windows/runner/udp_media_channel.h Encode writes it. */
internal fun udpReadingValue(reading: UdpReading): Map<String, Any> {
    val r = reading.report
    val report = HashMap<String, Long>()
    if (r.code != 0) {
        report["udpProbe"] = r.code.toLong()
        if (r.relay >= 0) report["udpRelayIndex"] = r.relay.toLong()
        if (r.rttP50Ms >= 0) { report["udpRttP50Ms"] = r.rttP50Ms.toLong(); report["udpRttP95Ms"] = r.rttP95Ms.toLong() }
    }
    if (r.lossPct >= 0) { report["udpLossPct"] = r.lossPct.toLong(); report["udpKeepSent"] = r.keepSent; report["udpKeepLost"] = r.keepLost }
    if (reading.media) {
        report["udpActivePath"] = reading.path.active.toLong(); report["udpFallbacks"] = reading.path.fallbacks; report["udpGaps"] = reading.gaps
        // Why the Host may hear no progress (its 271 cause 3): requests, stalls and the last one's wait.
        report["udpKeyRequests"] = reading.keyRequests; report["udpStalls"] = reading.stalls
        if (reading.stalls > 0) report["udpStallCause"] = reading.stallCause.toLong()
    }
    if(reading.directPath>0){report["udpDirectPath"]=reading.directPath.toLong();if(reading.directRttMs>=0)report["udpDirectRttMs"]=reading.directRttMs;if(reading.relayRttMs>=0)report["udpRelayRttMs"]=reading.relayRttMs}
    // A plain list of ints, never a typed array (see udp_media_channel.h).
    val value = hashMapOf<String, Any>("report" to report, "running" to reading.running, "arrivals" to ArrayList(reading.arrivals),
        "audioBytes" to reading.audioBytes, "keyedStream" to reading.path.keyedStream)
    reading.publicKey?.let { value["publicKey"] = it }
    reading.local?.let { value["local"] = mapOf("address" to it.address.hostAddress!!,"port" to it.port) }
    reading.reflexive?.let { value["reflexive"] = mapOf("address" to it.address.hostAddress!!,"port" to it.port) }
    return value
}
