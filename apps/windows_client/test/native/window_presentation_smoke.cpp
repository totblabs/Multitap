#include <windows.h>
#include <cstdio>
#include "window_presentation.h"
int main() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HWND window = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"Multitap fullscreen QA", WS_OVERLAPPEDWINDOW,
      123, 111, 740, 460, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!window) return 1;
  ShowWindow(window, SW_SHOWNOACTIVATE);
  RECT original{}; GetWindowRect(window, &original);
  const auto style = GetWindowLongPtrW(window, GWL_STYLE), ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
  multitap::WindowPresentation presentation;
  bool passed = true;
  for (int i = 0; i < 3; ++i) {
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    passed = passed && presentation.Set(window, true) && presentation.fullscreen();
    RECT actual{}; GetWindowRect(window, &actual);
    passed = passed && EqualRect(&actual, &monitor.rcMonitor) && !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION);
    passed = passed && presentation.Set(window, false) && !presentation.fullscreen();
    GetWindowRect(window, &actual);
    passed = passed && EqualRect(&actual, &original) && GetWindowLongPtrW(window, GWL_STYLE) == style && GetWindowLongPtrW(window, GWL_EXSTYLE) == ex;
  }
  WINDOWPLACEMENT maximize{sizeof(maximize)};
  GetWindowPlacement(window, &maximize); maximize.showCmd = SW_SHOWMAXIMIZED;
  SetWindowPlacement(window, &maximize);
  WINDOWPLACEMENT before{sizeof(before)}; GetWindowPlacement(window, &before);
  passed = passed && presentation.Set(window, true);
  MONITORINFO fullMonitor{sizeof(fullMonitor)};
  GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&fullMonitor);
  RECT fullClient{};GetClientRect(window,&fullClient);
  POINT clientOrigin{};ClientToScreen(window,&clientOrigin);
  const bool maximizedPixels=fullClient.right==fullMonitor.rcMonitor.right-fullMonitor.rcMonitor.left&&
      fullClient.bottom==fullMonitor.rcMonitor.bottom-fullMonitor.rcMonitor.top&&
      clientOrigin.x==fullMonitor.rcMonitor.left&&clientOrigin.y==fullMonitor.rcMonitor.top;
  passed=passed&&maximizedPixels&&presentation.Set(window,false);
  WINDOWPLACEMENT after{sizeof(after)}; GetWindowPlacement(window, &after);
  passed = passed && before.showCmd == after.showCmd && EqualRect(&before.rcNormalPosition, &after.rcNormalPosition);
  DestroyWindow(window);
  std::printf("{\"fullscreenRestoreCycles\":4,\"physicalMonitorBounds\":%s,\"maximizedPlacementRestored\":%s,\"keysInjected\":0}\n", passed ? "true" : "false", passed ? "true" : "false");
  return passed ? 0 : 2;
}
