#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

// udp-media-v1 LOSS (native/windows_video/udp_media.hpp) for the encoders with reference
// invalidation (x264, NVENC). Pure bookkeeping: which frames an encoder must stop
// referencing so that its next frame uses only the client's last good frame or older.
namespace host {
// Reference frames to keep for that: the H.264 level's DPB (MaxDpbMbs 184320 at levels 5.1
// and 5.2, the highest a 4K60 stream needs) and at most 16. Motion search still uses one
// reference; only older frames stay available. 1080p: 16 (267 ms at 60 fps), 1440p: 12, 4K: 5.
inline int recoveryReferenceFrames(int width, int height) {
  const int macroblocks = std::max(1, ((width + 15) / 16) * ((height + 15) / 16));
  return std::clamp(184320 / macroblocks, 1, 16);
}
class ReferenceHistory {
 public:
  explicit ReferenceHistory(size_t frames) : frames_(std::max<size_t>(frames, 1)) {}
  size_t frames() const { return frames_; }
  // Every output in order. Every frame is a reference (no B-frames, no non-reference P).
  void encoded(uint64_t pts, bool idr) {
    if (idr) window_.clear();
    window_.push_back(pts);
    while (window_.size() > frames_) window_.pop_front();
  }
  // The frames after `lastGood`, oldest first (none: the client has the newest), or nothing
  // when `lastGood` is no reference the next frame could use: it slid out of the DPB, lies
  // before this IDR period (the client lost the IDR) or belongs to another encoder instance.
  // Then only an IDR repairs the picture.
  std::optional<std::vector<uint64_t>> after(uint64_t lastGood) const {
    for (size_t n = window_.size(); n-- > 0;)
      if (window_[n] == lastGood) return std::vector<uint64_t>(window_.begin() + std::ptrdiff_t(n) + 1, window_.end());
    return std::nullopt;
  }
 private:
  size_t frames_;
  std::deque<uint64_t> window_;
};
}
