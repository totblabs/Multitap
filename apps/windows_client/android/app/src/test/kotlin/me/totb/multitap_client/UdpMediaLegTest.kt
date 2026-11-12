package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.SocketTimeoutException
import java.util.Collections
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

// The leg over real loopback sockets against a fake relay and Host, through to a
// fake decoder, as apps/windows_client/test/native/udp_media_leg_test.cpp runs the
// C++ leg. The fake Host here is not the Host's Sender (that is C++ only): it seals
// fragments with the port's own crypto in the Host's role, answers NACK from what
// it kept, and sends TAIL after each message and every 100 ms. Loopback only: it
// says nothing about a real network, relay or Host.
class UdpMediaLegTest {
    private class FakeHost(private val mode: Mode = Mode.FULL) : AutoCloseable {
        enum class Mode { SILENT, ACK_ONLY, FULL }
        private val socket = DatagramSocket(InetSocketAddress(InetAddress.getLoopbackAddress(), 0)).apply { soTimeout = 1; sendBufferSize = 4 * 1024 * 1024 }
        val port = socket.localPort
        val binds = AtomicInteger(); val pings = AtomicInteger(); val feedbacks = AtomicInteger(); val keyRequests = AtomicInteger()
        val lastKeyEpoch = AtomicInteger(-1)
        val repairs = AtomicLong(); val dropped = AtomicLong(); val garbled = AtomicLong()
        @Volatile var proven = false
        @Volatile var dropDownPerMille = 0
        @Volatile var dropUpPerMille = 0
        val firstPingsDropped = AtomicInteger()
        @Volatile var firstBind: ByteArray? = null
        /** Answers each KEYFRAME request with this key frame of the epoch it names, at once. */
        @Volatile var answer: ((Int) -> ByteArray)? = null
        private val actions = ConcurrentLinkedQueue<() -> Unit>()
        private val stop = AtomicBoolean()
        @Volatile private var client: InetSocketAddress? = null
        // Host thread only.
        private var sealer: UdpSealer? = null
        private var opener: UdpOpener? = null
        private val kept = LinkedHashMap<Long, ByteArray>()
        private var videoId = 1L
        private var audioId = 1L
        private var finished = 0L
        private var challenge = 0x5eed000000000001L
        private var tailAt = 0L
        private var last = ByteArray(0)
        private var state = 42L
        private val worker = Thread(::run, "fake host").apply { isDaemon = true; start() }

        /** Hands the Host's half of the keys to its thread, and returns its public key. */
        fun keys(client: ByteArray, sessionId: String): ByteArray {
            val own = UdpKeyPair()
            val shared = ByteArray(32)
            assertTrue(own.agree(client, shared))
            val keys = UdpCrypto.deriveKeys(shared, client, own.publicKey, sessionId.toByteArray(), false)!!
            post { sealer = UdpSealer(keys.send, ByteArray(8) { 0x99.toByte() }); opener = UdpOpener(keys.receive) }
            return own.publicKey
        }
        fun post(action: () -> Unit) { actions.add(action) }
        fun video(message: ByteArray, key: Boolean) = post { sendVideo(message) }
        /** On this thread and at once: the first datagram can beat the leg's next pass. */
        fun videoRightAfter(first: () -> Unit, message: ByteArray) = post { first(); sendVideo(message) }
        fun audio(message: ByteArray) = post {
            val id = audioId++
            for (index in 0 until Udp.fragmentCount(message.size)) seal(fragmentOf(Udp.CLASS_AUDIO, id, message, index), media = true)
        }
        fun raw(datagram: ByteArray) = post { send(datagram) }
        fun replay() = post { if (last.isNotEmpty()) send(last) }

        override fun close() { stop.set(true); worker.join(2000); socket.close() }

