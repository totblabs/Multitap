#pragma once
#include "capture_compose.hpp"
#include "frame_encode_policy.hpp"
#include "scroll_detect.hpp"
#include "../../../native/windows_video/lossless_tiles.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace host {
namespace lossless = multitap::lossless;
// ToTBSeamless lossless tiles, host side (docs/lossless-tiles-design.md).
// Client opt-in: its RTT ping nonce starts with this prefix.
inline constexpr const char* losslessTilesPingPrefix = "lt1-";
inline bool losslessTilesOptIn(const std::string& nonce) { return nonce.rfind(losslessTilesPingPrefix, 0) == 0; }
// tiles-v2 (moves): the same opt-in with an "m" after the prefix, "lt1-m<n>". A
// Host that does not know it still reads the tiles-v1 opt-in in it and sends
// plain tiles; this Host may then send moves and rectangles to that client.
inline constexpr const char* losslessTilesMovesPrefix = "lt1-m";
inline bool losslessTilesMoves(const std::string& nonce) { return nonce.rfind(losslessTilesMovesPrefix, 0) == 0; }
// The same client asks for no tiles (게임 마우스 holds the mouse):
// "lt0-<n>". A Host that does not know it reads it as a plain ping and keeps
// its tiles; a later "lt1-" ping turns them on again.
inline constexpr const char* losslessTilesPausePrefix = "lt0-";
inline bool losslessTilesPause(const std::string& nonce) { return nonce.rfind(losslessTilesPausePrefix, 0) == 0; }

// The composed BGRA frame of the current tick: exactly the pixels converted into
// the NV12 that the encoder receives, cursor included.
class ComposedFrame {
 public:
  virtual ~ComposedFrame() = default;
  // Rectangles that can differ from the previous tick; nullopt means anywhere.
  virtual std::optional<std::vector<capture::PixelRect>> dirty() = 0;
  // Disjoint pieces exactly covering each rectangle.
  virtual void visit(std::span<const capture::PixelRect> rects, const capture::ComposedVisitor& visitor) = 0;
};
// A host::Capture (or a test double with the same two members).
template<class CaptureLike>
class CapturedComposedFrame final : public ComposedFrame {
 public:
  explicit CapturedComposedFrame(CaptureLike& capture) : capture_(capture) {}
  std::optional<std::vector<capture::PixelRect>> dirty() override { return capture_.composedDirty(); }
  void visit(std::span<const capture::PixelRect> rects, const capture::ComposedVisitor& visitor) override { capture_.visitComposed(rects, visitor); }
 private:
  CaptureLike& capture_;
};
// Contiguous BGRA (synthetic sources, tests).
class BufferComposedFrame final : public ComposedFrame {
 public:
  BufferComposedFrame(const uint8_t* bgra, int width, int height, std::optional<std::vector<capture::PixelRect>> changed = std::nullopt)
      : bgra_(bgra), width_(width), height_(height), dirty_(std::move(changed)) {}
  std::optional<std::vector<capture::PixelRect>> dirty() override { return dirty_; }
  void visit(std::span<const capture::PixelRect> rects, const capture::ComposedVisitor& visitor) override {
    for (const auto& rect : rects) visitor(bgra_ + (size_t(rect.top) * width_ + rect.left) * 4, size_t(width_) * 4, rect);
  }
 private:
  const uint8_t* bgra_; int width_, height_; std::optional<std::vector<capture::PixelRect>> dirty_;
};

struct LosslessSettings {
  uint64_t settleUs = 200000;      // unchanged this long before a tile is sent
  uint64_t addIntervalUs = 50000;  // at most 20 add packets per second
  size_t rateBytesPerSecond = 1500000, burstBytes = 375000, packetBytes = 256 * 1024;
  double tickBudgetMs = 3.0;       // wall-clock add encoding per tick; <= 0 disables (deterministic tests)
  // While other content changes (video, scrolling) settled tiles still go, as
  // a small share running alongside the encode: <= 20 x 32 KiB per second.
  size_t motionPacketBytes = 32 * 1024;
  double motionTickBudgetMs = 1.0;
  // A small change (typing, a caret, a ticked box): when at most quickTiles
  // tiles of the picture changed on a tick, those tiles go losslessly WITH
  // that tick's video, so the changed text is never shown as blurred video.
  // Larger changes (scrolling, a moving map) stay video until they settle.
  // Bounded by the token bucket like every add; 0 turns it off.
  // Once no part of the picture changed for stillTicks ticks in a row (a
  // scroll or page switch ended), a tile needs only stillSettleUs unchanged.
  uint32_t stillTicks = 2;
  uint64_t stillSettleUs = 30000;
  uint32_t quickTiles = 24;
  size_t quickPacketBytes = 128 * 1024;
  double quickTickBudgetMs = 2.0;
  // The link share for tiles: the CURRENT video target in bytes/s, read every
  // tick (burst = a quarter second of it). An adaptive bitrate controller passes
  // a live callable; empty = the fixed rateBytesPerSecond/burstBytes above.
  std::function<size_t()> targetBytesPerSecond;
  // Lower bound of the callable rate; an adaptive session lowers it to share
  // one rate with video (the callable then returns the unused part).
  size_t minimumRateBytesPerSecond = 500000;
  // tiles-v2 (docs/lossless-tiles-design.md, "Moves"): a pane that only scrolled stays
  // lossless, one move and its exposed strip instead of a drop of every tile of it. Only
  // for a client that asked for it. The strip is charged to the token bucket and bounded
  // per packet; a larger one (a fling) leaves the tick as it was: drops.
  bool moves = false;
  size_t movePacketBytes = 128 * 1024;
  size_t currentRate() const { return targetBytesPerSecond ? std::max<size_t>(minimumRateBytesPerSecond, targetBytesPerSecond()) : rateBytesPerSecond; }
  size_t currentBurst() const { return targetBytesPerSecond ? std::max(currentRate() / 4, packetBytes) : burstBytes; }
  // Tiles may add up to the video target bitrate while the screen settles.
  static LosslessSettings forTargetRate(std::function<size_t()> videoBytesPerSecond) {
    LosslessSettings value; value.targetBytesPerSecond = std::move(videoBytesPerSecond);
    value.rateBytesPerSecond = value.currentRate(); value.burstBytes = value.currentBurst();
    return value;
  }
  static LosslessSettings forBitrate(int bitrate) { return forTargetRate([bytes = size_t(std::max(bitrate, 0)) / 8] { return bytes; }); }
};

