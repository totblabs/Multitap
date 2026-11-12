#pragma once
// audio-opus-v1: the stream's sound as Opus instead of raw PCM (1,536 kbps,
// two UDP datagrams per 10 ms packet).
//  - Negotiation: this Host advertises "audioCodecProtocol":"audio-opus-v1"; a
//    client that decodes Opus asks for it in its connect request; the broker
//    grants it and then adds "audioCodec":"opus" to session.start. Old clients
//    and brokers never ask; tracks-v1 gets it only with audio-opus-tracks-v1
//    (below); a stream whose encoder did not start keeps PCM.
//  - stream.info's audio object states what is actually sent: "format" is
//    "opus" or, exactly as before, "s16le" (streamAudioInfoJson).
//  - Packet: media kind 2, flags 0, the stream's epoch, the PTS of the frame's
//    first sample; payload: a u16 little-endian sequence number, then one Opus
//    packet of exactly 10 ms (480 samples per channel, 48 kHz stereo). The
//    number counts sent frames only: a gap is a packet lost or dropped on the
//    way, which the client conceals; the loopback's silent pauses leave none.
//  - Encoder: RESTRICTED_LOWDELAY (CELT only, 2.5 ms lookahead), 128 kbps VBR,
//    DTX off. Opus in-band FEC exists only for SILK/hybrid frames, so it cannot
//    apply here and stays off; the client conceals a loss with Opus PLC.
//  - Size: an Opus packet is capped so that the whole media packet always
//    fits one udp-media-v1 datagram (udp_media.hpp fragmentPayload).
#include "../../../native/windows_video/udp_media.hpp"
#include <opus.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace host {
inline constexpr const char* audioOpusProtocol = "audio-opus-v1";
// audio-opus-tracks-v1: independent outputs (tracks-v1) too. Their one shared
// audio path sends the same frames in kind-2 TVT1 packets (track_media.hpp
// parse/pack with `opus`), and stream.tracks.info's audio format says which.
inline constexpr const char* audioOpusTracksProtocol = "audio-opus-tracks-v1";
inline constexpr const char* audioOpusCodec = "opus";

// session.start of an ordinary stream whose broker granted audio-opus-v1
// (tracks-v1: trackAudioOpusGranted in track_source.hpp).
template<class JsonValue> bool opusAudioRequested(const JsonValue& start) {
  return start.get("type").string() == "session.start" && start.get("mode").string() == "stream" &&
         start.get("audioCodec").string() == audioOpusCodec && start.get("mediaProtocol").string().empty();
}

// stream.info's "audio" value. PCM keeps its released shape byte for byte.
inline std::string streamAudioInfoJson(bool available, bool opus) {
  return std::string("{\"sampleRate\":48000,\"channels\":2,\"format\":\"") + (opus ? audioOpusCodec : "s16le") + "\",\"available\":" + (available ? "true" : "false") + '}';
}

class OpusAudioEncoder {
 public:
  static constexpr int kRate = 48000, kChannels = 2, kFrameSamples = 480, kBitrate = 128000;
  static constexpr uint64_t kFrameUs = 10000;
  // Media envelope (12) + sequence (2) + Opus packet <= one udp-media-v1
  // fragment (1157 bytes). Typical 128 kbps frames are about 160 bytes.
  static constexpr size_t kEnvelopeBytes = 12, kSequenceBytes = 2, kMaxOpusBytes = multitap::udp::fragmentPayload - kEnvelopeBytes - kSequenceBytes;

  // Nothing when libopus refuses the configuration (the stream then sends PCM).
  static std::optional<OpusAudioEncoder> create() {
    int error = OPUS_OK;
    std::unique_ptr<OpusEncoder, Destroy> encoder(opus_encoder_create(kRate, kChannels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error));
    if (error != OPUS_OK || !encoder) return std::nullopt;
    if (opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(kBitrate)) != OPUS_OK ||
        opus_encoder_ctl(encoder.get(), OPUS_SET_VBR(1)) != OPUS_OK ||
        opus_encoder_ctl(encoder.get(), OPUS_SET_DTX(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder.get(), OPUS_SET_INBAND_FEC(0)) != OPUS_OK) return std::nullopt;
    return OpusAudioEncoder(std::move(encoder));
  }

  // s16le interleaved stereo `bytes` (a multiple of 4) whose first sample has
  // `ptsUs`. Each whole 10 ms frame goes to `emit(framePtsUs, payload)`.
  template<class Emit> void push(uint64_t ptsUs, const uint8_t* bytes, size_t size, Emit&& emit) {
    const size_t frames = size / (2 * kChannels);
    for (size_t at = 0; at < frames;) {
      if (!held_) heldPts_ = ptsUs + uint64_t(at) * 1000000 / kRate;
      const size_t take = std::min(frames - at, size_t(kFrameSamples) - held_);
      std::memcpy(pcm_.data() + held_ * kChannels, bytes + at * 2 * kChannels, take * 2 * kChannels);
      held_ += take; at += take;
      if (held_ == size_t(kFrameSamples)) encode(emit);
    }
  }
  // The loopback went quiet: the held part of a frame leaves now, padded with silence.
  template<class Emit> void flush(Emit&& emit) {
    if (!held_) return;
    std::fill(pcm_.begin() + held_ * kChannels, pcm_.end(), int16_t(0));
    held_ = kFrameSamples; encode(emit);
  }
  size_t held() const { return held_; }  // samples per channel waiting for a whole frame
  uint16_t nextSequence() const { return sequence_; }
  int lookahead() const { opus_int32 value = 0; return opus_encoder_ctl(encoder_.get(), OPUS_GET_LOOKAHEAD(&value)) == OPUS_OK ? int(value) : -1; }
  OpusEncoder* native() const { return encoder_.get(); }

 private:
  struct Destroy { void operator()(OpusEncoder* value) const { opus_encoder_destroy(value); } };
  explicit OpusAudioEncoder(std::unique_ptr<OpusEncoder, Destroy> encoder) : encoder_(std::move(encoder)) {}
  template<class Emit> void encode(Emit& emit) {
    std::vector<uint8_t> payload(kSequenceBytes + kMaxOpusBytes);
    const int length = opus_encode(encoder_.get(), pcm_.data(), kFrameSamples, payload.data() + kSequenceBytes, opus_int32(kMaxOpusBytes));
    const uint16_t sequence = sequence_++;
    held_ = 0;
    // A frame libopus refused is not sent; its number stays a gap the client conceals.
    if (length <= 0) return;
    payload[0] = uint8_t(sequence); payload[1] = uint8_t(sequence >> 8);
    payload.resize(kSequenceBytes + size_t(length));
    emit(heldPts_, std::move(payload));
  }
  std::unique_ptr<OpusEncoder, Destroy> encoder_;
  std::array<int16_t, size_t(kFrameSamples) * kChannels> pcm_{};
  size_t held_ = 0;
  uint64_t heldPts_ = 0;
  uint16_t sequence_ = 0;
};
}
