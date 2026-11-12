package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

// AndroidOpusPipe with a stand-in for MediaCodec: the worker's side of an Opus
// decoder made on another thread (making it on the worker delays the start
// answer and the video decoder by 130-190 ms). Codecs are names;
// "made" is whatever the test hands to opened(), when it chooses.
class AndroidOpusPipeTest {
    private class Input(val codec: String, val index: Int, val pts: Long)

    private class Codecs : AndroidOpusPipe.Codecs<String> {
        val asked = ArrayList<AndroidOpusPipe.Attempt<String>>()
        val inputs = ArrayList<Input>()
        val retired = ArrayList<String>()
        var refuse = false
        override fun open(attempt: AndroidOpusPipe.Attempt<String>): Boolean { asked.add(attempt); return true }
        override fun input(codec: String, index: Int, bytes: ByteArray, offset: Int, length: Int, pts: Long) {
            if (refuse) throw IllegalStateException("refused")
            inputs.add(Input(codec, index, pts))
        }
        override fun retire(codec: String) { retired.add(codec) }
    }

    private val codecs = Codecs()
    private val played = ArrayList<Long>()
    private val pipe = AndroidOpusPipe(codecs) { _, _, pts, _ -> played.add(pts) }
    private val pcm = ByteArray(AndroidOpus.FRAME_BYTES)

    /** A sequence number and a 10 ms CELT packet (the pipe trusts admission's check). */
    private fun packet(sequence: Int, pts: Long = sequence * AndroidOpus.FRAME_US) {
        val payload = byteArrayOf(sequence.toByte(), (sequence shr 8).toByte(), 0xF4.toByte(), 1, 2, 3)
        pipe.packet(payload, 0, payload.size, pts, pts * 1000)
    }

    private fun fed() = codecs.inputs.map { it.pts }

    @Test
    fun startAsksWithoutMakingAndEarlyPacketsGoInOnceMade() {
        pipe.prepare()
        val attempt = codecs.asked.single()
        for (n in 0 until 3) packet(n)
        assertTrue("nothing goes in before the decoder is there", codecs.inputs.isEmpty())
        // MediaCodec offers inputs right after start(), before the worker hears it was made.
        pipe.inputAvailable(attempt, 0)
        pipe.inputAvailable(attempt, 1)
        assertTrue(codecs.inputs.isEmpty())
        assertFalse(pipe.current(attempt))
        pipe.opened(attempt, "opus", "c2.android.opus.decoder", none = false)
        assertTrue(pipe.current(attempt))
        assertEquals("c2.android.opus.decoder", pipe.name)
        assertEquals(listOf(0L, 10_000L), fed())
        assertEquals(listOf(0, 1), codecs.inputs.map { it.index })
        pipe.inputAvailable(attempt, 2)
        assertEquals(listOf(0L, 10_000L, 20_000L), fed())
        // A packet with a free input goes straight in.
        pipe.inputAvailable(attempt, 3)
        packet(3)
        assertEquals(30_000L, fed().last())
        // Their PCM plays in order through the stream.
        for (n in 0 until 4) pipe.decoded(attempt, n * AndroidOpus.FRAME_US, pcm, pcm.size)
        assertEquals(listOf(0L, 10_000L, 20_000L, 30_000L), played)
        assertEquals(1, codecs.asked.size)
    }

    @Test
    fun aPacketBeforeAnyStartAsksForADecoderAndWaitsForIt() {
        packet(0)
        val attempt = codecs.asked.single()
        packet(1)
        pipe.inputAvailable(attempt, 5)
        pipe.opened(attempt, "opus", "opus", none = false)
        assertEquals(listOf(0L), fed())
        pipe.inputAvailable(attempt, 6)
        assertEquals(listOf(0L, 10_000L), fed())
    }

