#pragma once
// The stream's loopback sound capture, its rules without Windows audio objects
// (media.cpp Loopback::run applies them; audio_capture_control_test.cpp checks them).
//  - Restart: a capture that fails (any WASAPI error, an endpoint that went away)
//    or whose default render endpoint changed is opened again for the life of the
//    stream, after AudioCaptureRestart's backoff. The audio thread never ends for
//    an error; only the stream's stop ends it.
//  - Cadence: the loop wakes on the capture's own event (event-driven loopback,
//    Windows 10 1703 and later) or, until that event is seen to work, on a
//    high-resolution timer every audioPollPeriod. A packet the engine finished
//    leaves within that period, so 10 ms packets leave 10 ms apart instead of
//    1-2-1-2 per 15.6 ms timer tick.
//  - PTS: AudioPtsClock stamps the first sample of each packet from the
//    capture's own position (the QPC time WASAPI gives each packet) and the
//    count of samples sent, so consecutive packets are exactly their length
//    apart; it follows the stream clock slowly and starts over after a gap.
//  - Journal: the worker sends each event as stage 47 of its fixed record
//    (worker.stream.failure); the service writes it as hostAction 297
//    (audioCaptureJournal below). Fixed numbers only: no device name or id.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace host {
// Worker record stage (hostAction 200 stage 247 before the service turns it into 297).
inline constexpr uint32_t audioCaptureRecordStage = 47;
inline constexpr uint32_t audioCaptureAction = 297;

// hostAction 297 `stage`: what happened.
enum class AudioCaptureEvent : uint32_t { started = 1, failed = 2, restarting = 3, deviceChanged = 4 };
// hostAction 297 `cause` of a failed capture: the step that failed.
enum class AudioCaptureStep : uint32_t {
  none = 0, enumerator = 1, endpoint = 2, activate = 3, mixFormat = 4, formatUnsupported = 5, initialize = 6,
  service = 7, start = 8, packetSize = 9, buffer = 10, release = 11, notification = 12, output = 13, event = 14,
};
// hostAction 297 `format` of a started capture: the mix format's sample type.
enum class AudioCaptureSample : uint32_t { float32 = 0, pcm16 = 1, pcm24 = 2, pcm32 = 3 };

// The 32-bit worker record value (error of stage 47). Bits 0-2: the event.
//  started:       bits 3-5 channels - 1, bit 6 event-driven, bits 7-8 sample type, bits 9-31 rate (Hz).
//  failed:        bits 3-6 step, bit 7 the HRESULT's X bit (bit 27), bits 8-15 its facility (low 8 bits),
//                 bits 16-31 its code. A failure that is no HRESULT (format unsupported) carries 0.
//  restarting:    bits 3-31 the attempt (1 the first after a failure or a device change).
//  deviceChanged: bit 3 a capture was running when it changed.
inline uint32_t audioCaptureStarted(uint32_t rate, uint32_t channels, bool events, AudioCaptureSample sample) {
  return uint32_t(AudioCaptureEvent::started) | ((std::clamp<uint32_t>(channels, 1, 8) - 1) << 3) | (events ? 1u << 6 : 0u) |
         (uint32_t(sample) & 3u) << 7 | (std::min<uint32_t>(rate, 0x7fffff) << 9);
}
inline uint32_t audioCaptureFailed(AudioCaptureStep step, int32_t hresult) {
  const uint32_t value = uint32_t(hresult);
  const uint32_t failure = value & 0x80000000u ? value : 0u;
  return uint32_t(AudioCaptureEvent::failed) | (uint32_t(step) & 15u) << 3 | ((failure >> 27) & 1u) << 7 | ((failure >> 16) & 0xffu) << 8 | (failure & 0xffffu) << 16;
}
inline uint32_t audioCaptureRestarting(uint32_t attempt) { return uint32_t(AudioCaptureEvent::restarting) | (std::min<uint32_t>(attempt, 0x1fffffff) << 3); }
inline uint32_t audioCaptureDeviceChanged(bool running) { return uint32_t(AudioCaptureEvent::deviceChanged) | (running ? 1u << 3 : 0u); }

// What the service writes for a stage 47 value (hostAction 297): stage = event, and per event
//  started: format = sample type, items = channels, expected = rate (Hz), active = event-driven;
//  failed: cause = step, error = the HRESULT (0 for none), ok false;
//  restarting: expected = attempt; deviceChanged: active = a capture was running. ok is false only for failed.
struct AudioCaptureJournal {
  uint32_t stage = 0, cause = 0, format = 0, items = 0, expected = 0;
  int64_t error = 0;
  bool active = false, ok = true, valid = false;
};
inline AudioCaptureJournal audioCaptureJournal(uint32_t value) {
  AudioCaptureJournal row; row.stage = value & 7u;
  switch (AudioCaptureEvent(row.stage)) {
    case AudioCaptureEvent::started: row.items = ((value >> 3) & 7u) + 1; row.active = (value >> 6) & 1u; row.format = (value >> 7) & 3u; row.expected = value >> 9; row.valid = true; break;
    case AudioCaptureEvent::failed: {
      row.cause = (value >> 3) & 15u; row.ok = false; row.valid = true;
      const uint32_t facility = (value >> 8) & 0xffu, code = value >> 16, x = (value >> 7) & 1u;
      row.error = facility || code || x ? int64_t(0x80000000u | (x << 27) | (facility << 16) | code) : 0;
      break;
    }
    case AudioCaptureEvent::restarting: row.expected = value >> 3; row.valid = true; break;
    case AudioCaptureEvent::deviceChanged: row.active = (value >> 3) & 1u; row.valid = true; break;
    default: break;
  }
  return row;
}
// A stage 47 record is a failure only when it says so; the rest are observations.
inline bool audioCaptureRecordOk(uint32_t value) { return (value & 7u) != uint32_t(AudioCaptureEvent::failed); }

