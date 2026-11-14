// The cursor drawn into the video (cursor-compose-v1: 마우스 잠금) at the real display scales of this desktop. The primary output is
// set to 100, 150 and 200 % (display_scale.hpp, the same call display-scale-v1 makes) and put back afterwards; at each scale the real
// Capture class draws the cursor over two windows of this probe (the system arrow, and a 64 x 64 cursor of the probe's own) on its
// direct path (unscaled) and its DIB path (scaled to 3/4), and the drawn cursor is measured as the difference to the same frame without
// the cursor. The cursor position is put back too. Changes the display scale while it runs: run it on a test desktop.
#include "capture_selection.hpp"
#include "display_scale.hpp"
#include "cursor_channel.hpp"
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <tuple>

using namespace host;
namespace {
struct Box { int left = 1 << 30, top = 1 << 30, right = -1, bottom = -1; int width() const { return right < left ? 0 : right - left + 1; } int height() const { return bottom < top ? 0 : bottom - top + 1; } };
void pump(int ms) { const auto end = GetTickCount64() + ms; MSG message; while (GetTickCount64() < end) { while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } Sleep(10); } }
// A 64 x 64 colour cursor: a filled square inside a transparent 2-pixel border (60 x 60 visible), hotspot at its centre.
HCURSOR bigCursor() {
  std::vector<uint8_t> mask(64 * 8, 0), colour(64 * 64 * 4, 0);
  for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
    const bool inside = x >= 2 && y >= 2 && x < 62 && y < 62;
    if (!inside) mask[size_t(y) * 8 + x / 8] |= uint8_t(0x80u >> (x % 8));
    else { auto* p = colour.data() + (size_t(y) * 64 + x) * 4; p[0] = 20; p[1] = 200; p[2] = 60; p[3] = 255; }
  }
  ICONINFO info{}; info.fIcon = FALSE; info.xHotspot = 32; info.yHotspot = 32; info.hbmMask = CreateBitmap(64, 64, 1, 1, mask.data());
  BITMAPV5HEADER header{}; header.bV5Size = sizeof(header); header.bV5Width = 64; header.bV5Height = -64; header.bV5Planes = 1; header.bV5BitCount = 32; header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00ff0000; header.bV5GreenMask = 0x0000ff00; header.bV5BlueMask = 0x000000ff; header.bV5AlphaMask = 0xff000000;
  void* bits = nullptr; const HDC screen = GetDC(nullptr); info.hbmColor = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0); ReleaseDC(nullptr, screen);
  if (bits) std::memcpy(bits, colour.data(), colour.size());
  const HCURSOR made = reinterpret_cast<HCURSOR>(CreateIconIndirect(&info)); DeleteObject(info.hbmMask); DeleteObject(info.hbmColor); return made;
}
HWND window(const wchar_t* className, HCURSOR cursor, RECT where) {
  WNDCLASSW value{}; value.lpfnWndProc = DefWindowProcW; value.hInstance = GetModuleHandleW(nullptr); value.lpszClassName = className; value.hCursor = cursor; value.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
  RegisterClassW(&value);
  const HWND made = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, className, L"cursor probe", WS_POPUP | WS_VISIBLE, where.left, where.top, where.right - where.left, where.bottom - where.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  pump(200); return made;
}
// The luma of a capture frame (NV12: width x height bytes first).
std::vector<uint8_t> luma(Capture& capture, int width, int height) { capture.frame(); pump(60); const auto& frame = capture.frame(); return std::vector<uint8_t>(frame.begin(), frame.begin() + size_t(width) * height); }
Box measure(const RECT& source, int width, int height, POINT at) {
  Capture capture(width, height, source);
  capture.composeCursor(false); pump(100); const auto without = luma(capture, width, height);
  capture.composeCursor(true); const auto with = luma(capture, width, height);
  Box box;
  const int cx = int(int64_t(at.x - source.left) * width / (source.right - source.left)), cy = int(int64_t(at.y - source.top) * height / (source.bottom - source.top));
  for (int y = std::max(0, cy - 200); y < std::min(height, cy + 200); ++y) for (int x = std::max(0, cx - 200); x < std::min(width, cx + 200); ++x)
    if (std::abs(int(with[size_t(y) * width + x]) - int(without[size_t(y) * width + x])) > 8) { box.left = std::min(box.left, x); box.top = std::min(box.top, y); box.right = std::max(box.right, x); box.bottom = std::max(box.bottom, y); }
  return box;
}
}

