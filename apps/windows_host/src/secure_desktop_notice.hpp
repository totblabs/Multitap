#pragma once
// Secure desktop notice. A UAC prompt (or the Ctrl+Alt+Del screen) of an unlocked session shows on
// the Winlogon desktop, which the user-session worker can neither capture nor send input to; by design nothing in
// the Host gains SYSTEM control of it (signin tests: "Unlocked UAC must not gain SYSTEM control"). Before this, every
// capture tick threw "Interactive desktop is locked, secure or unavailable", the stream ended as
// no_interactive_session and the client said Windows was signed out or locked, which it was not.
//
// Now the stream stays: while the secure desktop shows, the picture is a fixed notice (this file) encoded like an
// idle screen, and when the desktop returns the capture is rebuilt and the stream goes on. Only the encoded picture
// changes, so every released client and relay shows it. A locked session (Win+L, the lock screen is Winlogon too)
// and a notice older than secureDesktopNoticeLimit still end the stream as before.
// Fixed record: hostAction 200 stage 244 (error 1 notice shown, 2 | seconds << 8 desktop returned, 3 ended
// locked, 4 ended at the limit).
//
// A UAC prompt must be visible and answerable in the stream.
// For that one case the first paragraph no longer holds: while the secure desktop of this unlocked session shows,
// the service's SYSTEM helper captures the real prompt and takes the client's input (secure_desktop_scope.hpp,
// secure_desktop_switch.hpp), and this notice is only the fallback picture. Records added: 5 the helper streamed
// the real prompt, 6 the helper could not launch / its scope was denied, 7 the helper was lost while it streamed,
// 8 no frame from the helper within 2 s. 5..8 keep the stream; after 6/7/8 this notice shows. 1 is recorded only
// for a stream that has no helper (the notice alone, as before); a helper answers with 5, 6 or 8. While the helper
// was asked for and has not answered (at most 2 s) nothing is encoded, so the notice never flashes before a
// prompt. The lock (3) is unchanged. The 10-minute limit (4) is for a picture nobody can answer: it does not
// run while the helper's real prompt shows (answerable()), and runs again from a fallback to the notice.
#include <windows.h>
#include <wtsapi32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "host_text.hpp"

namespace host {
inline constexpr uint32_t secureDesktopNoticeStage = 44;
inline constexpr std::chrono::minutes secureDesktopNoticeLimit{10};
inline constexpr char secureDesktopUnavailable[] = "Interactive desktop is locked, secure or unavailable";

// WTS session flags of `session`: true only for an unlocked session (a lock screen is WTS_SESSIONSTATE_LOCK).
inline bool sessionUnlocked(DWORD session) {
  if (session == 0xffffffff) return false;
  LPWSTR buffer = nullptr; DWORD length = 0; bool unlocked = false;
  if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx, &buffer, &length) && length >= sizeof(WTSINFOEXW)) { const auto* info = reinterpret_cast<WTSINFOEXW*>(buffer); unlocked = info->Level == 1 && info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_UNLOCK; }
  if (buffer) WTSFreeMemory(buffer); return unlocked;
}
inline bool ownSessionUnlocked() {
  DWORD session = 0;
  return ProcessIdToSessionId(GetCurrentProcessId(), &session) && session != 0 && sessionUnlocked(session);
}

enum class SecureDesktopStep { capture, notice, resume, end };
// Pure state of one capture loop's notice. `step` answers every tick that cannot capture (the capture threw
// secureDesktopUnavailable) and every tick while the notice shows.
class SecureDesktopNotice {
 public:
  using Clock = std::chrono::steady_clock;
  SecureDesktopStep step(bool desktopAvailable, bool unlocked, Clock::time_point now) {
    if (desktopAvailable) {
      if (!active_) return SecureDesktopStep::capture;
      active_ = false; lastSeconds_ = uint32_t(std::min<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(now - began_).count(), 0xffffff));
      return SecureDesktopStep::resume;
    }
    if (!unlocked) { endedBy_ = 3; active_ = false; return SecureDesktopStep::end; }
    if (!active_) { active_ = true; began_ = limitFrom_ = now; return SecureDesktopStep::notice; }
    if (now - limitFrom_ >= secureDesktopNoticeLimit) { endedBy_ = 4; active_ = false; return SecureDesktopStep::end; }
    return SecureDesktopStep::notice;
  }
  // The stream shows the real prompt right now (the SYSTEM helper's frame), so it can be answered: the limit
  // exists for a picture nobody can answer and counts from the last moment that was so.
  void answerable(Clock::time_point now) { if (active_) limitFrom_ = now; }
  bool active() const { return active_; }
  // The stage 244 error of the last resume or end.
  uint32_t resumedRecord() const { return 2u | (lastSeconds_ << 8); }
  uint32_t endedRecord() const { return endedBy_; }

 private:
  bool active_ = false;
  Clock::time_point began_{}, limitFrom_{};
  uint32_t lastSeconds_ = 0, endedBy_ = 0;
};