        private fun lose(perMille: Int): Boolean {
            state = state * 6364136223846793005L + 1442695040888963407L
            return ((state ushr 33) % 1000).toInt() < perMille
        }
        private fun run() {
            val buffer = ByteArray(2048)
            while (!stop.get()) {
                while (true) { (actions.poll() ?: break)() }
                val packet = DatagramPacket(buffer, buffer.size)
                try { socket.receive(packet); datagram(packet.data.copyOf(packet.length), packet.socketAddress as InetSocketAddress) }
                catch (_: SocketTimeoutException) {} catch (_: Exception) { if (stop.get()) return }
                val now = System.nanoTime() / 1000
                if (sealer != null && now >= tailAt) { tail(now); tailAt = now + 100000 }
            }
        }
        private fun send(datagram: ByteArray) {
            val to = client ?: return
            try { socket.send(DatagramPacket(datagram, datagram.size, to)) } catch (_: Exception) {}
        }
        private fun seal(plain: ByteArray, media: Boolean) {
            val out = ByteArray(Udp.MAX_DATAGRAM)
            val size = sealer?.seal(plain, plain.size, out) ?: return
            last = out.copyOf(size)
            if (media && lose(dropDownPerMille)) { dropped.incrementAndGet(); return }
            send(last)
        }
        private fun sendVideo(message: ByteArray) {
            val id = videoId++
            kept[id] = message
            while (kept.size > 256) kept.remove(kept.keys.first())
            for (index in 0 until Udp.fragmentCount(message.size)) seal(fragmentOf(Udp.CLASS_VIDEO, id, message, index), media = true)
            finished = id
            tail(System.nanoTime() / 1000)
        }
        private fun tail(now: Long) = seal(tailFor(finished, now, challenge, 0), media = false)
        private fun datagram(data: ByteArray, from: InetSocketAddress) {
            if (mode == Mode.SILENT) return
            client = from
            if (data.size == UdpProbe.PROOF_BYTES && data[0].toInt() == 1) {
                binds.incrementAndGet(); if (firstBind == null) firstBind = data
                send(ackBytes); return
            }
            if (mode == Mode.ACK_ONLY) return
            if (data.size == UdpProbe.PROBE_BYTES && data[0].toInt() == 3 && data[9].toInt() == 1) {
                pings.incrementAndGet()
                if (firstPingsDropped.get() > 0) { firstPingsDropped.decrementAndGet(); return }
                for (n in 1 until 9) data[n] = 0x99.toByte()
                data[9] = 2; send(data); return
            }
            val opener = opener ?: return
            if (lose(dropUpPerMille)) return
            val plain = ByteArray(Udp.MAX_PLAINTEXT)
            val size = opener.open(data, data.size, plain)
            if (size == 0) { garbled.incrementAndGet(); return }
            when {
                size == Udp.FEEDBACK_BYTES && plain[0].toInt() == Udp.KIND_FEEDBACK -> {
                    feedbacks.incrementAndGet()
                    if (Udp.get64(plain, 26) == challenge) proven = true
                }
                size == Udp.KEYFRAME_BYTES && plain[0].toInt() == Udp.KIND_KEYFRAME -> {
                    keyRequests.incrementAndGet(); lastKeyEpoch.set(Udp.get16(plain, 2))
                    answer?.let { sendVideo(it(Udp.get16(plain, 2))) }
                }
                plain[0].toInt() == Udp.KIND_NACK && size == 2 + (plain[1].toInt() and 0xff) * Udp.NACK_ENTRY_BYTES -> {
                    for (n in 0 until (plain[1].toInt() and 0xff)) {
                        val at = 2 + n * Udp.NACK_ENTRY_BYTES
                        val message = kept[Udp.get32(plain, at)] ?: continue
                        val first = Udp.get16(plain, at + 4)
                        val end = minOf(first + Udp.get16(plain, at + 6), Udp.fragmentCount(message.size))
                        for (index in first until end) { seal(fragmentOf(Udp.CLASS_VIDEO, Udp.get32(plain, at), message, index), media = true); repairs.incrementAndGet() }
                    }
                }
            }
        }
    }

    private fun grantFor(vararg ports: Int) = UdpGrant(ports.map { "127.0.0.1" to it }, idBytes.copyOf(), ticketBytes.copyOf(), "loopback-session")

    private fun waitFor(ms: Long, what: String, done: () -> Boolean) {
        val end = System.nanoTime() + ms * 1000000
        while (!done()) { if (System.nanoTime() > end) throw AssertionError("Timed out: $what"); Thread.sleep(2) }
    }

