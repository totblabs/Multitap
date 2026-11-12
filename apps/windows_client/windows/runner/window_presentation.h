#pragma once
#include <windows.h>

namespace multitap {
// Window coordinates/monitor bounds are physical pixels in the PMv2 process.
class WindowPresentation {
 public:
  bool Set(HWND window, bool fullscreen, bool all_monitors = false) {
    if (!window || !IsWindow(window)) return false;
    if (fullscreen == fullscreen_ && (!fullscreen || all_monitors == all_monitors_)) return true;
    if (fullscreen_ && fullscreen) {
      if (!Set(window, false)) return false;
    }
    if (fullscreen) {
      placement_.length = sizeof(placement_);
      if (!GetWindowPlacement(window, &placement_)) return false;
      // Leaving fullscreen returns to the normal window, not to a maximized one
      // (a maximize request is what entered fullscreen).
      if (placement_.showCmd == SW_SHOWMAXIMIZED) placement_.showCmd = SW_SHOWNORMAL;
      style_ = GetWindowLongPtrW(window, GWL_STYLE);
      ex_style_ = GetWindowLongPtrW(window, GWL_EXSTYLE);
      MONITORINFO monitor{sizeof(monitor)};
      if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
      SetWindowLongPtrW(window, GWL_STYLE, style_ & ~WS_OVERLAPPEDWINDOW);
      SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style_ & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE));
      RECT bounds = monitor.rcMonitor;
      if (all_monitors) {
        bounds.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        bounds.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        bounds.right = bounds.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        bounds.bottom = bounds.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
      }
      if (!SetWindowPos(window, HWND_TOP, bounds.left, bounds.top, bounds.right - bounds.left,
                        bounds.bottom - bounds.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW)) {
        SetWindowLongPtrW(window, GWL_STYLE, style_);
        SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style_);
        SetWindowPlacement(window, &placement_);
        return false;
      }
      fullscreen_ = true;
      all_monitors_ = all_monitors;
    } else {
      SetWindowLongPtrW(window, GWL_STYLE, style_);
      SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style_);
      SetWindowPlacement(window, &placement_);
      SetWindowPos(window, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
      fullscreen_ = false;
      all_monitors_ = false;
    }
    return true;
  }
  bool fullscreen() const { return fullscreen_; }
  // A display/DPI change must keep fullscreen bounds in current physical
  // pixels. Preserve the original window placement for eventual restoration.
  bool Refresh(HWND window) const {
    if (!fullscreen_) return true;
    if (!window || !IsWindow(window)) return false;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    RECT bounds = monitor.rcMonitor;
    if (all_monitors_) {
      bounds.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
      bounds.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
      bounds.right = bounds.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
      bounds.bottom = bounds.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
    RECT current{};
    if (GetWindowRect(window, &current) && EqualRect(&current, &bounds)) return true;
    return SetWindowPos(window, nullptr, bounds.left, bounds.top,
      bounds.right - bounds.left, bounds.bottom - bounds.top,
      SWP_NOACTIVATE | SWP_NOZORDER) != FALSE;
  }
 private:
  bool fullscreen_ = false;
  bool all_monitors_ = false;
  LONG_PTR style_ = 0, ex_style_ = 0;
  WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)};
};
}  // namespace multitap