// One instance per encoder (output, media epoch). Per tick the phases run in
// the order beforeEncode -> payload -> afterEncode; losslessTick() below places
// them around the video decision and encode, and its caller sends the payload
// (pts T-1) before the tick's video (pts T).
//
// The encoder's NV12 picture anchors every tile, whatever the composed BGRA
// view says: a tile whose part of the picture changes is dropped with that
// picture's video frame and cannot settle while it keeps changing; unscaled,
// a tile is added only if its BGRA converts to exactly the NV12 bytes the
// encoder got for it. The BGRA view alone can keep the old desktop's tiles
// present (540/540 in a field case) over a running game.
class LosslessTileSource {
 public:
  struct Stats {
    uint64_t packets = 0, addedTiles = 0, droppedTiles = 0, payloadBytes = 0, dropTicks = 0;
    // Unscaled only: the composed BGRA did not match the encoder's picture (the
    // picture changed where the BGRA did not, or an add was refused). Always 0
    // while the capture's BGRA view is the picture's source, as it should be.
    uint64_t mismatchTiles = 0;
    // Tiles sent with the video of the very tick they changed on (a small change).
    uint64_t quickTiles = 0;
    double beforeEncodeMs = 0, beforeEncodeMaxMs = 0, payloadMs = 0, afterEncodeMs = 0;
    // tiles-v2: scrolls sent as a move, their bytes (the move and the rectangles of
    // what it does not give), scrolls found but not sent (no lossless tile row of
    // the pane to move, or rectangles beyond the budget), the tiles of a scrolling
    // pane that came back while it scrolled, and the time spent looking.
    uint64_t moves = 0, moveBytes = 0, moveRefusals = 0, growTiles = 0;
    double moveMs = 0, moveCopyMs = 0, moveDetectMs = 0, moveAnchorMs = 0;
  };
  // `pictureWidth` x `pictureHeight`: the NV12 picture the encoder gets, smaller
  // than the source when the video is downscaled; 0 = the source size.
  LosslessTileSource(uint32_t width, uint32_t height, LosslessSettings settings, uint32_t pictureWidth = 0, uint32_t pictureHeight = 0)
      : width_(width), height_(height), columns_(lossless::tileColumns(width)), settings_(settings),
        pictureWidth_(pictureWidth ? pictureWidth : width), pictureHeight_(pictureHeight ? pictureHeight : height) {
    if (!lossless::validFrame(width, height) || !settings.packetBytes || settings.packetBytes > lossless::maximumPayloadBytes ||
        settings.packetBytes < lossless::headerBytes + 4096 * 2 + lossless::addHeaderBytes + lossless::maximumTileBytes)
      throw std::runtime_error("Invalid lossless tile settings");
    if (pictureWidth_ < 2 || pictureHeight_ < 2 || pictureWidth_ > width || pictureHeight_ > height || ((pictureWidth_ | pictureHeight_) & 1))
      throw std::runtime_error("Invalid lossless tile picture");
    count_ = columns_ * lossless::tileRows(height);
    tiles_.resize(count_); mirror_.assign(size_t(width) * height * 4, 0); tokens_ = double(settings_.currentBurst());
    pictureChanged_.assign(count_, 0); footprints_.resize(count_);
    for (uint32_t i = 0; i < count_; ++i) footprints_[i] = footprint(i);
  }
  // Phase 1, before the video decision: tiles the client shows losslessly are
  // compared exactly; changed ones become drops. True = submit video now.
  // `picture` is this tick's NV12 (what the encoder gets); `pictureChanged`
  // false = FrameEncodePolicy::prepare found it byte-equal to the picture it
  // was given before, i.e. the previous tick's: losslessTick gives both the
  // same picture every tick, and retire() forgets the picture.
  bool beforeEncode(ComposedFrame& frame, uint64_t nowUs, std::span<const uint8_t> picture, bool pictureChanged) {
    const auto began = Clock::now();
    if (haveTick_ && nowUs <= nowUs_) throw std::runtime_error("Lossless tile clock did not progress");
    nowUs_ = nowUs; haveTick_ = true; ++tick_; drops_.clear(); dirty(frame); pictureChanges(picture, pictureChanged);
    // Consecutive ticks on which no part of the picture changed (the screen stopped).
    stillTicks_ = std::find(pictureChanged_.begin(), pictureChanged_.end(), uint8_t(1)) != pictureChanged_.end() ? 0 : stillTicks_ + 1;
    // retire() freed it; every tile is unknown, so afterEncode refills it before any add reads it.
    if (mirror_.empty()) mirror_.assign(size_t(width_) * height_ * 4, 0);
    move_.reset(); strip_.clear(); pane_.reset();
    refill();
    // The picture rested: whatever moved before (a video, a map) is over.
    if (stillTicks_ >= moveQuietTicks) { moveMisses_ = 0; moveRetryTick_ = 0; }
    moveTiles(frame);
    if (present_) {
      std::vector<uint32_t> check;
      for (uint32_t i = 0; i < count_; ++i) if (tiles_[i].present && dirtyTile(i)) check.push_back(i);
      for (const auto i : compare(frame, check)) { tiles_[i].present = false; --present_; drops_.push_back(uint16_t(i)); }
      // The picture anchors a moved tile like any other: what it shows now must
      // convert to exactly what the encoder got there, or it goes back to video.
      if (move_) { const auto anchor = Clock::now(); for (uint32_t i = 0; i < count_; ++i) if (movedTiles_[i] && tiles_[i].present && !matchesPicture(i)) {
        tiles_[i].present = false; --present_; drops_.push_back(uint16_t(i)); ++stats_.mismatchTiles;
      } stats_.moveAnchorMs += elapsed(anchor); }
      stats_.droppedTiles += drops_.size();
    }
    const double ms = elapsed(began); stats_.beforeEncodeMs += ms; stats_.beforeEncodeMaxMs = std::max(stats_.beforeEncodeMaxMs, ms);
    if (!drops_.empty()) ++stats_.dropTicks;
    // A move applies on this tick's frame, like a drop: there has to be one.
    return !drops_.empty() || move_.has_value();
  }
  // Phase 2: the tick's tile packet (drops, then budgeted adds), sent BEFORE
  // this tick's video. Adds need the encoder to have emitted every earlier
  // submission (`addsAllowed`); on a tick whose content changed (video is being
  // encoded meanwhile) they use the small motion share.
  std::optional<std::vector<uint8_t>> payload(ComposedFrame& frame, bool contentChanged, bool addsAllowed) {
    const auto began = Clock::now();
    refill();
    const bool adds = !ended_ && initialized_ && addsAllowed && tokens_ > 0 && (!haveAdd_ || nowUs_ - lastAddUs_ >= settings_.addIntervalUs);
    const std::vector<uint32_t> quick = quickCandidates(frame, contentChanged, addsAllowed);
    if (drops_.empty() && !adds && quick.empty() && !move_) return std::nullopt;
    lossless::PayloadBuilder builder(width_, height_, sequence_ + 1, nowUs_, drops_);
    if (move_) {
      // First in the packet: the client shifts its layer, then takes the strip, then
      // whatever else this tick adds (a tile dropped above may come back at once).
      const size_t start = builder.size();
      builder.move(uint16_t(move_->x), uint16_t(move_->y), uint16_t(move_->width), uint16_t(move_->height), int16_t(move_->dy));
      for (const auto& block : strip_) builder.rectCoded(uint16_t(block.x), uint16_t(block.y), uint16_t(block.width), uint16_t(block.height), block.bytes);
      tokens_ -= double(builder.size() - start); ++stats_.moves; stats_.moveBytes += builder.size() - start;
    }
    const size_t before = builder.size();
    if (!quick.empty()) {
      // This tick's small change, verified against this tick's picture (what
      // the encoder got): an add replaces any drop of the same tile on the
      // client, so the new content shows at once and stays.
      const double quickBudgetMs = settings_.tickBudgetMs > 0 ? settings_.quickTickBudgetMs : 0;
      const size_t limit = std::min(settings_.packetBytes, before + std::min(settings_.quickPacketBytes, size_t(std::max(tokens_, 0.0))));
      for (size_t n = 0; n < std::min(paneFrom_, quick.size()); ++n) {
        const uint32_t i = quick[n];
        if (builder.size() + lossless::addHeaderBytes + lossless::maximumTileBytes > settings_.packetBytes || builder.size() >= limit) break;
        if (quickBudgetMs > 0 && elapsed(began) >= quickBudgetMs) break;
        if (!matchesPicture(i)) continue;
        const auto rect = lossless::tileRect(i, width_, height_);
        builder.add(uint16_t(i), mirror_.data() + (size_t(rect.y) * width_ + rect.x) * 4, size_t(width_) * 4);
        tiles_[i].present = true; ++present_; ++stats_.addedTiles; ++stats_.quickTiles;
      }
      // The tiles of a scrolling pane come back in whole tile rows (only a whole
      // row helps its next move), and they leave the bucket enough for the moves
      // of the next ticks: a move that cannot be paid drops every row again.
      // A row dearer than a tick may add (tiles shared with a photograph beside the
      // pane) goes alone, once the bucket holds it.
      const size_t spare = size_t(std::max(tokens_ - double(settings_.movePacketBytes / 2), 0.0));
      const size_t paneLimit = std::min(limit, before + spare), loneLimit = std::min(settings_.packetBytes, before + spare);
      for (size_t n = paneFrom_; n < quick.size();) {
        if (quickBudgetMs > 0 && elapsed(began) >= quickBudgetMs) break;
        const uint32_t row = quick[n] / columns_; const size_t rowBytes = builder.size(), first = n; const uint16_t rowAdds = builder.adds();
        bool whole = true;
        for (; n < quick.size() && quick[n] / columns_ == row; ++n) {
          const uint32_t i = quick[n];
          whole = whole && builder.size() + lossless::addHeaderBytes + lossless::maximumTileBytes <= settings_.packetBytes && matchesPicture(i);
          if (!whole) continue;
          const auto rect = lossless::tileRect(i, width_, height_);
          builder.add(uint16_t(i), mirror_.data() + (size_t(rect.y) * width_ + rect.x) * 4, size_t(width_) * 4);
        }
        if (!whole || builder.size() > (first == paneFrom_ && rowBytes == before ? loneLimit : paneLimit)) { builder.rewind(rowBytes, rowAdds); break; }
        for (size_t m = first; m < n; ++m) { tiles_[quick[m]].present = true; ++present_; ++stats_.addedTiles; ++stats_.quickTiles; ++stats_.growTiles; }
      }
      if (builder.size() > before) tokens_ -= double(builder.size() - before);
    }
    const size_t afterQuick = builder.size();
    const size_t packetBytes = contentChanged ? std::min(settings_.packetBytes, afterQuick + settings_.motionPacketBytes) : settings_.packetBytes;
    const double budgetMs = contentChanged ? settings_.motionTickBudgetMs : settings_.tickBudgetMs;
    const auto overBudget = [&] { return budgetMs > 0 && elapsed(began) >= budgetMs; };
    if (adds) {
      const size_t limit = std::min(packetBytes, afterQuick + size_t(std::max(tokens_, 0.0)));
      const uint32_t start = scan_;  // round-robin: continue where the last packet stopped
      for (uint32_t scanned = 0; scanned < count_ && builder.size() + lossless::addHeaderBytes + lossless::maximumTileBytes <= settings_.packetBytes &&
                                 builder.size() < limit && !overBudget();) {
        // Verify a batch of settled candidates against the current frame.
        std::vector<uint32_t> batch, verify;
        for (; scanned < count_ && batch.size() < 32; ++scanned) {
          const uint32_t i = (start + scanned) % count_;
          if (tiles_[i].known && !tiles_[i].present && nowUs_ - tiles_[i].changedUs >= settleUs()) {
            batch.push_back(i); if (dirtyTile(i) && tiles_[i].checked != tick_) verify.push_back(i);
          }
        }
        if (!verify.empty()) compare(frame, verify);
        uint32_t last = scan_;
        for (const auto i : batch) {
          if (tiles_[i].changedUs == nowUs_) continue;  // changed on this very tick
          if (builder.size() + lossless::addHeaderBytes + lossless::maximumTileBytes > settings_.packetBytes || builder.size() >= limit || overBudget()) break;
          // Not what the encoder got: wait for the BGRA view to show it.
          // Downscaled (no exact check): its last picture change must have come
          // with a change of its own BGRA.
          if (scaled() ? tiles_[i].pictureTick > tiles_[i].changedTick : !matchesPicture(i)) {
            tiles_[i].changedUs = nowUs_; if (!scaled()) ++stats_.mismatchTiles; continue;
          }
          const auto rect = lossless::tileRect(i, width_, height_);
          builder.add(uint16_t(i), mirror_.data() + (size_t(rect.y) * width_ + rect.x) * 4, size_t(width_) * 4);
          tiles_[i].present = true; ++present_; ++stats_.addedTiles; last = i;
        }
        if (builder.adds()) scan_ = (last + 1) % count_;
      }
      if (builder.size() > afterQuick) { tokens_ -= double(builder.size() - afterQuick); lastAddUs_ = nowUs_; haveAdd_ = true; }
    }
    stats_.payloadMs += elapsed(began);
    if (drops_.empty() && !builder.adds()) return std::nullopt;
    ++sequence_; ++stats_.packets; stats_.payloadBytes += builder.size();
    return builder.finish();
  }
  // Phase 3, after this tick's video was emitted: settle timing of the other tiles.
  void afterEncode(ComposedFrame& frame) {
    const auto began = Clock::now();
    std::vector<uint32_t> check;
    for (uint32_t i = 0; i < count_; ++i) if (tiles_[i].checked != tick_ && !tiles_[i].present && (!initialized_ || dirtyTile(i))) check.push_back(i);
    if (!check.empty()) compare(frame, check);
    initialized_ = true;
    stats_.afterEncodeMs += elapsed(began);
  }
  // The client must show no tile from the video frame of `nowUs` on, which
  // the caller submits: tiles paused (게임 마우스) or this source can no
  // longer describe the capture (rebuilt under the same client decoder). The
  // packet drops EVERY tile index, present here or not, so it also clears a
  // tile this source no longer knows about. The source then knows nothing,
  // as if new, and keeps its sequence: later ticks on the same client
  // decoder resume it. A grid beyond the u16 drop count instead skips one
  // sequence number, which makes the client stop compositing tiles for good.
  std::vector<uint8_t> retire(uint64_t nowUs) {
    if (haveTick_ && nowUs <= nowUs_) throw std::runtime_error("Lossless tile clock did not progress");
    nowUs_ = nowUs; haveTick_ = true; ++tick_;
    std::vector<uint16_t> all;
    if (count_ <= UINT16_MAX) { all.resize(count_); for (uint32_t i = 0; i < count_; ++i) all[i] = uint16_t(i); }
    else { ++sequence_; ended_ = true; }
    lossless::PayloadBuilder builder(width_, height_, sequence_ + 1, nowUs_, all);
    stats_.droppedTiles += present_;
    for (auto& tile : tiles_) tile = Tile{};
    present_ = 0; initialized_ = false; drops_.clear(); move_.reset(); strip_.clear(); pane_.reset(); std::vector<uint8_t>().swap(current_);
    moveMisses_ = 0; moveRetryTick_ = 0;
    // Ticks pass without this source now; the next one starts from its picture.
    // The BGRA mirror and the previous picture (8.5 + 3.2 MB at 1912x1114) are
    // freed, not kept for a resume that may never come (a follow never resumes).
    std::vector<uint8_t>().swap(previous_); std::vector<uint8_t>().swap(mirror_);
    picture_ = nullptr; std::fill(pictureChanged_.begin(), pictureChanged_.end(), uint8_t(0));
    ++sequence_; ++stats_.packets; stats_.payloadBytes += builder.size();
    return builder.finish();
  }
  // The client's latest opt-in: with or without moves (it may stop asking during a
  // session). From the next tick on; nothing already sent changes.
  void moves(bool on) { settings_.moves = on; }
  uint32_t presentTiles() const { return present_; }
  bool present(uint32_t index) const { return index < count_ && tiles_[index].present; }
  uint32_t tileCount() const { return count_; }
  const Stats& stats() const { return stats_; }
  // Frame-sized buffers held now (the BGRA mirror and the previous picture).
  size_t frameBytesHeld() const { return mirror_.capacity() + previous_.capacity(); }
 private:
  using Clock = std::chrono::steady_clock;
  // changedTick: the last tick its BGRA changed (or was first seen);
  // pictureTick: the last tick its part of the picture changed.
  struct Tile { uint64_t changedUs = 0; uint64_t checked = 0, changedTick = 0, pictureTick = 0; bool known = false, present = false; };
  static double elapsed(Clock::time_point began) { return std::chrono::duration<double, std::milli>(Clock::now() - began).count(); }
  capture::PixelRect rectOf(uint32_t i) const { const auto r = lossless::tileRect(i, width_, height_); return {int(r.x), int(r.y), int(r.x + r.width), int(r.y + r.height)}; }
  void dirty(ComposedFrame& frame) {
    const auto rects = frame.dirty();
    dirtyAll_ = !rects.has_value(); dirtyTiles_.assign(dirtyAll_ ? 0 : count_, 0);
    if (rects) for (const auto& r : *rects) {
      if (r.empty()) continue;
      const int x0 = std::max(r.left, 0) / int(lossless::tileSize), y0 = std::max(r.top, 0) / int(lossless::tileSize);
      const int x1 = (std::min(r.right, int(width_)) - 1) / int(lossless::tileSize), y1 = (std::min(r.bottom, int(height_)) - 1) / int(lossless::tileSize);
      for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) dirtyTiles_[size_t(y) * columns_ + x] = 1;
    }
  }
  bool dirtyTile(uint32_t i) const { return dirtyAll_ || dirtyTiles_[i]; }
  void refill() {
    tokens_ = std::min(double(settings_.currentBurst()), tokens_ + double(settings_.currentRate()) * double(nowUs_ - refillUs_) / 1e6);
    refillUs_ = nowUs_;
  }
  bool movedTile(uint32_t i) const { return move_ && movedTiles_[i]; }
  // tiles-v2. When what changed in the picture is a pane that only scrolled, the
  // mirror follows the scroll over the tile rows of the pane the client shows
  // losslessly (its layer will, on this tick's frame), and whatever the move does
  // not give is coded as rectangles: the exposed strip, and rows where something
  // else changed (a highlight under the pointer). The comparison that follows
  // then finds those tiles unchanged, so they stay. Without a move the tick is as
  // before: every changed tile is dropped.
  void moveTiles(ComposedFrame& frame) {
    if (!settings_.moves || scaled() || ended_ || !initialized_ || tick_ < moveRetryTick_) return;
    const auto began = Clock::now();
    int left = int(width_), top = int(height_), right = 0, bottom = 0; uint32_t changed = 0;
    for (uint32_t i = 0; i < count_; ++i) if (pictureChanged_[i]) {
      const auto r = rectOf(i); ++changed;
      left = std::min(left, r.left); top = std::min(top, r.top); right = std::max(right, r.right); bottom = std::max(bottom, r.bottom);
    }
    // A small change is not a scroll, and goes losslessly with its own tick already.
    if (changed <= settings_.quickTiles) return;
    const capture::PixelRect box{left, top, right, bottom};
    const size_t stride = size_t(width_) * 4;
    const capture::PixelRect boxes[] = {box};
    bool copied = false;
    // A box that is one piece (one surface) is read where it lies; one made of
    // several is put together first.
    frame.visit(boxes, [&](const uint8_t* origin, size_t pitch, const capture::PixelRect& piece) {
      if (!copied && piece.left == box.left && piece.top == box.top && piece.right == box.right && piece.bottom == box.bottom) { scrolled(origin, pitch, box); return; }
      const auto copying = Clock::now();
      if (!copied) { current_.resize(mirror_.size()); copied = true; }
      for (int y = 0; y < piece.height(); ++y) std::memcpy(current_.data() + size_t(piece.top + y) * stride + size_t(piece.left) * 4, origin + size_t(y) * pitch, size_t(piece.width()) * 4);
      stats_.moveCopyMs += elapsed(copying);
    });
    if (copied) scrolled(current_.data() + size_t(box.top) * stride + size_t(box.left) * 4, stride, box);
    stats_.moveMs += elapsed(began);
  }
  // 'cur': this tick's frame inside the box, from its top left pixel.
  void scrolled(const uint8_t* cur, size_t pitch, const capture::PixelRect& box) {
    const size_t stride = size_t(width_) * 4; const int tile = int(lossless::tileSize);
    const auto detecting = Clock::now();
    const auto found = detector_.detect(mirror_.data(), stride, cur, pitch, int(width_), int(height_), {box.left, box.top, box.width(), box.height()});
    stats_.moveDetectMs += elapsed(detecting);
    if (!found) {
      // Something else is moving (a video, a map): look less and less often
      // until the picture rests.
      if (++moveMisses_ > 2) moveRetryTick_ = tick_ + std::min<uint64_t>(30, uint64_t(1) << std::min<uint32_t>(moveMisses_ - 2, 5));
      return;
    }
    moveMisses_ = 0;
    const uint32_t c0 = uint32_t(found->x / tile), c1 = uint32_t((found->x + found->width - 1) / tile);
    const uint32_t r0 = uint32_t(found->y / tile), r1 = uint32_t((found->y + found->height - 1) / tile);
    // The most tile rows, one under the other, that the client shows losslessly
    // across the pane: the move is cut to them, the other rows stay video.
    uint32_t first = r0, count = 0;
    for (uint32_t r = r0, start = r0; r <= r1 + 1; ++r) {
      bool whole = r <= r1;
      for (uint32_t c = c0; whole && c <= c1; ++c) whole = tiles_[r * columns_ + c].present;
      if (!whole) { if (r - start > count) { first = start; count = r - start; } start = r + 1; }
    }
    const int rows = std::abs(found->dy);
    // A pane that scrolls slowly gets its other tiles back while it scrolls
    // (quickCandidates); a fast one changes faster than they could be sent.
    pane_ = Pane{c0, c1, r0, r1, first, count, rows <= tile};
    const int a = std::max(found->y, int(first) * tile), b = std::min(found->y + found->height, int(first + count) * tile);
    if (!count || b - a < rows + moveRows) { ++stats_.moveRefusals; return; }
    const ScrollMove move{found->x, a, found->width, b - a, found->dy};
    // Rows the move fills from inside the rectangle; of those, the ones the
    // detector found equal at the offset need nothing more.
    const int destTop = move.dy > 0 ? a : a + rows, destBottom = move.dy > 0 ? b - rows : b;
    explained_.assign(size_t(b - a), 0);
    for (const auto& run : detector_.moved()) for (int y = std::max(run.y, destTop); y < std::min(run.end, destBottom); ++y) explained_[size_t(y - a)] = 1;
    const auto now = [&](int x, int y) { return cur + size_t(y - box.top) * pitch + size_t(x - box.left) * 4; };
    // What the client's layer holds at (x, y) after the move alone.
    const auto shifted = [&](int x, int y) { return mirror_.data() + size_t(y >= destTop && y < destBottom ? y + move.dy : y) * stride + size_t(x) * 4; };
    size_t bytes = lossless::addHeaderBytes + lossless::moveBytes;
    for (int y = a; y < b;) {
      if (explained_[size_t(y - a)]) { ++y; continue; }
      int end = y;
      while (end < b && end - y < tile && !explained_[size_t(end - a)]) ++end;
      for (int x = move.x; x < move.x + move.width; x += tile) {
        const int w = std::min(tile, move.x + move.width - x);
        bool differs = false;
        for (int row = y; row < end && !differs; ++row) differs = std::memcmp(now(x, row), shifted(x, row), size_t(w) * 4) != 0;
        if (!differs) continue;  // a blank strip, the part of a row beside the highlight
        Block block{x, y, w, end - y, {}};
        lossless::encodeTq1(now(x, y), pitch, uint32_t(w), uint32_t(end - y), block.bytes);
        bytes += lossless::addHeaderBytes + lossless::rectHeaderBytes + block.bytes.size();
        strip_.push_back(std::move(block));
        // Its size decides whether the move is sent: too much (a fling) leaves the tick as it was.
        if (bytes > settings_.movePacketBytes || double(bytes) > tokens_) { strip_.clear(); pane_->grow = false; ++stats_.moveRefusals; return; }
      }
      y = end;
    }
    applyScroll(mirror_.data(), stride, move);
    for (const auto& block : strip_) for (int row = block.y; row < block.y + block.height; ++row)
      std::memcpy(mirror_.data() + size_t(row) * stride + size_t(block.x) * 4, now(block.x, row), size_t(block.width) * 4);
    move_ = move; movedTiles_.assign(count_, 0);
    for (uint32_t r = uint32_t(a / tile); r <= uint32_t((b - 1) / tile); ++r) for (uint32_t c = c0; c <= c1; ++c) movedTiles_[r * columns_ + c] = 1;
  }
  bool scaled() const { return pictureWidth_ != width_ || pictureHeight_ != height_; }
  // The part of the picture made from tile i's source pixels alone, in whole
  // NV12 2x2 blocks. Unscaled: the tile itself (bgraToNv12 converts every 2x2
  // block on its own). Downscaled: the picture pixels whose area-average box
  // (capture_scale.hpp; output j covers source [j*S/T, (j+1)*S/T)) lies inside
  // the tile, two more pixels in on each side for other resamplers and a
  // multi-surface layout; a picture pixel next to a tile edge also mixes the
  // neighbour, so it says nothing about this tile alone. A sliver edge tile
  // may have none (empty: then only its BGRA comparison applies).
  capture::PixelRect footprint(uint32_t i) const {
    const auto tile = rectOf(i);
    if (!scaled()) return tile;
    const auto axis = [](int64_t first, int64_t last, int64_t source, int64_t target) {
      constexpr int64_t inset = 2;
      int64_t low = (first * target + source - 1) / source + inset, high = last * target / source - inset;
      low = (low + 1) & ~int64_t(1); high &= ~int64_t(1);
      return std::pair<int, int>{int(low), int(std::max(low, high))};
    };
    const auto [left, right] = axis(tile.left, tile.right, width_, pictureWidth_);
    const auto [top, bottom] = axis(tile.top, tile.bottom, height_, pictureHeight_);
    return {left, top, right, bottom};
  }
  // Marks the tiles whose footprint differs from the previous tick's picture
  // (all of them on a source's first tick); keeps this picture for the next.
  void pictureChanges(std::span<const uint8_t> picture, bool changed) {
    if (picture.size() != size_t(pictureWidth_) * pictureHeight_ * 3 / 2) throw std::runtime_error("Lossless tiles need the encoder picture");
    picture_ = picture.data();
    std::fill(pictureChanged_.begin(), pictureChanged_.end(), uint8_t(0));
    if (previous_.empty()) { previous_.assign(picture.begin(), picture.end()); std::fill(pictureChanged_.begin(), pictureChanged_.end(), uint8_t(1)); }
    else if (changed) {
      const size_t stride = pictureWidth_, chroma = size_t(pictureWidth_) * pictureHeight_;
      for (uint32_t i = 0; i < count_; ++i) {
        const auto& f = footprints_[i]; if (f.empty()) continue;
        bool differs = false;
        for (int y = f.top; y < f.bottom && !differs; ++y) differs = std::memcmp(previous_.data() + size_t(y) * stride + f.left, picture_ + size_t(y) * stride + f.left, size_t(f.width())) != 0;
        for (int y = f.top / 2; y < f.bottom / 2 && !differs; ++y) differs = std::memcmp(previous_.data() + chroma + size_t(y) * stride + f.left, picture_ + chroma + size_t(y) * stride + f.left, size_t(f.width())) != 0;
        pictureChanged_[i] = uint8_t(differs);
      }
      std::memcpy(previous_.data(), picture_, picture.size());
    }
    if (!dirtyAll_) for (uint32_t i = 0; i < count_; ++i) if (pictureChanged_[i]) dirtyTiles_[i] = 1;
  }
  // How long a tile must stay unchanged before it is added: the short wait
  // once the whole picture stopped changing (a scroll or a page switch
  // ended), else settleUs (other content still moves, so this tile may too).
  uint64_t settleUs() const { return stillTicks_ >= settings_.stillTicks ? settings_.stillSettleUs : settings_.settleUs; }
  // The tiles that go with this tick's video, their mirror brought to this
  // tick's frame. A small change (LosslessSettings::quickTiles): every tile
  // whose part of this tick's picture changed and that is not shown losslessly
  // any more, when there are few. A pane that scrolls slowly: its tiles the
  // client lacks, tile row by tile row, nearest the lossless rows first, from
  // paneFrom_ on; the budgets decide how many go. Empty for a larger change, a
  // downscaled picture or a tick without video.
  std::vector<uint32_t> quickCandidates(ComposedFrame& frame, bool contentChanged, bool addsAllowed) {
    std::vector<uint32_t> quick; paneFrom_ = SIZE_MAX;
    if (!settings_.quickTiles || !contentChanged || !addsAllowed || ended_ || !initialized_ || tokens_ <= 0 || scaled()) return quick;
    const bool grow = pane_ && pane_->grow;
    const auto wanted = [&](uint32_t i) { return pictureChanged_[i] && !tiles_[i].present; };
    const auto inPane = [&](uint32_t i) { const uint32_t r = i / columns_, c = i % columns_; return grow && r >= pane_->r0 && r <= pane_->r1 && c >= pane_->c0 && c <= pane_->c1; };
    for (uint32_t i = 0; i < count_; ++i) {
      if (!wanted(i) || inPane(i)) continue;
      if (quick.size() == settings_.quickTiles) { quick.clear(); break; }
      quick.push_back(i);
    }
    if (grow) {
      std::vector<uint32_t> rows;
      for (uint32_t r = pane_->r0; r <= pane_->r1; ++r) rows.push_back(r);
      const uint32_t first = pane_->first, end = pane_->first + pane_->count;
      const auto distance = [&](uint32_t r) { return r < first ? first - r : r >= end ? r - end + 1 : 0u; };
      if (pane_->count) std::stable_sort(rows.begin(), rows.end(), [&](uint32_t x, uint32_t y) { return distance(x) < distance(y); });
      paneFrom_ = quick.size();
      for (const auto r : rows) for (uint32_t c = pane_->c0; c <= pane_->c1; ++c) if (wanted(r * columns_ + c)) quick.push_back(r * columns_ + c);
    }
    if (quick.empty()) return quick;
    std::vector<uint32_t> verify;
    for (const auto i : quick) if (tiles_[i].checked != tick_) verify.push_back(i);
    compare(frame, verify);
    return quick;
  }
  // Unscaled: the mirror of tile i converts to exactly this tick's picture there.
  bool matchesPicture(uint32_t i) {
    const auto r = rectOf(i); const size_t w = size_t(r.width()), stride = pictureWidth_, chroma = size_t(pictureWidth_) * pictureHeight_;
    converted_.resize(w * size_t(r.height()) * 3 / 2);
    pixels::bgraToNv12Block(mirror_.data() + (size_t(r.top) * width_ + size_t(r.left)) * 4, size_t(width_) * 4, r.width(), r.height(), converted_.data(), converted_.data() + w * size_t(r.height()), w);
    for (int y = 0; y < r.height(); ++y) if (std::memcmp(converted_.data() + size_t(y) * w, picture_ + size_t(r.top + y) * stride + size_t(r.left), w)) return false;
    for (int y = 0; y < r.height() / 2; ++y) if (std::memcmp(converted_.data() + w * size_t(r.height()) + size_t(y) * w, picture_ + chroma + size_t(r.top / 2 + y) * stride + size_t(r.left), w)) return false;
    return true;
  }
  // Exact comparison of the composed frame with the mirror; the mirror becomes
  // the current content. Returns the tiles that changed (or were first seen),
  // and every tile whose part of the picture changed on this tick.
  std::vector<uint32_t> compare(ComposedFrame& frame, const std::vector<uint32_t>& indices) {
    std::vector<uint32_t> changed;
    if (indices.empty()) return changed;
    std::vector<capture::PixelRect> rects; rects.reserve(indices.size());
    for (const auto i : indices) rects.push_back(rectOf(i));
    const size_t stride = size_t(width_) * 4;
    frame.visit(rects, [&](const uint8_t* origin, size_t pitch, const capture::PixelRect& piece) {
      const uint32_t i = uint32_t(piece.top / int(lossless::tileSize)) * columns_ + uint32_t(piece.left / int(lossless::tileSize));
      uint8_t* mirror = mirror_.data() + size_t(piece.top) * stride + size_t(piece.left) * 4;
      const size_t bytes = size_t(piece.width()) * 4; bool differs = false;
      for (int y = 0; y < piece.height(); ++y) {
        const uint8_t* source = origin + size_t(y) * pitch; uint8_t* target = mirror + size_t(y) * stride;
        if (differs || std::memcmp(target, source, bytes)) { differs = true; std::memcpy(target, source, bytes); }
      }
      if (differs) tiles_[i].changedTick = tick_;
    });
    for (const auto i : indices) {
      auto& tile = tiles_[i]; tile.checked = tick_;
      // Unscaled, NV12 is a function of the BGRA: a picture change without one is a broken view.
      // A moved tile's picture changed with its pixels: the mirror already holds the move.
      const bool pictureOnly = pictureChanged_[i] && !movedTile(i);
      if (pictureOnly && tile.known && tile.changedTick != tick_ && !scaled()) ++stats_.mismatchTiles;
      if (!tile.known) tile.changedTick = tick_;
      if (pictureChanged_[i]) tile.pictureTick = tick_;
      if (tile.changedTick == tick_ || pictureOnly) { tile.known = true; tile.changedUs = nowUs_; changed.push_back(i); }
    }
    return changed;
  }
  uint32_t width_, height_, columns_, count_ = 0, present_ = 0, sequence_ = 0, scan_ = 0;
  LosslessSettings settings_;
  uint32_t pictureWidth_, pictureHeight_;
  std::vector<Tile> tiles_;
  std::vector<uint8_t> mirror_, dirtyTiles_;
  // The previous tick's picture, this tick's (valid during the tick), the
  // tiles whose footprint it changed, and the footprints.
  std::vector<uint8_t> previous_, pictureChanged_, converted_; const uint8_t* picture_ = nullptr;
  std::vector<capture::PixelRect> footprints_;
  std::vector<uint16_t> drops_;
  // tiles-v2: this tick's move, the coded rectangles that go with it, the tiles it
  // touches, and the current frame where the picture changed when it had to be put
  // together (what the detector compares the mirror with).
  struct Block { int x, y, width, height; std::vector<uint8_t> bytes; };
  // pane_: the scroll found on this tick, sent or not: its tile columns and rows,
  // the lossless tile rows the move was cut to, and whether its other tiles may
  // come back on this tick.
  struct Pane { uint32_t c0, c1, r0, r1, first, count; bool grow; };
  static constexpr int moveRows = 32;              // rows a move must keep beyond its offset
  static constexpr uint32_t moveQuietTicks = 6;    // a picture at rest this long ends a run of misses
  std::optional<ScrollMove> move_;
  std::optional<Pane> pane_;
  std::vector<Block> strip_;
  std::vector<uint8_t> current_, movedTiles_, explained_;
  ScrollDetector detector_;
  uint32_t moveMisses_ = 0; uint64_t moveRetryTick_ = 0; size_t paneFrom_ = SIZE_MAX;
  uint64_t tick_ = 0, nowUs_ = 0, refillUs_ = 0, lastAddUs_ = 0;
  double tokens_ = 0;
  uint32_t stillTicks_ = 0;
  bool haveTick_ = false, haveAdd_ = false, initialized_ = false, dirtyAll_ = true, ended_ = false;
  Stats stats_;
};

