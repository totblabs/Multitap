package me.totb.multitap_client

/** JVM-only test of the same production gate/parser; no MediaCodec/device claim. */
fun main() {
    val ownership = AndroidMediaOwnership()
    var checks = 0
    fun verify(value: Boolean) { check(value); checks++ }
    verify(!ownership.owns(0))
    verify(!ownership.begin(0))
    verify(!ownership.begin(-1))
    verify(ownership.begin(1))
    verify(ownership.owns(1))
    verify(!ownership.begin(1))
    verify(ownership.begin(2))
    verify(!ownership.owns(1))
    verify(!ownership.end(1))
    verify(ownership.owns(2))
    verify(ownership.end(2))
    verify(!ownership.owns(2))
    verify(!ownership.begin(2))
    verify(ownership.begin(3))
    ownership.clear()
    verify(!ownership.owns(3))
    verify(!ownership.begin(1))
    verify(ownership.begin(4))

    val bytes = ByteArray(13)
    verify(androidMediaPacket(mapOf("lease" to 4, "bytes" to bytes))?.lease == 4L)
    verify(androidMediaPacket(mapOf("lease" to 4L, "bytes" to bytes))?.bytes === bytes)
    // The former native cast accepted the old bare-byte envelope and rejected
    // the current leased map. Test the actual production parser in both cases.
    verify(androidMediaPacket(bytes) == null)
    verify(androidMediaPacket(mapOf("lease" to 4, "bytes" to ByteArray(12))) == null)
    verify(androidMediaPacket(mapOf("lease" to 0, "bytes" to bytes)) == null)
    verify(androidMediaPacket(mapOf("lease" to 4.0, "bytes" to bytes)) == null)
    verify(androidMediaPacket(mapOf("lease" to "4", "bytes" to bytes)) == null)
    verify(androidMediaPacket(mapOf("lease" to 4, "bytes" to listOf(0))) == null)
    verify(androidMediaPacket(mapOf("lease" to 4, "bytes" to bytes, "extra" to true)) == null)
    verify(androidMediaPacket(mapOf("lease" to 4)) == null)
    // input-latency-v1: the shared Dart session adds exactly watch:true to a
    // watched video packet. preview.3 rejected that envelope as invalid_packet.
    val video = ByteArray(13).also { it[0] = 1 }
    verify(androidMediaPacket(mapOf("lease" to 4, "bytes" to video))?.watch == false)
    verify(androidMediaPacket(mapOf("lease" to 4, "bytes" to video, "watch" to true))?.watch == true)
    verify(androidMediaPacket(mapOf("lease" to 4, "bytes" to video, "watch" to false)) == null)
    verify(androidMediaPacket(mapOf("lease" to 4, "bytes" to video, "watch" to "true")) == null)
    verify(androidMediaPacket(mapOf("lease" to 4, "bytes" to video, "watch" to true, "extra" to 1)) == null)
    verify(androidMediaPacket(mapOf("lease" to 4, "watch" to true, "extra" to video)) == null)
    val audio = ByteArray(16).also { it[0] = 2 }
    verify(androidMediaPacket(mapOf("lease" to 4, "bytes" to audio, "watch" to true)) == null)

    val frames = AndroidInputFrameWatch()
    verify(frames.take().isEmpty())
    frames.watch(1000, 5_000_000)
    frames.rendered(999, 6_000_000)
    verify(frames.take().isEmpty())
    frames.rendered(1000, 9_000_000)
    verify(frames.take() == listOf(listOf(1000L, 4000L)))
    verify(frames.take().isEmpty())
    // A frame past two watched packets answers both; a later watch waits.
    frames.watch(2000, 10_000_000)
    frames.watch(3000, 11_000_000)
    frames.watch(5000, 12_000_000)
    frames.rendered(3500, 13_000_000)
    verify(frames.take() == listOf(listOf(2000L, 3000L), listOf(3000L, 2000L)))
    frames.clear()
    frames.rendered(9000, 20_000_000)
    verify(frames.take().isEmpty())
    // Bounded: at most 16 outstanding watches and 64 reported frames.
    for (pts in 0L until 40L) frames.watch(pts, 1_000)
    frames.rendered(100, 2_000)
    verify(frames.take().size == 16)
    frames.watch(-1, 0)
    frames.rendered(0, 1)
    verify(frames.take().isEmpty())
    println("{\"checks\":$checks,\"productionOwnershipAndEnvelope\":true,\"watchedEnvelope\":true,\"inputFrameWatch\":true,\"mediaCodecExecuted\":false}")
}
