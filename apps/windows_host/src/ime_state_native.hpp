#pragma once
// ime-state-v1, Windows side (ime_state.hpp has the rules and the protocol).
//  * readForeground: in the helper, the window that has the keyboard (the
//    foreground thread's focus window, so a UWP app's own window rather than
//    its frame) and its IME: WM_IME_CONTROL IMC_GETOPENSTATUS and
//    IMC_GETCONVERSIONMODE to its default IME window, 100 ms each, aborted if
//    the window hangs. A window of a higher integrity level (an elevated app)
//    refuses these messages (UIPI), so it is unknown, never "closed".
//  * helperEntry: `MultitapHost.exe --ime-state`.
//  * startChannel: the service starts it with the session user's own token.
#include "ime_state.hpp"
#include "media.hpp"
#include "drive_winfsp_service.hpp"
#pragma comment(lib, "imm32.lib")

namespace host::ime {
// WM_IME_CONTROL requests (the IMM documentation's values; the SDK keeps them in immdev.h).
inline constexpr WPARAM imcGetConversionMode = 0x0001, imcGetOpenStatus = 0x0005;
inline DWORD integrityLevel(HANDLE token) {
  DWORD size = 0; GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
  if (!size) return 0xffffffff;
  std::vector<BYTE> data(size);
  if (!GetTokenInformation(token, TokenIntegrityLevel, data.data(), size, &size)) return 0xffffffff;
  const auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(data.data());
  const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
  return count ? *GetSidSubAuthority(label->Label.Sid, count - 1) : 0xffffffff;
}
// Whether the helper may send to windows of `process` (not above its own level).
inline bool reachableProcess(DWORD process) {
  HANDLE own = nullptr; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own)) return false;
  WinHandle ownToken(own); const DWORD mine = integrityLevel(own);
  WinHandle target(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process));
  if (!target.value) return false;
  HANDLE raw = nullptr; if (!OpenProcessToken(target.value, TOKEN_QUERY, &raw)) return false;
  WinHandle token(raw); const DWORD theirs = integrityLevel(raw);
  return mine != 0xffffffff && theirs != 0xffffffff && theirs <= mine;
}
inline Reading readForeground() {
  Reading r;
  if (!interactiveDesktopAvailable()) return r;
  GUITHREADINFO gui{sizeof(gui)};
  HWND window = nullptr;
  if (GetGUIThreadInfo(0, &gui)) window = gui.hwndFocus ? gui.hwndFocus : gui.hwndActive;
  if (!window) window = GetForegroundWindow();
  if (!window) return r;
  DWORD process = 0; const DWORD thread = GetWindowThreadProcessId(window, &process);
  if (!thread || !process) return r;
  r.window = true;
  r.reachable = reachableProcess(process);
  if (!r.reachable) return r;
  r.language = LOWORD(reinterpret_cast<UINT_PTR>(GetKeyboardLayout(thread)));
  const HWND imeWindow = ImmGetDefaultIMEWnd(window);
  if (!imeWindow) return r;
  DWORD_PTR value = 0;
  if (SendMessageTimeoutW(imeWindow, WM_IME_CONTROL, imcGetOpenStatus, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &value)) { r.openKnown = true; r.open = value != 0; }
  value = 0;
  if (SendMessageTimeoutW(imeWindow, WM_IME_CONTROL, imcGetConversionMode, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &value)) { r.conversionKnown = true; r.conversion = DWORD(value); }
  return r;
}
// `--ime-state`: for each '?' on stdin one sample (k/e/u and a newline); ends
// when stdin closes (the service ended it) or stdout breaks.
inline int helperEntry() {
  const HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!in || in == INVALID_HANDLE_VALUE || !out || out == INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
  for (;;) {
    char letter = 0; DWORD count = 0;
    if (!ReadFile(in, &letter, 1, &count, nullptr) || !count) return 0;
    if (letter != '?') continue;
    Mode mode = Mode::unknown;
    try { mode = decide(readForeground()); } catch (...) { mode = Mode::unknown; }
    const char answer[2] = {modeLetter(mode), '\n'};
    DWORD written = 0;
    if (!WriteFile(out, answer, 2, &written, nullptr) || written != 2) return 0;
  }
}
class ProcessChannel final : public Channel {
 public:
  explicit ProcessChannel(std::unique_ptr<drive::fs::UserProcess> process) : process_(std::move(process)) {}
  ~ProcessChannel() override {
    try { process_->closeInput(); if (!process_->wait(500)) process_->terminate(); } catch (...) {}
  }
  std::optional<Mode> ask(uint32_t timeoutMs) override {
    if (!process_->write({'?', '\n'})) return std::nullopt;
    const auto deadline = GetTickCount64() + timeoutMs;
    char answer[2]{}; DWORD got = 0;
    while (got < 2) {
      DWORD available = 0;
      if (!PeekNamedPipe(process_->fromChild.value, nullptr, 0, nullptr, &available, nullptr)) return std::nullopt;
      if (available) {
        DWORD count = 0;
        if (!ReadFile(process_->fromChild.value, answer + got, 2 - got, &count, nullptr) || !count) return std::nullopt;
        got += count; continue;
      }
      if (WaitForSingleObject(process_->process.value, 0) == WAIT_OBJECT_0 || GetTickCount64() >= deadline) return std::nullopt;
      Sleep(5);
    }
    if (answer[1] != '\n') return std::nullopt;
    return modeFromLetter(answer[0]);
  }
 private:
  std::unique_ptr<drive::fs::UserProcess> process_;
};
inline std::unique_ptr<Channel> startChannel(DWORD session) {
  return std::make_unique<ProcessChannel>(drive::fs::startUserProcess(session, L"--ime-state", service_workers::Role::imeState));
}
inline WatchAccess nativeWatchAccess(std::function<std::optional<DWORD>()> session) {
  WatchAccess access; access.session = std::move(session); access.start = startChannel; return access;
}
}  // namespace host::ime
