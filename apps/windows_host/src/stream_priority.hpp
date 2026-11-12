#pragma once
#include <windows.h>
#include <mutex>

namespace host {
// Scheduling of a streaming capture/encode process while a stream runs. On a
// CPU-only guest the user's applications (browser renderer, WARP) keep every
// vCPU busy at normal priority; a capture/encode/x264 thread that wakes at the
// same priority waits behind them for up to a whole quantum (the field data
// showed 40-100 ms stage maxima and ~0.5 s input-to-frame p95 tails while the
// median work was 0.5 ms). ABOVE_NORMAL makes those short bursts preempt
// normal applications; it stays below the compositor (HIGH) and changes no
// system setting. HighQoS opts the process out of EcoQoS throttling (a worker
// without a foreground window can otherwise be treated as background work).
// Process-wide and reference counted; the previous class is restored when the
// last stream ends. Nothing here is required for correctness: failures are
// ignored and the stream runs at the inherited priority.
class StreamPriority {
 public:
  StreamPriority() {
    std::lock_guard lock(mutex());
    auto& state = shared();
    if (state.users++ == 0) {
      const HANDLE process = GetCurrentProcess();
      state.previous = GetPriorityClass(process);
      state.raised = (state.previous == NORMAL_PRIORITY_CLASS || state.previous == BELOW_NORMAL_PRIORITY_CLASS || state.previous == IDLE_PRIORITY_CLASS) &&
                     SetPriorityClass(process, ABOVE_NORMAL_PRIORITY_CLASS);
      PROCESS_POWER_THROTTLING_STATE high{}; high.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION; high.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED; high.StateMask = 0;
      state.qos = SetProcessInformation(process, ProcessPowerThrottling, &high, sizeof(high)) != FALSE;
    }
  }
  ~StreamPriority() {
    std::lock_guard lock(mutex());
    auto& state = shared();
    if (--state.users != 0) return;
    const HANDLE process = GetCurrentProcess();
    if (state.raised && GetPriorityClass(process) == ABOVE_NORMAL_PRIORITY_CLASS) SetPriorityClass(process, state.previous);
    if (state.qos) { PROCESS_POWER_THROTTLING_STATE automatic{}; automatic.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION; SetProcessInformation(process, ProcessPowerThrottling, &automatic, sizeof(automatic)); }
    state.raised = state.qos = false;
  }
  StreamPriority(const StreamPriority&) = delete;
  StreamPriority& operator=(const StreamPriority&) = delete;
  static bool raised() { std::lock_guard lock(mutex()); return shared().raised; }
 private:
  struct State { unsigned users = 0; DWORD previous = NORMAL_PRIORITY_CLASS; bool raised = false, qos = false; };
  static std::mutex& mutex() { static std::mutex value; return value; }
  static State& shared() { static State value; return value; }
};
}