int main() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // as the Host worker
  Runtime runtime;
  if (!interactiveDesktopAvailable()) { std::printf("no interactive desktop\n"); return 3; }
  MONITORINFOEXW monitor{}; monitor.cbSize = sizeof(monitor);
  if (!GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor)) { std::printf("no primary monitor\n"); return 2; }
  const auto target = nativeDisplayScaleTarget(monitor.szDevice);
  const auto range = target ? nativeDisplayScaleRead(*target) : std::nullopt;
  if (!target || !range || !validDisplayScaleRange(*range)) { std::printf("the primary output's scale cannot be read\n"); return 2; }
  POINT saved{}; GetCursorPos(&saved);
  struct Restore { DisplayScaleTarget target; int relative; POINT cursor; ~Restore() { nativeDisplayScaleWrite(target, relative); SetCursorPos(cursor.x, cursor.y); } } restore{*target, range->current, saved};
  const HCURSOR big = bigCursor();
  int failed = 0;
  const auto expect = [&](bool value, const char* what) { if (!value) { ++failed; std::printf("FAIL %s\n", what); } };
  for (const int percent : {100, 150, 200}) {
    const int relative = displayScaleRelativeFor(*range, percent);
    if (!nativeDisplayScaleWrite(*target, relative)) { std::printf("FAIL setting %d %%\n", percent); ++failed; continue; }
    pump(1500);
    const auto now = nativeDisplayScaleRead(*target);
    if (!now || now->current != relative) { std::printf("FAIL the scale did not stay at %d %% (something else set it)\n", percent); ++failed; continue; }
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT source = monitor.rcMonitor; const int w = (source.right - source.left) & ~1, h = (source.bottom - source.top) & ~1;
    const RECT even{source.left, source.top, source.left + w, source.top + h};
    const int midX = (even.left + even.right) / 2, midY = (even.top + even.bottom) / 2;
    const HWND arrowWindow = window(L"MultitapCursorProbeArrow", LoadCursorW(nullptr, IDC_ARROW), RECT{midX - 420, midY - 200, midX - 20, midY + 200});
    const HWND bigWindow = window(L"MultitapCursorProbeBig", big, RECT{midX + 20, midY - 200, midX + 420, midY + 200});
    const UINT dpi = GetDpiForWindow(arrowWindow);
    for (const auto& [name, at, wanted] : {std::tuple<const char*, POINT, int>{"arrow", POINT{midX - 220, midY}, 0}, {"64px", POINT{midX + 220, midY}, 60}}) {
      SetCursorPos(at.x, at.y); pump(400);
      const Box direct = measure(even, w, h, at);
      const int sw = (w * 3 / 4) & ~1, sh = (h * 3 / 4) & ~1;
      const Box scaled = measure(even, sw, sh, at);
      // The output may not go as far as asked (a small monitor stops at 175 %): the scale it has is printed.
      std::printf("%d %% (dpi %u) %s: direct %dx%d, scaled 3/4 %dx%d\n", displayScalePercentFor(*range, relative), dpi, name, direct.width(), direct.height(), scaled.width(), scaled.height());
      if (wanted) {
        expect(std::abs(direct.width() - wanted) <= 2 && std::abs(direct.height() - wanted) <= 2, "a 64-pixel cursor is drawn at 64 (60 visible), not squeezed to the session's size");
      } else {
        // The system arrow is drawn at the nominal size of this scale: 19 visible rows at 32 pixels, so 19 x scale.
        const double rows = 19.0 * double(GetSystemMetricsForDpi(SM_CYCURSOR, dpi)) / 32.0;
        expect(std::abs(direct.height() - rows) <= 3, "the arrow is drawn at the size Windows shows at this scale");
        expect(std::abs(scaled.height() - rows * 0.75) <= 3, "the arrow in a 3/4 picture is 3/4 of that");
      }
    }
    DestroyWindow(arrowWindow); DestroyWindow(bigWindow); pump(100);
  }
  DestroyCursor(big);
  if (failed) std::printf("cursor video probe: %d failed\n", failed); else std::printf("cursor video probe: passed\n");
  return failed ? 1 : 0;
}
