#pragma once
// Game mouse over USB (docs/usb-redirection.md). While the session
// window holds the mouse (relative_mouse.h), this PC's Raw Input becomes the
// reports of Multitap's virtual USB HID mouse, which the Host plugs into the VM
// through usb-v1, so a game reads a real USB mouse instead of injected input.
// A port of ToTB Link's smooth mode (src/UsbBridge.App/RawInput.cs
// VirtualMouseState, VirtualHid.cs SyntheticHidDevice): relative motion, five
// buttons, wheel and horizontal wheel, every mouse of this PC together.
// Absolute pointers (pen, touch, tablets) never get here: they end capture.
//
// The Host answers the device's control requests itself and keeps interrupt
// reads waiting here ahead of the VM's own. A Host that sends a control
// request instead is one that does not know the device: the caller gives it
// up and uses the injected path (SendInput) for this session.
//
// Standard C++ only, ASCII only. Everything runs on the window's thread; Dart
// owns the relay socket and passes frames both ways.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace multitap::game_mouse {
using Bytes = std::vector<uint8_t>;
// 5 button bits, X and Y as signed 16-bit, wheel and pan (AC Pan) signed 8-bit.
using Report = std::array<uint8_t, 7>;
constexpr uint8_t kEndpoint = 0x81, kInterrupt = 3;
constexpr size_t kReportLength = 7;

// usb-v1 relay frames (docs/usb-redirection.md): 24-byte little-endian header.
constexpr uint8_t kSubmit = 1, kReturn = 2, kUnlink = 3, kUnlinkReturn = 4;
constexpr int32_t kStall = -32, kReset = -104;
struct Frame {
  uint8_t type = 0, device = 0, endpoint = 0, transfer = 0;
  uint32_t seq = 0, length = 0;
  int32_t status = 0;
  uint8_t setup[8]{};
  Bytes payload;
};
inline uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void PutLe32(Bytes& out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(value >> shift));
}
inline std::optional<Frame> Decode(const uint8_t* data, size_t size) {
  if (!data || size < 24) return std::nullopt;
  Frame frame;
  frame.type = data[0];
  frame.device = data[1];
  frame.endpoint = data[2];
  frame.transfer = data[3];
  frame.seq = Le32(data + 4);
  frame.length = Le32(data + 8);
  frame.status = int32_t(Le32(data + 12));
  std::memcpy(frame.setup, data + 16, 8);
  frame.payload.assign(data + 24, data + size);
  return frame;
}
inline Bytes Encode(const Frame& frame) {
  Bytes out{frame.type, frame.device, frame.endpoint, frame.transfer};
  PutLe32(out, frame.seq);
  PutLe32(out, frame.length);
  PutLe32(out, uint32_t(frame.status));
  out.insert(out.end(), frame.setup, frame.setup + 8);
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());
  return out;
}

inline Report MakeReport(int buttons, int dx, int dy, int wheel, int pan) {
  return Report{uint8_t(buttons & 0x1F), uint8_t(dx), uint8_t(uint16_t(dx) >> 8), uint8_t(dy),
                uint8_t(uint16_t(dy) >> 8), uint8_t(wheel), uint8_t(pan)};
}
// One movement as reports: beyond 16 bits (or a wheel beyond 8) it is split,
// so nothing is lost (VirtualHidReports.Mouse).
inline std::vector<Report> Reports(int buttons, int64_t dx, int64_t dy, int64_t wheel, int64_t pan) {
  std::vector<Report> out;
  do {
    const int64_t x = std::clamp<int64_t>(dx, -32767, 32767), y = std::clamp<int64_t>(dy, -32767, 32767);
    const int64_t w = std::clamp<int64_t>(wheel, -127, 127), p = std::clamp<int64_t>(pan, -127, 127);
    out.push_back(MakeReport(buttons, int(x), int(y), int(w), int(p)));
    dx -= x;
    dy -= y;
    wheel -= w;
    pan -= p;
  } while (dx || dy || wheel || pan);
  return out;
}
// [into] takes the motion of [next]: same buttons, sums within range. A button
// change never joins, so no press or release is lost (SyntheticHidDevice.Join).
inline bool Join(Report& into, const Report& next) {
  if (into[0] != next[0]) return false;
  const auto wide = [](const Report& r, int at) { return int(int16_t(uint16_t(r[at] | r[at + 1] << 8))); };
  const int x = wide(into, 1) + wide(next, 1), y = wide(into, 3) + wide(next, 3);
  const int wheel = int(int8_t(into[5])) + int(int8_t(next[5])), pan = int(int8_t(into[6])) + int(int8_t(next[6]));
  if (x < -32767 || x > 32767 || y < -32767 || y > 32767 || wheel < -127 || wheel > 127 || pan < -127 || pan > 127) return false;
  into = MakeReport(into[0], x, y, wheel, pan);
  return true;
}

