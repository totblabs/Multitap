#ifndef MULTITAP_CLIENT_TRAY_H_
#define MULTITAP_CLIENT_TRAY_H_

// The client's notification-area icon (2026-10-27: closing the main window
// rests the client in the tray). Everything that touches Windows' shell goes
// through ShellApi, so the smoke test drives the same code with a fake shell on
// a private desktop and checks the exact add/modify/delete calls.

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "ui_language.h"

namespace multitap::tray {

// Callback message of the icon, delivered to the tray window (not the main one).
constexpr UINT kCallbackMessage = WM_APP + 1;
constexpr UINT kIconId = 1;
// A stable identity, so Windows keeps the user's "always show" choice for it.
// {7B3E5A1C-4D52-4F6B-9A0E-2C8D1F6B7A31}
constexpr GUID kIconGuid = {0x7b3e5a1c, 0x4d52, 0x4f6b,
                            {0x9a, 0x0e, 0x2c, 0x8d, 0x1f, 0x6b, 0x7a, 0x31}};
constexpr wchar_t kHostClass[] = L"MULTITAP_CLIENT_TRAY";
constexpr int kMenuOpen = 1, kMenuExit = 2;

inline std::wstring Tooltip(int connections) {
  if (connections <= 0) return L"Multitap";
  if (!multitap::UiKorean()) {
    return L"Multitap · " + std::to_wstring(connections) + (connections == 1 ? L" connection" : L" connections");
  }
  return L"Multitap · 연결 " + std::to_wstring(connections) + L"개";
}

// The windows Explorer creates for its notification area. Clicking the icon
// moves the foreground there, which is how a click on the tray icon of an
// active window is told apart from a click while it was behind other windows.
inline bool IsShellTrayClass(const wchar_t* name) {
  if (!name) return false;
  for (const wchar_t* known : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                               L"NotifyIconOverflowWindow",
                               L"TopLevelWindowForOverflowXamlIsland"})
    if (!wcscmp(name, known)) return true;
  return false;
}

enum class Toggle { kShow, kHide };

// A click on the icon brings the window forward, or puts it away when it is the
// window the user was just working in. [recently_active]: it was the active
// window until the tray took the foreground a moment ago.
inline Toggle DecideToggle(bool visible, bool minimized, bool recently_active) {
  return visible && !minimized && recently_active ? Toggle::kHide : Toggle::kShow;
}

// --- the shell -------------------------------------------------------------

struct ShellApi {
  virtual ~ShellApi() = default;
  virtual BOOL Notify(DWORD message, NOTIFYICONDATAW* data) = 0;
};

struct SystemShell final : ShellApi {
  BOOL Notify(DWORD message, NOTIFYICONDATAW* data) override {
    return Shell_NotifyIconW(message, data);
  }
};

// --- callback decoding -----------------------------------------------------

enum class Command { kNone, kToggle, kOpen, kMenu, kBalloonClick };

struct Decoded {
  Command command = Command::kNone;
  POINT at{};
};

class Input {
 public:
  explicit Input(DWORD double_click_ms = 500) : double_click_ms_(double_click_ms) {}

  // NOTIFYICON_VERSION_4: lParam = event | iconId << 16, wParam = anchor point.
  // Older shells (the version request was refused): lParam = the mouse
  // message, wParam = icon id.
  Decoded Decode(bool version4, WPARAM wparam, LPARAM lparam, ULONGLONG now_ms,
                 POINT cursor) {
    UINT event;
    UINT id;
    POINT anchor = cursor;
    if (version4) {
      event = LOWORD(lparam);
      id = HIWORD(lparam);
      anchor = POINT{GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)};
    } else {
      event = static_cast<UINT>(lparam);
      id = static_cast<UINT>(wparam);
    }
    if (id != kIconId) return {};
    switch (event) {
      case NIN_SELECT:
      case NIN_KEYSELECT:
        if (!version4) return {};
        return Select(now_ms);
      case WM_LBUTTONUP:
        if (version4) return {};  // NIN_SELECT already says it
        return Select(now_ms);
      case WM_LBUTTONDBLCLK:
        // The second click of a double click also ends in a "select": ignore
        // it, or the window would be put away again.
        ignore_select_until_ = now_ms + double_click_ms_;
        return {Command::kOpen, anchor};
      case WM_CONTEXTMENU:
        if (!version4) return {};
        return {Command::kMenu, anchor};
      case WM_RBUTTONUP:
        if (version4) return {};
        return {Command::kMenu, anchor};
      case NIN_BALLOONUSERCLICK:
        return {Command::kBalloonClick, anchor};
      default:
        return {};
    }
  }

 private:
  Decoded Select(ULONGLONG now_ms) const {
    if (now_ms < ignore_select_until_) return {};
    return {Command::kToggle, {}};
  }
  DWORD double_click_ms_;
  ULONGLONG ignore_select_until_ = 0;
};

