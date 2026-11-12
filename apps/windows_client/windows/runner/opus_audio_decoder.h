#ifndef MULTITAP_OPUS_AUDIO_DECODER_H_
#define MULTITAP_OPUS_AUDIO_DECODER_H_
#include <opus.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
namespace multitap {
// audio-opus-v1 (Host apps/windows_host/src/opus_audio.hpp): a kind-2 payload
// is a u16 little-endian sequence number and one Opus packet of exactly 10 ms
// (480 frames, 48 kHz stereo). Each becomes 10 ms of s16le PCM for the
// AudioJitterBuffer, stamped with the packet's PTS (not thread-safe;
// RemoteAudio guards it).
//  - A gap in the sequence (lost on UDP, dropped by a full Host queue) is
//    filled before the packet that shows it, at most kMaxConcealed frames:
//    Opus PLC, the last frame from the arriving packet's in-band FEC where it
//    carries one (SILK/hybrid only; the Host's CELT frames have none and
//    libopus conceals that frame too). The filled frames take the PTS right
//    before the arriving packet, so a loss after a silent pause stays a pause
//    for the jitter buffer. A longer gap is left unfilled: the sound resumes.
//  - When the next packet has not come and the device is about to run dry,
//    Conceal makes its 10 ms now (Opus PLC, the PTS it would have had) and the
//    sequence moves past it. A packet that then comes for a concealed number:
//     - its PTS where that number's would be (within kPauseUs): it is late
//       and dropped, in order, like any late packet;
//     - its PTS later: the VM was silent meanwhile (the Host numbers sent
//       frames only, so a pause leaves no gap in them): new sound, decoded
//       from that number on.
//  - Concealment in a row, at the next packet and ahead of it alike, is
//    bounded: kFullRun frames at PLC's own level, then a fade to silence by
//    the end of frame kMaxRun; no more after that until a packet comes. The
//    first frame after concealment fades in from where it ended.
//  - A packet up to kLateWindow older than the one expected (reordered or
//    repeated) is too late and dropped. Further back, the Host's encoder
//    started over (a new capture under the same stream): decoding starts over.
//  - A packet that is not one 10 ms Opus packet is malformed.
class OpusAudioDecoder {
 public:
  static constexpr int kRate = 48000, kChannels = 2, kFrameSamples = 480;
  static constexpr size_t kFrameBytes = size_t(kFrameSamples) * kChannels * 2;
  static constexpr uint64_t kFrameUs = 10000;
  static constexpr uint16_t kMaxConcealed = 4;  // 40 ms filled at the next packet
  static constexpr uint16_t kLateWindow = 64;   // 640 ms
  static constexpr uint16_t kFullRun = 3, kMaxRun = 8;  // 30 ms at PLC's level, silent by 80 ms
  // A packet's PTS beyond what its number's would be by more than this is
  // sound after a pause. The Host stamps frames from its loopback reads, which
  // a ~16 ms timer can delay.
  static constexpr uint64_t kPauseUs = 25000;
  static constexpr int kFadeInSamples = 120;  // 2.5 ms
  struct Counters {
    uint64_t packets = 0, concealed = 0, unfilled = 0, late = 0, restarts = 0;
    // Concealed ahead of the packet (Conceal); late packets whose time was so
    // concealed (in `late` too); pauses found among concealed numbers.
    uint64_t ahead = 0, late_concealed = 0, pauses = 0;
  };
  OpusAudioDecoder() = default;
  OpusAudioDecoder(const OpusAudioDecoder&) = delete;
  OpusAudioDecoder& operator=(const OpusAudioDecoder&) = delete;
  ~OpusAudioDecoder() { if (decoder_) opus_decoder_destroy(decoder_); }
  // A fresh stream: false when libopus refuses.
  bool Open() {
    if (!decoder_) { int error = OPUS_OK; decoder_ = opus_decoder_create(kRate, kChannels, &error); if (error != OPUS_OK) { if (decoder_) opus_decoder_destroy(decoder_); decoder_ = nullptr; } }
    else opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
    started_ = false; ahead_ = 0; run_ = 0; counters_ = Counters();
    return decoder_ != nullptr;
  }
  bool ready() const { return decoder_ != nullptr; }
  // `emit(pts_us, pcm, bytes)` for every 10 ms produced, in order. False for a
  // malformed packet.
  template <class Emit> bool Decode(uint64_t pts_us, const uint8_t* payload, size_t size, Emit&& emit) {
    if (!decoder_ || size < 3 || size > 2 + 1275) return false;
    const uint16_t sequence = uint16_t(payload[0] | (payload[1] << 8));
    const unsigned char* packet = payload + 2; const opus_int32 length = opus_int32(size - 2);
    if (opus_packet_get_nb_samples(packet, length, kRate) != kFrameSamples) return false;
    if (started_ && uint16_t(sequence - expected_) >= 0x8000) {
      const uint16_t behind = uint16_t(expected_ - sequence);
      if (behind <= ahead_ && pts_us > next_pts_ - behind * kFrameUs + kPauseUs) { expected_ = sequence; ahead_ = 0; ++counters_.pauses; }
      else if (behind <= kLateWindow) { ++counters_.late; if (behind <= ahead_) ++counters_.late_concealed; return true; }
      else { opus_decoder_ctl(decoder_, OPUS_RESET_STATE); started_ = false; ahead_ = 0; run_ = 0; ++counters_.restarts; }
    }
    if (started_) {
      const uint16_t gap = uint16_t(sequence - expected_);
      if (gap > kMaxConcealed || run_ + gap > kMaxRun) counters_.unfilled += gap;
      else for (uint16_t k = 0; k < gap; ++k) {
        const bool last = k + 1 == gap;
        if (opus_decode(decoder_, last ? packet : nullptr, last ? length : 0, pcm_.data(), kFrameSamples, last ? 1 : 0) != kFrameSamples) return false;
        Fade();
        const uint64_t back = uint64_t(gap - k) * kFrameUs;
        emit(pts_us > back ? pts_us - back : 0, reinterpret_cast<const uint8_t*>(pcm_.data()), kFrameBytes); ++counters_.concealed;
      }
    }
    if (opus_decode(decoder_, packet, length, pcm_.data(), kFrameSamples, 0) != kFrameSamples) return false;
    FadeIn();
    started_ = true; expected_ = uint16_t(sequence + 1); ahead_ = 0; ++counters_.packets;
    emit(pts_us, reinterpret_cast<const uint8_t*>(pcm_.data()), kFrameBytes); next_pts_ = pts_us + kFrameUs;
    return true;
  }
  // The render thread: the next packet has not come and the device is about
  // to run dry. Its 10 ms of Opus PLC to `emit` now, with the PTS it would
  // have had. False before the first packet and once the run has faded out.
  template <class Emit> bool Conceal(Emit&& emit) {
    if (!decoder_ || !started_ || run_ >= kMaxRun) return false;
    if (opus_decode(decoder_, nullptr, 0, pcm_.data(), kFrameSamples, 0) != kFrameSamples) return false;
    Fade();
    expected_ = uint16_t(expected_ + 1); ++ahead_; ++counters_.ahead;
    emit(next_pts_, reinterpret_cast<const uint8_t*>(pcm_.data()), kFrameBytes); next_pts_ += kFrameUs;
    return true;
  }
  const Counters& counters() const { return counters_; }
 private:
  // Gain at the end of `run` concealed frames in a row.
  static float RunGain(int run) {
    return run <= kFullRun ? 1.0f : float(kMaxRun - run) / float(kMaxRun - kFullRun);
  }
  // pcm_ is concealed frame run_ (from 0) of a run: past kFullRun frames it
  // falls linearly to silence at the end of frame kMaxRun - 1.
  void Fade() {
    if (run_ >= kFullRun) {
      const float from = RunGain(run_), to = RunGain(run_ + 1);
      for (int n = 0; n < kFrameSamples; ++n) Scale(n, from + (to - from) * float(n + 1) / kFrameSamples);
    }
    ++run_;
  }
  // pcm_ is the first decoded frame after concealment: from its last gain up.
  void FadeIn() {
    const float from = RunGain(run_);
    if (from < 1.0f) for (int n = 0; n < kFadeInSamples; ++n) Scale(n, from + (1.0f - from) * float(n) / kFadeInSamples);
    run_ = 0;
  }
  void Scale(int n, float gain) {
    for (int c = 0; c < kChannels; ++c) { auto& s = pcm_[size_t(n) * kChannels + c]; s = opus_int16(std::lround(float(s) * gain)); }
  }
  OpusDecoder* decoder_ = nullptr;
  bool started_ = false;
  uint16_t expected_ = 0;
  uint16_t ahead_ = 0;  // numbers before expected_ that Conceal made, not received
  int run_ = 0;         // frames concealed in a row
  uint64_t next_pts_ = 0;  // the PTS of the frame after the last one emitted
  Counters counters_;
  std::array<opus_int16, size_t(kFrameSamples) * kChannels> pcm_{};
};
}
#endif
