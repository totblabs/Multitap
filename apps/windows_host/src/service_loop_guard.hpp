#pragma once
#include "operational_log.hpp"
#include <array>
#include <atomic>
#include <functional>

namespace host {
// Steps of the boot service's one-second loop (serviceMain). Fixed numbers:
// they are the `stage` of hostAction 219.
enum class ServiceLoopStep : uint32_t { sessionChange = 1, reload = 2, maintenance = 3, profileRecovery = 4, displayV2Recovery = 5, capabilities = 6, renderObservation = 7, displayAdapters = 8, privateDiagnostic = 9, count = 10 };
// Each loop step runs contained. An exception that escaped one used to end
// serviceMain with SERVICE_STOPPED and an error code, which the installed SCM
// recovery actions do not act on (they cover crashes), so the VM kept no Host
// until someone restarted it. A failed step is skipped for this tick and runs
// again on the next one. Its fixed code is recorded (hostAction 219: stage =
// step, error = consecutive failures, ok = false) on the first failure and then
// at most once per window; the first success after failures records ok = true
// with the failure count. No exception text is recorded.
class ServiceLoopGuard {
 public:
  using Record = std::function<void(uint32_t step, bool ok, uint32_t failures)>;
  explicit ServiceLoopGuard(Record record = {}, uint64_t windowMs = 60000) : record_(std::move(record)), windowMs_(windowMs) {}
  template<class Body> bool run(ServiceLoopStep step, Body&& body, uint64_t nowMs = GetTickCount64()) noexcept {
    const auto index = size_t(step);
    if (!index || index >= states_.size()) return false;
    auto& state = states_[index];
    try { body(); }
    catch (...) {
      state.failures = state.failures < UINT32_MAX ? state.failures + 1 : UINT32_MAX;
      if (state.failures == 1 || nowMs - state.recordedMs >= windowMs_) { state.recordedMs = nowMs; emit(uint32_t(index), false, state.failures); }
      return false;
    }
    if (state.failures) { emit(uint32_t(index), true, state.failures); state = {}; }
    return true;
  }
  uint32_t failures(ServiceLoopStep step) const { const auto index = size_t(step); return index && index < states_.size() ? states_[index].failures : 0; }
 private:
  struct State { uint32_t failures = 0; uint64_t recordedMs = 0; };
  void emit(uint32_t step, bool ok, uint32_t failures) noexcept {
    try {
      if (record_) record_(step, ok, failures);
      else recordHostEvent("hostAction", {{"action", uint32_t(219)}, {"stage", step}, {"ok", ok}, {"error", failures}});
    } catch (...) {}
  }
  Record record_; uint64_t windowMs_; std::array<State, size_t(ServiceLoopStep::count)> states_{};
};
// A flag the loop reads on each pass. Setting it also wakes the loop (its
// change event), so a new readiness observation is announced at once instead
// of on the next one-second pass. The event outlives every setter (serviceMain
// closes it after the bridge and its threads are gone).
class LoopWakeFlag {
 public:
  LoopWakeFlag& operator=(bool value) noexcept { value_ = value; if (value) if (const HANDLE event = wake_.load()) SetEvent(event); return *this; }
  bool exchange(bool value) noexcept { return value_.exchange(value); }
  void wake(HANDLE event) noexcept { wake_ = event; }
 private:
  std::atomic<bool> value_{false}; std::atomic<HANDLE> wake_{nullptr};
};
}
