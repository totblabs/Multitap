#pragma once
// The picture rectangle and crash release for the mouse lock's clip
// (relative_mouse.h). The separate mouse-confine mode was removed on
// 2026-11-03. ASCII only: some QA targets build without /utf-8.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace multitap {

// Logical (Flutter) pixels relative to the window's client origin.
struct LogicalRect {
  double left = 0, top = 0, width = 0, height = 0;
};

// Logical pixels kept free inside every edge, so the cursor never touches the
// window's resize border or caption while confined. Small
// enough that the session bar's 4 px top-edge reveal strip stays reachable.
constexpr double kConfinementMarginLogical = 2;

// Physical screen rectangle for |rect| in a client area at |origin| (screen
// pixels) of |client| size. Rounds inward so the hot spot never reaches the
// letterbox beside the picture, never extends past the client area, and keeps
// a small margin inside each edge.
inline std::optional<RECT> ConfinementRect(const LogicalRect& rect, UINT dpi,
                                           POINT origin, SIZE client) {
  constexpr double limit = 65536;
  if (!std::isfinite(rect.left) || !std::isfinite(rect.top) ||
      !std::isfinite(rect.width) || !std::isfinite(rect.height) ||
      rect.width <= 0 || rect.height <= 0 || std::fabs(rect.left) > limit ||
      std::fabs(rect.top) > limit || rect.width > limit || rect.height > limit ||
      client.cx <= 0 || client.cy <= 0) {
    return std::nullopt;
  }
  if (dpi == 0) dpi = 96;
  if (dpi < 24 || dpi > 2400) return std::nullopt;
  const double scale = double(dpi) / 96;
  // A tiny tolerance keeps exact edges such as 1279.9999 from losing a pixel.
  const auto inward = [](double value, bool start) {
    return int64_t(start ? std::ceil(value - 1e-6) : std::floor(value + 1e-6));
  };
  int64_t left = std::clamp<int64_t>(inward(rect.left * scale, true), 0, client.cx);
  int64_t top = std::clamp<int64_t>(inward(rect.top * scale, true), 0, client.cy);
  int64_t right = std::clamp<int64_t>(inward((rect.left + rect.width) * scale, false), 0, client.cx);
  int64_t bottom = std::clamp<int64_t>(inward((rect.top + rect.height) * scale, false), 0, client.cy);
  if (right - left < 1 || bottom - top < 1) return std::nullopt;
  const int64_t margin = (std::max<int64_t>)(1, int64_t(std::lround(kConfinementMarginLogical * scale)));
  if (right - left > 4 * margin && bottom - top > 4 * margin) {
    left += margin; top += margin; right -= margin; bottom -= margin;
  }
  const int64_t low = (std::numeric_limits<LONG>::min)(), high = (std::numeric_limits<LONG>::max)();
  const int64_t x = origin.x, y = origin.y;
  if (x + left < low || y + top < low || x + right > high || y + bottom > high) return std::nullopt;
  return RECT{LONG(x + left), LONG(y + top), LONG(x + right), LONG(y + bottom)};
}

// Process-wide count of windows that currently hold the clip, read by the
// last-chance crash filter so a fault never leaves the cursor trapped.
inline std::atomic<int>& ClippedPointerWindows() {
  static std::atomic<int> count{0};
  return count;
}
inline LPTOP_LEVEL_EXCEPTION_FILTER& PreviousPointerCrashFilter() {
  static LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr;
  return previous;
}
inline LONG WINAPI ReleasePointerOnCrash(EXCEPTION_POINTERS* exception) {
  if (ClippedPointerWindows().load() > 0) ClipCursor(nullptr);
  const auto previous = PreviousPointerCrashFilter();
  return previous ? previous(exception) : EXCEPTION_CONTINUE_SEARCH;
}
inline void InstallPointerCrashRelease() {
  static std::once_flag once;
  std::call_once(once, [] { PreviousPointerCrashFilter() = SetUnhandledExceptionFilter(ReleasePointerOnCrash); });
}

}  // namespace multitap
