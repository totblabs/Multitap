// Hidden windows (backlog #62): when a session window shows none of its
// picture (session_visibility.h). The geometry rule on plain rectangles, then
// the z-order walk on two tool windows placed far outside every monitor (shown
// without activation, never on a screen, no taskbar button) with the test's own
// "screen" rectangle around them. No display, input or real session is used.
#include "session_visibility.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error(reason); }

void geometry() {
  using multitap::HiddenBehind;
  const RECT screen{0, 0, 1920, 1080}, target{100, 100, 900, 700};
  require(HiddenBehind(false, target, {}, screen), "A minimized/cloaked/unshown window must be hidden");
  require(!HiddenBehind(true, target, {}, screen), "A shown window with nothing over it must not be hidden");
  require(HiddenBehind(true, target, {{{0, 0, 1920, 1080}, true}}, screen), "A fullscreen window over it hides it");
  require(HiddenBehind(true, target, {{{100, 100, 900, 700}, true}}, screen), "An exactly equal cover hides it");
  require(!HiddenBehind(true, target, {{{0, 0, 1920, 1080}, false}}, screen), "A cover that is not shown hides nothing");
  require(!HiddenBehind(true, target, {{{101, 100, 900, 700}, true}}, screen), "One uncovered column keeps it shown");
  require(!HiddenBehind(true, target, {{{0, 0, 500, 1080}, true}, {{500, 0, 1920, 1080}, true}}, screen),
          "Two covers that only together cover it are not trusted");
  // A window partly beyond the screens: only its on-screen part must be covered.
  require(HiddenBehind(true, {1800, 900, 2400, 1300}, {{{0, 0, 1920, 1080}, true}}, screen), "Off-screen parts need no cover");
  require(!HiddenBehind(true, {1800, 900, 2400, 1300}, {{{0, 0, 1900, 1080}, true}}, screen), "An on-screen part left over shows");
  // Two monitors: fullscreen on the first does not hide a window on the second.
  require(!HiddenBehind(true, {2000, 100, 2600, 500}, {{{0, 0, 1920, 1080}, true}}, {0, 0, 3840, 1080}), "Another monitor shows");
}

LRESULT CALLBACK Plain(HWND window, UINT message, WPARAM wparam, LPARAM lparam) { return DefWindowProcW(window, message, wparam, lparam); }

void windows() {
  WNDCLASSW type{}; type.lpfnWndProc = Plain; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"Multitap.VisibilityTest";
  require(RegisterClassW(&type) != 0, "Window class");
  // Far beyond any monitor; the test's own screen rectangle surrounds them.
  const int x = -30000, y = -30000;
  const RECT screen{x - 100, y - 100, x + 2000, y + 2000};
  const auto create = [&](int left, int top, int width, int height) {
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, type.lpszClassName, L"", WS_POPUP,
                                        left, top, width, height, nullptr, nullptr, type.hInstance, nullptr);
    require(window != nullptr, "Window");
    ShowWindow(window, SW_SHOWNOACTIVATE);
    return window;
  };
  const HWND session = create(x + 100, y + 100, 800, 600);
  const HWND cover = create(x, y, 1920, 1080);
  SetWindowPos(session, cover, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  // session below cover
  using multitap::SessionWindowHidden;
  require(SessionWindowHidden(session, {cover}, screen), "A session under this client's fullscreen window is hidden");
  require(!SessionWindowHidden(session, {}, screen), "A window that is not edge to edge never covers");
  require(!SessionWindowHidden(cover, {cover}, screen), "A window does not cover itself");
  SetWindowPos(cover, session, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  // cover below session
  require(!SessionWindowHidden(session, {cover}, screen), "A cover below the session hides nothing");
  SetWindowPos(session, cover, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  require(SessionWindowHidden(session, {cover}, screen), "Covered again");
  SetWindowPos(cover, nullptr, x + 200, y, 1720, 1080, SWP_NOZORDER | SWP_NOACTIVATE);
  require(!SessionWindowHidden(session, {cover}, screen), "A cover moved off part of the session shows it");
  SetWindowPos(cover, nullptr, x, y, 1920, 1080, SWP_NOZORDER | SWP_NOACTIVATE);
  ShowWindow(cover, SW_HIDE);
  require(!SessionWindowHidden(session, {cover}, screen), "A hidden cover shows the session");
  ShowWindow(session, SW_HIDE);
  require(SessionWindowHidden(session, {}, screen), "A session window that is not shown is hidden");
  DestroyWindow(session);
  require(!SessionWindowHidden(session, {cover}, screen), "A destroyed window is not reported hidden");
  DestroyWindow(cover);
  UnregisterClassW(type.lpszClassName, type.hInstance);
}
}  // namespace

int main() {
  // Physical pixels like the client (per-monitor DPI aware): DWM's frame bounds
  // are physical, a DPI-unaware process's window rectangles would be scaled.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  try {
    geometry();
    windows();
    std::cout << "{\"kind\":\"session-visibility\",\"ok\":true,\"offscreenToolWindows\":true,\"actualSession\":false}" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "session visibility: " << error.what() << std::endl;
    return 2;
  }
}
