#ifndef MULTITAP_AUDIO_JITTER_H_
#define MULTITAP_AUDIO_JITTER_H_
#include <algorithm>
#include <array>
#include <cmath>
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
//  - The target is measured, not stepped: the depth that what happened lately
//    would have needed, plus a margin, in steps of `step`, within
//    [min_target, max_target]. A need is remembered for remember_us; one that
//    a target up to cheap_target meets is remembered for remember_cheap_us,
//    because sound that little behind the picture is not noticed (about 0.1 s)
//    and a blip each time it comes back is. What is measured:
//     - how late each packet came (Arrived): its arrival against its PTS,
//       beyond the earliest such difference of the last floor_us (the two
//       clocks drift, so the floor is not kept longer). A packet whose time
//       was concealed (NoteLate) was measured there too. The packets held up
//       behind a late one come less and less late: one pause, measured once,
//       by its first packet;
//     - a device that ran dry while the sound went on (an underrun: the next
//       audio goes on from where the played sound ended, PTS within gap_us):
//       the depth it had and the time it stayed dry. That covers what Arrived
//       cannot see: a render thread that did not run, lost PCM.
//    A need deeper than max_target allows is not met by any depth here, so it
//    does not raise the target (counted: uncovered). When what raised the
//    target is forgotten, the target comes back by itself, no faster
//    than the level can follow it (below), so nothing is cut for it.
//    Field 2026-11-24 (a game over Wi-Fi): the rule before this one (a step
//    up at each late packet, a small step down each calm ten seconds) kept
//    the sound 0.1 to 0.2 s behind the picture for five minutes after three
//    seconds of radio pauses, every five minutes.
//  - The Host's WASAPI loopback sends nothing while the VM is silent, so a
//    later PTS after a dry device is a pause of the sound, not a late
//    network: nothing is measured from it.
//  - Above target + headroom (a burst after a stall no depth covers) the
//    oldest audio is dropped down to the target at once: an overrun.
//  - Clock drift between the VM and this PC, and a level left off the target:
//    the level after each write, smoothed over smooth_us, above target +
//    trim_band is played a little fast and below target - insert_band a
//    little slow, one frame less or more in every slip_every played (0.5 %;
//    beyond far_band off, slip_every_far: 1 %). The frames in between are
//    resampled (a windowed sinc over kTaps frames), so the frame that goes or
//    comes leaves no edge: dropping or repeating a whole frame 240 times a
//    second, as before, put a 240 Hz buzz on every tone (a 5 kHz tone was
//    3 dB above its own distortion). A slip ends on a whole frame, and
//    between slips the audio passes untouched, bit for bit. In a slip the
//    newest kTaps / 2 frames wait for the ones after them; when the device is
//    about to run dry (the sound ends or is late) the slip is given up and
//    they go out as they are.
//  - Concealment (Opus only; RemoteAudio asks, PCM never does): while playing,
//    a level under conceal_below (the device about to run dry) is Starving;
//    the caller then hands in made-up audio (PushConcealed) that goes on from
//    the received sound. It never moves where the received sound ended, so
//    the underrun/silence rule above still measures from the real sound. When
//    it stood in for a pause of the VM's sound (NotePause), that is a silence
//    and the new sound is primed again.
// Never a reason to end a session: malformed packets are refused, everything
// else is played, dropped or resampled.
class AudioJitterBuffer {
 public:
  static constexpr uint32_t kRate = 48000;
  static constexpr size_t kFrameBytes = 4;
  static constexpr size_t kMaxPacketBytes = 38400;  // 200 ms; a larger packet is malformed
  static constexpr uint32_t kTaps = 16, kPhases = 200;  // the resampler; every slip span divides kPhases
  static constexpr uint32_t Ms(uint32_t ms) { return ms * (kRate / 1000); }
  struct Config {
    // The floor is the margin and one packet: the Host sends a sound packet
    // again with the next one, so a lost one comes 10 ms late, and a level
    // that holds that long plays it instead of concealing it.
    uint32_t min_target = Ms(40), max_target = Ms(200);
    // Beyond the lateness seen: the 15 ms under which concealment starts, the
    // 10 ms a level falls between two packets, and 5 ms for a level that sits
    // at the low edge of its band and is looked at every 5 ms.
    uint32_t margin = Ms(30), step = Ms(5);
    uint32_t headroom = Ms(40), trim_band = Ms(3), insert_band = Ms(3), far_band = Ms(20);
    uint64_t smooth_us = 500000, gap_us = 50000;
    uint32_t cheap_target = Ms(100);
    uint64_t remember_us = 15000000, remember_cheap_us = 120000000, floor_us = 10000000;
    uint32_t slip_every = 200, slip_every_far = 100;
    // Over one shared-mode engine period (10 ms), so a device looked at every
    // 5 ms gets the next 10 ms before an engine pass finds it empty.
    uint32_t conceal_below = Ms(15);
  };
  struct Counters {
    uint64_t underruns = 0, silences = 0, overruns = 0, overrun_frames = 0;
    uint64_t inserted_frames = 0, trimmed_frames = 0, written_frames = 0;
    uint64_t concealed_frames = 0, late_concealments = 0;
    uint64_t uncovered = 0;  // needs no allowed depth would have met
  };
  explicit AudioJitterBuffer(Config config = Config()) : config_(config), target_(config.min_target) {}
  static bool Valid(size_t size) { return size && size % kFrameBytes == 0 && size <= kMaxPacketBytes; }

