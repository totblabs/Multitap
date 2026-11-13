package me.totb.multitap_client

import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.abs

/**
 * JVM-only test of audio-opus-v1 on Android: AndroidOpusAudio.kt and the Opus
 * rule of androidMediaPacketValid, with packets made by the Host's encoder
 * settings (fixtures/opus-host-fixture.bin from fixtures/opus_host_fixture.c,
 * libopus 1.6.1, with libopus' decode of every packet). MediaCodec is not run
 * here: a stand-in hands back libopus' decode of each packet in input order,
 * trimming the pre-skip from its first output as the platform decoder does
 * with the codec delay this client configures. No device, decoder or audio.
 *
 * Run with the fixture path as the argument (default: the path from
 * apps/windows_client).
 */
private class Fixture(val packets: List<ByteArray>, val pcm: List<ByteArray>, val lookahead: Int)

private fun load(path: String): Fixture {
    val bytes = File(path).readBytes()
    check(String(bytes, 0, 8, Charsets.US_ASCII) == "TVOPUSF1") { "fixture magic" }
    val view = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
    view.position(8)
    val count = view.int
    val lookahead = view.int
    val packets = List(count) { ByteArray(view.short.toInt() and 0xffff).also { view.get(it) } }
    val pcm = List(count) { ByteArray(AndroidOpus.FRAME_BYTES).also { view.get(it) } }
    check(!view.hasRemaining()) { "fixture size" }
    return Fixture(packets, pcm, lookahead)
}

private fun payload(sequence: Int, opus: ByteArray) = ByteArray(2 + opus.size).also {
    it[0] = sequence.toByte(); it[1] = (sequence shr 8).toByte()
    System.arraycopy(opus, 0, it, 2, opus.size)
}

private fun mediaPacket(payload: ByteArray, flags: Int = 0) = ByteArray(12 + payload.size).also {
    it[0] = 2; it[1] = flags.toByte()
    ByteBuffer.wrap(it, 4, 8).order(ByteOrder.LITTLE_ENDIAN).putLong(1000)
    System.arraycopy(payload, 0, it, 12, payload.size)
}

private fun sample(pcm: ByteArray, index: Int): Int = (pcm[index * 2].toInt() and 0xff) or (pcm[index * 2 + 1].toInt() shl 8)

private class Emitted(val pcm: ByteArray, val pts: Long, val accepted: Long)

/**
 * MediaCodec "audio/opus" as configured here: outputs in input order, one per
 * input, [keep] of them still inside, presentation time shifted by [shiftUs];
 * the input numbers in [silent] give no output at all. [anchored]: the
 * output's presentation time as AOSP C2SoftOpusDec stamps it from
 * android16-qpr2 on (the first input's PTS plus the samples put out since,
 * whatever the later inputs' PTS); otherwise the input's, as before it.
 */
private class StandIn(private val fixture: Fixture, private val shiftUs: Long, private val keep: Int, private val silent: Set<Int>, private val anchored: Boolean = false) {
    private val inside = ArrayDeque<LongArray>()
    private var first = true
    private var inputs = 0
    private var anchor = 0L
    private var samplesOut = 0L
    private val buffer = ByteArray(AndroidOpus.FRAME_BYTES * 2)
    /** Every presentation time the decoder was given, in order. */
    val given = ArrayList<Long>()
    fun restart() { inside.clear(); first = true; samplesOut = 0 }
    fun input(payload: ByteArray, pts: Long) {
        val opus = payload.copyOfRange(2, payload.size)
        val index = fixture.packets.indexOfFirst { it.contentEquals(opus) }
        check(index >= 0)
        given.add(pts)
        inside.addLast(longArrayOf(index.toLong(), pts, (inputs++).toLong()))
    }
    fun drain(stream: AndroidOpusStream, out: MutableList<Emitted>, keep: Int = this.keep) {
        while (inside.size > keep) {
            val (index, pts, number) = inside.removeFirst()
            if (number.toInt() in silent) continue
            val skip = if (first) AndroidOpus.PRE_SKIP * 4 else 0
            if (first) anchor = pts
            first = false
            val length = AndroidOpus.FRAME_BYTES - skip
            System.arraycopy(fixture.pcm[index.toInt()], skip, buffer, 0, length)
            val stamped = if (anchored) anchor + samplesOut * 1_000_000L / AndroidOpus.RATE else pts + shiftUs
            samplesOut += length / 4
            stream.decoded(stamped, buffer, length) { pcm, bytes, at, accepted -> out.add(Emitted(pcm.copyOf(bytes), at, accepted)) }
        }
    }
}

