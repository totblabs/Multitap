#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace host::gpu {
// Diagnostic values only. None of these values controls admission, a driver
// call, a deadline, resource release or fallback. No media/PTS/identity data.
enum class NvencObservedInput : uint32_t { unknown = 0, bytes = 1, texture = 2 };
enum class NvencObservedClose : uint32_t {
  unknown = 0, noEncoder = 1, eosRefused = 2, eosIncomplete = 3,
  resourcesRefused = 4, destroyRefused = 5, released = 6,
};
struct NvencObservationSnapshot {
  bool observed = false;
  uint32_t phase = 0, input = 0, submitted = 0, outputs = 0, waitMs = 0;
  NvencObservedClose close = NvencObservedClose::unknown;
  uint32_t closeNative = 0;
  bool fenced = false;
  bool operator==(const NvencObservationSnapshot&) const = default;
};
inline uint32_t nvencObservationCount(uint64_t value) noexcept {
  return value > UINT32_MAX ? UINT32_MAX : uint32_t(value);
}
inline constexpr uint32_t nvencObservationNext(uint32_t value) noexcept {
  return value == UINT32_MAX ? value : value + 1;
}
// One state per candidate, shared only with that candidate's failure. Close
// publishes last, so a reader cannot mistake in-progress teardown for success.
// Atomic fields also make a diagnostic snapshot/late phase binding race-free;
// they do not introduce a lock into the driver close path.
class NvencObservationState {
 public:
  void submitted(NvencObservedInput input) noexcept {
    input_.store(uint32_t(input), std::memory_order_relaxed);
    increment(submitted_);
  }
  // Provider-validated/unlocked outputs, not wrapper-published/client-decoded
  // frames. The existing frameCount_ and all encoder decisions stay separate.
  void output() noexcept { increment(outputs_); }
  void waited(uint64_t before, uint64_t after) noexcept {
    waitValid_.store(after >= before, std::memory_order_relaxed);
    waitMs_.store(after >= before ? nvencObservationCount(after - before) : 0, std::memory_order_relaxed);
  }
  // Bound by VerifiedCandidates' actual accepted-output history at failure.
  // Provider initialization/hardware inventory cannot establish phase2.
  void phase(bool established) noexcept { phase_.store(established ? 2u : 1u, std::memory_order_relaxed); }
  void closed(NvencObservedClose disposition, uint32_t nativeStatus, bool fenced) noexcept {
    close_.store(uint32_t(disposition), std::memory_order_relaxed);
    closeNative_.store(nativeStatus, std::memory_order_relaxed);
    fenced_.store(fenced, std::memory_order_relaxed);
    closed_.store(true, std::memory_order_release);
  }
  NvencObservationSnapshot snapshot() const noexcept {
    NvencObservationSnapshot value;
    if (!closed_.load(std::memory_order_acquire) || !waitValid_.load(std::memory_order_relaxed)) return value;
    value.phase = phase_.load(std::memory_order_relaxed);
    value.input = input_.load(std::memory_order_relaxed);
    value.submitted = submitted_.load(std::memory_order_relaxed);
    value.outputs = outputs_.load(std::memory_order_relaxed);
    value.waitMs = waitMs_.load(std::memory_order_relaxed);
    value.close = NvencObservedClose(close_.load(std::memory_order_relaxed));
    value.closeNative = closeNative_.load(std::memory_order_relaxed);
    value.fenced = fenced_.load(std::memory_order_relaxed);
    value.observed = (value.phase == 1 || value.phase == 2) &&
      (value.input == 1 || value.input == 2) && value.submitted && value.outputs <= value.submitted &&
      value.close >= NvencObservedClose::noEncoder && value.close <= NvencObservedClose::released;
    return value.observed ? value : NvencObservationSnapshot{};
  }
 private:
  static void increment(std::atomic<uint32_t>& count) noexcept {
    // A candidate has one encoder writer; atomic load/store avoids an
    // unbounded compare/exchange retry on the diagnostic-only path.
    const uint32_t prior = count.load(std::memory_order_relaxed);
    count.store(nvencObservationNext(prior), std::memory_order_relaxed);
  }
  std::atomic<uint32_t> phase_{0}, input_{0}, submitted_{0}, outputs_{0}, waitMs_{0};
  std::atomic<uint32_t> close_{0}, closeNative_{0};
  std::atomic<bool> waitValid_{false}, fenced_{false}, closed_{false};
};
template<class Allocate>
std::shared_ptr<NvencObservationState> tryNvencObservation(Allocate&& allocate) noexcept {
  try { return std::forward<Allocate>(allocate)(); } catch (...) { return {}; }
}
inline std::shared_ptr<NvencObservationState> tryNvencObservation() noexcept {
  // Must run before driver-resource creation. Optional telemetry allocation
  // failure cannot turn an otherwise working GPU into CPU fallback.
  return tryNvencObservation([] { return std::make_shared<NvencObservationState>(); });
}
}