// Raw Input (RAWMOUSE usButtonFlags / usButtonData, per source device) as
// reports (VirtualMouseState). Buttons are the physical ones, as Raw Input
// gives them; this PC's own swapped buttons (left-handed) apply here, since
// the VM keeps its own setting. 120 wheel units are one notch; finer wheels
// keep their remainders.
class MouseState {
 public:
  explicit MouseState(bool swap = false) : swap_(swap) {}
  std::vector<Report> Mouse(uintptr_t source, int dx, int dy, unsigned flags, int16_t data, int64_t now_ms) {
    int state = buttons_[source];
    for (int bit = 0; bit < 5; ++bit) {
      if (flags & (1u << (bit * 2))) state |= 1 << bit;
      if (flags & (2u << (bit * 2))) state &= ~(1 << bit);
    }
    buttons_[source] = state;
    int wheel = 0, pan = 0;
    if (flags & 0x0400) { wheel_rest_ += data; wheel = wheel_rest_ / 120; wheel_rest_ -= wheel * 120; }
    if (flags & 0x0800) { pan_rest_ += data; pan = pan_rest_ / 120; pan_rest_ -= pan * 120; }
    int combined = 0;
    for (const auto& [_, held] : buttons_) combined |= held;
    if (!dx && !dy && !wheel && !pan && combined == held_) return {};
    for (int bit = 0; bit < 5; ++bit) if (combined & ~held_ & (1 << bit)) since_[size_t(bit)] = now_ms;
    held_ = combined;
    return Reports(Sent(held_), dx, dy, wheel, pan);
  }
  // A button let go while this window did not see it (another desktop took
  // the input) would stay pressed in the VM: what Windows says is up now, and
  // was held long enough to be sure, goes up there too. [physical_down] is by
  // button bit, as GetAsyncKeyState reports the physical buttons.
  std::optional<Report> Check(const std::function<bool(int)>& physical_down, int64_t now_ms) {
    int up = 0;
    for (int bit = 0; bit < 5; ++bit)
      if ((held_ & (1 << bit)) && now_ms - since_[size_t(bit)] > 300 && !physical_down(bit)) up |= 1 << bit;
    if (!up) return std::nullopt;
    for (auto& [_, held] : buttons_) held &= ~up;
    held_ &= ~up;
    return MakeReport(Sent(held_), 0, 0, 0, 0);
  }
  // Every button up, when capture ends.
  std::optional<Report> ReleaseAll() {
    const bool any = held_ != 0;
    buttons_.clear();
    held_ = wheel_rest_ = pan_rest_ = 0;
    if (!any) return std::nullopt;
    return MakeReport(0, 0, 0, 0, 0);
  }
  bool AnyHeld() const { return held_ != 0; }

 private:
  int Sent(int physical) const {
    return swap_ ? (physical & ~3) | (physical & 1) << 1 | (physical & 2) >> 1 : physical;
  }
  std::map<uintptr_t, int> buttons_;
  std::array<int64_t, 5> since_{};
  int held_ = 0, wheel_rest_ = 0, pan_rest_ = 0;
  bool swap_;
};

// The device's side of usb-v1 for one attachment. Reads the Host keeps waiting
// here are answered with reports as they happen; without a read waiting,
// reports wait in order (motion joined) and ones older than 300 ms are dropped,
// the newest standing for the state now (SyntheticHidDevice). Answers are
// paced: never more than one per millisecond (the relay's message budget), and
// otherwise spread to the rate the Host's reads come back at (the round trip
// over the reads it keeps waiting), so a slow link gets even reports instead
// of bursts that use every read and then a gap. Before the first round trip is
// measured the pace is 4 ms, today's moveRelative pace.
class Device {
 public:
  using Send = std::function<void(const Bytes&)>;
  static constexpr int64_t kStaleUs = 300000, kReadingUs = 1000000, kMinIntervalUs = 1000, kFirstIntervalUs = 4000, kMaxIntervalUs = 50000;
  enum class Seen { kOk, kOlderHost, kInvalid };
  Device(uint8_t id, Send send) : id_(id), send_(std::move(send)) {}

