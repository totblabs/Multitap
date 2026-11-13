package me.totb.multitap_client

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * JVM-only test of the production media policies (AndroidMediaPacing.kt):
 * frame pacing, decoder latency, buffer reuse, media-wire-v1, packet rules,
 * stale non-reference dropping, display mode choice and the
 * decoder order. No MediaCodec, AudioTrack, Flutter texture or device claim.
 */
private class RecordingSink : AndroidFrameSink {
    val calls = ArrayList<String>()
    override fun render(frame: Int) { calls.add("r$frame") }
    override fun discard(frame: Int) { calls.add("d$frame") }
    fun take(): List<String> = ArrayList(calls).also { calls.clear() }
}

private fun wire(lease: Long, packet: ByteArray, version: Int = 1, flags: Int = 0, at: Int = 0): ByteBuffer {
    val buffer = ByteBuffer.allocate(at + MEDIA_WIRE_HEADER + packet.size).order(ByteOrder.LITTLE_ENDIAN)
    buffer.position(at)
    buffer.put(version.toByte()).put(flags.toByte()).put(ByteArray(6)).putLong(lease).put(packet)
    buffer.position(at)
    return buffer
}

private fun media(kind: Int, length: Int, flags: Int = 0, pts: Long = 1000): ByteArray {
    val bytes = ByteArray(length)
    bytes[0] = kind.toByte(); bytes[1] = flags.toByte()
    ByteBuffer.wrap(bytes, 4, 8).order(ByteOrder.LITTLE_ENDIAN).putLong(pts)
    return bytes
}

private fun nal(vararg units: IntArray): ByteArray =
    units.flatMap { unit -> listOf(0, 0, 0, 1) + unit.toList() }.map { it.toByte() }.toByteArray()