// One persistent helper thread: the tile comparison runs while the capture
// thread encodes, so it does not lengthen a video frame's path.
class ParallelWork {
 public:
  ParallelWork() : thread_([this] { loop(); }) {}
  ~ParallelWork() { { std::lock_guard lock(mutex_); stop_ = true; } wake_.notify_all(); thread_.join(); }
  ParallelWork(const ParallelWork&) = delete;
  ParallelWork& operator=(const ParallelWork&) = delete;
  void start(std::function<void()> work) {
    std::lock_guard lock(mutex_);
    if (busy_) throw std::runtime_error("Lossless helper is still busy");
    work_ = std::move(work); busy_ = true; error_ = nullptr; wake_.notify_all();
  }
  // Waits for the started work; rethrows its failure.
  void wait() {
    std::unique_lock lock(mutex_); done_.wait(lock, [this] { return !busy_; });
    if (error_) { auto error = error_; error_ = nullptr; std::rethrow_exception(error); }
  }
 private:
  void loop() {
    std::unique_lock lock(mutex_);
    for (;;) {
      wake_.wait(lock, [this] { return stop_ || work_; });
      if (stop_ && !work_) return;
      auto work = std::move(work_); work_ = nullptr; lock.unlock();
      std::exception_ptr error;
      try { work(); } catch (...) { error = std::current_exception(); }
      lock.lock(); error_ = error; busy_ = false; done_.notify_all();
    }
  }
  std::mutex mutex_; std::condition_variable wake_, done_;
  std::function<void()> work_; std::exception_ptr error_; bool busy_ = false, stop_ = false;
  std::thread thread_;
};

