#pragma once
#include "capture_selection.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <vector>
namespace host {
// An application changed the display mode of the stream's own virtual output
// (League of Legends switches it to another resolution for exclusive
// fullscreen, and back when it leaves): the capture follows the output instead
// of ending the stream. Field evidence (a test PC): the
// capture thread failed at the topology check (hostAction 200 stage 206) or at
// the new WGC frame size (207 error 2), and the stream froze until timeouts.
//
// The stream keeps the size it announced (stream.info): the relay accepts a
// same-epoch stream.info only with the announced source size, and the client
// lays the picture out at that size. The followed output is scaled into the
// unchanged picture, input maps onto the output's new rectangle, and cursor
// positions are scaled into the announced size (cursor::logicalPosition).
// Nothing here changes a display: the mode stays whatever the application set.
inline constexpr std::chrono::milliseconds captureFollowSettle{8000};  // the output must settle within this, or the stream fails as before
inline constexpr std::chrono::milliseconds captureFollowGap{1000};     // at most one capture rebuild per second (a game may flip modes)
inline constexpr std::chrono::milliseconds captureFollowStep{100};     // between two observations of the output
// The owned output now: the stream's single selected monitor (same GDI name and
// device identity) at its current bounds, which its current display mode
// (`mode`, from EnumDisplaySettingsEx) agrees with, and still the only display
// in that rectangle. nullopt while Windows is between modes, or when the
// output is gone or replaced (no follow: the stream fails as before).
inline std::optional<RECT> followedOwnedOutput(const CaptureMonitor& selected, const std::vector<CaptureMonitor>& observed, const std::optional<RECT>& mode) {
  if (!mode || selected.name.empty() || selected.identity.empty()) return std::nullopt;
  const CaptureMonitor* found = nullptr; size_t matches = 0;
  for (const auto& monitor : observed) if (monitor.name == selected.name && monitor.identity == selected.identity) { found = &monitor; ++matches; }
  if (matches != 1 || !sameRect(found->bounds, *mode)) return std::nullopt;
  CaptureSelection next; next.bounds = found->bounds; next.monitors.push_back({selected.name, selected.identity, found->bounds, found->primary});
  try { validateCaptureSelection(next, observed); } catch (...) { return std::nullopt; }
  return found->bounds;
}
// The worker's notice when its capture ends with an error. Before its first
// stream.info a resize restart (and an initial viewport start) reports the
// failed resize, which the service still waits for. After it the service has
// completed the resize and drops a resize failure, so the stream itself is
// unavailable. Reported as a resize failure, every later failure of such a
// stream would be dropped: the picture freezes, nothing ends the session
// and the client gives up after 15 s (host_timeout; the service sees only its
// own pipe read time out, hostAction 260 cause 2, worker still running).
inline std::string captureFailureNotice(bool resized, bool announced, const std::string& epochFields, const std::string& session, const char* reason) {
  if (resized && !announced) return "{\"type\":\"stream.resize.failed\",\"reason\":\"resize_failed\"" + epochFields + '}';
  return "{\"type\":\"stream.unavailable\",\"sessionId\":" + jsonQuote(session) + ",\"reason\":" + jsonQuote(reason) + '}';
}
// The display mode's rectangle of a GDI device (ENUM_CURRENT_SETTINGS).
inline std::optional<RECT> currentDisplayModeRect(const std::wstring& device) {
  if (device.empty()) return std::nullopt;
  DEVMODEW mode{}; mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsExW(device.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) return std::nullopt;
  return RECT{mode.dmPosition.x, mode.dmPosition.y, LONG(int64_t(mode.dmPosition.x) + mode.dmPelsWidth), LONG(int64_t(mode.dmPosition.y) + mode.dmPelsHeight)};
}
// The same owned-output rectangles in two observations `step` apart (a mode
// change lands in several Windows steps), or nullopt at `deadline` or `stop`.
// Independent outputs observe all of them at once: Windows moves one output
// when the other changes its mode beside it.
inline std::optional<std::vector<RECT>> settledOwnedOutputs(const std::function<std::optional<std::vector<RECT>>()>& observe, const std::function<void(std::chrono::milliseconds)>& wait,
                                                            const std::function<std::chrono::steady_clock::time_point()>& now, std::chrono::steady_clock::time_point deadline,
                                                            const std::atomic<bool>& stop, std::chrono::milliseconds step = captureFollowStep) {
  std::optional<std::vector<RECT>> previous;
  while (!stop.load()) {
    auto current = observe();
    if (current && previous && std::equal(current->begin(), current->end(), previous->begin(), previous->end(), sameRect)) return current;
    previous = std::move(current);
    if (now() + step > deadline) return std::nullopt;
    wait(step);
  }
  return std::nullopt;
}
inline std::optional<RECT> settledOwnedOutput(const std::function<std::optional<RECT>()>& observe, const std::function<void(std::chrono::milliseconds)>& wait,
                                              const std::function<std::chrono::steady_clock::time_point()>& now, std::chrono::steady_clock::time_point deadline,
                                              const std::atomic<bool>& stop, std::chrono::milliseconds step = captureFollowStep) {
  const auto settled = settledOwnedOutputs([&]() -> std::optional<std::vector<RECT>> { const auto rect = observe(); if (!rect) return std::nullopt; return std::vector<RECT>{*rect}; }, wait, now, deadline, stop, step);
  if (!settled) return std::nullopt;
  return settled->front();
}
}
