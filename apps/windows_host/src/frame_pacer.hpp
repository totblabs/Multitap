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
// How late a wait returned against the time it meant to return at (`target`),
// counting only time spent inside the wait: a target already behind when the
// wait began (the tick itself ran past it) is no lateness of the wait.
inline std::chrono::microseconds paceLate(std::chrono::steady_clock::time_point entered, std::chrono::steady_clock::time_point target, std::chrono::steady_clock::time_point returned) {
  const auto from = std::max(entered, target);
  return returned > from ? std::chrono::duration_cast<std::chrono::microseconds>(returned - from) : std::chrono::microseconds::zero();
}
// The frame-rate cap of a loop that a newly composed frame wakes. The gap alone
// (captureMinimumGap) lets a source that composes faster than the target start a tick
// every nine tenths of a period: 11 % over the rate. Field 2026-11-25, a game on a
// 180 fps stream: 187 frames a second were encoded and sent, the client's 180 Hz screen
// could not show the rest, and it skipped 15 to 18 pictures a second. So the cap also
// keeps a schedule. Each tick that did work moves the time the next is due on by a
// period less a sixty-fourth, from the later of that tick and the time it was due, and
// a frame may start a tick up to one period before it is due. Frames that come a period
// apart, early or late by the gap's tenth, pass exactly as before, and so does a source
// up to 1.5 % faster than the target (a display's clock is never the Host's; holding
// that back would add up to a period of delay in a slow sawtooth). A source faster than
// that is held to 101.6 % of the target once its one period of credit is used up.
class FrameCap {
 public:
  using Clock = std::chrono::steady_clock;
  // The earliest a new frame may start the next tick. `lastWork`: the last tick that
  // captured or submitted a frame, as the loop keeps it (idle ticks do not count).
  Clock::time_point earliest(Clock::time_point lastWork, std::chrono::microseconds period) {
    if (lastWork != counted_) { counted_ = lastWork; due_ = std::max(lastWork, due_) + (period - period / 64); }
    return std::max(lastWork + captureMinimumGap(period), due_ - period);
  }
 private:
  Clock::time_point counted_{}, due_{};
};
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
  void wait(Clock::time_point deadline) { const auto entered = Clock::now(); until(deadline); late_ = paceLate(entered, deadline, Clock::now()); }
  // Waits until `deadline`; returns true instead when one of `frames` (auto-
  // reset events) is signaled at or after `earliest`. A signal before
  // `earliest` stays pending until then.
  bool wait(Clock::time_point deadline, std::span<const HANDLE> frames, Clock::time_point earliest) {
    const auto entered = Clock::now();
    if (frames.empty() || frames.size() > MAXIMUM_WAIT_OBJECTS - 1) { until(deadline); late_ = paceLate(entered, deadline, Clock::now()); return false; }
    const auto gate = std::min(earliest, deadline);
    until(gate);
    const auto gated = Clock::now();
    const auto remaining = deadline - gated;
    if (remaining <= Clock::duration::zero()) { late_ = paceLate(entered, deadline, gated); return poll(frames); }
    HANDLE handles[MAXIMUM_WAIT_OBJECTS]; DWORD count = 0;
    for (const HANDLE frame : frames) handles[count++] = frame;
    DWORD timeout = DWORD(std::clamp<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count() + 1, 1, 1100));
    if (timer_ && arm(remaining)) { handles[count++] = timer_; timeout = 1100; }
    const DWORD result = WaitForMultipleObjects(count, handles, FALSE, timeout);
    // A frame or input wake: only the gate's lateness is known (when the event
    // was signaled is not), so a later wake counts nothing more.
    if (result < WAIT_OBJECT_0 + frames.size()) { if (timer_) CancelWaitableTimer(timer_); late_ = paceLate(entered, gate, gated); return true; }
    // Timer, coarse timeout or a failed wait: finish exactly like the plain wait.
    until(deadline);
    late_ = paceLate(entered, deadline, Clock::now());
    return poll(frames);
  }
  // The last wait's lateness (paceLate): diagnostics only, never pacing.
  std::chrono::microseconds late() const { return late_; }
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
  std::chrono::microseconds late_{};
};
}
