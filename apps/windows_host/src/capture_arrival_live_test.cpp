// The real Capture (media.cpp) on the primary monitor while this test's own
// small window repaints: every tick that took a new WGC frame reports when the
// newest of them arrived (Capture::lastArrivedUs, FrameArrived on the QPC
// clock of hostMonotonicUs), after the previous tick's take and not after now;
// a tick that took none reports 0. Also reports whether the 1 ms
// MinUpdateInterval took effect (Capture::minUpdateIntervalSet). The desktop
// is read into memory only; nothing is saved or sent.
#include "media.hpp"
#include "qpc_clock.hpp"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <winrt/Windows.Foundation.Metadata.h>

namespace {
int checks = 0; std::string failures;
void require(bool ok, const std::string& what) { ++checks; if (!ok) { failures += what + "\n"; std::cerr << "FAIL: " << what << "\n"; } }
std::atomic<int> paintCount{0};
LRESULT CALLBACK ownWindow(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_PAINT) { PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); HBRUSH brush = CreateSolidBrush(RGB(paintCount.load() * 37 & 255, 90, 140)); RECT area{}; GetClientRect(window, &area); FillRect(dc, &area, brush); DeleteObject(brush); EndPaint(window, &paint); return 0; }
  return DefWindowProcW(window, message, w, l);
}
}

int main() try {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  host::Runtime runtime;
  const int w = GetSystemMetrics(SM_CXSCREEN) & ~1, h = GetSystemMetrics(SM_CYSCREEN) & ~1;
  if (w < 64 || h < 64) { std::cout << "{\"kind\":\"capture-arrival-live\",\"skipped\":\"no primary monitor\"}\n"; return 0; }
  // A small window in the bottom-right corner that repaints every ~16 ms (never activated, click-through).
  std::atomic<HWND> window{nullptr}; std::atomic<bool> stop{false};
  std::thread ui([&] {
    WNDCLASSW type{}; type.lpfnWndProc = ownWindow; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"MultitapCaptureArrivalTest"; RegisterClassW(&type);
    HWND created = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT, type.lpszClassName, L"Multitap capture arrival test", WS_POPUP, w - 48, h - 48, 32, 32, nullptr, nullptr, type.hInstance, nullptr);
    ShowWindow(created, SW_SHOWNOACTIVATE); UpdateWindow(created); window = created;
    while (!stop) { ++paintCount; RedrawWindow(created, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW); MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message); Sleep(16); }
    DestroyWindow(created);
  });
  while (!window.load()) Sleep(1);
  struct Stop { std::atomic<bool>& stop; std::thread& ui; ~Stop() { stop = true; ui.join(); } } stopper{stop, ui};
  host::Capture capture(w, h, RECT{0, 0, w, h});
  const std::string backend = capture.backend();
  int updated = 0, idle = 0, outOfOrder = 0, idleStamped = 0;
  std::vector<uint64_t> delays;
  uint64_t previousTake = 0;
  for (int tick = 0; tick < 240; ++tick) {
    Sleep(tick % 3 == 0 ? 40 : 8);  // some ticks find several frames pending, some none
    const uint64_t began = host::qpcMicroseconds();
    (void)capture.frame();
    const uint64_t after = host::qpcMicroseconds(), arrived = capture.lastArrivedUs();
    if (backend == "gdi") { if (arrived) ++idleStamped; continue; }
    if (capture.frameUpdated()) {
      ++updated;
      // The newest frame taken arrived after the previous take began and no later than now.
      if (!arrived || arrived > after || (previousTake && tick > 0 && arrived < previousTake)) ++outOfOrder;
      else delays.push_back(began > arrived ? began - arrived : 0);
    } else { ++idle; if (arrived) ++idleStamped; }
    previousTake = began;
  }
  if (backend != "gdi") {
    require(updated >= 20, "the repainting window gave frames (" + std::to_string(updated) + ")");
    require(outOfOrder == 0, "every taken frame's arrival lies between the previous take and now (" + std::to_string(outOfOrder) + " not)");
  }
  require(idleStamped == 0, "a tick that took no frame reports no arrival (" + std::to_string(idleStamped) + ")");
  // Where the property exists (IGraphicsCaptureSession5) the session takes 1 ms.
  bool property = false;
  try { property = winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval"); } catch (...) {}
  if (backend != "gdi" && property) require(capture.minUpdateIntervalSet(), "the WGC sessions took the 1 ms update interval");
  if (backend == "gdi") require(!capture.minUpdateIntervalSet(), "GDI reports no update interval");
  std::sort(delays.begin(), delays.end());
  const auto at = [&](double q) { return delays.empty() ? 0 : delays[std::min(delays.size() - 1, size_t(q * double(delays.size() - 1) + .5))]; };
  std::cout << "{\"kind\":\"capture-arrival-live\",\"backend\":\"" << backend << "\",\"minUpdateInterval\":" << (capture.minUpdateIntervalSet() ? "true" : "false")
            << ",\"updatedTicks\":" << updated << ",\"idleTicks\":" << idle << ",\"arrivalToTakeUs\":{\"p50\":" << at(.5) << ",\"p95\":" << at(.95) << ",\"max\":" << at(1)
            << "},\"checks\":" << checks << ",\"result\":\"" << (failures.empty() ? "pass" : "fail") << "\"}\n";
  return failures.empty() ? 0 : 1;
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
