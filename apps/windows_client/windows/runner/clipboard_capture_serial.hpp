#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace multitap::detail {
// Shared by normal STA error dispatch and the pending-token routing fence.
inline bool IsClipboardSourceJob(std::string_view method) noexcept {
  return method=="capture"||method=="retireSource"||
      method=="sourceChunkScoped"||method=="sourceDigestScoped";
}
// The production counter has process lifetime. Taking an atomic by reference
// lets the exact allocator's numeric boundary be checked without resetting it.
inline std::optional<uint64_t> ReserveClipboardCaptureSerial(
    std::atomic<uint64_t>& last) noexcept {
  constexpr auto maximum = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
  auto previous = last.load(std::memory_order_relaxed);
  while (previous < maximum) {
    if (last.compare_exchange_weak(previous, previous + 1,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
      return previous + 1;
    }
  }
  return std::nullopt;
}
}