    /** A leg through the probe and the keys, on a Host that may now send media. */
    private inner class Session(val host: FakeHost, epoch: Int = 1, keyed: Boolean = true) : AutoCloseable {
        val engine = FakeEngine()
        val gate = UdpPathGate()
        val leg: UdpMediaLeg
        var reading: UdpReading? = null
        private val arrivals = ArrayList<Long>()
        private var audioBytes = 0L
        init {
            gate.attach(TEST_LEASE, engine.offer); gate.disengage(TEST_LEG); gate.stream(epoch, 1)
            leg = UdpMediaLeg(grantFor(host.port), gate, TEST_LEG, 5000000)
            waitFor(9000, "the probe passed") { latest().publicKey != null }
            gate.engage(keyed)
            leg.hostKey(host.keys(reading!!.publicKey!!, "loopback-session"))
            waitFor(3000, "FEEDBACK proved the address") { host.proven }
        }
        fun latest(): UdpReading {
            val next = leg.read()
            arrivals.addAll(next.arrivals); audioBytes += next.audioBytes
            return UdpReading(next.report, next.running, next.media, next.publicKey, ArrayList(arrivals), audioBytes, next.gaps, next.path).also { reading = it }
        }
        override fun close() { leg.stop(); leg.join(2000) }
    }

    private fun frame(key: Boolean, epoch: Int, pts: Long, size: Int): ByteArray {
        val packet = mediaPacket(1, key, epoch, pts, size)
        for (n in 12 until packet.size) packet[n] = (pts * 31 + n * 7).toByte()
        return packet
    }
    private fun ptsOf(packet: ByteArray): Long { var at = 0L; for (n in 0 until 8) at = at or ((packet[4 + n].toLong() and 0xff) shl (8 * n)); return at }
    private fun check(value: Boolean, message: String) { if (!value) throw AssertionError(message) }

    private fun mediaWithLoss() = FakeHost().use { host ->
        Session(host, 1, false).use { session ->
            host.dropDownPerMille = 30; host.dropUpPerMille = 10
            val sentFrames = HashMap<Long, ByteArray>()
            var pts = 1000L
            var requests = 0
            fun send(key: Boolean, size: Int) { val message = frame(key, 1, pts, size); sentFrames[pts] = message; host.video(message, key); pts += 16667 }
            send(true, 120000)
            for (n in 0 until 150) {
                var keyWanted = false
                if (host.keyRequests.get() > requests) { requests = host.keyRequests.get(); keyWanted = true }
                send(keyWanted, 4000 + (n % 7) * 3000)
                if (n % 2 == 0) host.audio(mediaPacket(2, false, 1, pts, 1920, n))
                Thread.sleep(8)
            }
            host.dropDownPerMille = 0; host.dropUpPerMille = 0
            Thread.sleep(400)
            val last = pts
            send(true, 60000)
            waitFor(3000, "the last key frame") { session.engine.copy().any { it[0].toInt() == 1 && ptsOf(it) == last } }
            var previous = 0L; var videos = 0; var audios = 0
            for (packet in session.engine.copy()) {
                check(packet[2].toInt() == 0 && packet[3].toInt() == 0, "The epoch reached the decoder")
                if (packet[0].toInt() == 2) { ++audios; check(packet.size == 1932, "Audio changed size"); continue }
                val at = ptsOf(packet)
                check(at > previous, "Video left its order"); previous = at
                check(packet.contentEquals(onWire(sentFrames.getValue(at))), "A reassembled message differs from what the Host sent")
                ++videos
            }
            check(videos >= 40 && audios > 0, "Too little came through at 3% loss: $videos")
            check(host.repairs.get() > 0 && host.dropped.get() > 0, "No loss was repaired")
            summary["lossFrames"] = sentFrames.size.toLong(); summary["lossFramesDelivered"] = videos.toLong()
            summary["lossDatagramsDropped"] = host.dropped.get(); summary["lossRepairs"] = host.repairs.get(); summary["lossKeyRequests"] = host.keyRequests.get().toLong()
            val reading = session.latest()
            check(reading.path.active == 1 && reading.media, "The active path is not UDP")
            check(reading.arrivals.size == videos * 3, "Arrivals differ from what the decoder took")
            check(reading.arrivals[1] >= 5000000 && reading.arrivals[1] < 65000000, "Arrivals are not on the session clock")
            check(reading.path.keyedStream == 1L, "The key frame over UDP was not reported")
            check(reading.report.code == UdpProbe.PROBE_REACHABLE && reading.report.relay == 0 && reading.report.lossPct == 0, "The probe's numbers went missing")
            session.leg.stop(); session.leg.join(2000)
            check(session.engine.watches == videos, "Video on UDP was not watched")
        }
    }

