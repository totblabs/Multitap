// Smoke test of the clipboard transfer notice (runner/transfer_notice.cpp): the real card window and a
// recording taskbar. Everything that creates a window runs on a private desktop, so nothing appears on the
// user's screen and the user's foreground window cannot change; the test also reads the foreground window of the
// user's desktop before and after and fails if it moved.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "transfer_notice.h"

// GDI+ names min/max unqualified.
#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>

using namespace multitap;

namespace {
int failures = 0;
void Check(bool ok, const char* what, int line) {
  if (ok) return;
  ++failures;
  std::printf("FAIL line %d: %s\n", line, what);
}
#define CHECK(x) Check((x), #x, __LINE__)

struct Recorder final : TaskbarProgress {
  struct Call { HWND window; TaskbarMode mode; int percent; };
  std::vector<Call> calls;
  void Set(HWND window, TaskbarMode mode, int percent) override { calls.push_back({window, mode, percent}); }
};

LRESULT CALLBACK Plain(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

HWND MakeOwner() {
  static const ATOM atom = [] {
    WNDCLASSW cls{};
    cls.lpfnWndProc = Plain;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"TransferNoticeSmokeOwner";
    return RegisterClassW(&cls);
  }();
  (void)atom;
  return CreateWindowExW(0, L"TransferNoticeSmokeOwner", L"owner", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

TransferNoticeContent Running(int percent, int generation) {
  TransferNoticeContent c;
  c.kind = TransferNoticeContent::Kind::running;
  c.title = L"WorkPC → PC";
  c.subtitle = L"다운로드 · 41개 중 3개";
  c.folder = true;
  c.name = L"아주아주아주아주아주아주아주아주아주아주아주아주아주 긴 파일 이름이 여기에 들어갑니다.mp4";
  c.detail = L"12.3 MB / 40.0 MB · 5.2 MB/s";
  c.trailing = L"약 6초 남음";
  c.details = {{L"지금 파일", c.name}, {L"항목", L"폴더 2개 · 파일 41개"}, {L"저장 위치", L"C:\\Users\\<user>\\Downloads"}};
  c.percent = percent;
  c.generation = generation;
  return c;
}

constexpr int kCardWidth = 360;
int RunningHeight(UINT dpi) { return MulDiv(134, int(dpi), 96) + MulDiv(14, int(dpi), 96); }

struct Frame {
  int width = 0, height = 0;
  std::vector<uint8_t> pixels;
  // Premultiplied BGRA of one pixel as 0xAARRGGBB.
  uint32_t At(int x, int y) const {
    const uint8_t* p = &pixels[(size_t(y) * size_t(width) + size_t(x)) * 4];
    return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
  }
};

// With MULTITAP_NOTICE_BMP_DIR set, every frame is also written there as a BMP (over a mid grey) for a look.
void Dump(const Frame& frame, const std::wstring& name) {
  wchar_t dir[MAX_PATH]{};
  if (!GetEnvironmentVariableW(L"MULTITAP_NOTICE_BMP_DIR", dir, MAX_PATH)) return;
  std::vector<uint8_t> out(frame.pixels.size());
  for (size_t i = 0; i < frame.pixels.size(); i += 4) {
    const int a = frame.pixels[i + 3];
    for (int c = 0; c < 3; ++c) out[i + size_t(c)] = uint8_t(frame.pixels[i + size_t(c)] + (128 * (255 - a)) / 255);
    out[i + 3] = 255;
  }
  BITMAPFILEHEADER file{0x4D42, DWORD(sizeof(file) + sizeof(BITMAPINFOHEADER) + out.size()), 0, 0, DWORD(sizeof(file) + sizeof(BITMAPINFOHEADER))};
  BITMAPINFOHEADER header{sizeof(header), frame.width, -frame.height, 1, 32, BI_RGB, DWORD(out.size()), 0, 0, 0, 0};
  const std::wstring path = std::wstring(dir) + L"\\notice-" + name + L".bmp";
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
    std::fwrite(&file, sizeof(file), 1, f);
    std::fwrite(&header, sizeof(header), 1, f);
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
  }
}

RECT Rect(HWND window) { RECT r{}; GetWindowRect(window, &r); return r; }

void Pure() {
  const RECT area{0, 0, 1920, 1040};
  const RECT rect = NoticePlacement(area, 340, 136, 16);
  CHECK(rect.left == 790 && rect.top == 452 && rect.right - rect.left == 340 && rect.bottom - rect.top == 136);
  // A work area on a second monitor to the left (negative coordinates) and a taskbar on the right edge.
  const RECT left{-1600, 100, -100, 900};
  const RECT placed = NoticePlacement(left, 340, 136, 16);
  CHECK(placed.left == -1020 && placed.top == 432 && placed.right == -680 && placed.bottom == 568);
  // Too small to fit: pinned to the top left, never outside.
  const RECT tiny{0, 0, 200, 100};
  const RECT forced = NoticePlacement(tiny, 340, 136, 16);
  CHECK(forced.left == 0 && forced.top == 0);
  // Dragged half off screen comes back inside by the least move.
  const RECT out = KeepInside({1800, 1000, 2140, 1136}, area);
  CHECK(out.right == 1920 && out.bottom == 1040 && out.left == 1580 && out.top == 904);
  for (const UINT dpi : {96u, 120u, 144u, 192u, 240u}) {
    const SIZE running = NoticeSize(Running(31, 1), false, dpi);
    const SIZE done = NoticeSize(TransferNoticeContent::Kind::done, dpi);
    CHECK(running.cx == MulDiv(kCardWidth, int(dpi), 96) && running.cy == RunningHeight(dpi));
    CHECK(done.cy < running.cy && done.cx == running.cx);
    // Preparing (nothing measured yet) leaves no empty line under the bar.
    const SIZE preparing = NoticeSize(TransferNoticeContent::Kind::running, dpi);
    CHECK(preparing.cx == running.cx && preparing.cy == MulDiv(110, int(dpi), 96) + MulDiv(14, int(dpi), 96));
    // 자세히 grows the card by its rows; a long path takes a second line.
    const SIZE open = NoticeSize(Running(31, 1), true, dpi);
    CHECK(open.cx == running.cx && open.cy > running.cy + MulDiv(3 * 18, int(dpi), 96));
    // A failure grows with its cause and next step instead of leaving a gap.
    TransferNoticeContent failed;
    failed.kind = TransferNoticeContent::Kind::failed;
    failed.title = L"옮기지 못했어요";
    failed.message = L"받는 쪽 디스크 공간이 부족해요.";
    const SIZE shortFailure = NoticeSize(failed, false, dpi);
    failed.hint = L"공간을 확보한 뒤 다시 시도하세요. 이 문장은 일부러 길게 써서 두 줄 이상으로 넘어가게 합니다.";
    const SIZE longFailure = NoticeSize(failed, false, dpi);
    CHECK(longFailure.cy > shortFailure.cy + MulDiv(18, int(dpi), 96));
  }
}

// Every state drawn without a window, at the dpis of real displays: readable sizes, nothing outside the card, the
// direction and outcome icons and the accent bar where they belong. With a directory argument the frames are also
// written there as PNG over a dark and a light desktop for review.
int PngEncoder(CLSID* clsid) {
  UINT count = 0, bytes = 0;
  Gdiplus::GetImageEncodersSize(&count, &bytes);
  if (!bytes) return -1;
  std::vector<uint8_t> buffer(bytes);
  auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
  Gdiplus::GetImageEncoders(count, bytes, encoders);
  for (UINT i = 0; i < count; ++i)
    if (std::wcscmp(encoders[i].MimeType, L"image/png") == 0) { *clsid = encoders[i].Clsid; return int(i); }
  return -1;
}

void Preview(const wchar_t* directory) {
  struct Sample { const wchar_t* name; TransferNoticeContent content; bool expanded; int hot; };
  std::vector<Sample> samples;
  TransferNoticeContent preparing;
  preparing.title = L"WorkPC → PC";
  preparing.subtitle = L"준비하는 중…";
  samples.push_back({L"1-preparing", preparing, false, 0});
  TransferNoticeContent running = Running(6, 1);
  running.subtitle = L"다운로드 · 1,702개 중 120개";
  running.detail = L"113.3 MB / 1.8 GB · 8.0 MB/s";
  running.trailing = L"약 4분 남음";
  running.details = {{L"지금 파일", L"김정훈_고급2.jpg"}, {L"항목", L"폴더 297개 · 파일 1,702개"},
                     {L"저장 위치", L"C:\\Users\\<user>\\Downloads"}};
  samples.push_back({L"2-running-folder", running, false, 0});
  samples.push_back({L"3-running-details", running, true, 4});
  TransferNoticeContent unknown = running;
  unknown.folder = false;
  unknown.subtitle = L"1,702개 중 120개";
  unknown.details.pop_back();
  samples.push_back({L"4-running-no-folder", unknown, false, 1});
  TransferNoticeContent one = running;
  one.folder = false;
  one.sending = true;
  one.title = L"PC → WorkPC";
  one.subtitle = L"분기별 실적 보고서_최종_수정본(2026년 3분기 전체 부서 취합).xlsx";
  one.details = {};
  one.percent = 42;
  one.detail = L"25.2 MB / 60.0 MB · 3.1 MB/s";
  one.trailing = L"약 15초 남음";
  samples.push_back({L"5-sending-one-file", one, false, 0});
  TransferNoticeContent finishing = running;
  finishing.percent = 100;
  finishing.detail = L"1.8 GB / 1.8 GB";
  finishing.trailing = L"마무리하는 중…";
  samples.push_back({L"6-finishing", finishing, false, 0});
  TransferNoticeContent done;
  done.kind = TransferNoticeContent::Kind::done;
  done.title = L"옮겼어요";
  done.subtitle = L"다운로드에 있어요";
  done.openable = true;
  samples.push_back({L"7-done-folder", done, false, 3});
  TransferNoticeContent doneUnknown = done;
  doneUnknown.openable = false;
  doneUnknown.subtitle = L"WorkPC → PC · 파일 1,702개";
  samples.push_back({L"8-done-no-folder", doneUnknown, false, 0});
  TransferNoticeContent failed;
  failed.kind = TransferNoticeContent::Kind::failed;
  failed.title = L"옮기지 못했어요";
  failed.message = L"받는 쪽 디스크 공간이 부족해요.";
  failed.hint = L"공간을 확보한 뒤 다시 시도하세요.";
  samples.push_back({L"9-failed", failed, false, 2});
  TransferNoticeContent cancelled;
  cancelled.kind = TransferNoticeContent::Kind::cancelled;
  cancelled.title = L"전송을 취소했어요";
  cancelled.subtitle = L"WorkPC → PC";
  samples.push_back({L"10-cancelled", cancelled, false, 0});

  Gdiplus::GdiplusStartupInput input;
  ULONG_PTR token = 0;
  CHECK(Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok);
  struct Shutdown { ULONG_PTR token; ~Shutdown() { if (token) Gdiplus::GdiplusShutdown(token); } } shutdown{token};
  CLSID png{};
  const bool write = directory && PngEncoder(&png) >= 0;
  for (const UINT dpi : {96u, 120u, 144u, 192u}) {
    for (const bool dark : {true, false}) {
      for (const auto& sample : samples) {
        auto content = sample.content;
        content.dark = dark;
        std::vector<uint8_t> pixels;
        SIZE size{};
        CHECK(RenderTransferNotice(content, sample.expanded, dpi, pixels, size, sample.hot));
        if (pixels.empty()) continue;
        CHECK(size.cx == MulDiv(kCardWidth, int(dpi), 96));
        const auto at = [&](int x, int y) {
          const uint8_t* p = &pixels[(size_t(y) * size_t(size.cx) + size_t(x)) * 4];
          return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
        };
        // Rounded transparent corners, an opaque card inside.
        CHECK((at(0, 0) >> 24) == 0 && (at(size.cx - 1, size.cy - 1) >> 24) == 0);
        CHECK((at(size.cx / 2, size.cy / 2) >> 24) == 0xFF);
        // The badge is drawn in its colour (amber running, green done, red failed).
        const int bx = MulDiv(16 + 15, int(dpi), 96), by = MulDiv(content.kind == TransferNoticeContent::Kind::running ||
                                                                      content.kind == TransferNoticeContent::Kind::failed ? 14 + 15 : 18 + 15,
                                                                  int(dpi), 96);
        CHECK(at(bx, by) != at(size.cx - 2, size.cy / 2));
        if (content.kind == TransferNoticeContent::Kind::running && content.percent > 10) {
          // The bar's filled part is the accent colour.
          const uint32_t accent = dark ? 0xFFF5A623u : 0xFFA86200u;
          CHECK(at(MulDiv(16 + 4, int(dpi), 96), MulDiv(62 + 3, int(dpi), 96)) == accent);
        }
        if (!write) continue;
        for (const bool backdrop : {true, false}) {
          const int pad = MulDiv(24, int(dpi), 96);
          Gdiplus::Bitmap canvas(size.cx + 2 * pad, size.cy + 2 * pad, PixelFormat32bppARGB);
          {
            Gdiplus::Graphics g(&canvas);
            g.Clear(backdrop ? Gdiplus::Color(0xFF, 0x20, 0x2A, 0x33) : Gdiplus::Color(0xFF, 0xDD, 0xE3, 0xEA));
            Gdiplus::Bitmap card(size.cx, size.cy, size.cx * 4, PixelFormat32bppPARGB, pixels.data());
            g.DrawImage(&card, pad, pad, size.cx, size.cy);
          }
          const std::wstring path = std::wstring(directory) + L"\\native-" + sample.name + (dark ? L"-dark" : L"-light") +
                                    L"-" + std::to_wstring(dpi * 100 / 96) + L"pct" + (backdrop ? L"" : L"-on-light") + L".png";
          if (dark == backdrop) CHECK(canvas.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok);
        }
      }
    }
  }
}

void Window() {
  // Everything here happens on a private desktop.
  const std::wstring name = L"multitap-transfer-notice-smoke-" + std::to_wstring(GetCurrentProcessId());
  const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
  CHECK(desktop != nullptr);
  if (!desktop) return;
  if (!SetThreadDesktop(desktop)) {
    CHECK(false && "private desktop unavailable; no window may be created");
    CloseDesktop(desktop);
    return;
  }

  auto recorder = std::make_unique<Recorder>();
  Recorder* taskbar = recorder.get();
  std::atomic<bool> foreground{false};
  std::atomic<UINT> dpi{96};
  TransferNoticeAccess access;
  access.owner_foreground = [&](HWND) { return foreground.load(); };
  access.cursor = [] { return POINT{10, 10}; };
  access.dpi_override = [&] { return dpi.load(); };
  Frame last;
  int frames = 0;
  access.frame_sink = [&](const uint8_t* bits, int width, int height) {
    last.width = width;
    last.height = height;
    last.pixels.assign(bits, bits + size_t(width) * size_t(height) * 4);
    ++frames;
    Dump(last, std::to_wstring(frames));
  };
  TransferNotice notice(std::move(recorder), access);

  const HWND owner = MakeOwner(), other = MakeOwner();
  CHECK(owner && other);
  const HWND before = GetForegroundWindow();
  int cancels = 0, closes = 0;
  const auto onCancel = [&] { ++cancels; };
  const auto onClose = [&] { ++closes; };

  CHECK(notice.window() == nullptr && !notice.visible());
  notice.Show(owner, Running(31, 1), onCancel, onClose);
  const HWND card = notice.window();
  CHECK(card != nullptr && notice.visible());
  const LONG_PTR ex = GetWindowLongPtrW(card, GWL_EXSTYLE);
  CHECK((ex & WS_EX_NOACTIVATE) && (ex & WS_EX_TOOLWINDOW) && (ex & WS_EX_TOPMOST) && (ex & WS_EX_LAYERED));
  CHECK((GetWindowLongPtrW(card, GWL_STYLE) & WS_POPUP) != 0);
  CHECK(GetWindow(card, GW_OWNER) == nullptr);

  // Center of the work area of the monitor, sized for the dpi.
  MONITORINFO info{sizeof(info)};
  GetMonitorInfoW(MonitorFromPoint({10, 10}, MONITOR_DEFAULTTONEAREST), &info);
  RECT r = Rect(card);
  const int height = RunningHeight(96);
  CHECK(r.left == info.rcWork.left + (info.rcWork.right - info.rcWork.left - kCardWidth) / 2);
  CHECK(r.top == info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2);
  CHECK(r.right - r.left == kCardWidth && r.bottom - r.top == height);

  // What was drawn (96 dpi): transparent corners, the dark card, the accent part of the bar up to 31 % and the track after it.
  CHECK(frames >= 1 && last.width == kCardWidth && last.height == height);
  CHECK((last.At(0, 0) >> 24) == 0);
  CHECK(last.At(352, 100) == 0xFF161A1Eu);
  CHECK(last.At(50, 65) == 0xFFF5A623u);
  CHECK(last.At(250, 65) == 0xFF2A3238u);
  // A pixel row of the title / the line under it / sizes and time left has ink in it (text really drew).
  const auto inked = [&](int y0, int y1, int x0 = 58, int x1 = 300) {
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x)
        if (last.At(x, y) != 0xFF161A1Eu && last.At(x, y) != 0xFF2A3238u && last.At(x, y) != 0xFFF5A623u) return true;
    return false;
  };
  CHECK(inked(14, 34) && inked(36, 54) && inked(76, 94, 16, 200) && inked(76, 94, 280, 344));
  // The percent sits at the right end of the title row.
  CHECK(inked(14, 34, 312, 344));
  // The window's name is what a screen reader says: the direction while it runs, not every tick.
  wchar_t spoken[256]{};
  GetWindowTextW(card, spoken, 256);
  CHECK(std::wstring(spoken) == L"WorkPC → PC, 복사 중");

  // It never takes focus: not on show, not when it is clicked.
  CHECK(SendMessageW(card, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE);
  CHECK(GetForegroundWindow() == before);
  CHECK(GetActiveWindow() != card);

  // Hit testing: the buttons are clickable, everything else drags the card.
  const auto client = [&](int x, int y) { return MAKELPARAM(x, y); };
  const auto screen = [&](int x, int y) { return MAKELPARAM(r.left + x, r.top + y); };
  const int cancelX = kCardWidth - 16 - 34, betweenX = 200, detailsX = 40, buttonY = 118;
  CHECK(SendMessageW(card, WM_NCHITTEST, 0, screen(cancelX, buttonY)) == HTCLIENT);
  CHECK(SendMessageW(card, WM_NCHITTEST, 0, screen(detailsX, buttonY)) == HTCLIENT);
  CHECK(SendMessageW(card, WM_NCHITTEST, 0, screen(betweenX, buttonY)) == HTCAPTION);
  CHECK(SendMessageW(card, WM_NCHITTEST, 0, screen(60, 40)) == HTCAPTION);

  // 자세히 opens the rows: the card grows upward, its bottom edge stays; 간단히 folds it again.
  SendMessageW(card, WM_LBUTTONDOWN, MK_LBUTTON, client(detailsX, buttonY));
  SendMessageW(card, WM_LBUTTONUP, 0, client(detailsX, buttonY));
  CHECK(notice.expanded());
  const RECT grown = Rect(card);
  CHECK(grown.bottom == r.bottom && grown.top < r.top && grown.right - grown.left == kCardWidth);
  CHECK(last.height == grown.bottom - grown.top);
  CHECK(inked(height + 10, last.height - 14, 30, 340));
  notice.PressDetails();
  CHECK(!notice.expanded() && Rect(card).top == r.top);

  // A click on 취소 runs the callback once; a press that ends elsewhere does not.
  SendMessageW(card, WM_LBUTTONDOWN, MK_LBUTTON, client(cancelX, buttonY));
  SendMessageW(card, WM_LBUTTONUP, 0, client(60, 40));
  CHECK(cancels == 0);
  SendMessageW(card, WM_LBUTTONDOWN, MK_LBUTTON, client(cancelX, buttonY));
  SendMessageW(card, WM_LBUTTONUP, 0, client(cancelX, buttonY));
  CHECK(cancels == 1);

  // While cancelling the button is not clickable and does nothing.
  TransferNoticeContent cancelling = Running(31, 1);
  cancelling.cancelling = true;
  notice.Show(owner, cancelling, onCancel, onClose);
  CHECK(SendMessageW(card, WM_NCHITTEST, 0, screen(cancelX, buttonY)) == HTCAPTION);
  notice.PressCancel();
  CHECK(cancels == 1);
  notice.Show(owner, Running(40, 1), onCancel, onClose);

  // The session window comes to the front: the card steps aside, and returns when it leaves.
  foreground = true;
  notice.Refresh();
  CHECK(!notice.visible());
  foreground = false;
  notice.Refresh();
  CHECK(notice.visible());

  // A running copy cannot be hidden while its remote view is blocked.
  notice.PressClose();
  CHECK(closes == 0 && notice.visible() && !notice.dismissed());
  notice.Show(owner, Running(50, 1), onCancel, onClose);
  CHECK(notice.visible());
  notice.Show(owner, Running(0, 2), onCancel, onClose);
  CHECK(notice.visible() && !notice.dismissed());

  // Done stays centered even though its height is smaller; with no known folder it has no button.
  r = Rect(card);
  TransferNoticeContent done;
  done.kind = TransferNoticeContent::Kind::done;
  done.title = L"옮겼어요";
  done.subtitle = L"WorkPC → PC · 파일 41개";
  done.generation = 2;
  notice.Show(owner, done, nullptr, nullptr);
  const RECT d = Rect(card);
  CHECK(d.left + d.right == r.left + r.right && d.top + d.bottom == r.top + r.bottom && d.bottom - d.top == 68);
  CHECK(SendMessageW(card, WM_NCHITTEST, 0, MAKELPARAM(d.right - 16 - 48, d.top + 34)) == HTCAPTION);

  // A failure stays: title, the cause and the next step, 닫기 only.
  TransferNoticeContent failed;
  failed.kind = TransferNoticeContent::Kind::failed;
  failed.title = L"옮기지 못했어요";
  failed.message = L"VM의 디스크 공간이 부족해요.";
  failed.hint = L"공간을 확보한 뒤 다시 복사해 주세요.";
  failed.generation = 2;
  notice.Show(owner, failed, nullptr, onClose);
  const RECT f = Rect(card);
  CHECK(notice.visible() && f.left + f.right == r.left + r.right && f.top + f.bottom == r.top + r.bottom);
  CHECK(f.bottom - f.top > d.bottom - d.top);
  GetWindowTextW(card, spoken, 256);
  CHECK(std::wstring(spoken) == L"옮기지 못했어요, VM의 디스크 공간이 부족해요., 공간을 확보한 뒤 다시 복사해 주세요.");
  CHECK(SendMessageW(card, WM_NCHITTEST, 0, MAKELPARAM(f.right - 16 - 34, f.bottom - 14 - 16)) == HTCLIENT);
  notice.PressCancel();
  CHECK(cancels == 1);  // a failed card has no 취소

  // Another window's hide does not take this card away; the owner's does.
  notice.Hide(other);
  CHECK(notice.visible());
  notice.Hide(owner);
  CHECK(!notice.visible());

  // The same card drawn at the dpis of real displays (the window size follows the dpi).
  for (const UINT scale : {96u, 120u, 144u, 192u, 240u}) {
    notice.Hide(owner);
    notice.Refresh();
    dpi = scale;
    notice.Show(owner, Running(-1, 10 + int(scale)), onCancel, onClose);
    CHECK(notice.visible());
    const RECT s = Rect(card);
    CHECK(s.right - s.left == MulDiv(kCardWidth, int(scale), 96) && s.bottom - s.top == RunningHeight(scale));
    CHECK(s.right <= info.rcWork.right && s.bottom <= info.rcWork.bottom && s.left >= info.rcWork.left && s.top >= info.rcWork.top);
  }
  dpi = 96;

  // The taskbar: the owner's button gets exactly what the session sets, and Detach clears it and the card.
  // (Each earlier transfer's retirement already cleared its own button; only what follows is counted.)
  taskbar->calls.clear();
  notice.SetTaskbar(owner, TaskbarMode::indeterminate, 0);
  notice.SetTaskbar(owner, TaskbarMode::normal, 42);
  notice.SetTaskbar(owner, TaskbarMode::error, 100);
  notice.SetTaskbar(owner, TaskbarMode::none, 0);
  CHECK(taskbar->calls.size() == 4);
  CHECK(taskbar->calls[0].mode == TaskbarMode::indeterminate && taskbar->calls[0].window == owner);
  CHECK(taskbar->calls[1].mode == TaskbarMode::normal && taskbar->calls[1].percent == 42);
  CHECK(taskbar->calls[2].mode == TaskbarMode::error);
  CHECK(taskbar->calls[3].mode == TaskbarMode::none);
  notice.Show(owner, Running(5, 99), onCancel, onClose);
  CHECK(notice.visible());
  taskbar->calls.clear();
  notice.Detach(owner);
  CHECK(!notice.visible());
  CHECK(taskbar->calls.size() == 1 && taskbar->calls[0].mode == TaskbarMode::none && taskbar->calls[0].window == owner);
  // After Detach no callback of that owner runs.
  notice.PressCancel();
  notice.PressClose();
  CHECK(cancels == 1);

  // The real ITaskbarList3 wrapper tolerates a window that is not on a taskbar (no crash, no throw).
  auto real = CreateTaskbarProgress();
  real->Set(owner, TaskbarMode::normal, 10);
  real->Set(owner, TaskbarMode::none, 0);
  real->Set(nullptr, TaskbarMode::normal, 10);

  // 폴더 열기 on a finished paste whose folder is known reaches the session once; it is gone when no folder is known.
  {
    int opens = 0;
    TransferNoticeCallbacks callbacks;
    callbacks.prepare = [](const NoticeStamp&, std::function<void(bool)> ready) { ready(true); };
    callbacks.open = [&](const NoticeStamp&) { ++opens; };
    const auto reply = notice.Acquire(owner, "0123456789abcdef0123456789abcdef", 77, callbacks);
    CHECK(reply.accepted);
    TransferNoticeContent finished;
    finished.kind = TransferNoticeContent::Kind::done;
    finished.title = L"옮겼어요";
    finished.subtitle = L"다운로드에 있어요";
    finished.openable = true;
    finished.generation = 77;
    notice.Update(reply.state.identity, finished);
    const HWND openCard = notice.window();
    CHECK(notice.visible());
    const RECT o = Rect(openCard);
    CHECK(o.bottom - o.top == 68);
    CHECK(SendMessageW(openCard, WM_NCHITTEST, 0, MAKELPARAM(o.right - 16 - 48, o.top + 34)) == HTCLIENT);
    SendMessageW(openCard, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(kCardWidth - 16 - 48, 34));
    SendMessageW(openCard, WM_LBUTTONUP, 0, MAKELPARAM(kCardWidth - 16 - 48, 34));
    CHECK(opens == 1);
    finished.openable = false;
    notice.Update(reply.state.identity, finished);
    notice.PressOpen();
    CHECK(opens == 1);
    CHECK(SendMessageW(openCard, WM_NCHITTEST, 0, MAKELPARAM(o.right - 16 - 48, o.top + 34)) == HTCAPTION);
    // A cancelled transfer says so briefly, without buttons.
    TransferNoticeContent cancelled;
    cancelled.kind = TransferNoticeContent::Kind::cancelled;
    cancelled.title = L"전송을 취소했어요";
    cancelled.generation = 77;
    notice.Update(reply.state.identity, cancelled);
    CHECK(notice.visible() && Rect(openCard).bottom - Rect(openCard).top == 68);
    CHECK(SendMessageW(openCard, WM_NCHITTEST, 0, MAKELPARAM(o.right - 16 - 48, o.top + 34)) == HTCAPTION);
    notice.Detach(owner);
    CHECK(!notice.visible());
  }

  CHECK(GetForegroundWindow() == before);
  DestroyWindow(owner);
  DestroyWindow(other);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--placement-only") == 0) {
    Pure();
    Preview(nullptr);
    std::printf(failures ? "transfer notice placement FAILED (%d)\n" : "transfer notice placement OK\n", failures);
    return failures ? 1 : 0;
  }
  // --preview <folder>: every state drawn without a window, written as PNG for review.
  if (argc == 3 && std::strcmp(argv[1], "--preview") == 0) {
    wchar_t folder[MAX_PATH]{};
    MultiByteToWideChar(CP_ACP, 0, argv[2], -1, folder, MAX_PATH);
    CreateDirectoryW(folder, nullptr);
    Preview(folder);
    std::printf(failures ? "transfer notice preview FAILED (%d)\n" : "transfer notice preview OK\n", failures);
    return failures ? 1 : 0;
  }
  if (argc != 1) return 2;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  // The user's desktop: what is in front now must be in front at the end.
  const HWND foreground = GetForegroundWindow();
  Pure();
  Preview(nullptr);
  std::thread worker(Window);
  worker.join();
  if (GetForegroundWindow() != foreground) {
    ++failures;
    std::printf("FAIL: the foreground window of the user's desktop changed\n");
  }
  std::printf(failures ? "transfer notice smoke FAILED (%d)\n" : "transfer notice smoke OK\n", failures);
  return failures ? 1 : 0;
}
