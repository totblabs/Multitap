#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace multitap::clipboard {
// Accepted from inspect means admissible for a NEW reservation, not an ACK for
// an existing transfer. Only accept commits the prefix and high-watermark.
enum class RequestSequenceStatus { Accepted, Retired, Invalid };

class RequestSequence {
 public:
  static constexpr uint64_t maximum = 0x7fffffffffffffffULL;

  RequestSequenceStatus inspect(std::string_view id) const noexcept {
    const auto value = parse(id);
    return value ? inspectValue(*value) : RequestSequenceStatus::Invalid;
  }

  RequestSequenceStatus accept(std::string_view id) noexcept {
    const auto value = parse(id);
    if (!value) return RequestSequenceStatus::Invalid;
    const auto status = inspectValue(*value);
    if (status == RequestSequenceStatus::Accepted) {
      prefix_ = value->prefix;
      high_ = value->counter;
      bound_ = true;
    }
    return status;
  }

  bool bound() const noexcept { return bound_; }
  uint64_t highWatermark() const noexcept { return high_; }

 private:
  struct Value {
    std::array<char, 32> prefix{};
    uint64_t counter = 0;
  };
  static int digit(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
  }
  static std::optional<Value> parse(std::string_view id) noexcept {
    if (id.size() != 49 || id[32] != '-') return {};
    Value value;
    for (std::size_t i = 0; i < value.prefix.size(); ++i) {
      if (digit(id[i]) < 0) return {};
      value.prefix[i] = id[i];
    }
    for (std::size_t i = 33; i < id.size(); ++i) {
      const int next = digit(id[i]);
      if (next < 0 || value.counter > (maximum - uint64_t(next)) / 16)
        return {};
      value.counter = value.counter * 16 + uint64_t(next);
    }
    if (!value.counter) return {};
    return value;
  }
  RequestSequenceStatus inspectValue(const Value& value) const noexcept {
    if (bound_ && value.prefix != prefix_) return RequestSequenceStatus::Invalid;
    return value.counter <= high_ ? RequestSequenceStatus::Retired
                                  : RequestSequenceStatus::Accepted;
  }

  // Fixed storage: no unbounded set of previously seen transfer IDs. A fresh
  // object belongs to one authenticated clipboard pair and is never rebound.
  std::array<char, 32> prefix_{};
  uint64_t high_ = 0;
  bool bound_ = false;
};

// Caller contract:
//  * For a begin/read reservation: validate its policy/manifest/state, inspect,
//    then accept immediately before committing the reservation.
//  * For an ACK/data/cancel: process the exact active ID with its existing stage
//    checks FIRST. Only nonactive IDs classified Retired may be discarded.
//    Accepted here means an unreserved future ID, so reject it; Invalid is also
//    rejected. Never accept an ID just because an ACK names it.
//  * Retired includes skipped counters. Strictly increasing gaps are allowed
//    because local capture can fail before a request reaches the relay/host.
}  // namespace multitap::clipboard