    @Test
    fun atMostMaxPendingWaitTheOldestGo() {
        pipe.prepare()
        val attempt = codecs.asked.single()
        val count = AndroidOpusPipe.MAX_PENDING + 4
        for (n in 0 until count) packet(n)
        pipe.opened(attempt, "opus", "opus", none = false)
        for (n in 0 until count) pipe.inputAvailable(attempt, n)
        assertEquals((4 until count).map { it * AndroidOpus.FRAME_US }, fed())
        // The dropped ones made no output; the first that did plays, with no concealment before it.
        pipe.decoded(attempt, 4 * AndroidOpus.FRAME_US, pcm, pcm.size)
        assertEquals(listOf(4 * AndroidOpus.FRAME_US), played)
        assertEquals(0L, pipe.stream.concealed)
    }

    @Test
    fun aDecoderGivenUpWhileItIsMadeIsReleasedWhenItComes() {
        pipe.prepare()
        val attempt = codecs.asked.single()
        packet(0)
        pipe.close()
        pipe.inputAvailable(attempt, 0)
        pipe.opened(attempt, "late", "late", none = false)
        assertEquals(listOf("late"), codecs.retired)
        assertTrue(codecs.inputs.isEmpty())
        assertFalse(pipe.current(attempt))
        pipe.decoded(attempt, 0, pcm, pcm.size)
        assertTrue(played.isEmpty())
        packet(1)
        assertEquals(2, codecs.asked.size)
    }

    @Test
    fun aDecoderThatFailsBeforeItIsHandedOverIsReplaced() {
        pipe.prepare()
        val first = codecs.asked.single()
        packet(0)
        pipe.codecFailed(first)
        pipe.opened(first, "broken", "broken", none = false)
        assertEquals(listOf("broken"), codecs.retired)
        packet(1)
        val second = codecs.asked.last()
        assertEquals(2, codecs.asked.size)
        pipe.opened(second, "opus", "opus", none = false)
        pipe.inputAvailable(second, 0)
        assertEquals(listOf(10_000L), fed())
        assertEquals("opus", codecs.inputs.single().codec)
    }

    @Test
    fun noDecoderIsAskedAgainAtTheNextPacketAndGivenUpAfterMaxFailures() {
        var sequence = 0
        repeat(AndroidOpusPipe.MAX_FAILURES + 1) {
            packet(sequence++)
            pipe.opened(codecs.asked.last(), null, "", none = false)
        }
        assertTrue(pipe.failed)
        val asked = codecs.asked.size
        packet(sequence)
        assertEquals("a stream without sound asks no more", asked, codecs.asked.size)
    }

    @Test
    fun noOpusDecoderListedGivesUpAtOnce() {
        pipe.prepare()
        pipe.opened(codecs.asked.single(), null, "", none = true)
        assertTrue(pipe.failed)
        packet(0)
        assertEquals(1, codecs.asked.size)
    }

    @Test
    fun aRefusedInputReplacesTheDecoder() {
        pipe.prepare()
        val first = codecs.asked.single()
        pipe.opened(first, "opus", "opus", none = false)
        pipe.inputAvailable(first, 0)
        codecs.refuse = true
        packet(0)
        assertEquals(listOf("opus"), codecs.retired)
        codecs.refuse = false
        packet(1)
        assertEquals(2, codecs.asked.size)
    }

    @Test
    fun theHostEncoderStartingOverAsksForANewDecoderAndThatPacketWaitsForIt() {
        pipe.prepare()
        val first = codecs.asked.single()
        pipe.opened(first, "first", "first", none = false)
        for (n in 0 until 3) pipe.inputAvailable(first, n)
        for (n in 100 until 102) packet(n)
        packet(0, pts = 5_000_000)
        assertEquals(listOf("first"), codecs.retired)
        val second = codecs.asked.last()
        assertEquals(2, codecs.asked.size)
        pipe.inputAvailable(second, 9)
        pipe.opened(second, "second", "second", none = false)
        assertEquals(5_000_000L, fed().last())
        assertEquals("second", codecs.inputs.last().codec)
        pipe.decoded(second, 5_000_000, pcm, pcm.size)
        assertEquals(listOf(5_000_000L), played)
    }