// --- the icon ---------------------------------------------------------------

class Icon {
 public:
  Icon(ShellApi& shell, HWND window, HICON icon)
      : shell_(shell), window_(window), icon_(icon) {}

  // Puts the icon in the notification area. A registration that a crashed
  // earlier run left behind is cleared first; if Windows refuses the stable
  // identity (it is bound to the program's path, and the path changes with each
  // version), the icon is registered by id instead.
  bool Add(const std::wstring& tip) {
    tip_ = tip;
    added_ = false;
    if (use_guid_) {
      if (TryAdd(true)) return true;
      NOTIFYICONDATAW stale = Base(true);
      shell_.Notify(NIM_DELETE, &stale);
      if (TryAdd(true)) return true;
    }
    if (!TryAdd(false)) return false;
    // The shell works but will not take the stable identity: stay with the id.
    use_guid_ = false;
    return true;
  }
  void SetTip(const std::wstring& tip) {
    if (tip == tip_) return;
    tip_ = tip;
    if (!added_) return;
    NOTIFYICONDATAW data = Base(guid_in_use_);
    data.uFlags |= NIF_TIP | NIF_SHOWTIP;
    Copy(data.szTip, tip_);
    shell_.Notify(NIM_MODIFY, &data);
  }

  void Balloon(const std::wstring& title, const std::wstring& text) {
    if (!added_) return;
    NOTIFYICONDATAW data = Base(guid_in_use_);
    data.uFlags |= NIF_INFO;
    Copy(data.szInfoTitle, title);
    Copy(data.szInfo, text);
    data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_NOSOUND;
    data.hBalloonIcon = icon_;
    shell_.Notify(NIM_MODIFY, &data);
  }

  void Remove() {
    if (!added_) return;
    added_ = false;
    NOTIFYICONDATAW data = Base(guid_in_use_);
    shell_.Notify(NIM_DELETE, &data);
  }

  // Explorer restarted: its notification area is empty again.
  bool Readd() { return Add(tip_); }

  bool added() const { return added_; }
  bool version4() const { return version4_; }
  bool guid_in_use() const { return guid_in_use_; }
  const std::wstring& tip() const { return tip_; }

 private:
  bool TryAdd(bool guid) {
    NOTIFYICONDATAW data = Base(guid);
    data.uFlags |= NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon = icon_;
    Copy(data.szTip, tip_);
    if (!shell_.Notify(NIM_ADD, &data)) return false;
    guid_in_use_ = guid;
    NOTIFYICONDATAW version = Base(guid);
    version.uVersion = NOTIFYICON_VERSION_4;
    version4_ = shell_.Notify(NIM_SETVERSION, &version) != FALSE;
    added_ = true;
    return true;
  }
  NOTIFYICONDATAW Base(bool guid) const {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kIconId;
    if (guid) {
      data.uFlags |= NIF_GUID;
      data.guidItem = kIconGuid;
    }
    return data;
  }
  template <size_t N>
  static void Copy(wchar_t (&target)[N], const std::wstring& text) {
    wcsncpy_s(target, N, text.c_str(), _TRUNCATE);
  }
  ShellApi& shell_;
  HWND window_;
  HICON icon_;
  std::wstring tip_;
  bool added_ = false, version4_ = false;
  bool use_guid_ = true, guid_in_use_ = false;
};

// --- the menu ----------------------------------------------------------------

struct MenuColors {
  COLORREF background, hover, ink, muted, line, danger;
};

inline MenuColors ColorsFor(bool dark) {
  // The app's palette (ui/app_palette.dart): surface, quiet, ink, muted, line, red.
  if (dark)
    return {RGB(20, 28, 33), RGB(34, 40, 45), RGB(233, 241, 243),
            RGB(154, 173, 182), RGB(43, 57, 65), RGB(255, 138, 122)};
  return {RGB(255, 255, 255), RGB(237, 240, 242), RGB(25, 38, 45),
          RGB(89, 108, 118), RGB(220, 227, 231), RGB(184, 64, 75)};
}

