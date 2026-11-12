#ifndef MULTITAP_AUDIO_JITTER_H_
#define MULTITAP_AUDIO_JITTER_H_
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <vector>
namespace multitap {
// Received 48 kHz stereo s16 PCM and when to hand it to the render device (not
// thread-safe; RemoteAudio guards it). The level is what waits here plus what
// the device still holds (its padding); the target is the level aimed for.
//  - At start and after the device ran dry nothing is written until the level
//    reaches the target (priming). Then the device is kept at most `target`
//    full and the rest waits here.
//  - The device ran dry while playing: an underrun when the next audio goes on
//    from where the played sound ended (PTS within gap_us). The Host's WASAPI
//    loopback sends nothing while the VM is silent, so a later PTS is a pause
//    of the sound, not a late network, and the target stays.
//  - Each underrun grows the target a step; each calm_us without one shrinks
//    it a smaller step; always within [min_target, max_target].
//  - Above target + headroom (a burst after a stall) the oldest audio is
//    dropped down to the target at once: an overrun.
//  - Clock drift between the VM and this PC and what bursts leave behind: the
//    level after each write, smoothed over smooth_us, above target + trim_band
//    drops and below target - insert_band repeats one frame in every
//    correct_every written (0.5 %) until it is back. Healthy writes fill the
//    device to the target, so the smoothed level sits at or a little under it.
//  - Concealment (Opus only; RemoteAudio asks, PCM never does): while playing,
//    a level under conceal_below (the device about to run dry) is Starving;
//    the caller then hands in made-up audio (PushConcealed) that goes on from
//    the received sound. It never moves where the received sound ended, so
//    the underrun/silence rule above still measures from the real sound. When
//    concealed audio turns out to have been late, not lost (NoteLate), the
//    target grows as for an underrun, once per run of concealment; when it
//    stood in for a pause of the VM's sound (NotePause), that is a silence
//    and the new sound is primed again.
// Never a reason to end a session: malformed packets are refused, everything
// else is played, dropped or repeated.
class AudioJitterBuffer {
 public:
  static constexpr uint32_t kRate = 48000;
  static constexpr size_t kFrameBytes = 4;
  static constexpr size_t kMaxPacketBytes = 38400;  // 200 ms; a larger packet is malformed
  static constexpr uint32_t Ms(uint32_t ms) { return ms * (kRate / 1000); }
  struct Config {
    uint32_t min_target = Ms(30), start_target = Ms(40), max_target = Ms(200);
    uint32_t grow = Ms(20), shrink = Ms(5), headroom = Ms(40), trim_band = Ms(3), insert_band = Ms(5);
    uint64_t calm_us = 10000000, smooth_us = 2000000, gap_us = 50000;
    uint32_t correct_every = 200;
    // Over one shared-mode engine period (10 ms), so a device looked at every
    // 5 ms gets the next 10 ms before an engine pass finds it empty.
    uint32_t conceal_below = Ms(15);
  };
  struct Counters {
    uint64_t underruns = 0, silences = 0, overruns = 0, overrun_frames = 0;
    uint64_t inserted_frames = 0, trimmed_frames = 0, written_frames = 0;
    uint64_t concealed_frames = 0, late_concealments = 0;
  };
  explicit AudioJitterBuffer(Config config = Config()) : config_(config), target_(config.start_target) {}
  static bool Valid(size_t size) { return size && size % kFrameBytes == 0 && size <= kMaxPacketBytes; }

  // A received packet: its Host capture-clock PTS and this PC's time (us).
  bool Push(uint64_t pts_us, const uint8_t* bytes, size_t size, uint64_t now_us) {
    if (!Valid(size)) return false;
    if (awaiting_resume_) { awaiting_resume_ = false; Classify(pts_us, now_us); }
    concealing_ = false;
    chunks_.push_back({pts_us, std::vector<uint8_t>(bytes, bytes + size), false});
    queued_ += uint32_t(size / kFrameBytes);
    if (uint64_t(queued_) + device_ > uint64_t(target_) + config_.headroom) {
      const uint32_t keep = target_ > device_ ? target_ - device_ : 0;
      ++counters_.overruns; counters_.overrun_frames += Skip(queued_ - std::min(queued_, keep));
    }
    return true;
  }

  // Render thread: up to `room` frames into `out` for a device holding
  // `device_frames`; returns the frames written.
  uint32_t Pull(uint8_t* out, uint32_t room, uint32_t device_frames, uint64_t now_us) {
    if (!started_) { started_ = true; calm_since_ = now_us; }
    if (playing_ && device_frames == 0) {
      playing_ = false;
      if (queued_) Classify(chunks_.front().pts_us + FramesUs(front_), now_us); else awaiting_resume_ = true;
    }
    device_ = device_frames; pulled_at_ = now_us;
    if (!playing_) {
      if (!queued_ || queued_ + device_frames < target_) return 0;
      playing_ = true; smoothed_ = double(queued_ + device_frames); smoothed_at_ = now_us;
    }
    if (now_us - calm_since_ >= config_.calm_us) {
      target_ = std::max(config_.min_target, target_ > config_.shrink ? target_ - config_.shrink : 0);
      calm_since_ = now_us;
    }
    const uint32_t fill = uint32_t(std::min<uint64_t>(target_, uint64_t(device_frames) + room));
    const uint32_t want = fill > device_frames ? fill - device_frames : 0;
    uint32_t written = 0;
    while (written < want) {
      const bool due = since_correction_ >= config_.correct_every;
      if (due && correction_ < 0 && queued_ > 1) { Skip(1); ++counters_.trimmed_frames; since_correction_ = 0; continue; }
      if (due && correction_ > 0 && have_last_) {
        std::memcpy(out + size_t(written) * kFrameBytes, last_, kFrameBytes); ++written; ++counters_.inserted_frames; since_correction_ = 0; continue;
      }
      if (!queued_) break;
      const auto& chunk = chunks_.front();
      std::memcpy(last_, chunk.bytes.data() + size_t(front_) * kFrameBytes, kFrameBytes); have_last_ = true;
      std::memcpy(out + size_t(written) * kFrameBytes, last_, kFrameBytes);
      ended_pts_ = chunk.pts_us + FramesUs(front_ + 1);
      Skip(1); ++written; ++since_correction_;
    }
    counters_.written_frames += written; device_ = device_frames + written;
    const double level = double(queued_) + device_;
    smoothed_ += (level - smoothed_) * std::min(1.0, double(now_us - smoothed_at_) / double(config_.smooth_us));
    smoothed_at_ = now_us;
    correction_ = smoothed_ > double(target_) + config_.trim_band ? -1 : smoothed_ < double(target_) - config_.insert_band ? 1 : 0;
    return written;
  }

