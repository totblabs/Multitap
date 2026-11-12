#pragma once

namespace multitap::video {
enum class GpuCompletionState { pending, ready, failed };
enum class GpuCompletionResult { ready, timedOut, failed };
enum class GpuCompletionPollMode { doNotFlush };

// Submit/flush this completion marker once. Subsequent observation must not
// submit more work: the real D3D11 path uses ASYNC_GETDATA_DONOTFLUSH. This
// preserves the completion-before-publication boundary, not a new async queue.
// The caller owns the existing whole polling budget, clock and yield policy.
template<class Duration, class Now, class Submit, class Poll, class Pause>
GpuCompletionResult waitForGpuCompletion(Duration budget, Now now, Submit submit,
                                        Poll poll, Pause pause) {
  submit();
  const auto start = now();
  for (;;) {
    const auto state = poll(GpuCompletionPollMode::doNotFlush);
    switch (state) {
      case GpuCompletionState::ready: return GpuCompletionResult::ready;
      case GpuCompletionState::failed: return GpuCompletionResult::failed;
      case GpuCompletionState::pending: break;
    }
    const auto current = now();
    if (current < start || current - start >= budget) return GpuCompletionResult::timedOut;
    pause();
  }
}
}
