// Actual OS hook/focus checks plus explicitly simulated physical-key dispatch.
// Never inject global shortcuts: SendInput is limited to F24 in this QA window.
#include "remote_input.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace multitap {
struct RemoteInputTestPeer {
  static bool Installed(const RemoteInput& input) { return input.hook_ != nullptr; }
  static bool Dispatch(RemoteInput& input, UINT vk, UINT scan, bool down,
                       DWORD flags = 0, bool foreground = true, ULONG_PTR extra = 0) {
    KBDLLHOOKSTRUCT key{};
    key.vkCode = vk; key.scanCode = scan; key.flags = flags;
    key.dwExtraInfo = extra;
    return input.HandleKey(down ? WM_KEYDOWN : WM_KEYUP, key, foreground);
  }
};
}
namespace {
using V = flutter::EncodableValue;
using multitap::RemoteInput;
using Peer = multitap::RemoteInputTestPeer;
RemoteInput* current = nullptr;
HWND peer_window = nullptr;
int received_f24 = 0, observed_injected = 0, deactivations = 0;
bool peer_mode = false;
void Check(bool okay, const char* message) {
  if (!okay) throw std::runtime_error(message);
}
LRESULT CALLBACK Observe(int code, WPARAM message, LPARAM data) {
  if (code >= 0) {
    auto* key = reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
    if (key->vkCode == VK_F24 && (key->flags & LLKHF_INJECTED)) ++observed_injected;
  }
  return CallNextHookEx(nullptr, code, message, data);
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_KEYDOWN || message == WM_KEYUP) {
    if (wparam == VK_F24) ++received_f24;
  }
  if (message == WM_ACTIVATEAPP && !wparam && current) {
    ++deactivations;
    current->Capture(nullptr, false);  // Same dispatch as FlutterWindow.
  }
  if (message == WM_APP + 1) { peer_window = reinterpret_cast<HWND>(wparam); return 0; }
  if (message == WM_APP + 2 && peer_mode) { SetForegroundWindow(window); return 0; }
  if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
  if (message == WM_DESTROY) { if (peer_mode) PostQuitMessage(0); return 0; }
  return DefWindowProcW(window, message, wparam, lparam);
}
void Pump() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message); DispatchMessageW(&message);
  }
}
template <class Predicate> bool Wait(Predicate predicate, int milliseconds = 2500) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  do { Pump(); if (predicate()) return true; Sleep(5); } while (std::chrono::steady_clock::now() < until);
  return false;
}
HWND CreateQaWindow() {
  WNDCLASSW klass{}; klass.lpfnWndProc = WindowProc;
  klass.hInstance = GetModuleHandleW(nullptr); klass.lpszClassName = L"MultitapRemoteInputQA";
  klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassW(&klass);
  return CreateWindowW(klass.lpszClassName,
    peer_mode ? L"Multitap input QA - focus peer" : L"Multitap input QA - isolated keyboard check",
    WS_OVERLAPPEDWINDOW, 100, 100, 620, 180, nullptr, nullptr, klass.hInstance, nullptr);
}
std::string Type(const flutter::EncodableMap& event) {
  return std::get<std::string>(event.at(V("type")));
}
bool FieldBool(const flutter::EncodableMap& event, const char* field) {
  return std::get<bool>(event.at(V(field)));
}
int FieldInt(const flutter::EncodableMap& event, const char* field) {
  return std::get<int32_t>(event.at(V(field)));
}
}
int wmain(int argc, wchar_t** argv) {
  peer_mode = argc == 3 && std::wstring(argv[1]) == L"--peer";
  HWND window = CreateQaWindow();
  if (!window) return 2;
  if (peer_mode) {
    ShowWindow(window, SW_SHOWNOACTIVATE);
    const HWND parent = reinterpret_cast<HWND>(std::stoull(argv[2]));
    PostMessageW(parent, WM_APP + 1, reinterpret_cast<WPARAM>(window), 0);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message); DispatchMessageW(&message);
    }
    return 0;
  }
  PROCESS_INFORMATION child{};
  HHOOK observer = nullptr;
  try {
    ShowWindow(window, SW_SHOW); SetForegroundWindow(window);
    if (!Wait([&] { return GetForegroundWindow() == window; })) {
      DWORD foreground_pid = 0;
      GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
      std::cerr << "Foreground guard ownPid=" << GetCurrentProcessId()
                << " foregroundPid=" << foreground_pid << " hwnd=" << GetForegroundWindow() << '\n';
      Check(false, "QA window could not obtain foreground; no input sent");
    }
    std::vector<flutter::EncodableMap> events;
    RemoteInput input([&](auto event) { events.push_back(std::move(event)); });
    current = &input;
    auto capture = [&] {
      Check(GetForegroundWindow() == window, "Refusing capture outside QA foreground");
      Check(input.Capture(window, true) && Peer::Installed(input), "WH_KEYBOARD_LL registration failed");
    };
    auto key = [&](UINT vk, UINT scan, bool down, DWORD flags = 0) {
      Check(Peer::Dispatch(input, vk, scan, down, flags), "Simulated ordinary key was not consumed");
    };
    capture();
    observer = SetWindowsHookExW(WH_KEYBOARD_LL, Observe, GetModuleHandleW(nullptr), 0);
    Check(observer != nullptr, "QA observation hook failed");
    Check(GetForegroundWindow() == window, "Refusing SendInput outside QA foreground");
    INPUT harmless[2]{};
    for (auto& item : harmless) { item.type = INPUT_KEYBOARD; item.ki.wVk = VK_F24; item.ki.dwExtraInfo = 0x54425141; }
    harmless[1].ki.dwFlags = KEYEVENTF_KEYUP;
    Check(SendInput(2, harmless, sizeof(INPUT)) == 2, "QA F24 SendInput failed");
    Check(Wait([&] { return received_f24 == 2 && observed_injected == 2; }), "Injected F24 did not reach own QA window");
    Check(events.empty(), "Injected F24 incorrectly re-entered remote stream");
    UnhookWindowsHookEx(observer); observer = nullptr;

    // The following vectors call production dispatch, not an OS physical keyboard.
    for (auto vector : std::vector<std::pair<UINT, UINT>>{
      {VK_HANGUL, 0xF2}, {VK_HANJA, 0xF1}, {VK_HANGUL, 0x38},
      {VK_RMENU, 0x38}, {VK_LWIN, 0x5B}, {VK_RWIN, 0x5C}}) {
      key(vector.first, vector.second, true, LLKHF_EXTENDED);
      Check(FieldInt(events.back(), "vk") == static_cast<int>(vector.first) &&
            FieldInt(events.back(), "scan") == static_cast<int>(vector.second) &&
            FieldBool(events.back(), "extended") && FieldBool(events.back(), "down"), "VK/scan/extended changed");
      key(vector.first, vector.second, false, LLKHF_EXTENDED);
      Check(!FieldBool(events.back(), "down"), "Key release lost");
    }
    key(VK_LMENU, 0x38, true); key(VK_TAB, 0x0F, true);
    key(VK_TAB, 0x0F, false); key(VK_LMENU, 0x38, false);
    key(VK_LCONTROL, 0x1D, true); key(VK_RSHIFT, 0x36, true);
    key(VK_ESCAPE, 1, true); key(VK_ESCAPE, 1, false);
    key(VK_RSHIFT, 0x36, false); key(VK_LCONTROL, 0x1D, false);
    const size_t before_repeat = events.size();
    key('A', 0x1E, true); key('A', 0x1E, true); key('A', 0x1E, true); key('A', 0x1E, false);
    Check(events.size() == before_repeat + 4, "Auto-repeat suppressed");
    key(VK_LCONTROL, 0x1D, true); key(VK_RCONTROL, 0x1D, true, LLKHF_EXTENDED);
    key(VK_LCONTROL, 0x1D, false);  // Right Ctrl must remain held.
    key(VK_RMENU, 0x38, true, LLKHF_EXTENDED); key(VK_LSHIFT, 0x2A, true);
    Check(Peer::Dispatch(input, VK_F12, 0x58, true), "Release shortcut not consumed");
    Check(!Peer::Installed(input) && Type(events[events.size()-2]) == "input.reset" &&
          Type(events.back()) == "capture.released", "Tracked right-modifier release did not reset/unhook");
    const size_t released = events.size();
    input.Capture(nullptr, false);
    Check(events.size() == released, "Release not idempotent");
    capture();
    key(VK_F12, 0x58, true); key(VK_F12, 0x58, false);
    Check(Peer::Installed(input), "Modifier state leaked across capture");
    key(VK_LCONTROL, 0x1D, true); key(VK_LMENU, 0x38, true);
    Check(!Peer::Dispatch(input, VK_DELETE, 0x53, true, LLKHF_EXTENDED), "Secure attention Delete was consumed");
    Check(!Peer::Installed(input) && Type(events.back()) == "capture.released", "Secure attention did not release capture");
    capture();
    // A lone Ctrl+Alt is ordinary input (there is no mouse-confine release
    // chord), so it reaches the remote and keeps capture.
    key(VK_LCONTROL, 0x1D, true); key(VK_LMENU, 0x38, true);
    key(VK_LMENU, 0x38, false); key(VK_LCONTROL, 0x1D, false);
    Check(Peer::Installed(input) && !FieldBool(events.back(), "down"), "A lone Ctrl+Alt released input");
    Check(!Peer::Dispatch(input, 'A', 0x1E, true, LLKHF_INJECTED, true, 0x54425141), "Marked QA simulation was consumed");

    // A separate QA process creates an actual WM_ACTIVATEAPP(false) transition.
    wchar_t executable[MAX_PATH]{}; GetModuleFileNameW(nullptr, executable, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --peer " +
        std::to_wstring(reinterpret_cast<uintptr_t>(window));
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    Check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child), "QA peer launch failed");
    Check(Wait([] { return peer_window != nullptr; }), "QA peer did not create its window");
    AllowSetForegroundWindow(child.dwProcessId);
    const size_t before_focus = events.size();
    PostMessageW(peer_window, WM_APP + 2, 0, 0);
    Check(Wait([&] { return GetForegroundWindow() == peer_window && !Peer::Installed(input); }), "Actual own-process focus loss did not release hook");
    Check(deactivations > 0 && events.size() == before_focus + 2 &&
          Type(events[before_focus]) == "input.reset" && Type(events.back()) == "capture.released", "Focus loss did not emit reset/release");
    Check(!input.Capture(window, true), "Background QA window acquired keyboard hook");
    PostMessageW(peer_window, WM_CLOSE, 0, 0);
    Check(Wait([&] { return WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0; }), "QA peer did not exit");
    SetForegroundWindow(window);
    Check(Wait([&] { return GetForegroundWindow() == window; }), "QA window could not regain foreground");
    capture(); input.Capture(nullptr, false);
    current = nullptr;
    std::cout << "PASS actualHookRegistration=1 injectedF24PassThrough=2 actualOwnProcessFocusRelease=1 "
                 "idempotentUnhook=1 simulatedKoreanVkScan=3 simulatedWinAltTabCtrlShiftEsc=1 "
                 "simulatedRepeat=4 simulatedTrackedModifierRelease=1 simulatedLoneCtrlAltKept=1"
                 "physicalKeysTested=0\n";
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    DestroyWindow(window); return 0;
  } catch (const std::exception& failure) {
    current = nullptr;
    if (observer) UnhookWindowsHookEx(observer);
    if (peer_window) PostMessageW(peer_window, WM_CLOSE, 0, 0);
    if (child.hThread) CloseHandle(child.hThread);
    if (child.hProcess) CloseHandle(child.hProcess);
    DestroyWindow(window);
    std::cerr << "FAIL " << failure.what() << '\n'; return 1;
  }
}