// A dark/light popup in the app's colours. Windows draws the frame, shadow and
// (on Windows 11) the rounded corners; the rows are drawn here.
class Menu {
 public:
  struct Item {
    int id = 0;  // 0: separator
    std::wstring label;
    wchar_t glyph = 0;  // Segoe Fluent Icons / MDL2 Assets code point
    bool danger = false;
  };

  explicit Menu(bool dark) : dark_(dark), colors_(ColorsFor(dark)) {}
  ~Menu() {
    if (menu_) DestroyMenu(menu_);
    if (background_) DeleteObject(background_);
  }
  Menu(const Menu&) = delete;
  Menu& operator=(const Menu&) = delete;

  static std::vector<Item> ClientItems() {
    return {{kMenuOpen, multitap::Tr(L"열기", L"Open"), 0xE8A7, false},
            {0, L"", 0, false},
            {kMenuExit, multitap::Tr(L"완전 종료", L"Quit"), 0xE7E8, true}};
  }

  // Shows the menu at [at] (the monitor holding that point) and returns the
  // chosen command id, or 0 when it was dismissed (Esc, click away).
  int Show(HWND owner, POINT at, std::vector<Item> items) {
    items_ = std::move(items);
    dpi_ = DpiAt(at);
    menu_ = CreatePopupMenu();
    if (!menu_) return 0;
    background_ = CreateSolidBrush(colors_.background);
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_BACKGROUND | MIM_STYLE;
    info.hbrBack = background_;
    SetMenuInfo(menu_, &info);
    for (const auto& item : items_) {
      if (item.id == 0)
        AppendMenuW(menu_, MF_OWNERDRAW | MF_SEPARATOR, 0,
                    reinterpret_cast<LPCWSTR>(&item));
      else
        AppendMenuW(menu_, MF_OWNERDRAW | MF_STRING, static_cast<UINT_PTR>(item.id),
                    reinterpret_cast<LPCWSTR>(&item));
    }
    // A menu that does not own the foreground does not close on a click away.
    SetForegroundWindow(owner);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const int chosen = static_cast<int>(
        TrackPopupMenuEx(menu_, flags, at.x, at.y, owner, nullptr));
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu_);
    menu_ = nullptr;
    DeleteObject(background_);
    background_ = nullptr;
    return chosen;
  }

  bool Measure(HWND window, MEASUREITEMSTRUCT* measure) {
    const auto* item = Find(measure->itemData);
    if (!item) return false;
    ++measured_;
    const double scale = Scale(window);
    HDC dc = GetDC(window);
    FontSet fonts(dc, scale);
    SIZE text{};
    if (item->id != 0)
      GetTextExtentPoint32W(dc, item->label.c_str(),
                            static_cast<int>(item->label.size()), &text);
    fonts.Release(dc);
    ReleaseDC(window, dc);
    measure->itemWidth = static_cast<UINT>(
        (std::max)(132 * scale, (12 + 22 + 8 + 28) * scale + text.cx));
    measure->itemHeight = static_cast<UINT>((item->id == 0 ? 9 : 32) * scale);
    return true;
  }

  bool Draw(HWND window, const DRAWITEMSTRUCT* draw) {
    const auto* item = Find(draw->itemData);
    if (!item) return false;
    ++drawn_;
    const double scale = Scale(window);
    const HDC dc = draw->hDC;
    RECT row = draw->rcItem;
    HBRUSH background = CreateSolidBrush(colors_.background);
    FillRect(dc, &row, background);
    DeleteObject(background);
    if (item->id == 0) {
      RECT line{row.left + static_cast<LONG>(8 * scale),
                row.top + (row.bottom - row.top) / 2,
                row.right - static_cast<LONG>(8 * scale),
                row.top + (row.bottom - row.top) / 2 + 1};
      HBRUSH brush = CreateSolidBrush(colors_.line);
      FillRect(dc, &line, brush);
      DeleteObject(brush);
      return true;
    }
    const int inset = static_cast<int>(4 * scale);
    if (draw->itemState & ODS_SELECTED) {
      HBRUSH hover = CreateSolidBrush(colors_.hover);
      HGDIOBJ old_brush = SelectObject(dc, hover);
      HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
      const int radius = static_cast<int>(8 * scale);
      RoundRect(dc, row.left + inset, row.top + 2, row.right - inset,
                row.bottom - 2, radius, radius);
      SelectObject(dc, old_pen);
      SelectObject(dc, old_brush);
      DeleteObject(hover);
    }
    FontSet fonts(dc, scale);
    SetBkMode(dc, TRANSPARENT);
    const COLORREF ink = item->danger ? colors_.danger : colors_.ink;
    if (item->glyph && fonts.has_glyphs) {
      SelectObject(dc, fonts.glyph);
      SetTextColor(dc, item->danger ? colors_.danger : colors_.muted);
      RECT icon{row.left + static_cast<LONG>(14 * scale), row.top,
                row.left + static_cast<LONG>(34 * scale), row.bottom};
      wchar_t glyph[2] = {item->glyph, 0};
      DrawTextW(dc, glyph, 1, &icon, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }
    SelectObject(dc, fonts.text);
    SetTextColor(dc, ink);
    RECT text{row.left + static_cast<LONG>(44 * scale), row.top, row.right - inset, row.bottom};
    DrawTextW(dc, item->label.c_str(), static_cast<int>(item->label.size()), &text,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    fonts.Release(dc);
    return true;
  }

  bool dark() const { return dark_; }
  int measured() const { return measured_; }
  int drawn() const { return drawn_; }

 private:
  int measured_ = 0, drawn_ = 0;
  struct FontSet {
    HFONT text = nullptr, glyph = nullptr, original = nullptr;
    bool has_glyphs = false;
    FontSet(HDC dc, double scale) {
      NONCLIENTMETRICSW metrics{sizeof(metrics)};
      const UINT dpi = static_cast<UINT>(96 * scale + .5);
      if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
        text = CreateFontIndirectW(&metrics.lfMenuFont);
      if (!text) text = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
      for (const wchar_t* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
        HFONT candidate = CreateFontW(-static_cast<int>(14 * scale + .5), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        if (!candidate) continue;
        HGDIOBJ previous = SelectObject(dc, candidate);
        wchar_t actual[LF_FACESIZE]{};
        GetTextFaceW(dc, LF_FACESIZE, actual);
        SelectObject(dc, previous);
        if (!_wcsicmp(actual, face)) {
          glyph = candidate;
          has_glyphs = true;
          break;
        }
        DeleteObject(candidate);
      }
      original = static_cast<HFONT>(SelectObject(dc, text));
    }
    void Release(HDC dc) {
      SelectObject(dc, original);
      if (text && text != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(text);
      if (glyph) DeleteObject(glyph);
      text = glyph = nullptr;
    }
  };
  // The menu opens on the monitor of the click; scale rows to that monitor.
  double Scale(HWND window) const {
    const UINT dpi = dpi_ ? dpi_ : GetDpiForWindow(window);
    return (dpi ? dpi : 96) / 96.0;
  }
  static UINT DpiAt(POINT at) {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const auto get = reinterpret_cast<GetDpiForMonitorFn>(
        GetProcAddress(LoadLibraryW(L"Shcore.dll"), "GetDpiForMonitor"));
    UINT x = 0, y = 0;
    if (get && SUCCEEDED(get(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), 0, &x, &y)))
      return x;
    return 0;
  }
  UINT dpi_ = 0;
  const Item* Find(ULONG_PTR data) const {
    for (const auto& item : items_)
      if (reinterpret_cast<ULONG_PTR>(&item) == data) return &item;
    return nullptr;
  }
  bool dark_;
  MenuColors colors_;
  std::vector<Item> items_;
  HMENU menu_ = nullptr;
  HBRUSH background_ = nullptr;
};

// --- the tray window ------------------------------------------------------

struct Delegate {
  virtual ~Delegate() = default;
  virtual void TrayToggle() = 0;  // left click / Enter
  virtual void TrayOpen() = 0;    // double click, 열기, a clicked balloon
  virtual void TrayExit() = 0;    // 완전 종료
};

// A hidden top-level window of its own: it receives the icon's callbacks and
// the "TaskbarCreated" broadcast (message-only windows get no broadcasts),
// and owns the menu.
class Host {
 public:
  Host(ShellApi& shell, Delegate& delegate, HICON icon, bool dark)
      : shell_(shell), delegate_(delegate), icon_(icon), dark_(dark) {}
  ~Host() { Destroy(); }
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  // Creates the window and adds the icon; true when the icon is in place. The
  // window exists either way, so a later TaskbarCreated can still add it.
  bool Create(const std::wstring& tip) {
    if (window_) return icon_added();
    WNDCLASSW cls{};
    cls.lpfnWndProc = &Host::Proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kHostClass;
    RegisterClassW(&cls);  // already registered: fine
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"Multitap Tray", WS_POPUP,
                              0, 0, 0, 0, nullptr, nullptr, cls.hInstance, this);
    if (!window_) return false;
    // A client started elevated still hears Explorer.
    ChangeWindowMessageFilterEx(window_, taskbar_created_, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(window_, kCallbackMessage, MSGFLT_ALLOW, nullptr);
    icon_state_ = std::make_unique<Icon>(shell_, window_, icon_);
    return icon_state_->Add(tip);
  }

  // Removes the icon (every exit path) and the window.
  void Destroy() {
    if (icon_state_) icon_state_->Remove();
    icon_state_.reset();
    if (window_) {
      HWND window = window_;
      window_ = nullptr;
      DestroyWindow(window);
    }
  }

  void RemoveIcon() {
    if (icon_state_) icon_state_->Remove();
  }
  void SetTooltip(const std::wstring& tip) {
    if (icon_state_) icon_state_->SetTip(tip);
  }
  void Balloon(const std::wstring& title, const std::wstring& text) {
    if (icon_state_) icon_state_->Balloon(title, text);
  }
  void SetDark(bool dark) { dark_ = dark; }
  bool icon_added() const { return icon_state_ && icon_state_->added(); }
  HWND window() const { return window_; }
  const Icon* icon() const { return icon_state_.get(); }
  struct MenuStats { int measured = 0, drawn = 0; };
  MenuStats menu_stats() const { return menu_stats_; }

  // For the smoke test: replaces the real popup menu.
  void SetMenuRunner(std::function<int(HWND, POINT, bool)> runner) {
    menu_runner_ = std::move(runner);
  }

 private:
  static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self) {
      LRESULT result = 0;
      if (self->Handle(window, message, wparam, lparam, result)) return result;
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }

  bool Handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) {
    if (message == taskbar_created_ && taskbar_created_) {
      // Explorer restarted (or the taskbar was recreated): add the icon again.
      if (icon_state_) icon_state_->Readd();
      return true;
    }
    switch (message) {
      case kCallbackMessage: {
        if (!icon_state_) return true;
        POINT cursor{};
        GetCursorPos(&cursor);
        const auto decoded = input_.Decode(icon_state_->version4(), wparam, lparam,
                                           GetTickCount64(), cursor);
        switch (decoded.command) {
          case Command::kToggle: delegate_.TrayToggle(); break;
          case Command::kOpen:
          case Command::kBalloonClick: delegate_.TrayOpen(); break;
          case Command::kMenu: ShowMenu(window, decoded.at); break;
          case Command::kNone: break;
        }
        return true;
      }
      case WM_MEASUREITEM:
        if (active_menu_ && active_menu_->Measure(window, reinterpret_cast<MEASUREITEMSTRUCT*>(lparam))) {
          result = TRUE;
          return true;
        }
        return false;
      case WM_DRAWITEM:
        if (active_menu_ && active_menu_->Draw(window, reinterpret_cast<const DRAWITEMSTRUCT*>(lparam))) {
          result = TRUE;
          return true;
        }
        return false;
      case WM_ENDSESSION:
        // Logoff/shutdown: do not leave the icon behind.
        if (wparam && icon_state_) icon_state_->Remove();
        result = 0;
        return true;
      case WM_QUERYENDSESSION:
        result = TRUE;
        return true;
      default:
        return false;
    }
  }

  void ShowMenu(HWND window, POINT at) {
    if (menu_open_) return;
    menu_open_ = true;
    int chosen = 0;
    if (menu_runner_) {
      chosen = menu_runner_(window, at, dark_);
    } else {
      Menu menu(dark_);
      active_menu_ = &menu;
      chosen = menu.Show(window, at, Menu::ClientItems());
      active_menu_ = nullptr;
      menu_stats_ = {menu.measured(), menu.drawn()};
    }
    menu_open_ = false;
    if (chosen == kMenuOpen) delegate_.TrayOpen();
    else if (chosen == kMenuExit) delegate_.TrayExit();
  }

  ShellApi& shell_;
  Delegate& delegate_;
  HICON icon_;
  bool dark_;
  HWND window_ = nullptr;
  UINT taskbar_created_ = 0;
  std::unique_ptr<Icon> icon_state_;
  Input input_;
  Menu* active_menu_ = nullptr;
  bool menu_open_ = false;
  MenuStats menu_stats_;
  std::function<int(HWND, POINT, bool)> menu_runner_;
};

}  // namespace multitap::tray

#endif  // MULTITAP_CLIENT_TRAY_H_
