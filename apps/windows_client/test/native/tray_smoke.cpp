// The client's notification-area icon (client_tray.h) against a fake shell, on
// a private desktop: the exact add/modify/delete calls, version-4 callback
// decoding, the TaskbarCreated re-add, click / double click / context menu
// mapping, the tooltip, the owner-drawn menu, and the logoff cleanup. Nothing
// here touches the user's real notification area.
#include "client_tray.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace multitap::tray;

void requireAt(bool ok, int line) {
  if (!ok) throw std::runtime_error("Tray QA failed at line " + std::to_string(line));
}
#define require(ok) requireAt((ok), __LINE__)

struct Call {
  DWORD message = 0;
  UINT flags = 0;
  HWND window = nullptr;
  UINT id = 0;
  UINT callback = 0;
  UINT version = 0;
  GUID guid{};
  std::wstring tip, info, title;
  HICON icon = nullptr;
  DWORD info_flags = 0;
  bool guid_flag() const { return (flags & NIF_GUID) != 0; }
};

struct FakeShell final : ShellApi {
  std::vector<Call> calls;
  // Decides whether the shell accepts a call.
  std::function<bool(const Call&)> accept = [](const Call&) { return true; };
  BOOL Notify(DWORD message, NOTIFYICONDATAW* data) override {
    Call call;
    call.message = message;
    call.flags = data->uFlags;
    call.window = data->hWnd;
    call.id = data->uID;
    call.callback = data->uCallbackMessage;
    call.version = data->uVersion;
    call.guid = data->guidItem;
    call.tip = data->szTip;
    call.info = data->szInfo;
    call.title = data->szInfoTitle;
    call.icon = data->hIcon;
    call.info_flags = data->dwInfoFlags;
    calls.push_back(call);
    return accept(call) ? TRUE : FALSE;
  }
  size_t count(DWORD message) const {
    size_t n = 0;
    for (const auto& call : calls) n += call.message == message;
    return n;
  }
};

struct FakeDelegate final : Delegate {
  int toggles = 0, opens = 0, exits = 0;
  void TrayToggle() override { ++toggles; }
  void TrayOpen() override { ++opens; }
  void TrayExit() override { ++exits; }
};

bool SameGuid(const GUID& a, const GUID& b) { return !memcmp(&a, &b, sizeof(GUID)); }

LPARAM V4(UINT event) { return MAKELPARAM(event, kIconId); }
WPARAM At(int x, int y) { return MAKEWPARAM(static_cast<WORD>(x), static_cast<WORD>(y)); }

void TestPure() {
  require(Tooltip(0) == L"Multitap");
  require(Tooltip(-1) == L"Multitap");
  require(Tooltip(1) == L"Multitap · 연결 1개");
  require(Tooltip(12) == L"Multitap · 연결 12개");

  // Toggle: put away only a visible, not minimized window the user was just in.
  require(DecideToggle(true, false, true) == Toggle::kHide);
  require(DecideToggle(true, false, false) == Toggle::kShow);   // behind other windows
  require(DecideToggle(true, true, true) == Toggle::kShow);     // minimized: restore
  require(DecideToggle(false, false, true) == Toggle::kShow);   // in the tray: show
  require(DecideToggle(false, true, false) == Toggle::kShow);

  require(IsShellTrayClass(L"Shell_TrayWnd"));
  require(IsShellTrayClass(L"NotifyIconOverflowWindow"));
  require(IsShellTrayClass(L"TopLevelWindowForOverflowXamlIsland"));
  require(!IsShellTrayClass(L"Chrome_WidgetWin_1"));
  require(!IsShellTrayClass(nullptr));

  POINT cursor{700, 800};
  // Version 4.
  Input v4(500);
  auto d = v4.Decode(true, At(10, 20), V4(NIN_SELECT), 1000, cursor);
  require(d.command == Command::kToggle);
  d = v4.Decode(true, At(10, 20), V4(NIN_KEYSELECT), 5000, cursor);
  require(d.command == Command::kToggle);  // Enter / Space on the icon
  d = v4.Decode(true, At(10, 20), V4(WM_LBUTTONUP), 6000, cursor);
  require(d.command == Command::kNone);    // NIN_SELECT already said it
  d = v4.Decode(true, At(-1920 + 5, 1060), V4(WM_CONTEXTMENU), 7000, cursor);
  require(d.command == Command::kMenu && d.at.x == -1915 && d.at.y == 1060);  // a monitor left of the primary
  d = v4.Decode(true, At(0, 0), V4(NIN_BALLOONUSERCLICK), 8000, cursor);
  require(d.command == Command::kBalloonClick);
  d = v4.Decode(true, At(0, 0), V4(NIN_BALLOONTIMEOUT), 8100, cursor);
  require(d.command == Command::kNone);
  d = v4.Decode(true, At(0, 0), V4(WM_MOUSEMOVE), 8200, cursor);
  require(d.command == Command::kNone);
  d = v4.Decode(true, At(0, 0), MAKELPARAM(NIN_SELECT, kIconId + 1), 8300, cursor);
  require(d.command == Command::kNone);    // not our icon
  // A double click opens, and the "select" that ends it is not a second toggle.
  d = v4.Decode(true, At(1, 1), V4(WM_LBUTTONDBLCLK), 20000, cursor);
  require(d.command == Command::kOpen);
  d = v4.Decode(true, At(1, 1), V4(NIN_SELECT), 20100, cursor);
  require(d.command == Command::kNone);
  d = v4.Decode(true, At(1, 1), V4(NIN_SELECT), 20600, cursor);
  require(d.command == Command::kToggle);  // a later, separate click

  // The shell refused version 4: mouse messages, id in wParam.
  Input legacy(500);
  d = legacy.Decode(false, kIconId, WM_LBUTTONUP, 1000, cursor);
  require(d.command == Command::kToggle);
  d = legacy.Decode(false, kIconId, NIN_SELECT, 1100, cursor);
  require(d.command == Command::kNone);
  d = legacy.Decode(false, kIconId, WM_RBUTTONUP, 1200, cursor);
  require(d.command == Command::kMenu && d.at.x == 700 && d.at.y == 800);  // the cursor
  d = legacy.Decode(false, kIconId, WM_CONTEXTMENU, 1300, cursor);
  require(d.command == Command::kNone);
  d = legacy.Decode(false, kIconId, WM_LBUTTONDBLCLK, 1400, cursor);
  require(d.command == Command::kOpen);
  d = legacy.Decode(false, 2, WM_LBUTTONUP, 3000, cursor);
  require(d.command == Command::kNone);
}

