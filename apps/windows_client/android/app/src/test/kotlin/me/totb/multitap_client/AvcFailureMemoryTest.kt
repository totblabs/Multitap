package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

// AvcFailureMemory: a decoder that fails before its first frame is tried once
// more with the plain format before it is moved behind the others (which is
// usually the software decoder), and every such step is counted.
class AvcFailureMemoryTest {
    private val hardware = "c2.qti.avc.decoder"
    private val software = "c2.android.avc.decoder"
    private val candidates = listOf(AvcDecoderCandidate(hardware, true), AvcDecoderCandidate(software, false))
    private val tuned = 1 or 2 or 4 or 8 or 16

    @Test fun aTunedDecoderThatFailsGetsThePlainFormatAndKeepsItsPlace() {
        val memory = AvcFailureMemory()
        memory.failedBeforeFirstFrame(hardware, early = false, tuningFlags = tuned)
        assertTrue(memory.plainOnly(hardware))
        assertEquals(listOf(hardware, software), orderAvcDecoders(candidates, memory.demoted()))
        assertEquals(1, memory.plainFallbacks)
        assertEquals(0, memory.demotions)
        // Its next start has one attempt: no key at all.
        val attempts = avcTuningsFor(33, lowLatencyDeclared = true, vendorKeys = true, plainOnly = memory.plainOnly(hardware))
        assertEquals(1, attempts.size)
        assertEquals(emptyMap<String, Int>(), avcTuningKeys(attempts.single(), AvcVendorKeys.QUALCOMM.keys, 60))
        assertEquals(16, avcTuningFlags(attempts.single(), lowLatencyDeclared = true))
    }

    @Test fun failingAgainWithThePlainFormatMovesItLastAsBefore() {
        val memory = AvcFailureMemory()
        memory.failedBeforeFirstFrame(hardware, early = false, tuningFlags = tuned)
        // Plain: only the "declared" bit is left, which changes nothing in the format.
        memory.failedBeforeFirstFrame(hardware, early = false, tuningFlags = 16)
        assertEquals(listOf(software, hardware), orderAvcDecoders(candidates, memory.demoted()))
        assertEquals(1, memory.plainFallbacks)
        assertEquals(1, memory.demotions)
        // A third failure is not counted again.
        memory.failedBeforeFirstFrame(hardware, early = false, tuningFlags = 16)
        assertEquals(1, memory.demotions)
    }

    @Test fun aDecoderThatWasNeverTunedIsMovedLastAtOnce() {
        val memory = AvcFailureMemory()
        memory.failedBeforeFirstFrame(hardware, early = false, tuningFlags = 0)
        assertFalse(memory.plainOnly(hardware))
        assertEquals(setOf(hardware), memory.demoted())
        val unknown = AvcFailureMemory()
        unknown.failedBeforeFirstFrame(hardware, early = false, tuningFlags = null)
        assertEquals(setOf(hardware), unknown.demoted())
        assertEquals(0, unknown.plainFallbacks)
    }

    @Test fun aSecondTunedFailureOfADecoderAlreadyOnPlainDemotesIt() {
        // The flags of a decoder are what it was started with; a stale tuned value must not loop.
        val memory = AvcFailureMemory()
        memory.failedBeforeFirstFrame(hardware, early = false, tuningFlags = tuned)
        memory.failedBeforeFirstFrame(hardware, early = false, tuningFlags = tuned)
        assertEquals(setOf(hardware), memory.demoted())
        assertEquals(1, memory.plainFallbacks)
        assertEquals(1, memory.demotions)
    }

    @Test fun anEarlyFailureOnlyAsksForParameterSetsInTheFormat() {
        val memory = AvcFailureMemory()
        assertTrue(memory.startsEarly(hardware))
        memory.failedBeforeFirstFrame(hardware, early = true, tuningFlags = tuned)
        assertFalse(memory.startsEarly(hardware))
        assertFalse(memory.plainOnly(hardware))
        assertTrue(memory.demoted().isEmpty())
        assertEquals(0, memory.plainFallbacks + memory.demotions)
        memory.needsFormatParameterSets(software)
        assertFalse(memory.startsEarly(software))
    }

    @Test fun withoutAFailureTheLadderIsTheUsualOne() {
        assertEquals(avcTunings(33, true, true), avcTuningsFor(33, true, true, plainOnly = false))
        assertEquals(avcTunings(29, false, false), avcTuningsFor(29, false, false, plainOnly = false))
    }

    @Test fun decodersAreRememberedApart() {
        val memory = AvcFailureMemory()
        memory.failedBeforeFirstFrame(hardware, early = false, tuningFlags = tuned)
        assertFalse(memory.plainOnly(software))
        memory.failedBeforeFirstFrame(software, early = false, tuningFlags = 2)
        assertTrue(memory.plainOnly(software))
        assertEquals(2, memory.plainFallbacks)
    }
}
