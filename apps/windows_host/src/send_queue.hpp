#pragma once
#include "../../../native/windows_video/lossless_tiles.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace host {
// One message waiting for the broker channel's sending thread.
struct OutgoingPacket {
  bool text = false;
  std::vector<uint8_t> bytes;
  bool capabilities = false;
  std::chrono::steady_clock::time_point queuedAt{};
};
// What a backlog may drop. Video waits for the next IDR once one P frame is
// gone (the decoder could not use the frames after it). Lossless tiles
// (tiles-v1 payload in a non-key kind-1 packet) and audio can simply be
// skipped; a skipped tile packet makes the client stop compositing tiles
// (plain video, never a stale tile). Text and cursor-v1 records are never
// dropped: a later cursor state names an earlier shape id and the relay ends
// the whole Host channel for an id it has not seen.
enum class OutgoingClass { control, video, keyVideo, tiles, audio };
inline OutgoingClass outgoingClass(const OutgoingPacket& packet) {
  if (packet.text || packet.capabilities) return OutgoingClass::control;
  const auto& bytes = packet.bytes;
  if (bytes.size() > 12 && bytes[0] == 1)
    return multitap::lossless::isTilePayload(bytes.data() + 12, bytes.size() - 12) ? OutgoingClass::tiles
         : (bytes[1] & 1) ? OutgoingClass::keyVideo : OutgoingClass::video;
  if (bytes.size() > 12 && bytes[0] == 2) return OutgoingClass::audio;
  return OutgoingClass::control;
}
// A cursor-v1 state record (media kind 3, record type 2: visibility, shape id
// and an optional position) as the worker sends it: 12-byte media header and
// the 16-byte record (flags at 13, x at 20, y at 24).
inline bool cursorStateRecord(const OutgoingPacket& packet) {
  return !packet.text && !packet.capabilities && packet.bytes.size() == 28 && packet.bytes[0] == 3 && packet.bytes[12] == 2;
}
struct SendQueueAdmission {
  bool queued = false;           // the packet is in the queue
  bool stalled = false;          // control backlog beyond its bound: the sender is stuck
  bool requestKeyFrame = false;  // video was dropped: the encoder must start a new IDR
  size_t droppedVideo = 0, droppedOther = 0;
  size_t droppedAudio = 0;       // the audio part of droppedOther (audio-stats-v1)
};
// The broker channel's send queue. A media backlog (the uplink slower than
// the stream) degrades the stream instead of closing the channel: queued video
// is dropped and the stream resumes at the next IDR. Only a control backlog,
// which never drops, can still end the channel (the sender itself is stuck).
class HostSendQueue {
 public:
  static constexpr size_t mediaBytesLimit = 8 * 1024 * 1024;
  static constexpr size_t controlBytesLimit = 4 * 1024 * 1024, controlPacketsLimit = 1024;
  // `mediaPacketLimit`: media packets (video, tiles, audio) that may wait.
  SendQueueAdmission admit(OutgoingPacket packet, size_t mediaPacketLimit) {
    SendQueueAdmission result;
    const auto kind = outgoingClass(packet);
    const size_t size = packet.bytes.size();
    if (kind == OutgoingClass::control) {
      if (cursorStateRecord(packet)) coalesceCursorState(packet);
      if (controlPackets_ >= controlPacketsLimit || controlBytes_ + size > controlBytesLimit) { result.stalled = true; return result; }
      push(std::move(packet), kind); result.queued = true; return result;
    }
    const bool video = kind == OutgoingClass::video || kind == OutgoingClass::keyVideo;
    const auto fits = [&] { return mediaPackets_ < mediaPacketLimit && mediaBytes_ + size <= mediaBytesLimit; };
    const auto isVideo = [](OutgoingClass value) { return value == OutgoingClass::video || value == OutgoingClass::keyVideo; };
    const auto isAudio = [](OutgoingClass value) { return value == OutgoingClass::audio; };
    if (!fits()) {
      const size_t evicted = evict(isVideo, [] { return true; });
      result.droppedVideo += evicted;
      if (evicted || video) awaitKeyFrame(result);
    }
    if (kind == OutgoingClass::video && awaitingKeyFrame_) {
      // Still waiting for the IDR; ask again in case the request was lost.
      ++result.droppedVideo; result.requestKeyFrame = true; return result;
    }
    // The IDR that ends a drop takes the place of stale audio if it must.
    if (kind == OutgoingClass::keyVideo && !fits()) { const size_t evicted = evict(isAudio, [&] { return !fits(); }); result.droppedOther += evicted; result.droppedAudio += evicted; }
    if (!fits()) {
      if (video) { ++result.droppedVideo; awaitKeyFrame(result); result.requestKeyFrame = true; }
      else { ++result.droppedOther; if (kind == OutgoingClass::audio) ++result.droppedAudio; }
      return result;
    }
    if (kind == OutgoingClass::keyVideo) { awaitingKeyFrame_ = false; result.requestKeyFrame = false; }
    push(std::move(packet), kind); result.queued = true; return result;
  }
  // Audio leaves before the video and tiles queued ahead of it, never before
  // a control message (stream.info opens the epoch its audio belongs to): a
  // backlog of pictures must not become a gap in the sound (#98). Everything
  // else keeps its order.
  bool pop(OutgoingPacket& out) {
    if (queue_.empty()) return false;
    auto at = queue_.begin();
    for (auto scan = queue_.begin(); scan != queue_.end() && scan->kind != OutgoingClass::control; ++scan)
      if (scan->kind == OutgoingClass::audio) { at = scan; break; }
    account(*at, false); out = std::move(at->packet); queue_.erase(at); return true;
  }
  void clear() { queue_.clear(); mediaPackets_ = mediaBytes_ = controlPackets_ = controlBytes_ = 0; awaitingKeyFrame_ = false; }
  // A new media epoch (resize restart) starts with an IDR of its own.
  void dropMedia() { evict([](OutgoingClass value) { return value != OutgoingClass::control; }, [] { return true; }); awaitingKeyFrame_ = false; }
  bool empty() const { return queue_.empty(); }
  size_t packets() const { return queue_.size(); }
  size_t bytes() const { return mediaBytes_ + controlBytes_; }
  std::chrono::steady_clock::time_point oldest() const { return queue_.empty() ? std::chrono::steady_clock::time_point{} : queue_.front().packet.queuedAt; }
  bool awaitingKeyFrame() const { return awaitingKeyFrame_; }
 private:
  struct Entry { OutgoingPacket packet; OutgoingClass kind; };
  std::deque<Entry> queue_;
  size_t mediaPackets_ = 0, mediaBytes_ = 0, controlPackets_ = 0, controlBytes_ = 0;
  bool awaitingKeyFrame_ = false;
  void awaitKeyFrame(SendQueueAdmission& result) { if (!awaitingKeyFrame_) { awaitingKeyFrame_ = true; result.requestKeyFrame = true; } }
  // A cursor state supersedes every state of its media epoch still waiting
  // here: those leave the queue and `packet` (appended after any shape they
  // waited behind) carries the newest of their positions when it has none.
  // A waiting cursor is one state however long the uplink stalls. Echo
  // records (cursor-echo-v1) used to fill the 1024-packet control bound and
  // close the whole Host channel (send_queue_stalled) within seconds of a stall.
  void coalesceCursorState(OutgoingPacket& packet) {
    std::optional<std::array<uint8_t, 8>> position;
    for (auto at = queue_.begin(); at != queue_.end();) {
      const auto& waiting = at->packet.bytes;
      if (!cursorStateRecord(at->packet) || waiting[2] != packet.bytes[2] || waiting[3] != packet.bytes[3]) { ++at; continue; }
      if (waiting[13] & 2) { position.emplace(); std::copy(waiting.begin() + 20, waiting.end(), position->begin()); }
      account(*at, false); at = queue_.erase(at);
    }
    if (position && !(packet.bytes[13] & 2)) { packet.bytes[13] |= 2; std::copy(position->begin(), position->end(), packet.bytes.begin() + 20); }
  }
  void account(const Entry& entry, bool added) {
    const size_t size = entry.packet.bytes.size();
    auto& packets = entry.kind == OutgoingClass::control ? controlPackets_ : mediaPackets_;
    auto& bytes = entry.kind == OutgoingClass::control ? controlBytes_ : mediaBytes_;
    if (added) { ++packets; bytes += size; } else { --packets; bytes -= size; }
  }
  void push(OutgoingPacket packet, OutgoingClass kind) { queue_.push_back({std::move(packet), kind}); account(queue_.back(), true); }
  // Removes matching entries oldest first while `more()` holds.
  template<class Match, class More>
  size_t evict(Match match, More more) {
    size_t removed = 0;
    for (auto at = queue_.begin(); at != queue_.end() && more();) {
      if (!match(at->kind)) { ++at; continue; }
      account(*at, false); at = queue_.erase(at); ++removed;
    }
    return removed;
  }
};
}