    private fun idleFeedbackAndBind() = FakeHost().use { host ->
        Session(host).use { session ->
            val before = host.feedbacks.get()
            Thread.sleep(1000)
            val count = host.feedbacks.get() - before
            check(count in 7..13, "FEEDBACK did not keep its timer on an idle screen: $count")
            check(host.firstBind.contentEquals(bindBytes), "The real BIND differs from the relay's reference")
            check(session.engine.count() == 0, "Media from nowhere")
        }
    }

    private fun pathsOverSockets() = FakeHost().use { host ->
        Session(host).use { session ->
            host.video(frame(true, 1, 100, 30000), true)
            waitFor(2000, "UDP active") { session.gate.view().active == 1 }
            check(offerWss(session.gate, session.engine, onWire(frame(false, 1, 150, 100))) == AndroidMediaStatus.OK && session.engine.count() == 1, "WSS passed while UDP is active")
            host.video(frame(false, 1, 200, 30000), false)
            waitFor(2000, "UDP P-frame") { session.engine.count() == 2 }
            // Back to WSS at a forced IDR: in-flight UDP frames die, and the receiver is told.
            check(offerWss(session.gate, session.engine, onWire(frame(true, 1, 300, 100))) == AndroidMediaStatus.OK && session.engine.count() == 3, "The WSS key frame")
            check(session.gate.view().active == 0 && session.gate.view().fallbacks == 1L, "Fallback")
            host.video(frame(false, 1, 400, 30000), false)
            Thread.sleep(150)
            check(session.engine.count() == 3, "A UDP P-frame passed after the fallback")
            host.video(frame(true, 1, 500, 30000), true)
            waitFor(2000, "The switch back") { session.engine.count() == 4 }
            check(session.gate.view().active == 1, "The switch back did not make UDP active")
            val reading = session.latest()
            check(reading.report.code == UdpProbe.PROBE_REACHABLE, "A path change touched the probe")
            check(reading.arrivals.size == 3 * 3, "Arrivals count what the decoder never took")
        }
    }

    private fun hostileDatagrams() = FakeHost().use { host ->
        Session(host).use { session ->
            host.video(frame(true, 1, 100, 3000), true)
            waitFor(2000, "The first key frame") { session.engine.count() == 1 }
            for (size in listOf(1, 16, 17, 33, 34, 35, 41, 100, 1199, 1200, 1201, 1500)) {
                val junk = ByteArray(size) { size.toByte() }
                host.raw(junk.copyOf())
                junk[0] = 3; host.raw(junk.copyOf())
                if (size > 17) { for (n in 9 until 16) junk[n] = 0; junk[16] = 1; host.raw(junk.copyOf()) }
            }
            host.raw(pingBytes.copyOf().also { it[9] = 2 })  // a PONG for no PING
            repeat(5) { host.replay() }
            // Epochs the announcement did not name: dropped, and the session goes on.
            host.video(frame(true, 0, 200, 3000), true)
            host.video(frame(true, 2, 300, 3000), true)
            host.audio(mediaPacket(2, false, 2, 300, 1920))
            Thread.sleep(150)
            check(session.engine.count() == 1, "Hostile data reached the decoder")
            // The receiver asks for a key frame after a future epoch; its number is the current one.
            session.gate.stream(2, 2)
            session.gate.attach(TEST_LEASE, session.engine.offer)
            host.video(frame(true, 2, 400, 3000), true)
            waitFor(2000, "The next epoch's key frame") { session.engine.count() == 2 }
            host.audio(mediaPacket(2, false, 2, 410, 1920))
            host.audio(mediaPacket(2, false, 1, 420, 1920))
            waitFor(2000, "This epoch's audio") { session.engine.count() >= 3 }
            Thread.sleep(100)
            check(session.engine.count() == 3, "A stale epoch's audio passed")
            check(session.latest().running, "A hostile datagram stopped the leg")
        }
    }

