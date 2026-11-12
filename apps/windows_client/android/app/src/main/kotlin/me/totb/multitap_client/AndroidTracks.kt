package me.totb.multitap_client

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Immutable geometry/identity; native admission never guesses an output. */
internal data class AndroidTrack(val id: String, val epoch: Long, val width: Int, val height: Int,
    val sourceWidth: Int, val sourceHeight: Int, val left: Int, val top: Int, val primary: Boolean)
internal data class AndroidTracks(val topology: Long, val tracks: List<AndroidTrack>, val audio: Boolean, val opus: Boolean)
internal data class AndroidTrackBinding(val track: String, val topology: Long, val epoch: Long, val primary: Boolean)

internal fun androidTracks(value: Any?): AndroidTracks? {
    val map = value as? Map<*, *> ?: return null
    val topology = udpInteger(map["topologyEpoch"]) ?: return null
    val audio = map["audioEnabled"] as? Boolean ?: return null
    val rows = map["tracks"] as? List<*> ?: return null
    if (topology !in 1..0xffffffffL || rows.size !in 1..2 || map["audioFormat"] !in listOf(null, "opus")) return null
    val tracks = ArrayList<AndroidTrack>()
    for (raw in rows) {
        val row = raw as? Map<*, *> ?: return null
        val id = row["id"] as? String ?: return null
        val epoch = udpInteger(row["epoch"]) ?: return null
        fun integer(name: String): Int? = udpInteger(row[name])?.takeIf { it in Int.MIN_VALUE..Int.MAX_VALUE }?.toInt()
        val width = integer("width") ?: return null; val height = integer("height") ?: return null
        val sw = integer("sourceWidth") ?: width; val sh = integer("sourceHeight") ?: height
        val left = integer("left") ?: return null; val top = integer("top") ?: return null
        val primary = row["primary"] as? Boolean ?: return null
        if (id !in listOf("0", "1") || tracks.any { it.id == id } || epoch !in 1..0xffffffffL ||
            width !in 2..16384 || height !in 2..16384 || sw !in width..16384 || sh !in height..16384 ||
            width % 2 != 0 || height % 2 != 0 || left.toLong()+sw > Int.MAX_VALUE || top.toLong()+sh > Int.MAX_VALUE) return null
        tracks.add(AndroidTrack(id, epoch, width, height, sw, sh, left, top, primary))
    }
    if (tracks.count { it.primary } != 1) return null
    return AndroidTracks(topology, tracks.toList(), audio, map["audioFormat"] == "opus")
}

internal data class AndroidTrackPacket(val track: AndroidTrack, val bytes: ByteArray, val video: Boolean)

/** The UDP gate already authenticated this leg/epoch and cleared bytes 2..3. */
internal fun androidTrackUdpPacket(binding: AndroidTrackBinding, bytes: ByteArray, opus: Boolean): Boolean {
    if (binding.track !in listOf("0", "1") || binding.topology !in 1..0xffffffffL ||
        binding.epoch !in 1..65535 || !androidMediaPacketValid(bytes, bytes.size, opus) ||
        (bytes[0].toInt() == 2 && !binding.primary)) return false
    return ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).getLong(4) in 0..9007199254740991L
}

/** TVT1 -> the existing bounded decoder queue. Cursor records stay in Dart. */
internal fun androidTrackPacket(configuration: AndroidTracks, bytes: ByteArray): AndroidTrackPacket? {
    if (bytes.size !in 29..4*1024*1024 || bytes[0] != 84.toByte() || bytes[1] != 86.toByte() ||
        bytes[2] != 84.toByte() || bytes[3] != 49.toByte() || bytes[7] != 0.toByte()) return null
    val data = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
    val kind = bytes[4].toInt() and 255; val flags = bytes[5].toInt() and 255
    val track = bytes[6].toInt() and 255; val topology = data.getInt(8).toLong() and 0xffffffffL
    val epoch = data.getInt(12).toLong() and 0xffffffffL; val pts = data.getLong(16)
    if (topology != configuration.topology || pts !in 0..9007199254740991L || data.getInt(24) != bytes.size-28) return null
    val target = if (kind == 1) configuration.tracks.find { it.id == track.toString() && it.epoch == epoch }
        else if (kind == 2 && configuration.audio && track == 255 && epoch == 0L) configuration.tracks.find { it.primary }
        else null
    if (target == null || (kind == 1 && flags and 0xfe != 0) || (kind == 2 && flags != 0)) return null
    val result = ByteArray(bytes.size-16)
    result[0] = kind.toByte(); result[1] = flags.toByte()
    System.arraycopy(bytes, 16, result, 4, 8); System.arraycopy(bytes, 28, result, 12, bytes.size-28)
    if (!androidMediaPacketValid(result, result.size, configuration.opus)) return null
    return AndroidTrackPacket(target, result, kind == 1)
}
