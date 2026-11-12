#pragma once
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <span>
#include <thread>

namespace host {
// Capture loops wake for a newly composed frame no sooner than this share of
// the period after the previous tick began: the frame-rate cap, with room for
// arrival jitter (owned outputs compose at 60 Hz, at most every 16.7 ms).
inline std::chrono::microseconds captureMinimumGap(std::chrono::microseconds period) { return period * 9 / 10; }
// Paces a capture/encode loop on a high-resolution waitable timer. A loop
// that owns a WGC capture also passes its frame-arrival events: a newly
// composed frame then starts the next tick at once instead of waiting for the
// fixed tick (on average half a period, at 60 fps 8.3 ms, up to 16.7 ms of
// input-to-frame latency), but never before `earliest` (the frame-rate cap).
class FramePacer {
 public:
  using Clock = std::chrono::steady_clock;
  FramePacer() { timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS); }
  ~FramePacer() { if (timer_) CloseHandle(timer_); }
  FramePacer(const FramePacer&) = delete;
  FramePacer& operator=(const FramePacer&) = delete;
  void wait(Clock::time_point deadline) { until(deadline); }
  // Waits until `deadline`; returns true instead when one of `frames` (auto-
  // reset events) is signaled at or after `earliest`. A signal before
  // `earliest` stays pending until then.
  bool wait(Clock::time_point deadline, std::span<const HANDLE> frames, Clock::time_point earliest) {
    if (frames.empty() || frames.size() > MAXIMUM_WAIT_OBJECTS - 1) { until(deadline); return false; }
    until(std::min(earliest, deadline));
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero()) return poll(frames);
    HANDLE handles[MAXIMUM_WAIT_OBJECTS]; DWORD count = 0;
    for (const HANDLE frame : frames) handles[count++] = frame;
    DWORD timeout = DWORD(std::clamp<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count() + 1, 1, 1100));
    if (timer_ && arm(remaining)) { handles[count++] = timer_; timeout = 1100; }
    const DWORD result = WaitForMultipleObjects(count, handles, FALSE, timeout);
    if (result < WAIT_OBJECT_0 + frames.size()) { if (timer_) CancelWaitableTimer(timer_); return true; }
    // Timer, coarse timeout or a failed wait: finish exactly like the plain wait.
    until(deadline);
    return poll(frames);
  }
 private:
  static bool poll(std::span<const HANDLE> frames) {
    for (const HANDLE frame : frames) if (WaitForSingleObject(frame, 0) == WAIT_OBJECT_0) return true;
    return false;
  }
  bool arm(Clock::duration remaining) {
    LARGE_INTEGER due{}; due.QuadPart = -std::max<LONGLONG>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
    return SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) != FALSE;
  }
  void until(Clock::time_point deadline) {
    auto remaining = deadline - Clock::now(); if (remaining <= Clock::duration::zero()) return;
    if (timer_ && arm(remaining) && WaitForSingleObject(timer_, 1100) == WAIT_OBJECT_0) return;
    remaining = deadline - Clock::now(); if (remaining > std::chrono::milliseconds(1)) std::this_thread::sleep_for(remaining - std::chrono::milliseconds(1));
    while (Clock::now() < deadline) SwitchToThread();
  }
  HANDLE timer_ = nullptr;
};
}