void TestIcon(HWND window) {
  const HICON icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
  // Plain success: add with the stable identity, then ask for version 4.
  {
    FakeShell shell;
    Icon item(shell, window, icon);
    require(item.Add(L"Multitap"));
    require(item.added() && item.version4() && item.guid_in_use());
    require(shell.calls.size() == 2);
    const auto& add = shell.calls[0];
    require(add.message == NIM_ADD && add.window == window && add.id == kIconId);
    require(add.guid_flag() && SameGuid(add.guid, kIconGuid));
    require((add.flags & NIF_MESSAGE) && (add.flags & NIF_ICON) && (add.flags & NIF_TIP) && (add.flags & NIF_SHOWTIP));
    require(add.callback == kCallbackMessage && add.icon == icon && add.tip == L"Multitap");
    const auto& version = shell.calls[1];
    require(version.message == NIM_SETVERSION && version.version == NOTIFYICON_VERSION_4 && version.guid_flag());

    // Tooltip: a change modifies, the same text does not call the shell again.
    item.SetTip(Tooltip(2));
    require(shell.calls.size() == 3 && shell.calls[2].message == NIM_MODIFY);
    require(shell.calls[2].tip == L"Multitap · 연결 2개" && (shell.calls[2].flags & NIF_TIP) && (shell.calls[2].flags & NIF_SHOWTIP));
    item.SetTip(Tooltip(2));
    require(shell.calls.size() == 3);
    item.SetTip(Tooltip(0));
    require(shell.calls.size() == 4 && shell.calls[3].tip == L"Multitap");

    // A balloon is quiet, carries the product icon and the exact text.
    item.Balloon(L"Multitap", L"트레이");
    const auto& balloon = shell.calls.back();
    require(balloon.message == NIM_MODIFY && (balloon.flags & NIF_INFO) && balloon.info == L"트레이" && balloon.title == L"Multitap");
    require((balloon.info_flags & NIIF_NOSOUND) && (balloon.info_flags & NIIF_USER));

    // Removal happens once.
    const auto before = shell.calls.size();
    item.Remove();
    require(shell.calls.size() == before + 1 && shell.calls.back().message == NIM_DELETE && shell.calls.back().guid_flag());
    require(!item.added());
    item.Remove();
    item.SetTip(L"after");
    item.Balloon(L"x", L"y");
    require(shell.calls.size() == before + 1);
  }
  // A registration left by a crashed run: delete it, then add again.
  {
    FakeShell shell;
    int adds = 0;
    shell.accept = [&](const Call& call) { return call.message != NIM_ADD || ++adds > 1; };
    Icon item(shell, window, icon);
    require(item.Add(L"Multitap") && item.guid_in_use());
    require(shell.calls[0].message == NIM_ADD && shell.calls[1].message == NIM_DELETE && shell.calls[2].message == NIM_ADD);
    require(shell.calls[1].guid_flag() && shell.calls[2].guid_flag());
  }
  // Windows refuses the stable identity (the program moved): by id from then on.
  {
    FakeShell shell;
    shell.accept = [](const Call& call) { return call.message != NIM_ADD || !call.guid_flag(); };
    Icon item(shell, window, icon);
    require(item.Add(L"Multitap") && item.added() && !item.guid_in_use());
    require(shell.count(NIM_ADD) == 3);   // guid, guid after the clean-up, id
    require(!shell.calls.back().guid_flag() && shell.calls.back().message == NIM_SETVERSION);
    shell.calls.clear();
    require(item.Readd());               // Explorer restarted
    require(shell.calls.size() == 2 && shell.calls[0].message == NIM_ADD && !shell.calls[0].guid_flag());
    item.Remove();
    require(shell.calls.back().message == NIM_DELETE && !shell.calls.back().guid_flag());
  }
  // The shell is not there (Explorer not running): not added, and a later
  // attempt (TaskbarCreated) succeeds and still uses the stable identity.
  {
    FakeShell shell;
    bool shell_up = false;
    shell.accept = [&](const Call&) { return shell_up; };
    Icon item(shell, window, icon);
    require(!item.Add(L"Multitap") && !item.added());
    item.SetTip(L"nothing to modify");
    item.Balloon(L"x", L"y");
    item.Remove();
    shell_up = true;
    shell.calls.clear();
    require(item.Readd() && item.added() && item.guid_in_use());
    require(shell.calls[0].message == NIM_ADD && shell.calls[0].guid_flag());
  }
  // The shell refuses version 4: the icon is still added; callbacks are legacy.
  {
    FakeShell shell;
    shell.accept = [](const Call& call) { return call.message != NIM_SETVERSION; };
    Icon item(shell, window, icon);
    require(item.Add(L"Multitap") && item.added() && !item.version4());
  }
  // Tooltips are bounded to the shell's buffer.
  {
    FakeShell shell;
    Icon item(shell, window, icon);
    item.Add(std::wstring(500, L'x'));
    require(shell.calls[0].tip.size() == 127);
  }
}