    private fun announcedEpochs() = FakeHost().use { host ->
        Session(host).use { session ->
            for (epoch in 2 until 12) {
                val before = session.engine.count()
                host.videoRightAfter({ session.gate.stream(epoch, epoch.toLong()); session.gate.attach(TEST_LEASE, session.engine.offer) }, frame(true, epoch, 100, 2000))
                waitFor(1000, "A key frame right after its announcement") { session.engine.count() == before + 1 }
            }
        }
    }

    private fun fullQueue() = FakeHost().use { host ->
        Session(host).use { session ->
            host.video(frame(true, 1, 100, 3000), true)
            waitFor(2000, "The first key frame") { session.engine.count() == 1 }
            // Full for longer than the leg waits for it (UdpHandoff.RETRY_US).
            session.engine.fullFor(200)
            for (n in 0L until 5L) { host.video(frame(false, 1, 200 + n, 3000), false); Thread.sleep(5) }
            waitFor(2000, "A key frame request") { host.keyRequests.get() > 0 }
            check(session.engine.count() == 1, "P-frames passed the hole a full queue left")
            check(host.lastKeyEpoch.get() == 1, "The request named another epoch")
            waitFor(2000, "The decoder's queue drains") { !session.engine.full() }
            host.video(frame(true, 1, 300, 3000), true)
            waitFor(2000, "The next key frame") { session.engine.count() == 2 }
            host.video(frame(false, 1, 310, 3000), false)
            waitFor(2000, "P-frames after the key frame") { session.engine.count() == 3 }
            check(session.latest().running, "A full queue stopped the leg")
            // Waiting again, a key frame on the other path answers the request.
            val asked = host.keyRequests.get()
            session.engine.fullFor(150)
            host.video(frame(false, 1, 320, 3000), false)
            waitFor(2000, "Another key frame request") { host.keyRequests.get() > asked }
            waitFor(2000, "The decoder's queue drains") { !session.engine.full() }
            check(offerWss(session.gate, session.engine, onWire(frame(true, 1, 330, 100))) == AndroidMediaStatus.OK, "The WSS key frame")
            Thread.sleep(100)
            val settled = host.keyRequests.get()
            Thread.sleep(800)
            check(host.keyRequests.get() == settled, "The leg kept asking after WSS brought the key frame")
        }
    }

    // A decoder slow to start is waited for, and one that refuses every key frame does not
    // get a new IDR every round trip (the Host here answers each request at once).
    private fun slowDecoder() = FakeHost().use { host ->
        Session(host).use { session ->
            session.engine.fullFor(20)
            host.video(frame(true, 1, 100, 3000), true)
            waitFor(2000, "The key frame a starting decoder took late") { session.engine.count() == 1 }
            check(host.keyRequests.get() == 0, "A decoder slow to start asked for a key frame")
            val pts = AtomicLong(1000)
            host.answer = { epoch -> frame(true, epoch, pts.getAndAdd(16667), 3000) }
            session.engine.stopped = true
            host.video(frame(false, 1, 200, 3000), false)
            Thread.sleep(2000)
            val asked = host.keyRequests.get().toLong()
            summary["refusingDecoderKeyRequestsIn2s"] = asked
            summary["refusingDecoderRefusals"] = session.engine.refusedVideo.toLong()
            check(asked in 2..6, "A decoder that refuses everything asked for $asked key frames in 2 s")
        }
    }

    // A resize with UDP active: the second key frame lands while the decoder restarts, and
    // the first one, on WSS, makes WSS active. Every later UDP P-frame is refused, and only
    // a new key frame from the Host can end that.
    private fun refusedSwitchKey() = FakeHost().use { host ->
        Session(host).use { session ->
            host.video(frame(true, 1, 1000, 3000), true)
            waitFor(2000, "UDP active") { session.gate.view().active == 1 }
            session.gate.stream(2, 2)
            session.gate.attach(0, null)
            host.video(frame(true, 2, 40000, 3000), true)
            Thread.sleep(150)
            session.gate.attach(TEST_LEASE, session.engine.offer)
            check(offerWss(session.gate, session.engine, onWire(frame(true, 2, 10000, 100))) == AndroidMediaStatus.OK, "The WSS key frame")
            check(session.gate.view().active == 0, "WSS did not take over")
            val asked = host.keyRequests.get()
            for (n in 0L until 8L) { host.video(frame(false, 2, 60000 + n * 16667, 3000), false); Thread.sleep(16) }
            waitFor(2000, "A key frame request after refused frames") { host.keyRequests.get() > asked }
            check(host.lastKeyEpoch.get() == 2, "The request named another epoch")
            val before = session.engine.count()
            host.video(frame(true, 2, 400000, 3000), true)
            waitFor(2000, "The key frame that ends it") { session.engine.count() == before + 1 }
            check(session.gate.view().active == 1, "UDP did not take the path back")
        }
    }