// When to open the capture again. Not thread-safe (the audio thread).
class AudioCaptureRestart {
 public:
  using Clock = std::chrono::steady_clock;
  // The waits after the 1st, 2nd, ... failure in a row; the last one repeats for the life of the stream.
  static constexpr std::chrono::milliseconds delays[] = {std::chrono::milliseconds(50), std::chrono::milliseconds(200), std::chrono::milliseconds(500), std::chrono::milliseconds(1000), std::chrono::milliseconds(2000)};
  // A capture that ran this long counts as working: the next failure starts the backoff from its first wait.
  static constexpr std::chrono::seconds healthyAfter{5};
  // A changed default device is opened after this (Windows sends one change per role, close together).
  static constexpr std::chrono::milliseconds settle{100};
  // The journal: the first `burst` events of a stream, then one per `spacing`.
  static constexpr uint32_t burst = 12;
  static constexpr std::chrono::seconds spacing{60};

  // The capture opened at `now`.
  void opened(Clock::time_point now) { openedAt_ = now; open_ = true; }
  // The capture failed at `now` (opening or while it ran): the time of the next attempt.
  Clock::time_point failed(Clock::time_point now) {
    if (open_ && now - openedAt_ >= healthyAfter) failures_ = 0;
    open_ = false;
    const size_t index = std::min<size_t>(failures_, std::size(delays) - 1);
    ++failures_; ++attempt_;
    due_ = now + delays[index];
    return due_;
  }
  // The default render endpoint changed at `now`: opened again soon, whatever the backoff was.
  Clock::time_point changed(Clock::time_point now) { open_ = false; failures_ = 0; ++attempt_; due_ = now + settle; return due_; }
  Clock::time_point due() const { return due_; }
  // Attempts since the stream's capture first opened (the journal's restart number).
  uint32_t attempt() const { return attempt_; }
  uint32_t failuresInRow() const { return failures_; }
  // Whether an event at `now` goes to the journal (a failing device must not fill it).
  bool record(Clock::time_point now) {
    if (recorded_ < burst) { ++recorded_; lastRecord_ = now; return true; }
    if (now - lastRecord_ >= spacing) { lastRecord_ = now; return true; }
    return false;
  }
 private:
  Clock::time_point openedAt_{}, due_{}, lastRecord_{};
  bool open_ = false;
  uint32_t failures_ = 0, attempt_ = 0, recorded_ = 0;
};

// How long the capture loop sleeps at most before it looks again. Until its
// event was seen to fire, the loop polls (event-driven loopback before Windows
// 10 1703 never fires); after that the event wakes it and the timer only keeps
// the silence flush and the stop on time.
inline constexpr std::chrono::microseconds audioPollPeriod{2000};
inline constexpr std::chrono::microseconds audioEventBackstop{10000};
inline std::chrono::microseconds audioWakePeriod(bool eventsSeen) { return eventsSeen ? audioEventBackstop : audioPollPeriod; }
// An event counts as working once it woke the loop with a packet waiting this many times.
inline constexpr uint32_t audioEventsProven = 3;

// PTS of each packet's first sample (µs on the stream clock, 48 kHz output samples).
class AudioPtsClock {
 public:
  static constexpr int64_t outputRate = 48000;
  // A measurement this far from the continued stamps starts the stamps over (a gap: the VM was
  // silent and WASAPI sent nothing, or the capture was opened again).
  static constexpr int64_t restartUs = 20000;
  // Within that, the stamps follow the measurements by 1/64 of the difference per packet: device
  // clock drift and timer jitter never turn into uneven spacing.
  static constexpr int64_t follow = 64;
  // `measuredUs`: the packet's first frame on the stream clock (the QPC time WASAPI gave it, or the
  // read time minus its length when it gave none); `discontinuity`: WASAPI or the loop said the
  // samples do not follow the previous ones. Returns the packet's PTS; then `advance` by its samples.
  int64_t stamp(int64_t measuredUs, bool discontinuity) {
    const int64_t continued = anchorUs_ + sent_ * 1000000 / outputRate;
    const int64_t difference = measuredUs - continued;
    if (!anchored_ || discontinuity || std::llabs(difference) > restartUs) {
      anchored_ = true; anchorUs_ = std::max<int64_t>(0, measuredUs); sent_ = 0; ++restarts_;
      return anchorUs_;
    }
    anchorUs_ += difference / follow;
    return std::max<int64_t>(0, anchorUs_ + sent_ * 1000000 / outputRate);
  }
  void advance(uint64_t samples) { sent_ += int64_t(samples); }
  void reset() { anchored_ = false; }
  uint32_t restarts() const { return restarts_; }
 private:
  bool anchored_ = false;
  int64_t anchorUs_ = 0, sent_ = 0;
  uint32_t restarts_ = 0;
};
}