  // The render thread, before Pull: playing, and what is left here and in a
  // device that has not run dry yet is under conceal_below.
  bool Starving(uint32_t device_frames) const {
    return playing_ && device_frames && uint64_t(queued_) + device_frames < config_.conceal_below;
  }
  // Made-up audio standing in for what has not come (its PTS goes on from the
  // received sound); played like received audio.
  bool PushConcealed(uint64_t pts_us, const uint8_t* bytes, size_t size) {
    if (!Valid(size)) return false;
    if (!concealing_) { concealing_ = true; late_noted_ = false; }
    chunks_.push_back({pts_us, std::vector<uint8_t>(bytes, bytes + size), true});
    queued_ += uint32_t(size / kFrameBytes); counters_.concealed_frames += size / kFrameBytes;
    return true;
  }
  // Audio concealed since the last received audio came after all: the network
  // was late, as an underrun would have shown without the concealment.
  void NoteLate(uint64_t now_us) {
    if (!concealing_ || late_noted_) return;
    late_noted_ = true; ++counters_.late_concealments; Grow(now_us);
  }
  // Concealed audio stood in for a pause of the VM's sound, and what came now
  // is new sound: a silence, primed again to the target like any after one.
  // (When the device already ran dry, Push counted it.)
  void NotePause() {
    if (!playing_) return;
    ++counters_.silences; playing_ = false;
  }

  // The Host capture-clock PTS at the device's play head now, while playing.
  std::optional<int64_t> PlayingPtsUs(uint64_t now_us) const {
    if (!playing_ || !device_) return std::nullopt;
    const int64_t elapsed = int64_t(std::min<uint64_t>(now_us - pulled_at_, FramesUs(device_)));
    return int64_t(ended_pts_) - int64_t(FramesUs(device_)) + elapsed;
  }
  void Clear() {
    chunks_.clear(); queued_ = front_ = device_ = 0; playing_ = awaiting_resume_ = started_ = have_last_ = concealing_ = late_noted_ = false;
    correction_ = 0; since_correction_ = 0; target_ = config_.start_target;
  }
  uint32_t queued() const { return queued_; }
  uint32_t level() const { return queued_ + device_; }  // frames, as of the last Pull or Push
  uint32_t target() const { return target_; }
  bool playing() const { return playing_; }
  const Counters& counters() const { return counters_; }
  void ResetCounters() { counters_ = Counters(); }
  static uint64_t FramesUs(uint64_t frames) { return frames * 1000000 / kRate; }

 private:
  struct Chunk { uint64_t pts_us; std::vector<uint8_t> bytes; bool concealed; };
  // The sound that ran dry goes on (underrun) or starts again later (silence).
  // Audio this buffer dropped itself (overrun, trim) counts as gone on;
  // concealed audio does not (it is not the VM's sound).
  void Classify(uint64_t pts_us, uint64_t now_us) {
    if (pts_us > continued_pts_ + config_.gap_us) { ++counters_.silences; return; }
    ++counters_.underruns; Grow(now_us);
  }
  void Grow(uint64_t now_us) { target_ = std::min(config_.max_target, target_ + config_.grow); calm_since_ = now_us; }
  // Removes up to `frames` of the oldest queued frames; returns how many.
  uint32_t Skip(uint32_t frames) {
    uint32_t removed = 0;
    while (removed < frames && !chunks_.empty()) {
      const uint32_t left = uint32_t(chunks_.front().bytes.size() / kFrameBytes) - front_;
      const uint32_t step = std::min(left, frames - removed);
      removed += step; queued_ -= step;
      if (!chunks_.front().concealed) continued_pts_ = chunks_.front().pts_us + FramesUs(front_ + step);
      if (step == left) { chunks_.pop_front(); front_ = 0; } else front_ += step;
    }
    return removed;
  }
  Config config_;
  std::deque<Chunk> chunks_;
  uint32_t queued_ = 0, front_ = 0, device_ = 0, target_, since_correction_ = 0;
  uint64_t ended_pts_ = 0, continued_pts_ = 0, pulled_at_ = 0, calm_since_ = 0, smoothed_at_ = 0;
  double smoothed_ = 0;
  int correction_ = 0;
  bool playing_ = false, awaiting_resume_ = false, started_ = false, have_last_ = false;
  bool concealing_ = false, late_noted_ = false;  // concealed audio since the last received; NoteLate grew for it
  uint8_t last_[kFrameBytes] = {};
  Counters counters_;
};
}
#endif
