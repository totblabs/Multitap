package me.totb.multitap_client

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.*
import org.junit.Test

class AndroidTracksTest {
    private fun config() = mapOf("topologyEpoch" to 7, "audioEnabled" to true,
        "tracks" to listOf(row("0", false), row("1", true)))
    private fun row(id: String, primary: Boolean) = mapOf("id" to id, "epoch" to 3,
        "width" to 640, "height" to 360, "sourceWidth" to 1280, "sourceHeight" to 720,
        "left" to (if (id == "0") -1280 else 0), "top" to 0, "primary" to primary)
    private fun packet(track: Int, audio: Boolean = false): ByteArray {
        val result = ByteArray(32)
        result[0] = 84; result[1] = 86; result[2] = 84; result[3] = 49
        result[4] = (if (audio) 2 else 1).toByte(); result[5] = (if (audio) 0 else 1).toByte()
        result[6] = track.toByte()
        ByteBuffer.wrap(result).order(ByteOrder.LITTLE_ENDIAN).apply {
            putInt(8, 7); putInt(12, if (audio) 0 else 3); putLong(16, 99); putInt(24, 4)
        }
        result[28] = 0; result[29] = 0; result[30] = 1; result[31] = 0x65
        return result
    }
    @Test fun independentGeometryAndEpochArePreserved() {
        val configuration = androidTracks(config())!!
        assertEquals(-1280, configuration.tracks[0].left)
        assertEquals(640, configuration.tracks[1].width)
        assertEquals(1280, configuration.tracks[1].sourceWidth)
        for (id in 0..1) {
            val normalized = androidTrackPacket(configuration, packet(id))!!
            assertEquals(id.toString(), normalized.track.id)
            assertEquals(16, normalized.bytes.size)
            assertEquals(0, normalized.bytes[2].toInt())
            assertEquals(99L, ByteBuffer.wrap(normalized.bytes).order(ByteOrder.LITTLE_ENDIAN).getLong(4))
        }
        assertEquals("1", androidTrackPacket(configuration, packet(255, true))!!.track.id)
    }
    @Test fun malformedAndStalePacketsNeverReachAnotherLane() {
        val configuration = androidTracks(config())!!
        for (offset in listOf(0, 7, 8, 12, 24)) {
            val bad = packet(0); bad[offset] = (bad[offset]+1).toByte()
            assertNull(androidTrackPacket(configuration, bad))
        }
        assertNull(androidTrackPacket(configuration, packet(2)))
        assertNull(androidTrackPacket(configuration, packet(0, true)))
        assertNull(androidTracks(config()+mapOf("tracks" to listOf(row("0", true), row("0", false)))))
        assertNull(androidTracks(config()+mapOf("tracks" to listOf(row("0", true), row("1", true)))))
        assertNull(androidTracks(config()+mapOf("topologyEpoch" to 0)))
    }
    @Test fun udpHasTheSameTimestampAndPrimaryOnlyAudioBounds() {
        val configuration = androidTracks(config())!!
        val video = androidTrackPacket(configuration, packet(0))!!.bytes
        val audio = androidTrackPacket(configuration, packet(255, true))!!.bytes
        val secondary = AndroidTrackBinding("0", 7, 3, false)
        val primary = AndroidTrackBinding("1", 7, 3, true)
        assertTrue(androidTrackUdpPacket(secondary, video, false))
        assertTrue(androidTrackUdpPacket(primary, audio, false))
        assertFalse(androidTrackUdpPacket(secondary, audio, false))
        ByteBuffer.wrap(video).order(ByteOrder.LITTLE_ENDIAN).putLong(4, 9007199254740992L)
        assertFalse(androidTrackUdpPacket(secondary, video, false))
        assertFalse(androidTrackUdpPacket(primary.copy(epoch=65536), audio, false))
    }
}