void TestHost() {
  FakeShell shell;
  FakeDelegate delegate;
  const HICON icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
  Host host(shell, delegate, icon, true);
  require(host.Create(L"Multitap") && host.icon_added() && host.window());
  const HWND window = host.window();
  require(shell.calls[0].message == NIM_ADD && shell.calls[0].window == window);
  require(shell.calls[1].message == NIM_SETVERSION);

  // The icon's callbacks, as the shell sends them (version 4).
  SendMessageW(window, kCallbackMessage, At(5, 6), V4(NIN_SELECT));
  require(delegate.toggles == 1 && delegate.opens == 0);
  SendMessageW(window, kCallbackMessage, At(5, 6), V4(NIN_KEYSELECT));
  require(delegate.toggles == 2);
  Sleep(600);   // out of the double-click interval of nothing
  SendMessageW(window, kCallbackMessage, At(5, 6), V4(WM_LBUTTONDBLCLK));
  require(delegate.opens == 1);
  SendMessageW(window, kCallbackMessage, At(5, 6), V4(NIN_SELECT));
  require(delegate.toggles == 2);    // the end of the double click
  SendMessageW(window, kCallbackMessage, At(5, 6), V4(NIN_BALLOONUSERCLICK));
  require(delegate.opens == 2);      // a clicked balloon opens the window

  // The context menu: at the anchor the shell gave, in the app's appearance;
  // the choice reaches the delegate; dismissing it does nothing.
  POINT seen{};
  bool seen_dark = false;
  int answer = kMenuOpen;
  host.SetMenuRunner([&](HWND owner, POINT at, bool dark) {
    require(owner == window);
    seen = at;
    seen_dark = dark;
    return answer;
  });
  SendMessageW(window, kCallbackMessage, At(1200, 1040), V4(WM_CONTEXTMENU));
  require(seen.x == 1200 && seen.y == 1040 && seen_dark && delegate.opens == 3 && delegate.exits == 0);
  host.SetDark(false);
  answer = kMenuExit;
  SendMessageW(window, kCallbackMessage, At(-30, 1040), V4(WM_CONTEXTMENU));
  require(seen.x == -30 && !seen_dark && delegate.exits == 1);
  answer = 0;
  SendMessageW(window, kCallbackMessage, At(10, 10), V4(WM_CONTEXTMENU));
  require(delegate.exits == 1 && delegate.opens == 3);

  // The tooltip follows the connection count.
  host.SetTooltip(Tooltip(3));
  require(shell.calls.back().message == NIM_MODIFY && shell.calls.back().tip == L"Multitap · 연결 3개");

  // Explorer restarts: the shell announces TaskbarCreated to every top-level
  // window. The tray window is a hidden top-level window, so the broadcast
  // reaches it, and the icon (with its current tooltip) is added again.
  const size_t adds_before = shell.count(NIM_ADD);
  DWORD_PTR ignored = 0;
  SendMessageTimeoutW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0,
                      SMTO_ABORTIFHUNG, 3000, &ignored);
  require(shell.count(NIM_ADD) == adds_before + 1);
  require(shell.calls[shell.calls.size() - 2].message == NIM_ADD &&
          shell.calls[shell.calls.size() - 2].tip == L"Multitap · 연결 3개");
  require(host.icon_added());

  // The real owner-drawn menu: it opens, measures and draws its rows, and a
  // dismissal (EndMenu, as Esc or a click away does) returns without a choice.
  host.SetMenuRunner(nullptr);
  SetTimer(window, 77, 400, [](HWND, UINT, UINT_PTR, DWORD) { EndMenu(); });
  SendMessageW(window, kCallbackMessage, At(200, 200), V4(WM_CONTEXTMENU));
  KillTimer(window, 77);
  require(delegate.exits == 1 && delegate.opens == 3);
  const auto stats = host.menu_stats();
  require(stats.measured == 3 && stats.drawn >= 3);   // open, separator, exit
  // And the other appearance.
  host.SetDark(true);
  SetTimer(window, 78, 400, [](HWND, UINT, UINT_PTR, DWORD) { EndMenu(); });
  SendMessageW(window, kCallbackMessage, At(200, 200), V4(WM_CONTEXTMENU));
  KillTimer(window, 78);
  require(host.menu_stats().drawn >= 3);

  // Logoff / shutdown: never blocked, and the icon goes quietly.
  require(SendMessageW(window, WM_QUERYENDSESSION, 0, 0) == TRUE);
  const size_t deletes = shell.count(NIM_DELETE);
  SendMessageW(window, WM_ENDSESSION, FALSE, 0);          // vetoed elsewhere: stays
  require(shell.count(NIM_DELETE) == deletes && host.icon_added());
  SendMessageW(window, WM_ENDSESSION, TRUE, 0);
  require(shell.count(NIM_DELETE) == deletes + 1 && !host.icon_added());

  // Destroy removes the icon in every path and is safe to repeat.
  FakeShell second_shell;
  FakeDelegate second_delegate;
  {
    Host again(second_shell, second_delegate, icon, false);
    require(again.Create(L"Multitap"));
    again.Destroy();
    require(second_shell.count(NIM_DELETE) == 1 && !again.window());
    again.Destroy();
    require(second_shell.count(NIM_DELETE) == 1);
  }
  {
    Host scoped(second_shell, second_delegate, icon, false);
    require(scoped.Create(L"Multitap"));
  }                                                          // the destructor
  require(second_shell.count(NIM_DELETE) == 2);

  // No shell yet: the window still exists, so TaskbarCreated can add the icon.
  FakeShell late_shell;
  bool up = false;
  late_shell.accept = [&](const Call&) { return up; };
  FakeDelegate late_delegate;
  Host late(late_shell, late_delegate, icon, true);
  require(!late.Create(L"Multitap") && late.window() && !late.icon_added());
  up = true;
  SendMessageTimeoutW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0,
                      SMTO_ABORTIFHUNG, 3000, &ignored);
  require(late.icon_added());
}
}  // namespace

