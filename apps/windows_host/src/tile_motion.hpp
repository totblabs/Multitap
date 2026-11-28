#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>

// motion-rest-v1: lossless tiles rest while the whole picture keeps moving (a game, a video at full screen).
//
// A tick that wants tiles reads its picture back to the CPU (gpu_capture.hpp: such a tick is never a GPU
// tick), compares it tile by tile and sends what stays still losslessly. A picture that changes almost
// everywhere, frame after frame, has nothing that stays still: the tiles cost the readback, the comparison and
// lossless bytes for nothing that shows, and the Host's load holds the frame rate down. Field 2026-11-28, a
// game at 2012x1268 from a PC with an NVIDIA GPU: tiles were wanted the whole time, 4 to 21 Mbps of lossless
// data went beside the video, the readback waited 1.8 to 2.1 ms a frame, and the size controller held the
// capture at 90 a second for the Host's load (stream_size.hpp, encodeLimit host_cpu) for all 7 minutes. The
// same game with the tiles paused by 게임 마우스 (11-26) ran at 120 a second with no readback wait.
//
// Per second of capture ticks, the second is `moving` when at least movingFrames pictures changed and, where
// the tiles measured them, those changed over at least movingShare of the picture on average, and no pane
// scrolled as a move (lossless_tile_source.hpp tiles-v2: text that scrolls stays exact, which is what tiles
// are for). Small moving parts of a still screen (a clock, a spinner, a video in a window) keep the tiles, and
// so does a video beside a document (half the picture): movingShare is more than half, the whole picture of a
// game or a video at full screen, whose static parts (a game's HUD, a video's bars) are a small share.
// After movingSeconds moving seconds in a row the tiles rest; one second with fewer than stillFrames changed
// pictures (the picture stopped, or nearly) brings them back. While they rest nothing measures how much of
// the picture changes (the ticks are GPU ticks; a CPU readback to measure it would make that tick late), so
// the count of changed pictures alone ends the rest: a part that keeps moving at 15 a second or more (a video
// left playing in a window after its full screen) keeps the rest until it stops.
namespace host {
// The worker's observation stage of a rest that began or ended (hostAction 200 stage 253):
// error = 1 began or 2 ended, plus the session's count of rests << 8. At most one record every recordUs,
// except that the first rest of a stream is always recorded. (50, 51 and 52 are the capture's: topology,
// WGC failed, WGC again; agent.hpp checks that no two worker records share a stage.)
inline constexpr uint32_t tileMotionRecordStage = 53;

class TileMotion {
 public:
  static constexpr uint32_t movingFrames = 40, stillFrames = 15, movingSeconds = 2;
  static constexpr double movingShare = 0.6;
  static constexpr uint64_t windowUs = 1000000, recordUs = 10000000;
  enum class Change { none, began, ended };
  // One capture tick at `nowUs` (monotonic): whether its picture changed; when the tiles compared it, the share of
  // the picture that changed (0..1); whether a pane scrolled as a move on it. The first tick a second after the
  // window began closes it (without itself: it is the next window's first) and may begin or end the rest.
  Change tick(uint64_t nowUs, bool changed, std::optional<double> share = std::nullopt, bool moved = false) {
    if (!started_) { started_ = true; windowAt_ = nowUs; }
    Change change = Change::none;
    if (nowUs - windowAt_ >= windowUs) { change = close(); windowAt_ = nowUs; }
    if (changed) { ++frames_; if (share) { shareSum_ += *share; ++shared_; } }
    if (moved) moved_ = true;
    return change;
  }
  bool resting() const { return resting_; }
  uint64_t rests() const { return rests_; }
  // Whether a change at `nowUs` is recorded (see tileMotionRecordStage), and its record's error value.
  std::optional<uint32_t> record(Change change, uint64_t nowUs) {
    if (change == Change::none) return std::nullopt;
    if (recordedAt_ && nowUs - *recordedAt_ < recordUs) return std::nullopt;
    recordedAt_ = nowUs;
    return uint32_t(change == Change::began ? 1 : 2) | uint32_t(std::min<uint64_t>(rests_, 0xffffff) << 8);
  }

 private:
  Change close() {
    // While the tiles rest there is no share: the count alone decides.
    const bool wide = shared_ ? shareSum_ / double(shared_) >= movingShare : false;
    const bool moving = frames_ >= movingFrames && wide && !moved_;
    const bool still = frames_ < stillFrames;
    frames_ = shared_ = 0; shareSum_ = 0; moved_ = false;
    if (!resting_) {
      movingRun_ = moving ? movingRun_ + 1 : 0;
      if (movingRun_ < movingSeconds) return Change::none;
      resting_ = true; movingRun_ = 0; ++rests_;
      return Change::began;
    }
    if (!still) return Change::none;
    resting_ = false;
    return Change::ended;
  }
  bool started_ = false, resting_ = false, moved_ = false;
  uint64_t windowAt_ = 0, rests_ = 0;
  uint32_t frames_ = 0, shared_ = 0, movingRun_ = 0;
  double shareSum_ = 0;
  std::optional<uint64_t> recordedAt_;
};
}
