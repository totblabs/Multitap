/// audio-opus-v1 (Host apps/windows_host/src/opus_audio.hpp, broker
/// server/src/audio-codec.mjs): the stream's sound as 10 ms Opus packets
/// (about 140 kbps) instead of 48 kHz s16le PCM (1.5 Mbps). A client asks only
/// where the desktop list advertises it (a released broker refuses the field)
/// and only where its native player decodes Opus (the Windows runner, and
/// Android's engine through MediaCodec). stream.info then says which format
/// the Host actually sends.
const audioOpusProtocol = 'audio-opus-v1';

bool audioOpusAdvertised(Object? capabilities) =>
    capabilities is Map &&
    capabilities['audioCodecProtocol'] == audioOpusProtocol;

/// audio-opus-tracks-v1: independent outputs (tracks-v1, "모든 모니터") too.
/// The tracks connect asks with the same audio-opus-v1 field only where the
/// Host also shows this (and only the Windows runner decodes it there);
/// stream.tracks.info then says which format the Host sends.
const audioOpusTracksProtocol = 'audio-opus-tracks-v1';

bool audioOpusTracksAdvertised(Object? capabilities) =>
    audioOpusAdvertised(capabilities) &&
    (capabilities as Map)['audioCodecTracksProtocol'] ==
        audioOpusTracksProtocol;

/// Whether stream.info's audio object announces Opus packets. Opus this
/// connection was not granted is a protocol error; anything else is the
/// released PCM.
bool streamAudioOpus(Object? audio, {required bool granted}) {
  final opus = audio is Map && audio['format'] == 'opus';
  if (opus && !granted) {
    throw const FormatException('Opus audio without the grant.');
  }
  return opus;
}

/// A media player that decodes audio-opus-v1 (the Windows runner, Android).
abstract interface class AudioFormatMedia {
  /// The format of the audio packets from the next start on.
  set audioOpus(bool value);
}
