#pragma once
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace host {
inline constexpr const char* thumbnailPrivacyProtocol = "thumbnail-privacy-v1";

// Thumbnail invalidation only. Neither the epoch nor allowed grants capture,
// stream admission, input, or proof that a frame reached a normal desktop.
class ThumbnailPrivacyEpoch {
#ifdef MULTITAP_THUMBNAIL_PRIVACY_TEST
  friend struct ThumbnailPrivacyEpochTestAccess;
#endif
 public:
  using Factory = std::function<std::string()>;
  struct Ticket { uint64_t revision = 0, observation = 0; };
  struct Snapshot { std::string epoch; bool allowed = false; };

  explicit ThumbnailPrivacyEpoch(Factory factory) : factory_(std::move(factory)) { rotate(); }
  static bool valid(const std::string& value) {
    if (value.size() != 32) return false;
    for (const char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
  }
  Ticket begin() const {
    std::lock_guard lock(mutex_);
    return {revision_, observation_};
  }
  // One random 64-bit owner prefix, then a monotonic 64-bit event suffix.
  // Poll/send reordering is detectable without adding a fourth wire field.
  // RNG/factory is outside our lock. A later initialization wins on reentry.
  void rotate() noexcept {
    uint64_t revision = 0;
    {
      std::lock_guard lock(mutex_);
      ready_ = false;
      if (revision_ == std::numeric_limits<uint64_t>::max() || observation_ == std::numeric_limits<uint64_t>::max()) return;
      revision = ++revision_;
      ++observation_; denied_ = false;
      if (!prefix_.empty()) { ready_ = true; return; }
    }
    std::string next;
    try { if (factory_) next = factory_(); } catch (...) {}
    std::lock_guard lock(mutex_);
    if (revision != revision_) return;
    if (valid(next)) { next.resize(16); prefix_ = std::move(next); ready_ = true; }
  }
  Snapshot snapshot(Ticket ticket, bool observedAllowed) {
    std::lock_guard lock(mutex_);
    if (!ticket.revision || ticket.revision > revision_) return {};
    // An old observation must not deny (burn) a successor's epoch. It may only
    // deny its own captured epoch, which the broker's monotonic guard can drop.
    if (!ready_ || ticket.revision != revision_ || ticket.observation != observation_) return {epoch(ticket.revision), false};
    if (!observedAllowed) {
      if (!denied_) {
        denied_ = true;
        if (observation_ == std::numeric_limits<uint64_t>::max()) ready_ = false;
        else ++observation_; // revoke true observations that began before this denial
      }
      return {epoch(revision_), false};
    }
    if (denied_) {
      // A fresh successful read after uncertainty recovers without waiting for
      // another WTS event. Never revive a false-terminal broker epoch in place.
      if (revision_ == std::numeric_limits<uint64_t>::max() || observation_ == std::numeric_limits<uint64_t>::max()) {
        ready_ = false; return {epoch(revision_), false};
      }
      ++revision_; ++observation_; denied_ = false;
    }
    return {epoch(revision_), true};
  }
 private:
  std::string epoch(uint64_t revision) const {
    if (prefix_.empty() || !revision) return {};
    auto value = prefix_ + std::string(16, '0');
    const char* hex = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) { value[31 - i] = hex[revision & 15]; revision >>= 4; }
    return value;
  }
  Factory factory_;
  mutable std::mutex mutex_;
  uint64_t revision_ = 0;
  uint64_t observation_ = 0;
  std::string prefix_;
  bool ready_ = false, denied_ = false;
};

inline std::string thumbnailPrivacyCapabilityFields(const ThumbnailPrivacyEpoch::Snapshot& value) {
  if (!ThumbnailPrivacyEpoch::valid(value.epoch)) return {}; // optional/unknown, never allowed
  return ",\"thumbnailPrivacy\":{\"protocol\":\"" + std::string(thumbnailPrivacyProtocol) +
    "\",\"epoch\":\"" + value.epoch + "\",\"allowed\":" + (value.allowed ? "true" : "false") + '}';
}
}