    private fun givenUp() = FakeHost().use { host ->
        Session(host).use { session ->
            host.video(frame(true, 1, 100, 3000), true)
            waitFor(2000, "The first key frame") { session.engine.count() == 1 }
            // A frame and its repairs all lost: NACK runs out, then the key frame request.
            host.dropDownPerMille = 1000
            host.video(frame(false, 1, 200, 3000), false)
            waitFor(3000, "The give-up's key frame request") { host.keyRequests.get() > 0 }
            host.dropDownPerMille = 0
            host.video(frame(false, 1, 300, 3000), false)
            Thread.sleep(100)
            check(session.engine.count() == 1, "A P-frame passed a frame given up")
            waitFor(1000, "The gap in the statistics") { session.latest().gaps > 0 }
            host.video(frame(true, 1, 400, 3000), true)
            waitFor(2000, "The key frame after the gap") { session.engine.count() == 2 }
            check(session.latest().running, "A gap stopped the leg")
        }
    }

    private fun phase2Only() = FakeHost().use { host ->
        // A Broker that never forwards udp.key: the probe runs, no media path changes.
        val engine = FakeEngine()
        val gate = UdpPathGate()
        gate.attach(TEST_LEASE, engine.offer); gate.disengage(TEST_LEG); gate.stream(1, 1)
        val leg = UdpMediaLeg(grantFor(host.port), gate, TEST_LEG, 0)
        try {
            waitFor(9000, "The probe passed") { leg.read().publicKey != null }
            val reading = leg.read()
            check(reading.report.lossPct == 0 && reading.report.rttP50Ms >= 0 && !reading.media && reading.publicKey != null, "Phase 2 numbers")
            val packet = onWire(frame(false, 1, 5, 200))
            check(offerWss(gate, engine, packet) == AndroidMediaStatus.OK && engine.count() == 1 && engine.copy()[0].contentEquals(packet), "The WSS path changed without keys")
            check(gate.view().active == 0 && gate.view().fallbacks == 0L, "A path changed without keys")
        } finally { leg.stop(); leg.join(2000) }
    }

    // Wake-ups of an idle leg: the loop waits for its next deadline, not a fixed 2 ms.
    private fun idlePasses() {
        FakeHost().use { host ->
            // Probe only (no udp.key, or media not allowed): keepalives every 20 s.
            val leg = UdpMediaLeg(grantFor(host.port), UdpPathGate(), TEST_LEG, 0)
            try {
                waitFor(9000, "The probe passed") { leg.read().publicKey != null }
                val before = leg.passes(); Thread.sleep(1000); val probing = leg.passes() - before
                summary["probeOnlyPassesPerSecond"] = probing
                check(probing <= 8, "Probe only: $probing loop passes in an idle second")
            } finally { leg.stop(); leg.join(2000) }
        }
        FakeHost().use { host ->
            // Media, idle screen: FEEDBACK every 100 ms and the Host's TAIL every 100 ms.
            Session(host).use { session ->
                val before = session.leg.passes(); Thread.sleep(1000); val idle = session.leg.passes() - before
                summary["mediaIdlePassesPerSecond"] = idle
                check(idle <= 75, "Media idle: $idle loop passes in an idle second")
            }
        }
    }

    private fun firstRelayClosed() = FakeHost().use { host ->
        val closed = DatagramSocket(InetSocketAddress(InetAddress.getLoopbackAddress(), 0))
        val port = closed.localPort
        closed.close()
        val leg = UdpMediaLeg(grantFor(port, host.port), UdpPathGate(), TEST_LEG, 0)
        try {
            waitFor(11000, "The next relay answered") { leg.read().report.code != 0 }
            val reading = leg.read()
            check(reading.report.code == UdpProbe.PROBE_REACHABLE && reading.report.relay == 1 && reading.running, "An ICMP answer from a closed port broke the socket: ${reading.report}")
        } finally { leg.stop(); leg.join(2000) }
    }

