#pragma once
// A value read from Windows that is kept until a change signal or its maximum
// age (backlog #70, idle cost of the capability report; capability_inputs.hpp).
// Self-contained: native_display_status.hpp uses it below machine.hpp.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace host {
// Counted by the service control handler for each WTS session notification
// (SERVICE_CONTROL_SESSIONCHANGE), before the service loop handles it, so a
// value read before a lock, logon or console change is never served after it.
inline std::atomic<uint64_t>& hostSessionChangeEpoch() { static std::atomic<uint64_t> value{0}; return value; }
inline void noteHostSessionChange() noexcept { hostSessionChangeEpoch().fetch_add(1); }
// Counted after each status this process reports to the SCM (reportService).
inline std::atomic<uint64_t>& hostServiceStatusEpoch() { static std::atomic<uint64_t> value{0}; return value; }
inline void noteHostServiceStatusReported() noexcept { hostServiceStatusEpoch().fetch_add(1); }
// Counted for each display change the service can see without a worker
// (display_change_signals.hpp): a monitor or display adapter interface came or
// went, Windows saved a display configuration, the virtual display's own state
// changed. The display probe (display_cache.hpp) runs again after one.
inline std::atomic<uint64_t>& hostDisplayChangeEpoch() { static std::atomic<uint64_t> value{0}; return value; }
inline void noteHostDisplayChange() noexcept { hostDisplayChangeEpoch().fetch_add(1); }

// get() returns the kept value while it is younger than maxAge and was read
// under the same key (the signals' counters, a file stamp, a session ID);
// otherwise it reads again. invalidate() drops it. A value whose read began
// before an invalidate() or a key change is returned to its caller but never
// kept. A read that throws keeps nothing. `keep` limits what may be kept.
template<class T, class Key> class SignalCache {
 public:
  using Clock = std::chrono::steady_clock;
  explicit SignalCache(Clock::duration maxAge) : maxAge_(maxAge) {}
  void invalidate() noexcept { std::lock_guard lock(mutex_); ++generation_; value_.reset(); }
  template<class Read> T get(Clock::time_point now, const Key& key, Read&& read) { return get(now, key, read, [](const T&) { return true; }); }
  template<class Read, class Keep> T get(Clock::time_point now, const Key& key, Read&& read, Keep&& keep) {
    uint64_t generation = 0;
    {
      std::lock_guard lock(mutex_);
      if (value_ && key_ == key && now >= at_ && now - at_ < maxAge_) return *value_;
      generation = generation_;
    }
    T value = read();
    if (keep(value)) { std::lock_guard lock(mutex_); if (generation_ == generation) { value_ = value; key_ = key; at_ = now; } }
    return value;
  }
 private:
  std::mutex mutex_; const Clock::duration maxAge_; uint64_t generation_ = 0; std::optional<T> value_; Key key_{}; Clock::time_point at_{};
};
}