  // Every packet received, before it is decoded (one that comes too late to
  // be played too): its Host capture-clock PTS and this PC's time (us).
  void Arrived(uint64_t pts_us, uint64_t now_us) {
    const int64_t offset = int64_t(now_us) - int64_t(pts_us);
    int64_t floor = offset;
    for (const auto& slot : floor_) if (slot.set && now_us - slot.at < config_.floor_us) floor = std::min(floor, slot.value);
    // A second apart is another clock (the Host's capture started over), not a late packet.
    if (offset - floor > 1000000) { for (auto& slot : floor_) slot.set = false; floor = offset; }
    Slot& slot = floor_[(now_us / kSlotUs) % floor_.size()];
    if (!slot.set || now_us / kSlotUs != slot.at / kSlotUs) slot = {now_us, offset, true}; else slot.value = std::min(slot.value, offset);
    // Due before the last late packet came and no later than it was: held up behind it.
    const uint64_t late = uint64_t(offset - floor);
    if (held_until_ <= now_us && now_us - late <= held_until_ && late <= held_late_) return;
    held_until_ = now_us; held_late_ = late;
    Need(late, now_us);
  }

  // A decoded packet: its Host capture-clock PTS and this PC's time (us).
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
    if (playing_ && device_frames == 0) {
      playing_ = false; dry_at_ = std::min(now_us, pulled_at_ + FramesUs(device_)); slip_ = 0; take_ = 1;
      if (queued_) Classify(chunks_.front().pts_us + FramesUs(front_), now_us); else awaiting_resume_ = true;
    }
    Retarget(now_us);
    device_ = device_frames; pulled_at_ = now_us;
    if (!playing_) {
      if (!queued_ || queued_ + device_frames < target_) return 0;
      playing_ = true; smoothed_ = double(queued_ + device_frames); smoothed_at_ = now_us;
    }
    const uint32_t fill = uint32_t(std::min<uint64_t>(target_, uint64_t(device_frames) + room));
    const uint32_t want = fill > device_frames ? fill - device_frames : 0;
    uint32_t written = 0;
    while (written < want) {
      if (Next(out + size_t(written) * kFrameBytes)) { ++written; continue; }
      if (!slip_ || !queued_ || device_frames + written >= config_.conceal_below) break;
      slip_ = 0; take_ = 1;  // nothing comes after them in time: no slip, the frames themselves
    }
    counters_.written_frames += written; device_ = device_frames + written;
    const double level = double(queued_) + device_;
    smoothed_ += (level - smoothed_) * std::min(1.0, double(now_us - smoothed_at_) / double(config_.smooth_us));
    smoothed_at_ = now_us;
    correction_ = smoothed_ > double(target_) + config_.trim_band ? -1 : smoothed_ < double(target_) - config_.insert_band ? 1 : 0;
    far_ = std::abs(smoothed_ - double(target_)) > double(config_.far_band);
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
  // Audio concealed since the last received audio came after all: counted,
  // once per run of concealment (Arrived measured how late it was).
  void NoteLate() {
    if (!concealing_ || late_noted_) return;
    late_noted_ = true; ++counters_.late_concealments;
  }
  // Concealed audio stood in for a pause of the VM's sound, and what came now
  // is new sound: a silence, primed again to the target like any after one.
  // (When the device already ran dry, Push counted it.)
  void NotePause() {
    if (!playing_) return;
    ++counters_.silences; playing_ = false; slip_ = 0; take_ = 1;
  }