private class Run(private val fixture: Fixture, shiftUs: Long = 0, keep: Int = 0, silent: Set<Int> = emptySet(), anchored: Boolean = false) {
    val stream = AndroidOpusStream()
    val codec = StandIn(fixture, shiftUs, keep, silent, anchored)
    val out = ArrayList<Emitted>()
    val actions = ArrayList<Int>()

    /** Fixture packet [index] received as [sequence] with [pts]. */
    fun packet(index: Int, sequence: Int, pts: Long) {
        val bytes = payload(sequence, fixture.packets[index])
        check(AndroidOpus.payloadValid(bytes, 0, bytes.size))
        val action = stream.admit(AndroidOpus.sequence(bytes, 0), pts, pts * 1000)
        actions.add(action)
        if (action == AndroidOpusStream.LATE) return
        if (action == AndroidOpusStream.RESTART) codec.restart()
        // As AndroidOpusPipe: the decoder gets the stream's decoder timestamp, never the Host's PTS.
        codec.input(bytes, stream.admittedDecoderPts)
        codec.drain(stream, out)
    }

    fun finish(): Run { codec.drain(stream, out, 0); return this }
}

private const val BASE = 1_000_000L
private fun pts(index: Int) = BASE + index * AndroidOpus.FRAME_US

fun main(args: Array<String>) {
    var checks = 0
    fun verify(value: Boolean, what: String) { check(value) { what }; checks++ }
    val fixture = load(args.getOrNull(0) ?: "android/test/fixtures/opus-host-fixture.bin")
    val ref = fixture.pcm
    val n = fixture.packets.size
    verify(n == 20 && fixture.lookahead == AndroidOpus.PRE_SKIP, "fixture: 20 packets, the Host encoder's 2.5 ms lookahead is the pre-skip")

    // --- packets: the Host's encoder output ------------------------------
    for ((index, opus) in fixture.packets.withIndex()) {
        verify(AndroidOpus.samples(opus, 0, opus.size) == 480, "packet $index is 10 ms")
        verify((opus[0].toInt() and 0xff) shr 3 == 30, "packet $index is CELT fullband 10 ms (TOC config 30)")
        val media = mediaPacket(payload(index, opus))
        verify(androidMediaPacketValid(media, media.size, opus = true), "packet $index admitted as audio-opus-v1")
    }
    // Without the Opus rule (the released PCM rule: whole 4-byte frames) most are refused.
    val pcmRefused = fixture.packets.withIndex().count { (index, opus) ->
        val media = mediaPacket(payload(index, opus)); !androidMediaPacketValid(media, media.size)
    }
    verify(pcmRefused > 0, "the PCM rule refuses Opus packets ($pcmRefused of $n)")
    val pcm = ByteArray(12 + 1920).also { it[0] = 2 }
    verify(androidMediaPacketValid(pcm, pcm.size) && !androidMediaPacketValid(pcm, pcm.size, opus = true), "PCM is not Opus (too long for one packet)")
    val good = payload(1, fixture.packets[0])
    verify(!androidMediaPacketValid(mediaPacket(good, flags = 1), 12 + good.size, opus = true), "audio flags zero")
    verify(!androidMediaPacketValid(mediaPacket(byteArrayOf(1, 0)), 14, opus = true), "a sequence number without a packet")
    verify(!AndroidOpus.payloadValid(ByteArray(2 + 1276).also { it[2] = 0xF0.toByte() }, 0, 2 + 1276), "longer than one Opus packet")
    verify(AndroidOpus.payloadValid(ByteArray(2 + 1275).also { it[2] = 0xF0.toByte() }, 0, 2 + 1275), "the largest Opus packet")
    val twenty = good.copyOf().also { it[2] = (it[2].toInt() or 0x08).toByte() }   // config 31: CELT FB 20 ms
    verify(!AndroidOpus.payloadValid(twenty, 0, twenty.size), "a 20 ms packet is not audio-opus-v1")
    val two = good.copyOf().also { it[2] = (it[2].toInt() and 0xfc or 1).toByte() } // code 1: two 10 ms frames
    verify(!AndroidOpus.payloadValid(two, 0, two.size), "two frames in one packet")

    // opus_packet_get_nb_samples at 48 kHz.
    fun samples(vararg bytes: Int) = AndroidOpus.samples(ByteArray(bytes.size) { bytes[it].toByte() }, 0, bytes.size)
    verify(samples(0x00, 0) == 480 && samples(0x08, 0) == 960 && samples(0x18, 0) == 2880, "SILK 10/20/60 ms")
    verify(samples(0x60, 0) == 480 && samples(0x68, 0) == 960, "hybrid 10/20 ms")
    verify(samples(0x80, 0) == 120 && samples(0x88, 0) == 240 && samples(0xF0, 0) == 480 && samples(0xF8, 0) == 960, "CELT 2.5/5/10/20 ms")
    verify(samples(0xF1, 0) == 960 && samples(0xF2, 0) == 960, "codes 1 and 2: two frames")
    verify(samples(0xF3, 0x02) == 960 && samples(0x83, 0x30) == 5760, "code 3: the count byte, up to 120 ms")
    verify(samples(0x83, 0x31) == -1 && samples(0x1B, 0x03) == -1, "more than 120 ms is invalid")
    verify(samples(0xF3) == -1 && AndroidOpus.samples(ByteArray(0), 0, 0) == -1, "code 3 without its count; empty")

    // --- MediaCodec configuration ----------------------------------------
    val csd = AndroidOpus.codecSpecificData(littleEndian = true)
    verify(csd.size == 3 && csd[0].size == 19 && String(csd[0], 0, 8, Charsets.US_ASCII) == "OpusHead", "csd-0 is an OpusHead")
    verify(csd[0].copyOfRange(8, 19).map { it.toInt() and 0xff } == listOf(1, 2, 120, 0, 0x80, 0xBB, 0, 0, 0, 0, 0),
        "version 1, stereo, pre-skip 120, 48 kHz, gain 0, mapping family 0")
    fun long(bytes: ByteArray, little: Boolean) = ByteBuffer.wrap(bytes).order(if (little) ByteOrder.LITTLE_ENDIAN else ByteOrder.BIG_ENDIAN).long
    verify(long(csd[1], true) == 2_500_000L && long(csd[2], true) == 80_000_000L, "codec delay 2.5 ms, seek pre-roll 80 ms (ns)")
    val big = AndroidOpus.codecSpecificData(littleEndian = false)
    verify(long(big[1], false) == 2_500_000L && long(big[2], false) == 80_000_000L && big[0].contentEquals(csd[0]), "native order only changes csd-1/2")
    verify(AndroidOpus.decoderOrder(listOf(AvcDecoderCandidate("c2.vendor.opus.decoder", true), AvcDecoderCandidate("c2.android.opus.decoder", false),
        AvcDecoderCandidate("c2.vendor.opus.decoder.secure", true))) == listOf("c2.android.opus.decoder", "c2.vendor.opus.decoder"),
        "the platform's software decoder first, never a secure one")

    // --- round trip: no loss ---------------------------------------------
    val clean = Run(fixture).apply { for (i in 0 until n) packet(i, i, pts(i)) }.finish()
    verify(clean.out.size == n && clean.stream.concealed == 0L && clean.stream.packets == n.toLong(), "every packet once, nothing concealed")
    verify(clean.out[0].pcm.contentEquals(ref[0].copyOfRange(AndroidOpus.PRE_SKIP * 4, 1920)), "first output: libopus' decode after the pre-skip")
    verify((1 until n).all { clean.out[it].pcm.contentEquals(ref[it]) }, "then libopus' decode, unchanged and in order")
    verify((0 until n).all { clean.out[it].pts == pts(it) && clean.out[it].accepted == pts(it) * 1000 }, "each with its packet's PTS and arrival")
    verify(clean.out.sumOf { it.pcm.size } / 4 == n * 480 - 120, "${n * 10} ms of sound less the 2.5 ms pre-skip")

    // A decoder answering three packets late, its PTS shifted by 2.5 ms: the same.
    val lagging = Run(fixture, shiftUs = 2_500, keep = 3).apply { for (i in 0 until n) packet(i, i, pts(i)) }.finish()
    verify(lagging.out.size == n && (0 until n).all { lagging.out[it].pcm.contentEquals(clean.out[it].pcm) && lagging.out[it].pts == pts(it) },
        "a later, shifted output is matched to its packet")

    // --- one packet lost -------------------------------------------------
    fun lose(vararg lost: Int, keep: Int = 0) = Run(fixture, keep = keep).apply { for (i in 0 until n) if (i !in lost) packet(i, i, pts(i)) }.finish()
    // The first lost frame: the last decoded one backwards in time under a half-cosine fade to
    // silence (computed here, independently of PcmFade); any further lost frame is silence.
    fun mirrored(from: ByteArray, s: Int): Int {
        val frame = s / 2
        val gain = 0.5 * (1 + Math.cos(Math.PI * (frame + 1) / 480))
        return Math.round(sample(from, (479 - frame) * 2 + s % 2) * gain).toInt()
    }
    fun concealment(frame: ByteArray, from: ByteArray, k: Int) =
        if (k > 0) frame.all { it.toInt() == 0 } else (0 until 960).all { abs(sample(frame, it) - mirrored(from, it)) <= 1 }
    val one = lose(5)
    verify(one.out.size == n && one.stream.concealed == 1L && one.stream.unfilled == 0L, "a lost packet is concealed: still 20 frames")
    verify(one.out[5].pts == pts(5) && concealment(one.out[5].pcm, ref[4], 0), "packet 4 mirrored at its time, fading 1 -> 0")
    verify(abs(sample(one.out[5].pcm, 0) - sample(ref[4], 958)) <= 1 && abs(sample(one.out[5].pcm, 1) - sample(ref[4], 959)) <= 1 &&
        sample(one.out[5].pcm, 958) == 0 && sample(one.out[5].pcm, 959) == 0, "it starts where the sound was (no edge) and ends silent")
    verify(!one.out[5].pcm.contentEquals(ref[4]), "the lost frame is not a copy of the one before it")
    verify(sample(one.out[6].pcm, 0) == 0 && sample(one.out[6].pcm, 1) == 0 &&
        one.out[6].pcm.copyOfRange(480, 1920).contentEquals(ref[6].copyOfRange(480, 1920)), "the next packet fades in from silence over 2.5 ms")
    verify((7 until n).all { one.out[it].pcm.contentEquals(ref[it]) }, "and the rest is unchanged")
    verify(lose(5, keep = 2).out.map { it.pcm.toList() } == one.out.map { it.pcm.toList() }, "the same with the decoder two packets behind")

    // --- 60 ms lost: concealed to silence; 70 ms: left unfilled ------------
    val six = lose(5, 6, 7, 8, 9, 10)
    verify(six.out.size == n && six.stream.concealed == 6L, "six lost packets are concealed")
    verify((0 until 6).all { k -> concealment(six.out[5 + k].pcm, ref[4], k) && six.out[5 + k].pts == pts(5 + k) }, "one mirrored fade-out, then silence over 60 ms")
    verify((5 until 11).count { k -> six.out[k].pcm.any { it.toInt() != 0 } } == 1, "no frame is played more than twice: one made-up frame has sound")
    verify(sample(six.out[10].pcm, 958) == 0 && sample(six.out[10].pcm, 959) == 0, "silent at the end of the 60 ms")
    verify(sample(six.out[11].pcm, 0) == 0 && six.out[11].pcm.copyOfRange(480, 1920).contentEquals(ref[11].copyOfRange(480, 1920)), "sound fades in from silence")
    val seven = lose(5, 6, 7, 8, 9, 10, 11)
    verify(seven.out.size == n - 7 && seven.stream.concealed == 0L && seven.stream.unfilled == 7L, "seven lost packets are left unfilled")
    verify(seven.out[5].pcm.contentEquals(ref[12]) && seven.out[5].pts == pts(12), "the sound resumes as it comes")

    // --- the VM's silence (the pause rule) --------------------------------
    // The Host sends nothing while the VM is silent and numbers sent frames
    // only: no gap, nothing concealed, nothing inserted.
    val pause = 2_000_000L
    val paused = Run(fixture).apply { for (i in 0 until n) packet(i, i, pts(i) + if (i >= 10) pause else 0) }.finish()
    verify(paused.out.size == n && paused.stream.concealed == 0L && (0 until n).all { paused.out[it].pcm.contentEquals(clean.out[it].pcm) }, "a pause is not a loss")
    // A packet lost right after a pause: repeating sound from before it would be wrong.
    val afterPause = Run(fixture).apply { for (i in 0 until n) if (i != 10) packet(i, i, pts(i) + if (i >= 10) pause else 0) }.finish()
    verify(afterPause.stream.concealed == 0L && afterPause.stream.unfilled == 1L && afterPause.out[10].pcm.contentEquals(ref[11]), "a loss across a pause is left unfilled")
    // The Host's loopback timer delays a stamp by up to ~16 ms: still a loss.
    val jitter = Run(fixture).apply { for (i in 0 until n) if (i != 10) packet(i, i, pts(i) + if (i >= 11) 20_000 else 0) }.finish()
    verify(jitter.stream.concealed == 1L && jitter.out.size == n, "a late stamp within 25 ms is still concealed")

    // --- the newer platform decoder (C2SoftOpusDec from android16-qpr2 on) ----
    // It stamps each output as the first input's PTS plus the samples put out
    // since. Given the Host's PTS, which jumps at the VM's pauses, every output
    // after the first pause was older than any waiting packet and was dropped:
    // no sound for the rest of the stream (field data, androidApi 37).
    val dts = List(n) { AndroidOpusStream.DECODER_START_US + it * AndroidOpus.FRAME_US }
    verify(paused.codec.given == dts, "the decoder is given even timestamps from 1 s across the pause, never the Host's PTS (never 0)")
    val anchoredPause = Run(fixture, anchored = true).apply { for (i in 0 until n) packet(i, i, pts(i) + if (i >= 10) pause else 0) }.finish()
    verify(anchoredPause.out.size == n && anchoredPause.stream.lost == 0L && anchoredPause.stream.concealed == 0L &&
        (0 until n).all { anchoredPause.out[it].pcm.contentEquals(clean.out[it].pcm) }, "a decoder that stamps from its first PTS plays on after the VM's pause")
    verify((0 until n).all { anchoredPause.out[it].pts == pts(it) + if (it >= 10) pause else 0 }, "each frame with its Host PTS (the jitter buffer's pause rule)")
    val jumps = Run(fixture, anchored = true).apply { var offset = 0L; for (i in 0 until n) { if (i % 4 == 0 && i > 0) offset += 1_500_000L + i * 7_000L; packet(i, i, pts(i) + offset) } }.finish()
    verify(jumps.out.size == n && jumps.stream.lost == 0L && (0 until n).all { jumps.out[it].pcm.contentEquals(clean.out[it].pcm) }, "a pause every 40 ms: every frame once, in order")
    val restamped = Run(fixture, anchored = true).apply { for (i in 0 until n) packet(i, i, pts(i) + if (i >= 7) 21_000 else 0) }.finish()
    verify(restamped.out.size == n && restamped.stream.concealed == 0L && restamped.stream.lost == 0L, "the Host's stamps starting over 21 ms later within sound (AudioPtsClock.restartUs)")
    val behindStamp = Run(fixture, anchored = true).apply { for (i in 0 until n) packet(i, i, pts(i) - if (i >= 7) 30_000 else 0) }.finish()
    verify(behindStamp.out.size == n && behindStamp.stream.lost == 0L, "or 30 ms earlier")
    val anchoredLoss = Run(fixture, anchored = true).apply { for (i in 0 until n) if (i != 5) packet(i, i, pts(i)) }.finish()
    verify(anchoredLoss.out.map { it.pcm.toList() } == one.out.map { it.pcm.toList() } && anchoredLoss.stream.concealed == 1L, "a loss is concealed as with the older decoder")
    val anchoredLagging = Run(fixture, keep = 3, anchored = true).apply { for (i in 0 until n) packet(i, i, pts(i) + if (i >= 10) pause else 0) }.finish()
    verify(anchoredLagging.out.map { it.pcm.toList() } == clean.out.map { it.pcm.toList() }, "and with the decoder three packets behind")
    val anchoredRestart = Run(fixture, anchored = true).apply { for (i in 0 until 10) packet(i, 100 + i, pts(i)); for (i in 10 until n) packet(i, i - 10, pts(i) + pause) }.finish()
    verify(anchoredRestart.stream.restarts == 1L && anchoredRestart.out.size == n &&
        anchoredRestart.codec.given.subList(10, n) == dts.subList(0, n - 10), "a new decoder starts its timestamps over")

    // --- late, repeated, reordered ------------------------------------------
    val repeated = Run(fixture).apply { for (i in 0..5) packet(i, i, pts(i)); packet(5, 5, pts(5)); for (i in 6 until n) packet(i, i, pts(i)) }.finish()
    verify(repeated.actions[6] == AndroidOpusStream.LATE && repeated.out.size == n && repeated.stream.late == 1L, "a repeated packet is dropped")
    val reordered = Run(fixture).apply { for (i in 0..5) packet(i, i, pts(i)); packet(7, 7, pts(7)); packet(6, 6, pts(6)); for (i in 8 until n) packet(i, i, pts(i)) }.finish()
    verify(reordered.actions[7] == AndroidOpusStream.LATE && reordered.stream.concealed == 1L && reordered.out.size == n, "6 after 7: concealed, then dropped as late")
    verify(reordered.out[6].pts == pts(6) && concealment(reordered.out[6].pcm, ref[5], 0) && reordered.out[7].pts == pts(7), "in time order")

    // --- the sequence wraps; the Host's encoder starts over -----------------
    val wrap = Run(fixture).apply { for (i in 0 until n) packet(i, (65530 + i) and 0xffff, pts(i)) }.finish()
    verify(wrap.out.size == n && wrap.stream.late == 0L && wrap.stream.concealed == 0L, "65535 -> 0 is the next number")
    val wrapLoss = Run(fixture).apply { for (i in 0 until n) if (i != 6) packet(i, (65530 + i) and 0xffff, pts(i)) }.finish()
    verify(wrapLoss.stream.concealed == 1L && wrapLoss.out.size == n, "a loss at the wrap is concealed")
    val restart = Run(fixture).apply { for (i in 0 until 10) packet(i, 100 + i, pts(i)); for (i in 10 until n) packet(i, i - 10, pts(i)) }.finish()
    verify(restart.actions[10] == AndroidOpusStream.RESTART && restart.stream.restarts == 1L && restart.out.size == n, "numbers far behind: a new decoder")
    verify(restart.out[10].pcm.contentEquals(ref[10].copyOfRange(AndroidOpus.PRE_SKIP * 4, 1920)), "which trims its own pre-skip")
    val behind = Run(fixture).apply { for (i in 0 until 10) packet(i, 100 + i, pts(i)); packet(10, 46, pts(10)) }.finish()
    verify(behind.actions[10] == AndroidOpusStream.LATE && behind.stream.restarts == 0L, "64 behind is still late")

    // --- a decoder that loses or keeps packets ------------------------------
    val swallowed = Run(fixture, silent = setOf(7)).apply { for (i in 0 until n) packet(i, i, pts(i)) }.finish()
    verify(swallowed.out.size == n - 1 && swallowed.stream.lost == 1L && swallowed.out[7].pcm.contentEquals(ref[8]), "an input without output is skipped")
    val stuck = AndroidOpusStream()
    for (i in 0 until 40) stuck.admit(i, pts(i), 0)
    verify(stuck.waiting == AndroidOpusStream.MAX_WAITING && stuck.lost == 8L, "a decoder that answers nothing holds at most 32")
    verify(stuck.admittedDecoderPts == AndroidOpusStream.DECODER_START_US + 39 * AndroidOpus.FRAME_US, "decoder timestamps 10 ms apart")
    verify(!stuck.decoded(AndroidOpusStream.DECODER_START_US, ByteArray(1920), 1920) { _, _, _, _ -> } &&
        stuck.decoded(stuck.admittedDecoderPts, ByteArray(1920), 1920) { _, _, _, _ -> }, "an output older than every waiting packet is no one's")
    verify(!AndroidOpusStream().decoded(BASE, ByteArray(1920), 1920) { _, _, _, _ -> }, "an output before any packet is no one's")

    // --- bitrate ------------------------------------------------------------
    val opusBytes = fixture.packets.sumOf { it.size }.toDouble() / n
    val opusKbps = (opusBytes + 14) * 8 * 100 / 1000
    val pcmKbps = (1920.0 + 12) * 8 * 100 / 1000
    verify(opusKbps < 160 && opusKbps * 10 < pcmKbps, "about a tenth of PCM")
    println("fixture: %.1f Opus bytes/packet, media packets %.1f kbps (PCM %.1f kbps)".format(opusBytes, opusKbps, pcmKbps))
    println("AndroidOpusAudioCheck: $checks checks passed")
}
