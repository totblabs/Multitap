#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <vector>

namespace multitap {
// Hidden windows (backlog #62): whether a session window shows any of its
// remote picture. Hidden: minimized, not shown, cloaked (another virtual
// desktop), or covered entirely by one of this client's own fullscreen or
// maximized windows above it (no rounded corners, opaque). Another program's
// window never counts as a cover: its translucency is not known here.
// Physical pixels (the client is per-monitor DPI aware).
struct VisibilityCover { RECT bounds{}; bool shown = false; };

// `covers`: this client's edge-to-edge windows above the target in z-order.
inline bool HiddenBehind(bool shown, RECT target, const std::vector<VisibilityCover>& covers, const RECT& screen) {
  if (!shown) return true;
  // Only what lies on the screens has to be covered.
  if (!IntersectRect(&target, &target, &screen)) return false;
  for (const auto& cover : covers) {
    if (cover.shown && cover.bounds.left <= target.left && cover.bounds.top <= target.top &&
        cover.bounds.right >= target.right && cover.bounds.bottom >= target.bottom) return true;
  }
  return false;
}

inline bool WindowShown(HWND window) {
  if (!window || !IsWindow(window) || !IsWindowVisible(window) || IsIconic(window)) return false;
  DWORD cloaked = 0;
  return FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || !cloaked;
}

// The frame as drawn (without the invisible resize borders and the shadow).
inline RECT VisibleBounds(HWND window) {
  RECT bounds{};
  if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) GetWindowRect(window, &bounds);
  return bounds;
}

inline RECT VirtualScreen() {
  const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  return {left, top, left + GetSystemMetrics(SM_CXVIRTUALSCREEN), top + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

// `edge_to_edge`: this client's own fullscreen or maximized top-level windows.
inline bool SessionWindowHidden(HWND target, const std::vector<HWND>& edge_to_edge, const RECT& screen = VirtualScreen()) {
  if (!target || !IsWindow(target)) return false;
  std::vector<VisibilityCover> covers;
  // Bounded: the z-order can change under the walk.
  int steps = 0;
  for (HWND above = GetWindow(target, GW_HWNDPREV); above && ++steps < 4096; above = GetWindow(above, GW_HWNDPREV)) {
    for (const HWND window : edge_to_edge) {
      if (window == above && window != target) covers.push_back({VisibleBounds(window), WindowShown(window)});
    }
  }
  return HiddenBehind(WindowShown(target), VisibleBounds(target), covers, screen);
}
}  // namespace multitap