  // The Host capture-clock PTS at the device's play head now, while playing.
  std::optional<int64_t> PlayingPtsUs(uint64_t now_us) const {
    if (!playing_ || !device_) return std::nullopt;
    const int64_t elapsed = int64_t(std::min<uint64_t>(now_us - pulled_at_, FramesUs(device_)));
    return int64_t(ended_pts_) - int64_t(FramesUs(device_)) + elapsed;
  }
  void Clear() {
    chunks_.clear(); queued_ = front_ = device_ = 0; playing_ = awaiting_resume_ = concealing_ = late_noted_ = far_ = false;
    correction_ = 0; slip_ = 0; take_ = 1; history_ = {}; newest_ = 0;
    for (auto& slot : floor_) slot.set = false;
    for (auto& slot : needs_) slot.set = false;
    for (auto& slot : cheap_) slot.set = false;
    held_until_ = held_late_ = 0; target_ = config_.min_target;
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
  // One slot of a measurement: when its span was first written, and its value.
  struct Slot { uint64_t at = 0; int64_t value = 0; bool set = false; };
  static constexpr uint64_t kSlotUs = 1000000, kCheapSlotUs = 8000000, kPacketUs = 10000;
  // The sound that ran dry goes on (underrun) or starts again later (silence).
  // Audio this buffer dropped itself (overrun, trim) counts as gone on;
  // concealed audio does not (it is not the VM's sound).
  void Classify(uint64_t pts_us, uint64_t now_us) {
    if (pts_us > continued_pts_ + config_.gap_us) { ++counters_.silences; return; }
    ++counters_.underruns;
    // The depth it had did not last the time it then stayed dry; and it ran
    // dry from wherever between two packets the level stood.
    const uint32_t had = target_ > config_.margin ? target_ - config_.margin : 0;
    Need(FramesUs(had) + (now_us > dry_at_ ? now_us - dry_at_ : 0) + kPacketUs, now_us);
  }
  // A depth (as lateness, us) that would have been needed just now. False:
  // deeper than any target allowed.
  bool Need(uint64_t lateness_us, uint64_t now_us) {
    const uint64_t frames = std::min<uint64_t>(lateness_us, 10000000) * kRate / 1000000;
    if (frames + config_.margin > config_.max_target) { ++counters_.uncovered; return false; }
    Keep(needs_[(now_us / kSlotUs) % needs_.size()], kSlotUs, int64_t(frames), now_us);
    if (frames + config_.margin <= config_.cheap_target) Keep(cheap_[(now_us / kCheapSlotUs) % cheap_.size()], kCheapSlotUs, int64_t(frames), now_us);
    Retarget(now_us);
    return true;
  }
  static void Keep(Slot& slot, uint64_t slot_us, int64_t frames, uint64_t now_us) {
    if (!slot.set || now_us / slot_us != slot.at / slot_us) slot = {now_us, frames, true}; else slot.value = std::max(slot.value, frames);
  }
  void Retarget(uint64_t now_us) {
    int64_t need = 0;
    for (const auto& slot : needs_) if (slot.set && now_us - slot.at < config_.remember_us) need = std::max(need, slot.value);
    for (const auto& slot : cheap_) if (slot.set && now_us - slot.at < config_.remember_cheap_us) need = std::max(need, slot.value);
    const uint32_t wanted = uint32_t(std::clamp<uint64_t>((uint64_t(need) + config_.margin + config_.step - 1) / config_.step * config_.step, config_.min_target, config_.max_target));
    if (wanted >= target_) { target_ = wanted; lowered_at_ = now_us; return; }
    // Down a step in the time the fastest slip takes a step off the level.
    const uint64_t step_us = FramesUs(config_.step) * config_.slip_every_far;
    const uint64_t steps = std::min<uint64_t>((now_us - lowered_at_) / step_us, (target_ - wanted + config_.step - 1) / config_.step);
    target_ = uint32_t(std::max<uint64_t>(wanted, target_ - std::min<uint64_t>(target_, steps * config_.step))); lowered_at_ += steps * step_us;
  }
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

  // The resampler. Passing: each output frame is the next queued frame. In a
  // slip of `span_` output frames the read position moves span_ + slip_
  // frames (slip_ +1 plays fast and loses a frame, -1 plays slow and gains
  // one), so it starts and ends on a whole frame. take_ is how many queued
  // frames the next output frame moves on by.
  bool Next(uint8_t* out) {
    if (!slip_ && correction_ && queued_ > kTaps / 2) {
      slip_ = -correction_; span_ = far_ ? config_.slip_every_far : config_.slip_every; at_ = 0;
      ++(slip_ > 0 ? counters_.trimmed_frames : counters_.inserted_frames);
    }
    if (queued_ < take_ + (slip_ ? kTaps / 2 : 0)) return false;
    for (uint32_t n = 0; n < take_; ++n) Take();
    const uint32_t moved = slip_ ? at_ * uint32_t(int32_t(span_) + slip_) : 0, phase = slip_ ? moved % span_ : 0;
    if (!phase) std::memcpy(out, &history_[newest_], kFrameBytes);
    else {
      // The frame `phase / span_` after the newest taken one: taps over the
      // eight taken last and the eight to come.
      const float* taps = Taps(phase * (kPhases / span_));
      float left = 0, right = 0;
      for (uint32_t k = 0; k < kTaps / 2; ++k) {
        const auto& past = history_[(newest_ + kTaps / 2 - k) % (kTaps / 2)];
        left += taps[kTaps / 2 - 1 - k] * past[0]; right += taps[kTaps / 2 - 1 - k] * past[1];
        int16_t ahead[2]; Peek(k, ahead);
        left += taps[kTaps / 2 + k] * ahead[0]; right += taps[kTaps / 2 + k] * ahead[1];
      }
      const int16_t frame[2] = {Sample(left), Sample(right)};
      std::memcpy(out, frame, kFrameBytes);
    }
    if (!slip_) { take_ = 1; return true; }
    const uint32_t rate = uint32_t(int32_t(span_) + slip_);
    if (++at_ == span_) { take_ = rate - (at_ - 1) * rate / span_; slip_ = 0; }
    else take_ = at_ * rate / span_ - (at_ - 1) * rate / span_;
    return true;
  }
  void Take() {
    const auto& chunk = chunks_.front();
    newest_ = (newest_ + 1) % (kTaps / 2);
    std::memcpy(&history_[newest_], chunk.bytes.data() + size_t(front_) * kFrameBytes, kFrameBytes);
    ended_pts_ = chunk.pts_us + FramesUs(front_ + 1);
    Skip(1);
  }
  // Queued frame `index` (0 the next); Next keeps that many waiting in a slip.
  void Peek(uint32_t index, int16_t frame[2]) const {
    uint32_t skip = front_ + index; frame[0] = frame[1] = 0;
    for (const auto& chunk : chunks_) {
      const uint32_t frames = uint32_t(chunk.bytes.size() / kFrameBytes);
      if (skip < frames) { std::memcpy(frame, chunk.bytes.data() + size_t(skip) * kFrameBytes, kFrameBytes); return; }
      skip -= frames;
    }
  }
  static int16_t Sample(float value) { return int16_t(std::lround(std::clamp(value, -32768.0f, 32767.0f))); }
  // Tap t weighs the frame t - (kTaps / 2 - 1) from the newest taken one, for
  // a point `phase / kPhases` of a frame after it: a sinc under a Blackman
  // window, scaled to sum 1.
  static const float* Taps(uint32_t phase) {
    struct Table {
      std::array<std::array<float, kTaps>, kPhases> taps;
      Table() {
        const double pi = 3.14159265358979323846, half = kTaps / 2;
        for (uint32_t p = 0; p < kPhases; ++p) {
          std::array<double, kTaps> row; double sum = 0;
          for (uint32_t t = 0; t < kTaps; ++t) {
            const double x = double(t) - (half - 1) - double(p) / kPhases;
            const double sinc = x == 0 ? 1.0 : std::sin(pi * x) / (pi * x);
            const double window = std::abs(x) >= half ? 0.0 : 0.42 + 0.5 * std::cos(pi * x / half) + 0.08 * std::cos(2 * pi * x / half);
            row[t] = sinc * window; sum += row[t];
          }
          for (uint32_t t = 0; t < kTaps; ++t) taps[p][t] = float(row[t] / sum);
        }
      }
    };
    static const Table table;
    return table.taps[phase].data();
  }

  Config config_;
  std::deque<Chunk> chunks_;
  uint32_t queued_ = 0, front_ = 0, device_ = 0, target_;
  uint64_t ended_pts_ = 0, continued_pts_ = 0, pulled_at_ = 0, smoothed_at_ = 0, dry_at_ = 0, lowered_at_ = 0;
  double smoothed_ = 0;
  int correction_ = 0;  // +1: the level is under the target (play slow), -1: over it (play fast)
  bool far_ = false;
  bool playing_ = false, awaiting_resume_ = false;
  bool concealing_ = false, late_noted_ = false;  // concealed audio since the last received; NoteLate counted it
  int32_t slip_ = 0;                  // the slip under way: +1 loses a frame, -1 gains one
  uint32_t span_ = 0, at_ = 0, take_ = 1;
  std::array<std::array<int16_t, 2>, kTaps / 2> history_{};  // the frames taken last, newest at newest_
  uint32_t newest_ = 0;
  std::array<Slot, 10> floor_{};   // per second: the earliest arrival against PTS
  std::array<Slot, 16> needs_{};   // per second: the deepest need
  std::array<Slot, 16> cheap_{};   // per eight seconds: the deepest need a target up to cheap_target meets
  uint64_t held_until_ = 0, held_late_ = 0;  // the last packet measured: when it came and how late
  Counters counters_;
};
}
#endif
