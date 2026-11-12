// audio-opus-v1 (Host apps/windows_host/src/opus_audio.hpp): the stream's sound
// as 10 ms Opus packets (about 140 kbps on the wire) instead of 48 kHz s16le
// PCM (1.5 Mbps). A Host advertises it, a client that decodes Opus asks for it,
// and the stream session gets it: session.start then names the codec, and the
// Host's stream.info states the format it actually sends ("opus", or "s16le"
// exactly as before). Independent outputs (tracks-v1, one shared audio path)
// get it only from a Host that also advertises audio-opus-tracks-v1: their
// stream.tracks.info states the format and a kind-2 TVT1 payload is then the
// same u16 sequence + Opus packet. Released clients, Hosts and brokers and a
// Host whose encoder did not start keep PCM. Never a reason to refuse or end a
// connection.
export const audioOpusProtocol = 'audio-opus-v1';
export const audioOpusTracksProtocol = 'audio-opus-tracks-v1';
export const audioOpusCodec = 'opus';

// Host capability: exactly the marker.
export function audioOpusCapability(value) {
  return value?.audioCodecProtocol === audioOpusProtocol;
}
// The Host's track worker sends Opus too (only with audio-opus-v1 itself).
export function audioOpusTracksCapability(value) {
  return audioOpusCapability(value) && value?.audioCodecTracksProtocol === audioOpusTracksProtocol;
}

// Connect body field. A client sends it only after seeing the capability; any
// other value just leaves the stream on PCM.
export function requestedAudioOpus(mode, protocol) {
  return mode === 'stream' && protocol === audioOpusProtocol;
}

export function negotiateAudioOpus(requested, host, tracks) {
  return requested === true && (tracks ? audioOpusTracksCapability(host?.capabilities) : audioOpusCapability(host?.capabilities));
}

// stream.info's audio object: the released PCM shape for every session, Opus
// only for a session that was granted it.
export function validStreamAudio(audio, opus) {
  return audio?.sampleRate === 48000 && audio?.channels === 2 && (audio?.format === 's16le' || (opus === true && audio?.format === audioOpusCodec));
}

// A kind-2 payload length. PCM is whole 4-byte stereo frames; an Opus packet
// (sequence number + Opus bytes) has any length.
export function validAudioPayload(length, opus) {
  return opus === true || length % 4 === 0;
}
