package me.totb.multitap_client

/**
 * What this app run remembers about H.264 decoders that failed before their
 * first frame (AvcDecoders). Pure, so the JVM tests run it.
 *
 * Until 2026-11-10 one such failure sent the decoder to the end of the trial
 * order, which on most devices means the software decoder for the rest of
 * the run. A decoder that was started with any latency tuning (avcTuningFlags
 * bits 1-8) is now first tried once more with the plain format, the
 * platform's defaults: only a failure of that is taken as the decoder's own
 * and demotes it as before.
 *
 * A decoder started ahead of the key frame that fails is, as before, started
 * at the key frame with the parameter sets in its format, tuning unchanged.
 */
internal class AvcFailureMemory {
    private val plain = HashSet<String>()
    private val demoted = HashSet<String>()
    private val formatParameterSets = HashSet<String>()

    /** Decoders moved to the plain format, and decoders moved to the end of the order, this app run. */
    @Volatile var plainFallbacks = 0
        private set
    @Volatile var demotions = 0
        private set

    /** [tuningFlags]: avcTuningFlags of the failed decoder, null when unknown. */
    @Synchronized fun failedBeforeFirstFrame(name: String, early: Boolean, tuningFlags: Int?) {
        if (early) { formatParameterSets.add(name); return }
        val tuned = tuningFlags != null && tuningFlags and TUNED_BITS != 0
        if (tuned && name !in plain && name !in demoted) {
            plain.add(name); plainFallbacks++
        } else if (demoted.add(name)) demotions++
    }

    /** Could not be created or configured ahead of the key frame: it gets the parameter sets in its format. */
    @Synchronized fun needsFormatParameterSets(name: String) { formatParameterSets.add(name) }

    @Synchronized fun startsEarly(name: String) = name !in formatParameterSets
    @Synchronized fun plainOnly(name: String) = name in plain
    @Synchronized fun demoted(): Set<String> = demoted.toSet()

    companion object {
        /** avcTuningFlags bits that change the format: KEY_LOW_LATENCY, priority, operating rate, vendor keys. */
        const val TUNED_BITS = 1 or 2 or 4 or 8
    }
}

/** The configuration attempts of one decoder: the usual ladder, or the plain format alone after it failed tuned. */
internal fun avcTuningsFor(api: Int, lowLatencyDeclared: Boolean, vendorKeys: Boolean, plainOnly: Boolean): List<AvcTuning> =
    if (plainOnly) listOf(AvcTuning(lowLatency = false, priority = false, operatingRate = false, vendor = false))
    else avcTunings(api, lowLatencyDeclared, vendorKeys)