    private fun lateHost() = FakeHost().use { host ->
        host.firstPingsDropped.set(3)
        val leg = UdpMediaLeg(grantFor(host.port), UdpPathGate(), TEST_LEG, 0)
        try {
            waitFor(9000, "The burst") { leg.read().report.code != 0 }
            val reading = leg.read()
            check(reading.report.code == UdpProbe.PROBE_REACHABLE && reading.report.lossPct == 0 && host.pings.get() == 3 + 1 + 25, "A Host that binds late cost loss")
        } finally { leg.stop(); leg.join(2000) }
    }

    private fun failures() {
        FakeHost(FakeHost.Mode.SILENT).use { host ->
            val leg = UdpMediaLeg(grantFor(host.port), UdpPathGate(), TEST_LEG, 0)
            waitFor(7000, "The BIND budget") { !leg.read().running }
            check(leg.read().report.code == UdpProbe.PROBE_BIND_TIMEOUT, "4 from a silent relay")
        }
        run {
            val leg = UdpMediaLeg(grantFor(9), UdpPathGate(), TEST_LEG, 0, lookup = { null })
            waitFor(7000, "The BIND budget") { !leg.read().running }
            check(leg.read().report.code == UdpProbe.PROBE_LOOKUP_FAILED, "3 from a failed name")
        }
        FakeHost(FakeHost.Mode.ACK_ONLY).use { host ->
            val leg = UdpMediaLeg(grantFor(host.port), UdpPathGate(), TEST_LEG, 0)
            try {
                waitFor(5000, "No PONG") { leg.read().report.code != 0 }
                val reading = leg.read()
                check(reading.report.code == UdpProbe.PROBE_NO_PONG && reading.report.lossPct == 100 && reading.running, "5, and keepalives go on")
            } finally { leg.stop(); leg.join(2000) }
        }
        // Crypto that fails its self-check: nothing goes out, the ticket is gone, and the
        // reading says so (AndroidUdp answers poll with udp_leg_failed, Dart's udpLegFailed).
        FakeHost().use { host ->
            val grant = grantFor(host.port)
            val leg = UdpMediaLeg(grant, UdpPathGate(), TEST_LEG, 0, selfCheck = { false })
            waitFor(2000, "The leg ends") { !leg.read().running }
            Thread.sleep(300)
            val reading = leg.read()
            check(reading.failed && reading.report.code == 0 && reading.publicKey == null, "A failed self-check did not read as failed")
            check(host.binds.get() == 0 && host.pings.get() == 0, "A leg that failed its self-check sent UDP")
            check(grant.ticket.all { it.toInt() == 0 }, "A failed self-check kept the ticket")
        }
    }

