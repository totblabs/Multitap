#pragma once
// A capture that came up on GDI because Windows Graphics Capture failed to
// start. WGC made while Windows still applies a new display mode can fail
// (capture_follow.hpp already retries it on a follow); a stream start or a
// picture resize did not, and the GDI copy then ran for the whole stream.
// Field 2026-11-21, a vGPU VM with NVENC: a phone switching between its sizes
// restarted the stream on GDI several times; GDI took 17-33 ms a frame at
// 1142x862-1712x1292 against 0.2-2 ms on WGC, the encode scaler read that as
// an overloaded Host, halved the picture and remembered it, so the phone's
// sharper size came out at 56 % and the app gave it up.
//
// Pure: the schedule and the record code. The capture loop builds the
// captures (agent.hpp).
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace host {
// Before the encoder is made (it opens on the capture's device for the GPU
// path): a few tries, at most about a second added to that start.
inline constexpr std::array<std::chrono::milliseconds, 3> wgcStartRetries{std::chrono::milliseconds(150), std::chrono::milliseconds(300), std::chrono::milliseconds(600)};

// Records (the worker's stage N is hostAction 200 stage 200 + N):
// 251 the capture is on GDI because WGC failed; error = wgcFailureCode.
// 252 the capture is on WGC again; error = milliseconds it was on GDI.
inline constexpr uint32_t wgcFailedStage = 51, wgcRecoveredStage = 52;

// Capture::backendError() as a fixed number: an HRESULT where there is one
// (high bit set), else 2 pixel geometry mismatch (the mode still changing),
// 3 not contiguous, 4 cuts a monitor, 5 WGC unavailable, 1 anything else.
inline uint32_t wgcFailureCode(std::string_view what) {
  const auto hex = [](std::string_view digits) -> std::optional<uint32_t> {
    uint32_t value = 0; size_t used = 0;
    for (const char c : digits) {
      const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (d < 0) break;
      if (++used > 8) return std::nullopt;
      value = value << 4 | uint32_t(d);
    }
    return used ? std::optional<uint32_t>(value) : std::nullopt;
  };
  if (what.starts_with("wgc_hresult_0x")) if (const auto value = hex(what.substr(14)); value && *value) return *value;
  if (const auto at = what.rfind("(0x"); at != std::string_view::npos) if (const auto value = hex(what.substr(at + 3)); value && *value) return *value;
  if (what == "Capture monitor pixel geometry mismatch") return 2;
  if (what == "Selected displays are not contiguous") return 3;
  if (what == "Selected display rectangle cuts a monitor") return 4;
  if (what == "Windows Graphics Capture unavailable") return 5;
  return 1;
}

// While the capture is on GDI: when to try WGC again during the stream. The
// steps grow so a machine where WGC keeps failing pays a few milliseconds
// now and then, not every tick.
class WgcRetry {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::array<std::chrono::milliseconds, 6> steps{std::chrono::milliseconds(500), std::chrono::milliseconds(1000), std::chrono::milliseconds(2000),
                                                                  std::chrono::milliseconds(4000), std::chrono::milliseconds(8000), std::chrono::milliseconds(15000)};
  // A capture was built. True when it entered GDI from WGC or from none (record wgcFailedStage).
  bool built(bool gdi, Clock::time_point now) {
    if (!gdi) { if (since_) recovered_ = now - *since_; since_.reset(); attempt_ = 0; return false; }
    attempt_ = 0; next_ = now + steps[0];
    if (since_) return false;
    since_ = now; return true;
  }
  bool due(Clock::time_point now) const { return since_ && now >= next_; }
  // A try came back on GDI again.
  void failed(Clock::time_point now) { if (attempt_ + 1 < steps.size()) ++attempt_; next_ = now + steps[attempt_]; }
  bool onGdi() const { return since_.has_value(); }
  // After built(false) ended a GDI period: how long it lasted, once.
  std::optional<Clock::duration> takeRecovered() { const auto value = recovered_; recovered_.reset(); return value; }

 private:
  std::optional<Clock::time_point> since_;
  std::optional<Clock::duration> recovered_;
  Clock::time_point next_{};
  size_t attempt_ = 0;
};
}
