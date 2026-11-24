#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>

// The frame rate a narrow path still carries a picture at.
//
// The automatic rate does not go below 0.04 bit per pixel and frame (adaptiveBitrateFloor in
// bitrate_controller.hpp). Under it a frame is all blocks (NVENC on a Tesla P40: QP 42 at
// 0.04, and at 0.02 its worst quality, where it cannot make a frame smaller and sends more
// than its target), and the Microsoft MFT stops following its target at all. At a fixed
// frame rate that floor is a rate: 16 Mbps for 2084x1340 at 144 fps. A path that carried
// less was sent 16 Mbps all the same and lost it (udp_rate_loop_test.cpp: on a link at
// 8 Mbps a third of the frames arrived, and the stream spent most of the time on WebSocket).
//
// So the frame rate follows the target. When a frame's share of it falls under the floor,
// the capture paces at the highest rate whose frames get 0.05 or more, down to 30 a second,
// and the floor of the rate is that of the lowest rate (still 0.04 a frame: what an encoder
// can make of a frame at its worst quality depends on the picture, 0.019 to 0.037 measured
// on NVENC, and the MFT needs 0.04). It goes back up once the next rate's frames would have
// got 0.055 for two seconds, to the highest rate whose frames do. A stream whose target stays
// above its old floor is not touched. The encoder is told the paced rate
// (Encoder::setFrameRate), so the whole target goes into the fewer frames; with an encoder
// that cannot be told the stream keeps its rate and its old floor. A path that does not
// carry the lowest rate's floor (3.4 Mbps for 2084x1340) is beyond this rule.
namespace host {
constexpr double pathFrameFloorBpp = 0.04, pathFrameDownBpp = 0.05, pathFrameUpBpp = 0.055;
constexpr uint64_t pathFrameUpUs = 2000000;
constexpr int pathFrameRates[] = {240, 165, 144, 120, 90, 75, 60, 45, 30};
inline int pathFrameLowest(int requested) { return std::min(30, std::clamp(requested, 1, 240)); }

class PathFrameRate {
 public:
  explicit PathFrameRate(int requested) : lowest_(pathFrameLowest(requested)), requested_(std::clamp(requested, 1, 240)), fps_(requested_) {}
  int fps() const { return fps_; }
  // The lowest rate this stream may be paced at, from the rate it asked for.
  int lowest() const { return std::min(lowest_, requested_); }
  // The rate to pace at for this target (bits a second) and picture. `top`: the highest rate
  // the stream can be paced at now, when that is not the rate it asked for (the encoder's own
  // rate changes when a CPU encoder takes over or the GPU's comes back; the Host's or the
  // client's load lowers it). A stream at its top stays at the new top.
  int decide(uint64_t nowUs, int target, int width, int height, int top = 0) {
    if (top > 0 && top != requested_) { const bool atTop = fps_ >= requested_; requested_ = std::clamp(top, 1, 240); if (atTop || fps_ > requested_) fps_ = requested_; upSince_.reset(); }
    const double second = double(std::max(target, 0)) / (double(std::max(width, 1)) * double(std::max(height, 1)));
    const auto share = [&](int rate) { return second / rate; };  // bits per pixel of one frame at that rate
    if (fps_ > lowest() && share(fps_) < pathFrameFloorBpp) {
      int next = lowest();
      for (const int rate : pathFrameRates) if (rate < fps_ && rate >= lowest() && share(rate) >= pathFrameDownBpp) { next = rate; break; }
      fps_ = next; upSince_.reset();
      return fps_;
    }
    if (fps_ < requested_) {
      int next = requested_;
      for (const int rate : pathFrameRates) if (rate > fps_ && rate < next) next = rate;
      if (share(next) < pathFrameUpBpp) upSince_.reset();
      else if (!upSince_) upSince_ = nowUs;
      else if (nowUs - *upSince_ >= pathFrameUpUs) {
        fps_ = next; upSince_.reset();
        for (const int rate : pathFrameRates) if (rate > fps_ && rate < requested_ && share(rate) >= pathFrameUpBpp) { fps_ = rate; break; }
        if (share(requested_) >= pathFrameUpBpp) fps_ = requested_;
      }
    }
    return fps_;
  }
 private:
  int lowest_, requested_, fps_;
  std::optional<uint64_t> upSince_;
};
}
