#pragma once
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>

namespace multitap::clipboard {
// The sender's side of the clipboard sliding window (formats profile). A "unit" is one data chunk or one item end.
// The relay enforces the same bounds, so a sender that respects this class is never closed for the window.
// Acknowledgements are cumulative: an ack names the end of a unit it has received and releases every earlier unit.
class SendWindow {
 public:
  static constexpr size_t defaultUnits = 32;
  static constexpr uint64_t defaultBytes = 1024 * 1024;
  explicit SendWindow(size_t maxUnits = defaultUnits, uint64_t maxBytes = defaultBytes) : maxUnits_(maxUnits), maxBytes_(maxBytes) {}
  bool canSend(uint64_t dataBytes) const { return units_.size() < maxUnits_ && bytes_ + dataBytes <= maxBytes_; }
  bool canSendItemEnd() const { return units_.size() < maxUnits_; }
  void sentChunk(size_t index, uint64_t endOffset, uint64_t dataBytes) {
    if (!canSend(dataBytes)) throw std::runtime_error("clipboard_window_full");
    units_.push_back({false, index, endOffset, dataBytes}); bytes_ += dataBytes;
  }
  void sentItemEnd(size_t index) {
    if (units_.size() >= maxUnits_) throw std::runtime_error("clipboard_window_full");
    units_.push_back({true, index, 0, 0});
  }
  void ackChunk(size_t index, uint64_t offset) { release(false, index, offset); }
  void ackItem(size_t index) { release(true, index, 0); }
  bool empty() const { return units_.empty(); }
  size_t units() const { return units_.size(); }
  uint64_t bytes() const { return bytes_; }
 private:
  struct Unit { bool itemEnd; size_t index; uint64_t end, data; };
  void release(bool itemEnd, size_t index, uint64_t end) {
    size_t at = 0;
    while (at < units_.size() && !(units_[at].itemEnd == itemEnd && units_[at].index == index && units_[at].end == end)) ++at;
    if (at == units_.size()) throw std::runtime_error("clipboard_unexpected_ack");
    for (size_t i = 0; i <= at; ++i) { bytes_ -= units_.front().data; units_.pop_front(); }
  }
  size_t maxUnits_; uint64_t maxBytes_; std::deque<Unit> units_; uint64_t bytes_ = 0;
};

// The receiver's side: when to acknowledge. A pair without the formats profile (stop and wait) acknowledges
// every chunk; a windowed pair acknowledges after four chunks or 128 KiB, and always at an item end.
class AckPolicy {
 public:
  explicit AckPolicy(bool windowed, size_t everyChunks = 4, uint64_t everyBytes = 128 * 1024)
      : windowed_(windowed), everyChunks_(everyChunks), everyBytes_(everyBytes) {}
  // A chunk of `bytes` was applied; true when an acknowledgement of it should be sent now.
  bool chunk(uint64_t bytes) {
    ++chunks_; bytes_ += bytes;
    if (!windowed_ || chunks_ >= everyChunks_ || bytes_ >= everyBytes_) { reset(); return true; }
    return false;
  }
  void itemEnd() { reset(); }
 private:
  void reset() { chunks_ = 0; bytes_ = 0; }
  bool windowed_; size_t everyChunks_; uint64_t everyBytes_; size_t chunks_ = 0; uint64_t bytes_ = 0;
};
}
