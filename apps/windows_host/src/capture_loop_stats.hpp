#pragma once
// Where the single-output capture loop (agent.hpp) spent the time of one
// stream.stats window (~1 s) outside the measured stages, as fixed numbers
// appended to its "stall" member. Diagnostics only: nothing here paces,
// skips or changes a tick. Holds no pixels, input or text. Standard C++ only.
//   backgroundTicks/backgroundMs        ticks that only waited: the client's view is hidden (background_video.hpp)
//   secureHoldTicks/secureHoldMs        secure desktop: the helper was asked for and has not answered (nothing encoded)
//   secureNoticeTicks/secureNoticeMs    secure desktop: the notice picture instead of the capture
//   secureHelperTicks/secureHelperMs    secure desktop: the SYSTEM helper's frames instead of the capture
//   follows/followMs                    capture rebuilds after a mode change (capture_follow.hpp), however they ended
//   overshootMaxUs                      the longest a pacing wait returned after the time it meant to (FramePacer::late)
//   tailMaxUs                           the longest time from a tick's encode end to its pacing wait (cursor, statistics,
//                                       rescale and rebuild work that no stage counts); a window reports the ticks
//                                       measured since the previous report, so a slow statistics tick shows in the next
//   pipeWriteMaxUs                      worker only: the longest synchronous write of one packet to the service pipe
//                                       (pipe_packet.hpp), including the wait for the pipe's write lock
// Times are wall time from the tick's start until the loop went on. Counts and
// times reset with each report.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>

namespace host {
class CaptureLoopStats {
 public:
  enum class Skip : uint8_t { background, secureHold, secureNotice, secureHelper, follow };
  void skipped(Skip reason, std::chrono::steady_clock::duration spent) {
    auto& entry = entries_[size_t(reason)];
    if (entry.count < UINT32_MAX) ++entry.count;
    entry.us += uint64_t(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(spent).count()));
  }
  void overshoot(std::chrono::microseconds late) { overshootMaxUs_ = std::max(overshootMaxUs_, uint64_t(std::max<int64_t>(0, late.count()))); }
  void tail(std::chrono::steady_clock::duration spent) {
    tailMaxUs_ = std::max(tailMaxUs_, uint64_t(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(spent).count())));
  }
  // The members after ",\"changeLateMs\":N" inside "stall"; resets the window.
  // `pipeWriteMaxUs`: a worker's PipeWriteMax::take(); nullopt elsewhere.
  std::string takeJson(std::optional<uint64_t> pipeWriteMaxUs = std::nullopt) {
    static constexpr const char* names[] = {"background", "secureHold", "secureNotice", "secureHelper", "follow"};
    std::string out;
    for (size_t n = 0; n < std::size(names); ++n) {
      const std::string name = names[n];
      // "follows" counts rebuilds; the others count ticks.
      out += ",\"" + (n == size_t(Skip::follow) ? std::string("follows") : name + "Ticks") + "\":" + std::to_string(entries_[n].count);
      out += ",\"" + name + "Ms\":" + std::to_string(std::min<uint64_t>((entries_[n].us + 500) / 1000, maxMs));
    }
    out += ",\"overshootMaxUs\":" + std::to_string(std::min(overshootMaxUs_, maxUs));
    out += ",\"tailMaxUs\":" + std::to_string(std::min(tailMaxUs_, maxUs));
    if (pipeWriteMaxUs) out += ",\"pipeWriteMaxUs\":" + std::to_string(std::min(*pipeWriteMaxUs, maxUs));
    *this = {};
    return out;
  }
  // Bounds of the reported numbers (the stall member's 600 s).
  static constexpr uint64_t maxMs = 600000, maxUs = 600000000;
 private:
  struct Entry { uint32_t count = 0; uint64_t us = 0; };
  Entry entries_[5]{};
  uint64_t overshootMaxUs_ = 0, tailMaxUs_ = 0;
};

// The longest of concurrent durations (writes from the capture, audio and
// control threads), taken by the capture thread's report.
class PipeWriteMax {
 public:
  void observe(std::chrono::steady_clock::duration spent) {
    const auto us = uint64_t(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(spent).count()));
    auto seen = value_.load(std::memory_order_relaxed);
    while (us > seen && !value_.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {}
  }
  uint64_t take() { return value_.exchange(0, std::memory_order_relaxed); }
 private:
  std::atomic<uint64_t> value_{0};
};

}  // namespace host
