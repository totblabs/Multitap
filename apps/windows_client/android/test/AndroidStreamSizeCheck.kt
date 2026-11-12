package me.totb.multitap_client

/** Actual portable channel parser/policy/generation gate, not MediaCodec/device acceptance. */
fun main() {
    var checks = 0
    fun verify(value: Boolean) { check(value); checks++ }
    fun request(w: Any = 3840, h: Any = 2160, fps: Any = 60, hardware: Any = true, extra: Map<String, Any> = emptyMap()) =
        mapOf("schema" to 1, "tracks" to listOf(mapOf("width" to w, "height" to h, "fps" to fps) + extra), "hardware" to hardware)
    val broad = StreamSizeCandidate("memory.h264", 16384, 16384, 268435456L, 16106127360L, 2) { null }
    val room = 2L * 1024 * 1024 * 1024
    fun accepted(value: Map<String, Any>, candidates: List<StreamSizeCandidate> = listOf(broad), budget: Long = room): Map<String, Any?> {
        val parsed = streamSizeRequest(value) ?: error("fixture request invalid")
        return validateStreamSizeRequest(parsed, candidates, budget)
    }
    verify(accepted(request())["accepted"] == true)
    verify(accepted(request(5120, 1440))["accepted"] == true)
    verify(accepted(request(2880, 5120))["accepted"] == true)
    verify(accepted(request(7680, 4320))["accepted"] == true)
    verify(legacyStreamSize(StreamSizeTrack(3840, 2160, 60)))
    verify(!legacyStreamSize(StreamSizeTrack(5120, 1440, 60)))
    verify(broad.maxPixelRate > Int.MAX_VALUE.toLong())
    val duplicate = mapOf("schema" to 1, "hardware" to true,
        "tracks" to listOf(mapOf("width" to 3840, "height" to 2160, "fps" to 60), mapOf("width" to 3840, "height" to 2160, "fps" to 60)))
    verify(accepted(duplicate)["accepted"] == true)
    verify(accepted(duplicate, listOf(broad.copy(instances = 1)))["reason"] == "insufficient_instances")
    verify(accepted(duplicate, budget = 500L * 1024 * 1024)["reason"] == "memory_budget")
    verify(accepted(request(), emptyList())["reason"] == "capability_unknown")
    val shape = broad.copy(accepts = { if (it.width == 4096 && it.height == 4096) "unsupported_size" else null })
    verify(accepted(request(4096, 4096), listOf(shape))["accepted"] == false)
    val rate = broad.copy(accepts = { if (it.fps > 30) "unsupported_rate" else null })
    verify(accepted(request(), listOf(rate))["reason"] == "unsupported_rate")
    verify(accepted(request(fps = 30), listOf(rate))["accepted"] == true)
    val profile = broad.copy(accepts = { "unsupported_profile" })
    verify(accepted(request(), listOf(profile))["reason"] == "unsupported_profile")
    val hardware = broad.copy(name = "memory.hardware", accelerated = true)
    val software = broad.copy(name = "memory.software")
    verify(accepted(request(hardware = false), listOf(hardware, software))["decoder"] == "memory.software")
    verify(accepted(request(), listOf(hardware.copy(accepts = { "unsupported_size" }), software))["decoder"] == "memory.software")
    verify(accepted(request(hardware = false), listOf(hardware))["accepted"] == false)
    val throws = broad.copy(accepts = { throw IllegalStateException("PRIVATE_VENDOR_EXCEPTION") })
    val failed = accepted(request(), listOf(throws))
    verify(failed["reason"] == "capability_unknown")
    verify(!failed.toString().contains("PRIVATE_VENDOR_EXCEPTION"))
    val arbitrary = broad.copy(accepts = { "PRIVATE_UNBOUNDED_REASON" })
    verify(accepted(request(), listOf(arbitrary))["reason"] == "capability_unknown")
    val largerSource = request(1920, 1080, extra = mapOf("sourceWidth" to 15360, "sourceHeight" to 8640))
    val parsedSource = streamSizeRequest(largerSource)!!
    verify(accepted(largerSource)["accepted"] == true)
    verify(streamSizeMemoryBytes(parsedSource.tracks) == streamSizeMemoryBytes(listOf(StreamSizeTrack(1920, 1080, 60))))
    val echoed = (accepted(largerSource)["tracks"] as List<*>).single() as Map<*, *>
    verify(echoed["sourceWidth"] == 15360 && echoed["width"] == 1920)
    for (invalid in listOf(request(w = 3840.0), request(w = -2), request(w = 3841), request(fps = 61), request(hardware = "true"),
        request(extra = mapOf("sourceWidth" to 8192)), request(extra = mapOf("sourceWidth" to 1920, "sourceHeight" to 1080)),
        request() + mapOf("secret" to "must-not-be-accepted"))) verify(streamSizeRequest(invalid) == null)
    verify(streamSizeRequest(duplicate + mapOf("schema" to 1.0)) == null)
    verify(streamSizeRequest(duplicate + mapOf("tracks" to List(3) { mapOf("width" to 1920, "height" to 1080, "fps" to 60) })) == null)
    verify(streamSizeMemoryBytes(listOf(StreamSizeTrack(Int.MAX_VALUE - 1, Int.MAX_VALUE - 1, 60))) == null)
    val observation = StreamSizeObservation()
    val idle = observation.snapshot(); verify(observation.current(idle))
    observation.changed(); verify(!observation.current(idle))
    val active = observation.snapshot(); observation.changed(); verify(!observation.current(active))
    val ownership = AndroidMediaOwnership()
    verify(ownership.begin(1)); val pending = observation.snapshot()
    verify(ownership.end(1)); observation.changed(); verify(!ownership.owns(1) && !observation.current(pending))
    verify(ownership.begin(2)); verify(!ownership.end(1) && ownership.owns(2))
    println("android_stream_size checks=$checks failures=0 nativeApi=0 mediaCodecCreated=0 actualFrames=0")
}