struct LosslessTick { FrameEncodeDecision decision; std::optional<std::vector<uint8_t>> payload; double encodeMs = 0, tileMs = 0; };
// One output's tick with tiles active. `encode()` submits this tick's NV12 to
// the encoder and must BUFFER its outputs; the caller then sends the returned
// payload (pts T-1) before those outputs (pts T). When video is submitted, the
// three tile phases run on `helper` in parallel with the encode; otherwise they
// run here (no video waits) and a drop forces one submission. `force` submits
// this tick's video in any case (a requested IDR must be coded now).
template<class Encode>
LosslessTick losslessTick(LosslessTileSource& source, ParallelWork& helper, ComposedFrame& frame, FrameEncodePolicy& policy,
                          std::span<const uint8_t> nv12, uint64_t nowUs, Encode&& encode, bool force = false) {
  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::time_point began) { return std::chrono::duration<double, std::milli>(Clock::now() - began).count(); };
  LosslessTick tick;
  const bool idle = policy.pendingInputs() == 0 && policy.hasUsableIdr();
  tick.decision = policy.prepare(nv12, nowUs, force);
  if (tick.decision.submit) {
    const bool changed = tick.decision.contentChanged;
    double tileMs = 0;
    helper.start([&, changed] {
      const auto began = Clock::now();
      source.beforeEncode(frame, nowUs, nv12, changed); tick.payload = source.payload(frame, changed, idle); source.afterEncode(frame);
      tileMs = ms(began);
    });
    struct Join { ParallelWork& helper; bool armed = true; ~Join() { if (armed) try { helper.wait(); } catch (...) {} } } join{helper};
    const auto began = Clock::now(); encode(); tick.encodeMs = ms(began);
    join.armed = false; helper.wait(); tick.tileMs = tileMs;
    return tick;
  }
  const auto began = Clock::now();
  // A second prepare of the same unchanged frame, now forced, is valid only
  // after the first one submitted nothing.
  if (source.beforeEncode(frame, nowUs, nv12, tick.decision.contentChanged)) { tick.decision = policy.prepare(nv12, nowUs, true); if (!tick.decision.submit) throw std::runtime_error("Lossless drop without a video frame"); }
  tick.payload = source.payload(frame, tick.decision.contentChanged, idle);
  if (tick.decision.submit) { const auto encoding = Clock::now(); encode(); tick.encodeMs = ms(encoding); }
  source.afterEncode(frame);
  tick.tileMs = ms(began) - tick.encodeMs;
  return tick;
}
// One tick of a legacy stream that has a tile source (Agent::stream).
// `wanted`: the client wants tiles now (opted in and not paused by 게임 마우스)
// and the capture still describes the source the tiles were made for. The
// first tick on which that stops being true retires the tiles: one packet
// drops every tile, due with this tick's video, which is submitted in any
// case. Later unwanted ticks return nothing (the caller encodes as without
// tiles), except that a forced tick (a requested IDR: the send queue dropped
// media, perhaps that packet) retires again; if the first packet was lost,
// the client sees the sequence gap and stops compositing tiles. A wanted tick
// resumes on the same sequence. `retired` is the caller's state of this
// source. Before this, a source destroyed under a running client decoder
// (capture_follow.hpp) left the client compositing its last tiles over the
// video for the rest of the stream.
// The unwanted half of losslessStreamTick: nothing while retired (unless
// forced), else one packet dropping every tile with this tick's video, whose
// decision `prepare(true)` makes (the CPU picture's bytes, or a GPU picture's
// change evidence: gpu_capture.hpp, which never runs while tiles are wanted).
template<class Prepare, class Encode>
std::optional<LosslessTick> losslessRetireTick(LosslessTileSource& source, bool& retired, Prepare&& prepare, uint64_t nowUs, Encode&& encode, bool force = false) {
  if (retired && !force) return std::nullopt;
  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::time_point began) { return std::chrono::duration<double, std::milli>(Clock::now() - began).count(); };
  LosslessTick tick;
  const auto began = Clock::now();
  tick.payload = source.retire(nowUs); tick.tileMs = ms(began);
  tick.decision = prepare(true);
  if (!tick.decision.submit) throw std::runtime_error("Lossless retirement without a video frame");
  const auto encoding = Clock::now(); encode(); tick.encodeMs = ms(encoding);
  retired = true;
  return tick;
}
template<class Encode>
std::optional<LosslessTick> losslessStreamTick(LosslessTileSource& source, bool& retired, bool wanted, ParallelWork& helper, ComposedFrame& frame,
                                               FrameEncodePolicy& policy, std::span<const uint8_t> nv12, uint64_t nowUs, Encode&& encode, bool force = false) {
  if (wanted) { retired = false; return losslessTick(source, helper, frame, policy, nv12, nowUs, std::forward<Encode>(encode), force); }
  return losslessRetireTick(source, retired, [&](bool forced) { return policy.prepare(nv12, nowUs, forced); }, nowUs, std::forward<Encode>(encode), force);
}
}