  // A Host frame for this device (submit or unlink).
  Seen Frame(const uint8_t* data, size_t size, int64_t now_us) {
    const auto frame = Decode(data, size);
    if (!frame || frame->device != id_) return Seen::kInvalid;
    if (frame->type == kUnlink) {
      const auto held = std::find(held_.begin(), held_.end(), frame->length);
      const bool waiting = held != held_.end();
      if (waiting) held_.erase(held);
      Reply(kUnlinkReturn, frame->seq, 0, 0, waiting ? kReset : 0, {});
      if (waiting) Reply(kReturn, frame->length, kEndpoint, kInterrupt, kReset, {});
      answered_.clear();  // the reads coming back no longer pair with answers
      return Seen::kOk;
    }
    if (frame->type != kSubmit) return Seen::kInvalid;
    const bool read = frame->transfer == kInterrupt && frame->endpoint == kEndpoint && frame->length >= kReportLength && frame->payload.empty();
    if (frame->transfer == 0 || older_ || !read) {
      // A Host that forwards control does not answer this device itself.
      Reply(kReturn, frame->seq, frame->endpoint, frame->transfer, kStall, {});
      if (frame->transfer == 0 && !confirmed_) older_ = true;
      return older_ ? Seen::kOlderHost : Seen::kOk;
    }
    confirmed_ = true;
    held_.push_back(frame->seq);
    most_held_ = std::max(most_held_, held_.size());
    last_read_us_ = now_us;
    if (!answered_.empty()) {
      // Reads come back in the order they were answered: the gap is the round trip.
      const int64_t sample = now_us - answered_.front();
      answered_.pop_front();
      rtt_us_ = rtt_us_ ? (7 * rtt_us_ + sample) / 8 : sample;
    }
    Answer(now_us);
    return Seen::kOk;
  }
  // A report from this PC's input.
  void Report(const game_mouse::Report& report, int64_t now_us) {
    if (!queued_.empty() && Join(queued_.back().first, report)) {
      queued_.back().second = now_us;
    } else {
      queued_.emplace_back(report, now_us);
      while (queued_.size() > 256) queued_.pop_front();
    }
    Answer(now_us);
  }
  void Tick(int64_t now_us) { Answer(now_us); }
  // When a paced report may go (the caller wakes then), or nothing to wait for.
  std::optional<int64_t> Wake() const {
    if (held_.empty() || queued_.empty()) return std::nullopt;
    return last_answer_us_ + Interval();
  }
  // The VM reads it now: a Host that answers it itself, and one of its reads
  // waiting here now or within the last second.
  bool TakesInput(int64_t now_us) const {
    return confirmed_ && !older_ && (!held_.empty() || now_us - last_read_us_ < kReadingUs);
  }
  bool confirmed() const { return confirmed_ && !older_; }
  bool older() const { return older_; }
  size_t held() const { return held_.size(); }
  size_t queued() const { return queued_.size(); }
  int64_t Interval() const {
    // Until a round trip is known, the injected path's pace: a first stroke
    // must not use every read at once and then wait a whole round trip.
    if (!rtt_us_) return kFirstIntervalUs;
    return std::clamp<int64_t>(rtt_us_ / int64_t(std::max<size_t>(1, most_held_)), kMinIntervalUs, kMaxIntervalUs);
  }

 private:
  void Answer(int64_t now_us) {
    if (held_.empty() || queued_.empty() || now_us - last_answer_us_ < Interval()) return;
    // The oldest report; stale ones dropped, the newest of them as a state.
    while (queued_.size() > 1 && now_us - queued_.front().second > kStaleUs) queued_.pop_front();
    auto [report, at] = queued_.front();
    queued_.pop_front();
    if (now_us - at > kStaleUs) report = MakeReport(report[0], 0, 0, 0, 0);
    Reply(kReturn, held_.front(), kEndpoint, kInterrupt, 0, Bytes(report.begin(), report.end()));
    held_.pop_front();
    last_answer_us_ = now_us;
    answered_.push_back(now_us);
    while (answered_.size() > 64) answered_.pop_front();
  }
  void Reply(uint8_t type, uint32_t seq, uint8_t endpoint, uint8_t transfer, int32_t status, Bytes payload) {
    game_mouse::Frame out;
    out.type = type;
    out.device = id_;
    out.endpoint = endpoint;
    out.transfer = transfer;
    out.seq = seq;
    out.length = uint32_t(payload.size());
    out.status = status;
    out.payload = std::move(payload);
    if (send_) send_(Encode(out));
  }

  uint8_t id_;
  Send send_;
  std::deque<uint32_t> held_;
  std::deque<std::pair<game_mouse::Report, int64_t>> queued_;
  std::deque<int64_t> answered_;
  int64_t rtt_us_ = 0, last_answer_us_ = std::numeric_limits<int64_t>::min() / 2, last_read_us_ = std::numeric_limits<int64_t>::min() / 2;
  size_t most_held_ = 1;
  bool confirmed_ = false, older_ = false;
};
}  // namespace multitap::game_mouse
