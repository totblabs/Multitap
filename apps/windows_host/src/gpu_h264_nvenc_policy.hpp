#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include "gpu_h264_nvenc_observation.hpp"

namespace host::gpu {
// Existing output-completion budget, shared by event waiting and nonblocking
// LOCK_BUSY retries. Native API calls themselves are not cancellable deadlines.
inline constexpr uint32_t nvencOutputBudgetMs = 250;
inline constexpr uint32_t nvencLockRetryMs = 1;
struct NvencOutputDeadline {
  uint64_t started = 0;
  uint32_t remaining(uint64_t now) const noexcept {
    if (now < started || now - started >= nvencOutputBudgetMs) return 0;
    return nvencOutputBudgetMs - uint32_t(now - started);
  }
};
struct NvencOutputLockResult {
  bool ready = false, expired = false;
  uint32_t nativeStatus = 0, attempts = 0;
};
template<class Clock, class Lock, class Pause>
NvencOutputLockResult nvencLockOutputBounded(NvencOutputDeadline deadline,
    uint32_t success, uint32_t busy, Clock&& clock, Lock&& lock, Pause&& pause) {
  NvencOutputLockResult result;
  // The attempt bound also terminates a faulty/frozen injected clock. Every
  // retry pauses at least 1 ms and no retry starts outside the shared budget.
  for (uint32_t attempt = 0; attempt < nvencOutputBudgetMs / nvencLockRetryMs; ++attempt) {
    if (!deadline.remaining(clock())) { result.expired = true; return result; }
    result.nativeStatus = uint32_t(lock()); ++result.attempts;
    if (result.nativeStatus == success) { result.ready = true; return result; }
    if (result.nativeStatus != busy) return result;
    const uint32_t remaining = deadline.remaining(clock());
    if (!remaining) { result.expired = true; return result; }
    pause(std::min(nvencLockRetryMs, remaining));
  }
  result.expired = true; return result;
}

// Called under the production process-quarantine lock. Existing dual encoders
// may both be admitted. The first incomplete teardown fences all new admission;
// only those already live can subsequently require process-lifetime retention.
struct NvencProcessAdmission {
  size_t live = 0, retained = 0, retentionLimit = 0;
  bool fenced = false;
  bool acquire() noexcept {
    if (fenced || live == std::numeric_limits<size_t>::max()) return false;
    ++live; return true;
  }
  bool release() noexcept { if (!live) return false; --live; return true; }
  bool retain() noexcept {
    if (!live) return false;
    if (!fenced) { fenced = true; retentionLimit = live; }
    --live; ++retained;
    return retained <= retentionLimit;
  }
};
inline constexpr bool nvencFlushAllowsRelease(bool pictureMayBePending,
    bool flushAttempted, bool flushAccepted, bool flushCompleted) noexcept {
  return !pictureMayBePending && !flushAttempted ? true : flushAccepted && flushCompleted;
}
struct NvencDrainResult {
  bool attempted = false, accepted = false, completed = false;
  uint32_t nativeStatus = 0;
};
struct NvencNativeResult { bool ok; uint32_t nativeStatus; };
struct NvencCloseResult {
  bool released; uint32_t nativeStatus;
  NvencObservedClose disposition = NvencObservedClose::unknown;
};
template<class Drain, class FreeResources, class Destroy>
NvencCloseResult nvencCloseInSdkOrder(bool pictureMayBePending,
    Drain&& drain, FreeResources&& freeResources, Destroy&& destroy) {
  const NvencDrainResult flushed = drain();
  if (!nvencFlushAllowsRelease(pictureMayBePending, flushed.attempted, flushed.accepted, flushed.completed))
    return {false, flushed.nativeStatus, flushed.accepted ? NvencObservedClose::eosIncomplete : NvencObservedClose::eosRefused};
  const NvencNativeResult freed = freeResources();
  if (!freed.ok) return {false, freed.nativeStatus, NvencObservedClose::resourcesRefused};
  const NvencNativeResult destroyed = destroy();
  return {destroyed.ok, destroyed.nativeStatus, destroyed.ok ? NvencObservedClose::released : NvencObservedClose::destroyRefused};
}
// gpu-capture-v1: the input textures registered with one NVENC session, at
// most N; a new one evicts (unregisters) the least recently used. An entry
// holds its key (a referenced texture), so a key's address is never reused
// while it is listed. `reg(key)` returns the handle and `unreg(handle)`
// releases it; both throw on failure. A refused release leaves the cache
// unchanged; a refused registration after an eviction leaves that slot free.
// The close path releases `entries` itself, in the SDK order (after the drain).
template<class Key, class Handle, size_t N>
struct NvencRegistrationCache {
  struct Entry { Key key{}; Handle handle{}; uint64_t used = 0; bool live = false; };
  std::array<Entry, N> entries{};
  uint64_t clock = 0;
  template<class Register, class Unregister>
  Handle acquire(const Key& key, Register&& reg, Unregister&& unreg) {
    for (auto& entry : entries) if (entry.live && entry.key == key) { entry.used = ++clock; return entry.handle; }
    auto slot = std::find_if(entries.begin(), entries.end(), [](const Entry& entry) { return !entry.live; });
    if (slot == entries.end()) {
      slot = std::min_element(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
      unreg(slot->handle);
      *slot = Entry{};
    }
    const Handle handle = reg(key);
    *slot = Entry{key, handle, ++clock, true};
    return handle;
  }
  size_t live() const { return size_t(std::count_if(entries.begin(), entries.end(), [](const Entry& entry) { return entry.live; })); }
};
enum class NvencMetadataFailure { none, buffer, timestamp, pictureType };
inline constexpr bool nvencForwardPictureType(uint32_t type) noexcept {
  // Exact pinned NV_ENC_PIC_TYPE constants. Production static_asserts their
  // identity against nvEncodeAPI.h. NONREF_P remains a forward P picture.
  return type == 0 || type == 2 || type == 3 || type == 7;
}
// Called only after successful NvEncLockBitstream. Its separate uint32_t
// hwEncodeStatus has no declared NVENCSTATUS/zero-success mapping in the pinned
// API header, so it is deliberately not an admission input. Actual API errors
// remain handled before this metadata check.
inline constexpr NvencMetadataFailure nvencOutputMetadataFailure(
    bool hasBuffer, uint64_t bytes, uint64_t maximumBytes,
    uint64_t outputPts, uint64_t inputPts, uint32_t pictureType) noexcept {
  if (!hasBuffer || !bytes || bytes > maximumBytes) return NvencMetadataFailure::buffer;
  if (outputPts != inputPts) return NvencMetadataFailure::timestamp;
  if (!nvencForwardPictureType(pictureType)) return NvencMetadataFailure::pictureType;
  return NvencMetadataFailure::none;
}
}
