#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

// udp-media-v1 LOSS (native/windows_video/udp_media.hpp). Pure bookkeeping for the two ways
// an encoder answers one without an IDR: reference invalidation (x264: which frames it
// must stop referencing so that its next frame uses only the client's last good frame or
// older) and long-term references (NVENC: which kept frame the client holds as well).
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

// A LOSS reaches back as far as the client waited before it gave up, plus its way to the
// Host and the frames coded meanwhile: 0.1-0.2 s, which above 60 fps is more frames than a
// DPB keeps, and on a GPU with one reference frame any distance is too far. Long-term
// references do not slide out: the encoder marks a frame into each of a few slots, a slot
// keeps its frame in both DPBs until that slot is marked again, and the answer to a LOSS is a
// P-frame coded from the newest slot whose frame the client decoded.
//
// The slots: what the DPB of the stream's own H.264 level holds beside the one short-term
// frame, 4 at most. The level is the one in the running stream's SPS, not the highest the
// picture could have: an encoder that would have to raise it to make room marks every slot
// and then codes from none (NVENC on a Tesla P40: 1080p60 is level 4.2, four frames, and a
// fourth slot moved the SPS to level 5 while no answer came from any slot; three work).
// Every slot is filled at once (the IDR, then the frames after it) and from then on the
// oldest is marked again once per interval, so the newest slot the client holds is at most
// one interval older than its last good frame, and a LOSS up to longTermReachUs late finds one.
constexpr uint64_t longTermReachUs = 375000;
// Frames a stream of this level_idc may keep (H.264 Table A-1, MaxDpbMbs), 16 at most; 0 for a level not in it.
inline int levelReferenceFrames(int levelIdc, int width, int height) {
  const int macroblocks = std::max(1, ((width + 15) / 16) * ((height + 15) / 16));
  const int limit = levelIdc == 10 ? 396 : levelIdc == 11 ? 900 : levelIdc == 12 || levelIdc == 13 || levelIdc == 20 ? 2376 : levelIdc == 21 ? 4752 : levelIdc == 22 || levelIdc == 30 ? 8100
    : levelIdc == 31 ? 18000 : levelIdc == 32 ? 20480 : levelIdc == 40 || levelIdc == 41 ? 32768 : levelIdc == 42 ? 34816 : levelIdc == 50 ? 110400 : levelIdc == 51 || levelIdc == 52 ? 184320
    : levelIdc == 60 || levelIdc == 61 || levelIdc == 62 ? 696320 : 0;
  return std::min(limit / macroblocks, 16);
}
inline int longTermSlots(int levelIdc, int width, int height, int supported) { return std::clamp(std::min(levelReferenceFrames(levelIdc, width, height) - 1, supported), 0, 4); }
class LongTermReferences {
 public:
  explicit LongTermReferences(size_t slots, uint64_t reachUs = longTermReachUs)
    : slots_(slots), intervalUs_(reachUs / (slots > 1 ? slots - 1 : 1)) {}
  size_t slots() const { return slots_.size(); }
  uint64_t intervalUs() const { return intervalUs_; }
  // Before a frame is coded: the slot it is to be marked into, if any. An IDR empties every
  // slot and takes the first; a slot that is empty, or whose frame the client never got, is
  // filled by the next frame; otherwise the oldest once per interval.
  std::optional<uint32_t> due(uint64_t pts, bool idr) const {
    if (slots_.empty()) return std::nullopt;
    if (idr) return 0;
    std::optional<uint32_t> oldest;
    for (uint32_t n = 0; n < slots_.size(); ++n) {
      if (!slots_[n].held) return n;
      if (!oldest || slots_[n].pts < slots_[*oldest].pts) oldest = n;
    }
    return pts >= markedAt_ + intervalUs_ ? oldest : std::nullopt;
  }
  // Every output in order, with the slot the encoder reports it marked into.
  void encoded(uint64_t pts, bool idr, std::optional<uint32_t> marked) {
    if (idr) { for (auto& slot : slots_) slot = {}; filledAt_.reset(); }
    if (!marked || *marked >= slots_.size()) return;
    slots_[*marked] = {pts, true, true}; markedAt_ = pts;
    if (!filledAt_ && std::all_of(slots_.begin(), slots_.end(), [](const Slot& slot) { return slot.marked; })) filledAt_ = pts;
  }
  // The encoder did not mark the slot asked for: it and those above it are not used.
  void refused(uint32_t slot) {
    if (slot >= slots_.size()) return;
    slots_.resize(slot);
    if (!filledAt_ && !slots_.empty() && std::all_of(slots_.begin(), slots_.end(), [](const Slot& held) { return held.marked; })) filledAt_ = markedAt_;
  }
  // LOSS: nothing after `lastGood` reached the decoder. The slot to code the next frame
  // from, or nothing: then only an IDR repairs the picture. A slot marked after `lastGood`
  // holds a frame the client never got until it is marked again. While the slots were still
  // being filled for the first time a lost mark leaves the client's DPB with one long-term
  // frame fewer than the encoder's, which only an IDR sets right.
  std::optional<uint32_t> recover(uint64_t lastGood) {
    if (!filledAt_ || lastGood < *filledAt_) return std::nullopt;
    std::optional<uint32_t> newest;
    for (uint32_t n = 0; n < slots_.size(); ++n) {
      auto& slot = slots_[n];
      if (!slot.held) continue;
      if (slot.pts > lastGood) { slot.held = false; continue; }
      if (!newest || slot.pts > slots_[*newest].pts) newest = n;
    }
    return newest;
  }
 private:
  // held: the frame both sides have in this slot; marked: filled at least once since the IDR.
  struct Slot { uint64_t pts = 0; bool held = false, marked = false; };
  std::vector<Slot> slots_;
  uint64_t intervalUs_ = 0, markedAt_ = 0;
  std::optional<uint64_t> filledAt_;
};
}