    private fun teardown() {
        fun ends(leg: UdpMediaLeg, what: String) {
            val began = System.nanoTime()
            leg.stop(); leg.stop()
            leg.join(500)
            val ms = (System.nanoTime() - began) / 1000000
            check(!leg.read().running && ms < 500, "$what took $ms ms")
        }
        // A name lookup that never returns stays behind.
        run {
            val release = AtomicBoolean(false)
            val leg = UdpMediaLeg(grantFor(9), UdpPathGate(), TEST_LEG, 0, lookup = { while (!release.get()) Thread.sleep(5); null })
            Thread.sleep(30)
            ends(leg, "Teardown over a stuck lookup")
            release.set(true)
        }
        FakeHost(FakeHost.Mode.SILENT).use { host ->
            val leg = UdpMediaLeg(grantFor(host.port), UdpPathGate(), TEST_LEG, 0)
            Thread.sleep(50)
            ends(leg, "Stop while binding")
        }
        FakeHost().use { host ->
            val leg = UdpMediaLeg(grantFor(host.port), UdpPathGate(), TEST_LEG, 0)
            waitFor(3000, "The burst") { host.pings.get() > 2 }
            ends(leg, "Stop in the burst")
        }
        // During media: detach, then stop the engine, with frames still arriving.
        FakeHost().use { host ->
            Session(host, 1, false).use { session ->
                val go = AtomicBoolean(true)
                val feed = Thread {
                    var pts = 1L
                    while (go.get()) { host.video(frame(pts % 30 == 1L, 1, pts, 3000), pts % 30 == 1L); host.audio(mediaPacket(2, false, 1, pts, 1920)); ++pts; Thread.sleep(1) }
                }.apply { start() }
                waitFor(3000, "Media flowing") { session.engine.count() > 20 }
                session.gate.attach(0, null)
                session.engine.stopped = true
                Thread.sleep(100)
                check(session.engine.afterStop == 0, "An offer reached a stopped engine")
                session.gate.attach(TEST_LEASE, session.engine.offer)  // the leg outlives a decoder restart
                session.engine.stopped = false
                ends(session.leg, "Stop during media")
                go.set(false); feed.join()
            }
        }
        // A Host silent from the start of media: the leg just keeps its timer.
        FakeHost().use { host ->
            Session(host).use { session ->
                host.dropUpPerMille = 1000
                Thread.sleep(150)
                check(session.latest().running, "Silence stopped the leg")
                ends(session.leg, "Stop with a silent Host")
            }
        }
        // A stopped leg cannot reach the decoder even before its thread ends: the gate names the current leg.
        FakeHost().use { host ->
            Session(host).use { session ->
                host.video(frame(true, 1, 100, 3000), true)
                waitFor(2000, "The first key frame") { session.engine.count() == 1 }
                session.gate.disengage(TEST_LEG + 1); session.gate.engage(true)
                host.video(frame(true, 1, 200, 3000), true)
                Thread.sleep(200)
                check(session.engine.count() == 1, "A replaced leg's message reached the decoder")
            }
        }
    }

    private val summary: MutableMap<String, Long> = Collections.synchronizedMap(LinkedHashMap())

    // On the JVM android.os.Process is a stub that throws: every loopback case runs past
    // the leg's guarded priority call, so a failing call cannot stop a leg.
    @Test fun priorityCallFailsHere() {
        assertTrue(runCatching { android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY) }.isFailure)
    }

    @Test fun waitUntilTheNextDeadline() {
        val cap = UdpMediaLeg.MEDIA_WAIT_MS
        assertEquals(UdpMediaLeg.MIN_WAIT_MS, udpWaitMs(-5000, cap))
        assertEquals(UdpMediaLeg.MIN_WAIT_MS, udpWaitMs(0, cap))
        assertEquals(UdpMediaLeg.MIN_WAIT_MS, udpWaitMs(500, cap))
        assertEquals("Rounded up: a wait never ends before its deadline", 3L, udpWaitMs(2500, cap))
        assertEquals(cap, udpWaitMs(19001, cap))
        assertEquals(cap, udpWaitMs(5000000, cap))
        assertEquals(UdpMediaLeg.IDLE_WAIT_MS, udpWaitMs(Long.MAX_VALUE - 1, UdpMediaLeg.IDLE_WAIT_MS))
        assertTrue(UdpMediaLeg.IDLE_WAIT_MS >= 100 && UdpMediaLeg.MEDIA_WAIT_MS in 10..20)
    }

    @Test fun overLoopback() {
        // Side by side, as the C++ test runs them: each probe burst takes 6 s.
        val cases = listOf<Pair<String, () -> Unit>>("media with loss" to ::mediaWithLoss, "idle feedback" to ::idleFeedbackAndBind, "paths" to ::pathsOverSockets,
            "hostile datagrams" to ::hostileDatagrams, "announced epochs" to ::announcedEpochs, "full queue" to ::fullQueue, "slow decoder" to ::slowDecoder, "refused switch key" to ::refusedSwitchKey,
            "given up" to ::givenUp, "phase 2 only" to ::phase2Only, "idle passes" to ::idlePasses, "closed relay" to ::firstRelayClosed, "late Host" to ::lateHost, "failures" to ::failures, "teardown" to ::teardown)
        val failed = Collections.synchronizedList(ArrayList<String>())
        val threads = cases.map { (name, case) -> Thread { try { case() } catch (failure: Throwable) { failed.add("$name: $failure") } }.apply { start() } }
        threads.forEach { it.join() }
        println("udp-media-leg-test loopback ${cases.size} cases, failed ${failed.size}, $summary, realNetwork=false realHost=false realRelay=false")
        assertEquals(failed.joinToString("\n"), 0, failed.size)
    }
}
