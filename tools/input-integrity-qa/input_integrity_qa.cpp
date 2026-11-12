// Independent OS QA. Includes the production RemoteInput unchanged. No network,
// Task Manager, other application, token material, or account name is accessed.
#include "agent.hpp"
#include <iostream>

namespace {
using host::JsonParser;
constexpr ULONG_PTR kMarker = 0x54425141;
struct Shared {
  volatile LONG controllerPid = 0, targetPid = 0, targetReady = 0, mediumDone = 0, highDone = 0, stop = 0;
  alignas(8) volatile LONG64 window = 0;
  volatile LONG down = 0, up = 0, mouseDown = 0, mouseUp = 0;
  volatile LONG rawDown = 0, rawUp = 0, rawMouseDown = 0, rawMouseUp = 0;
  volatile LONG mediumCalls = 0, mediumAccepted = 0, highCalls = 0, highAccepted = 0;
  volatile LONG highIntegrity = 0, targetIntegrity = 0, safetyAbort = 0, cursorRestored = 0;
  POINT originalCursor{};
};
LONG read(volatile LONG& value) { return InterlockedCompareExchange(&value, 0, 0); }
void write(volatile LONG& value, LONG next) { InterlockedExchange(&value, next); }
struct Handle { HANDLE value = nullptr; ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
struct Mapping {
  Handle handle; Shared* state = nullptr;
  Mapping(const std::wstring& id, bool create) {
    require(id.size() == 36 && id.find_first_not_of(L"0123456789abcdefABCDEF-") == std::wstring::npos, "invalid_run_id");
    const auto name = L"Local\\Multitap_InputIntegrity_" + id;
    handle.value = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name.c_str()) : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    require(handle.value != nullptr, "mapping_open_failed");
    if (create) require(GetLastError() != ERROR_ALREADY_EXISTS, "run_already_exists");
    state = static_cast<Shared*>(MapViewOfFile(handle.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    require(state != nullptr, "mapping_view_failed");
  }
  ~Mapping() { if (state) UnmapViewOfFile(state); }
};
std::wstring modulePath(HANDLE process) {
  std::wstring path(32768, L'\0'); DWORD size = static_cast<DWORD>(path.size());
  require(QueryFullProcessImageNameW(process, 0, path.data(), &size) != FALSE, "process_path_unavailable");
  path.resize(size); return path;
}
std::vector<unsigned char> tokenData(HANDLE process, TOKEN_INFORMATION_CLASS type) {
  Handle token; require(OpenProcessToken(process, TOKEN_QUERY, &token.value) != FALSE, "process_token_query_failed");
  DWORD size = 0; GetTokenInformation(token.value, type, nullptr, 0, &size);
  require(size && size < 65536, "token_information_size_invalid");
  std::vector<unsigned char> data(size);
  require(GetTokenInformation(token.value, type, data.data(), size, &size) != FALSE, "token_information_unavailable"); return data;
}
DWORD integrity(HANDLE process) {
  const auto data = tokenData(process, TokenIntegrityLevel);
  const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(data.data());
  const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
  require(count > 0, "integrity_information_invalid"); return *GetSidSubAuthority(label->Label.Sid, count - 1);
}
bool sameOwnProcess(DWORD pid) {
  Handle process; process.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process.value) return false;
  try {
    if (_wcsicmp(modulePath(process.value).c_str(), modulePath(GetCurrentProcess()).c_str()) != 0) return false;
    const auto own = tokenData(GetCurrentProcess(), TokenUser), other = tokenData(process.value, TokenUser);
    if (!EqualSid(reinterpret_cast<const TOKEN_USER*>(own.data())->User.Sid, reinterpret_cast<const TOKEN_USER*>(other.data())->User.Sid)) return false;
    DWORD ownSession = 0, otherSession = 0;
    return ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) && ProcessIdToSessionId(pid, &otherSession) && ownSession == otherSession;
  } catch (...) { return false; }
}
std::string guardReason(Shared& state);
bool modifiersIdle() {
  for (const int key : {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU, VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN}) if (GetAsyncKeyState(key) & 0x8000) return false;
  return true;
}
bool mouseIdle() { for (const int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2}) if (GetAsyncKeyState(key) & 0x8000) return false; return true; }
HWND ownTarget(Shared& state) {
  const HWND window = reinterpret_cast<HWND>(InterlockedCompareExchange64(&state.window, 0, 0));
  DWORD pid = 0; if (!window || !IsWindow(window)) return nullptr;
  GetWindowThreadProcessId(window, &pid);
  return pid && pid == DWORD(read(state.targetPid)) && sameOwnProcess(pid) ? window : nullptr;
}
bool guarded(Shared& state, bool requireCursor = false) {
  const HWND window = ownTarget(state);
  if (!window || GetForegroundWindow() != window || !modifiersIdle() || read(state.stop)) return false;
  if (requireCursor) { POINT point{}; if (!GetCursorPos(&point) || GetAncestor(WindowFromPoint(point), GA_ROOT) != window) return false; }
  return true;
}
std::string guardReason(Shared& state) {
  const HWND window = ownTarget(state);
  if (!window) return "own_target_path_pid_owner_or_session_not_verified";
  if (!IsWindowVisible(window)) return "own_target_window_hidden";
  if (GetForegroundWindow() != window) return "own_target_not_foreground";
  if (!modifiersIdle()) return "existing_modifier_is_held";
  if (read(state.stop)) return "qa_session_stopped";
  POINT point{};
  if (!GetCursorPos(&point) || GetAncestor(WindowFromPoint(point), GA_ROOT) != window) return "cursor_not_on_own_target";
  return {};
}
RECT clientScreen(HWND window) {
  RECT rect{}; require(GetClientRect(window, &rect) != FALSE, "own_client_bounds_failed");
  POINT first{rect.left, rect.top}, last{rect.right, rect.bottom};
  ClientToScreen(window, &first); ClientToScreen(window, &last); return {first.x, first.y, last.x, last.y};
}
void pump(unsigned milliseconds) {
  const auto end = GetTickCount64() + milliseconds;
  do { MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } Sleep(2); } while (GetTickCount64() < end);
}
bool waitUntil(const std::function<bool()>& condition, unsigned timeout) {
  const auto end = GetTickCount64() + timeout;
  while (!condition()) { if (GetTickCount64() >= end) return false; pump(10); } return true;
}
void file(const std::filesystem::path& folder, const wchar_t* name, const std::string& text) { std::ofstream stream(folder / name, std::ios::binary); stream << text << '\n'; require(bool(stream), "qa_result_write_failed"); }
void inject(Shared& state, bool high) {
  require(sameOwnProcess(DWORD(read(state.controllerPid))) && ownTarget(state), "target_process_path_pid_or_owner_mismatch");
  if (!waitUntil([&] { return guardReason(state).empty(); }, 60000)) throw std::runtime_error(guardReason(state));
  require(mouseIdle(), "mouse_button_already_held");
  const auto level = integrity(GetCurrentProcess());
  require(high ? level >= SECURITY_MANDATORY_HIGH_RID && level < SECURITY_MANDATORY_SYSTEM_RID : level >= SECURITY_MANDATORY_MEDIUM_RID && level < SECURITY_MANDATORY_HIGH_RID, "unexpected_injector_integrity");
  if (high) write(state.highIntegrity, 1);
  host::RemoteInput input([&](INPUT& event) -> UINT {
    const bool button = event.type == INPUT_MOUSE && !(event.mi.dwFlags & MOUSEEVENTF_MOVE);
    if (!guarded(state, button)) { write(state.safetyAbort, 1); return 0; }
    if (event.type == INPUT_KEYBOARD) event.ki.dwExtraInfo = kMarker; else event.mi.dwExtraInfo = kMarker;
    InterlockedIncrement(high ? &state.highCalls : &state.mediumCalls);
    const UINT accepted = SendInput(1, &event, sizeof(event));
    if (accepted) InterlockedIncrement(high ? &state.highAccepted : &state.mediumAccepted);
    return accepted;
  });
  input.area(clientScreen(ownTarget(state)));
  input.apply(JsonParser::parse("{\"event\":\"key\",\"vk\":119,\"scan\":66,\"down\":true}"));
  input.apply(JsonParser::parse("{\"event\":\"key\",\"vk\":119,\"scan\":66,\"down\":false}"));
  input.apply(JsonParser::parse("{\"event\":\"button\",\"button\":\"left\",\"down\":true,\"x\":0.5,\"y\":0.5}"));
  input.apply(JsonParser::parse("{\"event\":\"button\",\"button\":\"left\",\"down\":false,\"x\":0.5,\"y\":0.5}"));
  if (input.pending() && guarded(state, true)) input.reset();
  require(input.pending() == 0 && !read(state.safetyAbort), "input_stopped_on_focus_or_safety_change");
  pump(200);
}
Shared* targetState = nullptr;
ULONGLONG targetDeadline = 0;
bool cursorPositioned = false;
void restoreCursor(HWND window, Shared& state) {
  if (GetForegroundWindow() != window || !modifiersIdle() || !mouseIdle()) return;
  POINT point{}; GetCursorPos(&point); const RECT bounds = clientScreen(window);
  if (PtInRect(&bounds, point) && SetCursorPos(state.originalCursor.x, state.originalCursor.y)) write(state.cursorRestored, 1);
}
LRESULT CALLBACK targetProcedure(HWND window, UINT message, WPARAM key, LPARAM parameter) {
  auto& state = *targetState;
  if (message == WM_KEYDOWN && key == VK_F8) InterlockedIncrement(&state.rawDown);
  if (message == WM_KEYUP && key == VK_F8) InterlockedIncrement(&state.rawUp);
  if (message == WM_LBUTTONDOWN) InterlockedIncrement(&state.rawMouseDown);
  if (message == WM_LBUTTONUP) InterlockedIncrement(&state.rawMouseUp);
  if (ULONG_PTR(GetMessageExtraInfo()) == kMarker) {
    if (message == WM_KEYDOWN && key == VK_F8) InterlockedIncrement(&state.down);
    if (message == WM_KEYUP && key == VK_F8) InterlockedIncrement(&state.up);
    if (message == WM_LBUTTONDOWN) InterlockedIncrement(&state.mouseDown);
    if (message == WM_LBUTTONUP) InterlockedIncrement(&state.mouseUp);
  }
  if (message == WM_TIMER) {
    if (!cursorPositioned && GetForegroundWindow() == window && modifiersIdle() && mouseIdle()) {
      const auto bounds = clientScreen(window);
      cursorPositioned = SetCursorPos((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2) != FALSE;
    }
    if (read(state.stop) || GetTickCount64() >= targetDeadline || !sameOwnProcess(DWORD(read(state.controllerPid)))) { restoreCursor(window, state); DestroyWindow(window); return 0; }
  }
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); RECT bounds{}; GetClientRect(window, &bounds);
    DrawTextW(dc, L"Multitap isolated input integrity QA\nOwn target only: F8 down/up and center mouse click\nNo credentials, Task Manager, other apps or remote VM", -1, &bounds, DT_CENTER | DT_WORDBREAK); EndPaint(window, &paint); return 0;
  }
  if (message == WM_DESTROY) {
    // Restore only if our target still owns the foreground and the cursor;
    // user intervention is never overwritten.
    if (!read(state.cursorRestored)) restoreCursor(window, state);
    write(state.targetReady, 0); PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(window, message, key, parameter);
}
int target(const std::wstring& id) {
  Mapping mapping(id, false); auto& state = *mapping.state; targetState = &state;
  require(sameOwnProcess(DWORD(read(state.controllerPid))), "controller_identity_mismatch");
  const auto level = integrity(GetCurrentProcess());
  require(level >= SECURITY_MANDATORY_HIGH_RID && level < SECURITY_MANDATORY_SYSTEM_RID, "target_not_high_integrity");
  write(state.targetIntegrity, 1); write(state.targetPid, LONG(GetCurrentProcessId()));
  WNDCLASSW type{}; type.lpfnWndProc = targetProcedure; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"Multitap_InputIntegrity_OwnTarget"; type.hbrBackground = HBRUSH(COLOR_WINDOW + 1); type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  require(RegisterClassW(&type) != 0, "own_target_class_failed");
  const HWND window = CreateWindowExW(0, type.lpszClassName, L"Multitap input integrity QA - own elevated target", WS_OVERLAPPEDWINDOW, 240, 180, 700, 340, nullptr, nullptr, type.hInstance, nullptr);
  require(window != nullptr, "own_target_create_failed");
  InterlockedExchange64(&state.window, LONG64(window));
  // The launcher must stay hidden, but this explicitly interactive QA target
  // must be visible. Consume STARTUPINFO's SW_HIDE before the explicit show.
  ShowWindow(window, SW_HIDE); ShowWindow(window, SW_SHOW);
  require(IsWindowVisible(window) != FALSE, "own_target_window_hidden");
  SetForegroundWindow(window); SetFocus(window); pump(100);
  if (GetForegroundWindow() == window && modifiersIdle() && mouseIdle()) { const auto bounds = clientScreen(window); SetCursorPos((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2); }
  targetDeadline = GetTickCount64() + 100000; SetTimer(window, 1, 100, nullptr); write(state.targetReady, 1);
  MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
  return 0;
}
int controller(const std::wstring& id, const std::filesystem::path& folder) {
  Mapping mapping(id, true); auto& state = *mapping.state;
  write(state.controllerPid, LONG(GetCurrentProcessId())); GetCursorPos(&state.originalCursor);
  const auto level = integrity(GetCurrentProcess());
  require(level >= SECURITY_MANDATORY_MEDIUM_RID && level < SECURITY_MANDATORY_HIGH_RID, "controller_not_medium_integrity");
  file(folder, L"controller.ready", "ready");
  bool passed = false;
  std::string reason;
  std::array<LONG, 4> mediumObserved{};
  std::array<LONG, 4> mediumRawObserved{};
  try {
    require(waitUntil([&] { return read(state.targetReady) != 0; }, 20000), "elevated_target_not_ready");
    inject(state, false);
    mediumObserved = {read(state.down), read(state.up), read(state.mouseDown), read(state.mouseUp)};
    mediumRawObserved = {read(state.rawDown), read(state.rawUp), read(state.rawMouseDown), read(state.rawMouseUp)};
    // SendInput's return alone is not delivery evidence. Require no actual
    // target messages, including an unfiltered counter as a marker control.
    require(read(state.mediumCalls) == 6 && read(state.down) == 0 && read(state.up) == 0 && read(state.mouseDown) == 0 && read(state.mouseUp) == 0 && read(state.rawDown) == 0 && read(state.rawUp) == 0 && read(state.rawMouseDown) == 0 && read(state.rawMouseUp) == 0, "medium_to_high_messages_were_not_blocked");
    write(state.mediumDone, 1); file(folder, L"medium.ready", "blocked");
    require(waitUntil([&] { return read(state.highDone) != 0; }, 20000), "high_injector_not_finished");
    require(read(state.highIntegrity) && read(state.targetIntegrity) && read(state.highCalls) == 6 && read(state.highAccepted) == 6 && read(state.down) == 1 && read(state.up) == 1 && read(state.mouseDown) == 1 && read(state.mouseUp) == 1 && !read(state.safetyAbort), "high_to_high_messages_not_observed");
    passed = true;
  } catch (const std::exception& error) { reason = error.what(); }
  write(state.stop, 1); waitUntil([&] { return read(state.targetReady) == 0; }, 3000);
  std::ostringstream json;
  json << "{\"passed\":" << (passed ? "true" : "false") << ",\"reason\":" << host::jsonQuote(reason)
      << ",\"mediumIntegrity\":\"medium\",\"targetIntegrity\":\"high\",\"highInjectorVerified\":" << (read(state.highIntegrity) ? "true" : "false")
      << ",\"mediumSendInputCalls\":" << read(state.mediumCalls) << ",\"mediumAccepted\":" << read(state.mediumAccepted)
      << ",\"mediumTargetF8Down\":" << mediumObserved[0] << ",\"mediumTargetF8Up\":" << mediumObserved[1]
      << ",\"mediumTargetMouseDown\":" << mediumObserved[2] << ",\"mediumTargetMouseUp\":" << mediumObserved[3]
      << ",\"mediumRawTargetF8Down\":" << mediumRawObserved[0] << ",\"mediumRawTargetF8Up\":" << mediumRawObserved[1]
      << ",\"mediumRawTargetMouseDown\":" << mediumRawObserved[2] << ",\"mediumRawTargetMouseUp\":" << mediumRawObserved[3]
      << ",\"highSendInputCalls\":" << read(state.highCalls) << ",\"highAccepted\":" << read(state.highAccepted)
      << ",\"targetF8Down\":" << read(state.down) << ",\"targetF8Up\":" << read(state.up)
      << ",\"targetMouseDown\":" << read(state.mouseDown) << ",\"targetMouseUp\":" << read(state.mouseUp)
      << ",\"rawTargetF8Down\":" << read(state.rawDown) << ",\"rawTargetF8Up\":" << read(state.rawUp)
      << ",\"rawTargetMouseDown\":" << read(state.rawMouseDown) << ",\"rawTargetMouseUp\":" << read(state.rawMouseUp)
      << ",\"cursorRestored\":" << (read(state.cursorRestored) ? "true" : "false")
      << ",\"productionRemoteInputIncluded\":true,\"sameExePidOwnerSessionVerified\":true,\"physicalKeys\":false,\"otherApplicationInput\":false,\"remoteVmConnections\":0}";
  file(folder, L"result.json", json.str()); return passed ? 0 : 2;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  try {
    require(argc >= 3, "invalid_arguments");
    const std::wstring mode = argv[1], id = argv[2];
    if (mode == L"--controller" && argc == 4) return controller(id, argv[3]);
    if (mode == L"--target" && argc == 3) return target(id);
    if (mode == L"--high" && argc == 3) {
      Mapping mapping(id, false); auto& state = *mapping.state;
      try { require(read(state.mediumDone) != 0, "medium_phase_not_completed"); inject(state, true); write(state.highDone, 1); return 0; }
      catch (...) { write(state.safetyAbort, 1); write(state.highDone, 1); throw; }
    }
    throw std::runtime_error("invalid_mode");
  } catch (const std::exception& error) { std::cerr << "Own-window QA stopped: " << error.what() << '\n'; return 1; }
}