fun main() {
    var checks = 0
    fun verify(value: Boolean, what: String) { check(value) { what }; checks++ }
    val ms = 1_000_000L

    // --- frame pacing -----------------------------------------------------
    val sink = RecordingSink()
    var pacer = AndroidFramePacer(unconsumedLimitNanos = 1500 * ms)
    verify(pacer.presentQueuePackets == 0 && pacer.presentQueueOldestNanos(5 * ms) == 0L, "empty presentation snapshot")
    pacer.decoded(0, 10 * ms, sink)
    verify(pacer.presentQueuePackets == 1 && pacer.presentQueueOldestNanos(15 * ms) == 5 * ms, "rendered buffer awaits texture take")
    verify(pacer.presentQueueOldestNanos(9 * ms) == 0L, "presentation age never negative")
    verify(sink.take() == listOf("r0"), "first frame renders at once")
    pacer.decoded(1, 12 * ms, sink)
    verify(pacer.presentQueuePackets == 2 && pacer.presentQueueOldestNanos(15 * ms) == 5 * ms, "held buffer adds to known queue without changing oldest")
    verify(sink.take().isEmpty() && pacer.queued == 1L, "a frame waits behind one Flutter has not taken")
    pacer.decoded(2, 14 * ms, sink)
    verify(sink.take() == listOf("d1") && pacer.skipped == 1L, "a newer frame replaces the held one")
    pacer.consumed(20 * ms, sink)
    verify(pacer.presentQueuePackets == 1 && pacer.presentQueueOldestNanos(21 * ms) == 7 * ms, "consumption leaves newest held buffer with original decode age")
    verify(sink.take() == listOf("r2"), "taking the waiting frame renders the newest")
    verify(pacer.taken == 1L && pacer.waitTotalNanos == 10 * ms, "wait = decoded -> taken")
    pacer.consumed(40 * ms, sink)
    verify(pacer.presentQueuePackets == 0 && pacer.presentQueueOldestNanos(41 * ms) == 0L, "all consumed resets observed queue")
    verify(pacer.taken == 2L && pacer.waitTotalNanos == 36 * ms && pacer.waitMaxNanos == 26 * ms, "held wait counts from its decode")
    verify(sink.take().isEmpty(), "nothing held: nothing rendered")
    pacer.consumed(41 * ms, sink)
    verify(pacer.taken == 2L, "a take without a waiting frame is ignored")
    pacer.decoded(3, 50 * ms, sink)
    verify(sink.take() == listOf("r3"), "after a take a frame renders at once")
    verify(pacer.checkAt() == -1L, "no fallback once Flutter took frames")
    pacer.decoded(4, 51 * ms, sink); pacer.decoded(5, 52 * ms, sink); pacer.decoded(6, 53 * ms, sink)
    verify(sink.take() == listOf("d4", "d5") && pacer.skipped == 3L, "a burst keeps only the newest")
    verify(!pacer.check(10_000 * ms, sink) && pacer.gated, "never stops holding after a take")
    pacer.consumed(60 * ms, sink)
    verify(sink.take() == listOf("r6") && pacer.rendered == 4L, "the newest of a burst is shown")
    pacer.reset()
    verify(pacer.presentQueuePackets == 0 && pacer.presentQueueOldestNanos(70 * ms) == 0L, "reset drops stale presentation observations")
    pacer.decoded(0, 70 * ms, sink)
    verify(sink.take() == listOf("r0"), "reset forgets the waiting frame")

    // No frame-consumed callbacks: stop holding rather than freeze.
    pacer = AndroidFramePacer(unconsumedLimitNanos = 1500 * ms)
    pacer.decoded(0, 0, sink)
    verify(pacer.checkAt() == -1L, "nothing held: no check due")
    pacer.decoded(1, 100 * ms, sink)
    verify(pacer.checkAt() == 1500 * ms, "check due one limit after the first render")
    verify(!pacer.check(1499 * ms, sink) && pacer.gated && sink.take() == listOf("r0"), "not before the limit")
    verify(pacer.check(1500 * ms, sink) && !pacer.gated, "falls back at the limit")
    verify(pacer.presentQueuePackets == null && pacer.presentQueueOldestNanos(1501 * ms) == null, "callback fallback makes surface queue unknown, not zero")
    verify(sink.take() == listOf("r1"), "the held frame is shown at the fallback")
    pacer.decoded(2, 1600 * ms, sink); pacer.decoded(3, 1601 * ms, sink)
    verify(sink.take() == listOf("r2", "r3") && pacer.skipped == 0L, "ungated: every frame renders")
    pacer.consumed(1610 * ms, sink)
    verify(pacer.taken == 1L, "takes still counted when ungated")

    // --- decoder latency --------------------------------------------------
    val clock = AndroidDecodeClock(capacity = 4)
    clock.queued(100, 1 * ms); clock.queued(200, 2 * ms); clock.queued(300, 3 * ms)
    verify(clock.decoded(200, 7 * ms) == 5 * ms, "latency by presentation time")
    verify(clock.decoded(100, 8 * ms) == -1L, "an input passed over is forgotten")
    verify(clock.decoded(250, 8 * ms) == -1L && clock.decoded(300, 9 * ms) == 6 * ms, "an unknown output keeps later inputs")
    for (pts in 1L..6L) clock.queued(pts * 10, pts * ms)
    verify(clock.decoded(10, 9 * ms) == -1L && clock.decoded(30, 9 * ms) == 6 * ms, "bounded: the oldest inputs drop")
    clock.clear()
    verify(clock.decoded(40, 9 * ms) == -1L, "cleared")

    // Utilisation is the union of in-codec time, not the sum of overlapping latencies.
    val busy = AndroidDecodeClock()
    busy.queued(1, 100 * ms); busy.queued(2, 102 * ms)
    verify(busy.decoded(1, 110 * ms) == 10 * ms && busy.decoded(2, 112 * ms) == 10 * ms, "overlapping latencies are 10 ms each")
    verify(busy.busyNanos == 12 * ms, "...but the decoder was busy 12 ms (sum would be 20)")
    busy.queued(3, 200 * ms); busy.decoded(3, 205 * ms)
    verify(busy.busyNanos == 17 * ms, "idle time between frames is not busy")
    busy.queued(4, 300 * ms)
    busy.queued(5, 5_000 * ms)
    verify(busy.busyNanos == 17 * ms + AndroidDecodeClock.MAX_GAP_NANOS, "an input a codec never outputs counts at most one gap")
    busy.resetBusy()
    verify(busy.busyNanos == 0L, "busy time restarts with a new stream")
    // 40 fps x 5 ms is 20% busy over a second, whatever the latency overlap.
    val rate = AndroidDecodeClock()
    for (frame in 0 until 40) {
        val at = frame * 25 * ms
        rate.queued(frame.toLong(), at); rate.decoded(frame.toLong(), at + 5 * ms)
    }
    verify(rate.busyNanos == 200 * ms, "40 fps at 5 ms decode is 200 per mille")

    // --- buffer reuse -----------------------------------------------------
    val pool = AndroidBufferPool(maxRetainedBytes = 64 * 1024, maxPerClass = 2)
    val small = pool.take(100)
    verify(small.size == 4096, "smallest class 4 KiB")
    pool.give(small)
    verify(pool.take(4000) === small, "a returned buffer is reused")
    verify(pool.take(4097).size == 8192, "power-of-two classes")
    verify(pool.take(4 * 1024 * 1024).size == 4 * 1024 * 1024, "largest relay packet")
    verify(runCatching { pool.take(4 * 1024 * 1024 + 1) }.isFailure, "beyond the largest packet refused")
    val a = pool.take(5000); val b = pool.take(5000); val c = pool.take(5000)
    pool.give(a); pool.give(b); pool.give(c)
    verify(pool.retainedBytes == 2L * 8192, "at most maxPerClass per class")
    pool.give(ByteArray(32 * 1024)); pool.give(ByteArray(32 * 1024))
    verify(pool.retainedBytes == 2L * 8192 + 32 * 1024, "bounded retained bytes")
    pool.give(ByteArray(5000))
    verify(pool.retainedBytes == 2L * 8192 + 32 * 1024, "foreign sizes are not kept")

    // --- media-wire-v1 ----------------------------------------------------
    val video = media(1, 20, flags = 1)
    val parsed = androidMediaWire(wire(7, video))
    verify(parsed != null && parsed.lease == 7L && !parsed.watch && parsed.offset == 16 && parsed.length == 20, "wire header")
    val offset = androidMediaWire(wire(8, video, flags = 1, at = 5))
    verify(offset != null && offset.watch && offset.offset == 21 && offset.length == 20, "wire at a buffer position")
    verify(androidMediaWire(wire(7, video, version = 2)) == null, "unknown version")
    verify(androidMediaWire(wire(7, video, flags = 4)) == null, "unknown flag")
    verify(androidMediaWire(wire(7, video, flags = 3))?.let { it.path && it.watch } == true, "udp-media-v1 path flag")
    verify(androidMediaWire(wire(7, media(2, 16), flags = 2))?.let { it.path && !it.watch } == true, "path on audio")
    verify(parsed?.path == false, "no path flag")
    verify(androidMediaWire(wire(0, video)) == null && androidMediaWire(wire(-1, video)) == null, "lease must be positive")
    verify(androidMediaWire(wire(7, media(2, 16), flags = 1)) == null, "watch on audio refused")
    verify(androidMediaWire(wire(7, media(1, 12))) == null, "packet shorter than 13 bytes")
    verify(androidMediaWire(wire(7, ByteArray(4194305))) == null, "packet beyond 4 MiB")
    verify(androidMediaWire(wire(7, media(1, 4194304)))?.length == 4194304, "4 MiB packet")
    val reserved = wire(7, video).also { it.put(4, 1) }
    verify(androidMediaWire(reserved) == null, "reserved header bytes must be zero")

    // --- media-v1 packet rules ---------------------------------------------
    verify(androidMediaPacketValid(media(1, 13), 13) && androidMediaPacketValid(media(1, 13, flags = 1), 13), "video")
    verify(!androidMediaPacketValid(media(1, 13, flags = 2), 13), "video flag bits")
    verify(androidMediaPacketValid(media(2, 16), 16) && !androidMediaPacketValid(media(2, 15), 15), "whole audio frames")
    verify(!androidMediaPacketValid(media(2, 16, flags = 1), 16), "audio flags zero")
    verify(!androidMediaPacketValid(media(3, 16), 16), "unknown kind")
    verify(!androidMediaPacketValid(media(1, 16).also { it[2] = 1 }, 16), "reserved epoch bytes zero")
    verify(!androidMediaPacketValid(media(1, 16, pts = -1), 16), "negative PTS")
    verify(androidMediaPacketValid(media(1, 4096).also { it[30] = 9 }, 20), "a reused larger buffer: only length counts")
    verify(!androidMediaPacketValid(media(1, 20), 21) && !androidMediaPacketValid(media(1, 20), 12), "length bounds")
    verify(androidMediaPts(media(1, 13, pts = 0x0102030405L)) == 0x0102030405L, "little-endian PTS")

    // --- H.264 units and reference pictures ----------------------------------
    val keyFrame = nal(intArrayOf(0x67, 1, 2), intArrayOf(0x68, 3), intArrayOf(0x65, 4, 5))
    val units = annexBUnits(keyFrame, 0)
    verify(units.size == 3 && units[0].contentEquals(byteArrayOf(0, 0, 0, 1, 0x67, 1, 2)) && units[2].size == 7, "Annex B units")
    val bounded = annexBUnits(keyFrame + byteArrayOf(0, 0, 1, 0x41, 9), 0, keyFrame.size)
    verify(bounded.size == 3 && bounded[2].size == 7, "units end at the packet length")
    val three = byteArrayOf(0, 0, 1, 0x67, 1, 0, 0, 1, 0x68, 2)
    verify(annexBUnits(three, 0).map { it[4].toInt() } == listOf(0x67, 0x68), "3-byte start codes")
    verify(!h264NonReference(keyFrame, 0, keyFrame.size), "IDR is a reference")
    verify(!h264NonReference(nal(intArrayOf(0x41, 1)), 0, 6), "nal_ref_idc 2 P slice is a reference")
    val nonRef = nal(intArrayOf(0x06, 5, 5), intArrayOf(0x01, 1))
    verify(h264NonReference(nonRef, 0, nonRef.size), "nal_ref_idc 0 slice after an SEI")
    verify(!h264NonReference(nonRef, 0, nonRef.size, limit = 6), "scan limit: unknown is kept")
    verify(!h264NonReference(nal(intArrayOf(0x67, 1)), 0, 6), "no slice: kept")

    // --- stale non-reference dropping -----------------------------------------
    class Item(val id: Int, val at: Long, val nonRef: Boolean)
    val dropped = ArrayList<Int>()
    fun queue(vararg items: Item) = ArrayDeque(items.toList())
    var waiting = queue(Item(1, 0, true), Item(2, 1 * ms, true))
    verify(dropStaleNonReference(waiting, 10 * ms, { it.at }, { it.nonRef }, { dropped.add(it.id) }) == 0, "short and fresh: kept")
    verify(dropStaleNonReference(waiting, 60 * ms, { it.at }, { it.nonRef }, { dropped.add(it.id) }) == 1 &&
        dropped == listOf(1) && waiting.single().id == 2, "stale: dropped except the newest")
    dropped.clear()
    waiting = queue(Item(1, 0, false), Item(2, 0, true), Item(3, 0, false), Item(4, 0, true))
    verify(dropStaleNonReference(waiting, 1 * ms, { it.at }, { it.nonRef }, { dropped.add(it.id) }) == 1 &&
        dropped == listOf(2) && waiting.map { it.id } == listOf(1, 3, 4), "deep queue: only non-reference, never the newest")

    // --- display mode -----------------------------------------------------------
    fun mode(id: Int, hz: Float, w: Int = 1080, h: Int = 2400) = AndroidDisplayMode(id, w, h, hz)
    val modes = listOf(mode(1, 60f), mode(2, 90f), mode(3, 120f), mode(4, 120f, w = 720, h = 1600), mode(5, 144f))
    verify(androidStreamDisplayMode(mode(1, 60f), modes) == 3, "60 Hz panel mode -> 120 Hz")
    verify(androidStreamDisplayMode(mode(2, 90f), modes) == 3, "90 -> 120 Hz")
    verify(androidStreamDisplayMode(mode(3, 120f), modes) == null, "already 120 Hz")
    verify(androidStreamDisplayMode(mode(1, 60f), listOf(mode(1, 60f), mode(2, 90f))) == null, "90 Hz alone is not a 60 fps cadence")
    verify(androidStreamDisplayMode(mode(6, 60.000004f), listOf(mode(6, 60.000004f), mode(7, 119.99999f))) == 7, "rounded rates")
    verify(androidStreamDisplayMode(mode(1, 60f, w = 720, h = 1600), modes) == 4, "same resolution only")
    // Holding the best mode also when the display already is in it (adaptive refresh).
    verify(androidStreamHoldMode(mode(3, 120f), modes) == 3, "already 120 Hz: held, not released")
    verify(androidStreamHoldMode(mode(1, 60f), modes) == 3, "60 Hz panel mode -> 120 Hz")
    verify(androidStreamHoldMode(mode(1, 60f), listOf(mode(1, 60f))) == 1, "a 60 Hz-only panel holds 60 Hz")
    verify(androidStreamHoldMode(mode(8, 30f), listOf(mode(8, 30f), mode(9, 60f))) == 9, "a panel at 30 Hz goes to 60 Hz")
    verify(androidStreamHoldMode(mode(8, 30f), listOf(mode(8, 30f))) == null, "no 60/120 Hz mode: nothing to hold")
    verify(androidStreamHoldMode(mode(1, 60f, w = 720, h = 1600), modes) == 4, "hold: same resolution only")
    verify(androidStreamFrameRate(modes, mode(1, 60f)) == 60f, "the stream surface declares 60 fps on a 60/120 Hz panel")
    verify(androidStreamFrameRate(listOf(mode(8, 30f)), mode(8, 30f)) == null, "no declaration without a 60 Hz mode")
    // The request is made again after a fold/rotation/new surface, and only then.
    verify(!androidRefreshNeedsReapply(3, 77, 3, 77, 3), "unchanged: no new request")
    verify(androidRefreshNeedsReapply(3, 77, 3, 78, 3), "a new surface: request again")
    verify(androidRefreshNeedsReapply(3, 77, 4, 77, 4), "another display mode wanted: request again")
    verify(androidRefreshNeedsReapply(3, 77, 3, 77, 1), "the system moved the preference: request again")
    verify(!androidRefreshNeedsReapply(null, 77, null, 77, 1), "no mode to hold and the same surface: nothing")

    // --- decoder order and configuration -------------------------------------------
    val listed = listOf(
        AvcDecoderCandidate("c2.android.avc.decoder", false),
        AvcDecoderCandidate("c2.qti.avc.decoder", true),
        AvcDecoderCandidate("c2.qti.avc.decoder.secure", true),
        AvcDecoderCandidate("c2.mtk.avc.decoder", true),
        AvcDecoderCandidate("c2.qti.avc.decoder", true),
    )
    verify(orderAvcDecoders(listed, emptySet()) == listOf("c2.qti.avc.decoder", "c2.mtk.avc.decoder", "c2.android.avc.decoder"), "hardware first, no secure")
    verify(orderAvcDecoders(listed, setOf("c2.qti.avc.decoder")) == listOf("c2.mtk.avc.decoder", "c2.android.avc.decoder", "c2.qti.avc.decoder"), "failed last")
    verify(avcSoftwareName("OMX.google.h264.decoder") && avcSoftwareName("c2.android.avc.decoder") && !avcSoftwareName("OMX.qcom.video.decoder.avc"), "software names")
    val tunings = avcTunings(33, lowLatencyDeclared = false, vendorKeys = true)
    verify(tunings.size == 4 && tunings[0] == AvcTuning(true, true, true, true) && tunings[1] == AvcTuning(true, true, true, false) &&
        tunings[2] == AvcTuning(false, true, false, false) && tunings[3] == AvcTuning(false, false, false, false), "tuning ladder")
    verify(avcTunings(33, lowLatencyDeclared = true, vendorKeys = false).let { it.size == 3 && it[1] == AvcTuning(true, true, false, false) }, "no vendor keys: no duplicate")
    verify(avcTunings(29, lowLatencyDeclared = true, vendorKeys = false).none { it.lowLatency }, "KEY_LOW_LATENCY needs API 30")
    verify(avcVendorLowLatencyKeys("c2.qti.avc.decoder").size == 2 && avcVendorLowLatencyKeys("C2.Exynos.H264.Decoder").size == 1 &&
        avcVendorLowLatencyKeys("c2.mtk.avc.decoder") == mapOf("vdec-lowlatency" to 1) &&
        avcVendorLowLatencyKeys("c2.android.avc.decoder").isEmpty(), "vendor keys by decoder")

    // --- this app's CPU cost -----------------------------------------------------
    val statLine = "4242 (multitap (main)) S 1 4242 0 0 -1 4194560 100 0 0 0 150 50 0 0 20 0 30 0 99 1000 100"
    verify(parseSelfStatTicks(statLine) == 200L, "utime + stime after a comm with spaces and parentheses")
    verify(parseSelfStatTicks("4242 (x) S 1") == null && parseSelfStatTicks("garbage") == null && parseSelfStatTicks("1 (x) S 1 1 0 0 -1 0 0 0 0 0 a b 0") == null, "unreadable stat")
    val cpu = AppCpuMeter(cores = 8, ticksPerSecond = 100)
    verify(cpu.sample(1000, 0L) == -1, "first reading only primes")
    // 1 s later 100 ticks = 1 CPU-second of 8 = 12.5% -> 125 tenths
    verify(cpu.sample(1100, 1_000_000_000L) == 125, "one core-second of eight")
    verify(cpu.sample(1150, 1_200_000_000L) == 125, "readings under half a second apart hold the value")
    verify(cpu.sample(1660, 2_000_000_000L) == 700, "560 ticks over 1 s on 8 cores")
    verify(cpu.sample(9999, 3_000_000_000L) == 1000, "clamped at 100%")
    verify(cpu.sample(10, 4_000_000_000L) == -1, "counter going back is unknown")
    verify(cpu.sample(null, 5_000_000_000L) == -1 && cpu.tenths == -1, "unreadable proc file is unknown")
    verify(cpu.sample(50, 6_000_000_000L) == -1 && cpu.sample(150, 7_000_000_000L) == 125, "recovers after re-priming")
    verify(AppCpuMeter(0, 100).sample(1, 0L) == -1, "no cores is unknown")
    println("{\"checks\":$checks,\"framePacing\":true,\"decodeClock\":true,\"bufferPool\":true,\"mediaWire\":true,\"staleDrop\":true,\"displayMode\":true,\"decoderOrder\":true,\"appCpu\":true,\"mediaCodecExecuted\":false}")
}