    @Test
    fun anUnusableOutputFormatPassesThatDecoderOverForTheNext() {
        // The first decoder with an output format that cannot play must not end the sound.
        pipe.prepare()
        val attempt = codecs.asked.single()
        assertTrue(attempt.skip.isEmpty())
        pipe.opened(attempt, "vendor", "c2.vendor.opus.decoder", none = false)
        pipe.unusable(attempt)
        assertFalse("the next decoder is asked for, the sound goes on", pipe.failed)
        assertEquals(listOf("vendor"), codecs.retired)
        packet(0)
        val next = codecs.asked.last()
        assertEquals(2, codecs.asked.size)
        assertEquals(setOf("c2.vendor.opus.decoder"), next.skip)
        pipe.opened(next, "software", "c2.android.opus.decoder", none = false)
        pipe.inputAvailable(next, 0)
        assertEquals(listOf(0L), fed())
        pipe.decoded(next, 0, pcm, pcm.size)
        assertEquals(listOf(0L), played)
    }

    @Test
    fun withEveryDecoderPassedOverTheSoundEndsNotTheStream() {
        pipe.prepare()
        val attempt = codecs.asked.single()
        pipe.opened(attempt, "opus", "c2.android.opus.decoder", none = false)
        pipe.unusable(attempt)
        packet(0)
        // The maker finds nothing outside the skip set: none.
        pipe.opened(codecs.asked.last(), null, "", none = true)
        assertTrue(pipe.failed)
        val asked = codecs.asked.size
        packet(1)
        assertEquals(asked, codecs.asked.size)
    }
}

// AndroidOpus.outputShape / toStereo16: what a decoder puts out becomes the track's s16 stereo.
class AndroidOpusOutputTest {
    @Test
    fun shapes() {
        assertEquals(PcmShape(2, false), AndroidOpus.outputShape(48_000, 2, null))
        assertEquals(PcmShape(2, false), AndroidOpus.outputShape(48_000, 2, AndroidOpus.PCM_16))
        assertEquals(PcmShape(2, true), AndroidOpus.outputShape(48_000, 2, AndroidOpus.PCM_FLOAT))
        assertEquals(PcmShape(1, false), AndroidOpus.outputShape(48_000, 1, null))
        assertEquals(null, AndroidOpus.outputShape(44_100, 2, null))
        assertEquals(null, AndroidOpus.outputShape(48_000, 6, null))
        assertEquals(null, AndroidOpus.outputShape(48_000, 2, 3 /* 8-bit */))
    }

    private fun s16(vararg samples: Int) = ByteArray(samples.size * 2).also { out ->
        samples.forEachIndexed { i, v -> out[2 * i] = v.toByte(); out[2 * i + 1] = (v shr 8).toByte() }
    }

    private fun f32(vararg samples: Float) = ByteArray(samples.size * 4).also { out ->
        samples.forEachIndexed { i, v ->
            val bits = java.lang.Float.floatToIntBits(v)
            for (b in 0 until 4) out[4 * i + b] = (bits shr (8 * b)).toByte()
        }
    }

    @Test
    fun stereo16IsCopied() {
        val from = s16(1, -2, 300, -32768)
        val to = ByteArray(64)
        assertEquals(8, AndroidOpus.toStereo16(from, from.size, PcmShape(2, false), to))
        assertEquals(from.toList(), to.copyOf(8).toList())
    }

    @Test
    fun monoPlaysOnBothSides() {
        val from = s16(1000, -1000)
        val to = ByteArray(64)
        assertEquals(8, AndroidOpus.toStereo16(from, from.size, PcmShape(1, false), to))
        assertEquals(s16(1000, 1000, -1000, -1000).toList(), to.copyOf(8).toList())
    }

    @Test
    fun floatIsScaledAndClipped() {
        val from = f32(0f, 1f, -1f, 2f, 0.5f, Float.NaN)
        val to = ByteArray(64)
        assertEquals(12, AndroidOpus.toStereo16(from, from.size, PcmShape(2, true), to))
        assertEquals(s16(0, 32767, -32767, 32767, 16384, 0).toList(), to.copyOf(12).toList())
    }

    @Test
    fun partialSamplesOrTooLittleRoomAreRefused() {
        val to = ByteArray(4)
        assertEquals(-1, AndroidOpus.toStereo16(ByteArray(6), 6, PcmShape(2, true), to))
        assertEquals(-1, AndroidOpus.toStereo16(ByteArray(16), 16, PcmShape(2, false), to))
    }
}