int wmain() {
  // A watchdog: a menu that never closes must fail the run, not hang it.
  std::thread([] {
    Sleep(60000);
    std::cerr << "Tray QA timed out\n";
    ExitProcess(3);
  }).detach();
  try {
    TestPure();
    // Everything below creates windows: on a private desktop, so nothing
    // appears on the user's screen and no real window receives the broadcast.
    const std::wstring name = L"Multitap.TrayQa." + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    require(desktop != nullptr);
    require(SetThreadDesktop(desktop) != FALSE);
    {
      WNDCLASSW cls{};
      cls.lpfnWndProc = DefWindowProcW;
      cls.hInstance = GetModuleHandleW(nullptr);
      cls.lpszClassName = L"MULTITAP_TRAY_QA_OWNER";
      RegisterClassW(&cls);
      HWND owner = CreateWindowExW(0, cls.lpszClassName, L"owner", WS_POPUP, 0, 0, 0, 0,
                                   nullptr, nullptr, cls.hInstance, nullptr);
      require(owner != nullptr);
      TestIcon(owner);
      DestroyWindow(owner);
    }
    TestHost();
    std::cout << "{\"passed\":true,\"trayIcon\":true,\"callbackDecoding\":true,\"taskbarCreated\":true,"
                 "\"menu\":true,\"logoffCleanup\":true,\"privateDesktop\":true}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