// The notice picture, BGRA top-down, width x height: the client launcher's charcoal with one panel, an amber mark,
// a title and two short lines in the signed-in person's language (host_text.hpp; this runs in the stream's worker,
// with that person's token). Grayscale antialiasing only: ClearType's colour
// fringes do not survive NV12 chroma subsampling. A picture too small for text keeps the panel and the mark.
inline std::vector<uint8_t> secureDesktopNoticePicture(int width, int height) {
  if (width < 2 || height < 2) throw std::runtime_error("Invalid notice size");
  std::vector<uint8_t> bgra(size_t(width) * size_t(height) * 4);
  BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
  HDC dc = CreateCompatibleDC(nullptr); void* bits = nullptr;
  HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
  if (!dc || !bitmap || !bits) { if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); throw std::runtime_error("Notice picture unavailable"); }
  HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
  const auto fill = [&](RECT r, COLORREF color) { HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &r, brush); DeleteObject(brush); };
  const COLORREF canvas = RGB(0x0F, 0x12, 0x15), tile = RGB(0x17, 0x1B, 0x1F), edge = RGB(0x26, 0x2C, 0x32), amber = RGB(0xF5, 0xA6, 0x23), ink = RGB(0xE9, 0xF1, 0xF3), muted = RGB(0x9A, 0xAD, 0xB6);
  fill(RECT{0, 0, width, height}, canvas);
  // Title height in pixels: about 1/22 of the shorter side of a 16:10 box, 9..40.
  const int unit = std::clamp(std::min(width * 10 / 16, height) / 22, 9, 40), body = std::max(8, unit * 62 / 100);
  const int margin = std::max(4, unit), panelWidth = std::min(width - 2 * margin, unit * 30), pad = std::max(4, unit * 9 / 10);
  const wchar_t* title = ui_text::get(ui_text::Id::SecureNoticeTitle);
  const wchar_t* lines = ui_text::get(ui_text::Id::SecureNoticeLines);
  HFONT titleFont = CreateFontW(-unit, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Malgun Gothic");
  HFONT bodyFont = CreateFontW(-body, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Malgun Gothic");
  HGDIOBJ previousFont = SelectObject(dc, titleFont);
  SetBkMode(dc, TRANSPARENT);
  const UINT format = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX;
  const int textWidth = panelWidth - 2 * pad, markHeight = std::max(2, unit / 4), gap = std::max(2, unit / 2);
  const bool text = textWidth >= unit * 6 && titleFont && bodyFont;
  RECT titleBox{0, 0, textWidth, 0}, bodyBox{0, 0, textWidth, 0};
  if (text) { DrawTextW(dc, title, -1, &titleBox, format | DT_CALCRECT); SelectObject(dc, bodyFont); DrawTextW(dc, lines, -1, &bodyBox, format | DT_CALCRECT); }
  const int contentHeight = markHeight + (text ? gap + (titleBox.bottom - titleBox.top) + gap + (bodyBox.bottom - bodyBox.top) : 0);
  const int panelHeight = std::min(height - 2 * std::max(2, margin / 2), contentHeight + 2 * pad);
  const int left = (width - panelWidth) / 2, top = (height - panelHeight) / 2;
  if (panelWidth > 2 && panelHeight > 2) {
    HBRUSH brush = CreateSolidBrush(tile); HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ previousBrush = SelectObject(dc, brush), previousPen = SelectObject(dc, pen);
    RoundRect(dc, left, top, left + panelWidth, top + panelHeight, unit, unit);
    SelectObject(dc, previousBrush); SelectObject(dc, previousPen); DeleteObject(brush); DeleteObject(pen);
    int y = top + std::min(pad, std::max(1, (panelHeight - contentHeight) / 2));
    const int markWidth = std::min(panelWidth - 2, std::max(4, unit * 2));
    fill(RECT{left + (panelWidth - markWidth) / 2, y, left + (panelWidth + markWidth) / 2, y + markHeight}, amber);
    y += markHeight;
    if (text) {
      y += gap;
      RECT box{left + pad, y, left + pad + textWidth, y + (titleBox.bottom - titleBox.top)};
      SelectObject(dc, titleFont); SetTextColor(dc, ink); DrawTextW(dc, title, -1, &box, format);
      y = box.bottom + gap;
      box = RECT{left + pad, y, left + pad + textWidth, std::min<LONG>(top + panelHeight, y + (bodyBox.bottom - bodyBox.top))};
      SelectObject(dc, bodyFont); SetTextColor(dc, muted); DrawTextW(dc, lines, -1, &box, format);
    }
  }
  GdiFlush();
  const auto* source = static_cast<const uint8_t*>(bits);
  for (size_t i = 0; i < bgra.size(); i += 4) { bgra[i] = source[i]; bgra[i + 1] = source[i + 1]; bgra[i + 2] = source[i + 2]; bgra[i + 3] = 255; }
  SelectObject(dc, previousFont); if (titleFont) DeleteObject(titleFont); if (bodyFont) DeleteObject(bodyFont);
  SelectObject(dc, previousBitmap); DeleteObject(bitmap); DeleteDC(dc);
  return bgra;
}
}  // namespace host
