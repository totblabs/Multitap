#include "agent.hpp"
#include <imm.h>
#include <iostream>

using namespace host;
namespace {
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
Json key(WORD vk, bool down, WORD scan = 0, bool extended = false) {
  return JsonParser::parse("{\"event\":\"key\",\"vk\":" + std::to_string(vk) + ",\"scan\":" + std::to_string(scan) + ",\"down\":" + (down ? "true" : "false") + ",\"extended\":" + (extended ? "true" : "false") + '}');
}
void simulated() {
  std::vector<INPUT> events;
  bool failUp = false;
  RemoteInput input([&](INPUT& event) -> UINT { events.push_back(event); if (failUp && event.type == INPUT_KEYBOARD && (event.ki.dwFlags & KEYEVENTF_KEYUP)) return 0; return 1; });
  for (const WORD vk : {WORD(VK_HANGUL), WORD(VK_HANJA)}) {
    for (const WORD scan : {WORD(0x38), WORD(0xf1), WORD(0xf2)}) {
      input.apply(key(vk, true, scan, true)); input.apply(key(vk, false));
      const auto& down = events[events.size() - 2].ki; const auto& up = events.back().ki;
      require(down.wVk == vk && down.wScan == 0 && down.dwFlags == 0, "IME semantic key was overridden by a physical scan");
      require(up.wVk == vk && up.dwFlags == KEYEVENTF_KEYUP, "IME semantic key release changed");
    }
  }
  // These check the exact OS INPUT structures; they deliberately do not open
  // Start, switch to another application or launch Task Manager on this PC.
  const auto chord = [&](std::initializer_list<std::pair<int, int>> keys) {
    events.clear();
    for (const auto [vk, scan] : keys) input.apply(key(WORD(vk), true, WORD(scan), vk == VK_LWIN));
    input.reset(); require(input.pending() == 0, "Shortcut reset retained successfully released keys");
    require(events.size() == keys.size() * 2, "Shortcut lost a press or release");
    size_t index = 0;
    for (const auto [vk, scan] : keys) {
      const auto& sent = events[index++].ki;
      require(sent.wVk == 0 && sent.wScan == scan && (sent.dwFlags & KEYEVENTF_SCANCODE), "Shortcut scan was not preserved");
      require(bool(sent.dwFlags & KEYEVENTF_EXTENDEDKEY) == (vk == VK_LWIN), "Shortcut extended flag changed");
      bool released = false;
      for (size_t j = keys.size(); j < events.size(); j++) if (events[j].ki.wScan == scan && (events[j].ki.dwFlags & KEYEVENTF_KEYUP)) released = true;
      require(released, "Shortcut key was not released on reset");
    }
  };
  chord({{VK_LWIN, 0x5b}}); chord({{VK_LMENU, 0x38}, {VK_TAB, 0x0f}}); chord({{VK_LCONTROL, 0x1d}, {VK_LSHIFT, 0x2a}, {VK_ESCAPE, 0x01}});
  events.clear();
  for (int i = 0; i < 3; i++) input.apply(key('A', true, 0x1e));
  input.apply(key('A', false, 0x30, true));
  require(events.size() == 4 && events.back().ki.wScan == 0x1e && events.back().ki.dwFlags == (KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP), "Repeat or matching key-up scan regression");
  input.apply(key(VK_RCONTROL, true, 0x1d, true)); failUp = true; input.reset();
  require(input.pending() == 1, "Failed release was discarded");
  const auto count = events.size(); input.apply(key('B', true, 0x30));
  require(events.size() == count + 1 && input.pending() == 1, "New key bypassed a pending failed release");
  failUp = false; input.retry(); require(input.pending() == 0 && (events.back().ki.dwFlags & KEYEVENTF_EXTENDEDKEY), "Extended modifier release retry failed");
  events.clear();
  bool failMouseRelease = true;
  RemoteInput mouse([&](INPUT& event) -> UINT { events.push_back(event); return failMouseRelease && (event.mi.dwFlags & MOUSEEVENTF_LEFTUP) ? 0U : 1U; });
  mouse.apply(JsonParser::parse("{\"event\":\"button\",\"button\":\"left\",\"down\":true}")); mouse.reset();
  require(mouse.pending() == 1, "Failed mouse release was discarded");
  failMouseRelease = false; mouse.retry(); require(mouse.pending() == 0 && events.size() == 3, "Mouse release retry failed");
  events.clear(); input.apply(key(VK_CONTROL, true)); input.apply(key(VK_MENU, true)); input.apply(key(VK_DELETE, true, 0x53, true));
  require(events.size() == 2, "Unsupported secure attention was simulated"); input.reset();
  // A key sent without a scan code (Android's hardware keyboard sends letters with
  // 0) keeps the virtual key that was sent and carries this layout's scan code,
  // extended where the layout says so; its release is the same key.
  // Layout facts (the Korean layout): 'A' 0x1E, Left 0x4B without E0,
  // Divide E0 35, Right Ctrl none (that layout makes it Hanja). Expectations follow
  // the active layout; a key it gives no single code keeps the old virtual-key path.
  for (const WORD vk : {WORD('A'), WORD('G'), WORD(VK_LEFT), WORD(VK_DELETE), WORD(VK_NEXT), WORD(VK_DIVIDE), WORD(VK_RCONTROL), WORD(VK_NUMPAD4), WORD(VK_SPACE)}) {
    const UINT mapped = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    const bool filled = (mapped & 0xff) && ((mapped >> 8) == 0 || (mapped >> 8) == 0xe0);
    const bool navigation = vk == VK_LEFT || vk == VK_DELETE || vk == VK_NEXT;
    events.clear(); input.apply(key(vk, true)); input.apply(key(vk, false));
    const WORD scan = filled ? WORD(mapped & 0xff) : 0;
    const DWORD extended = filled && ((mapped >> 8) == 0xe0 || navigation) ? KEYEVENTF_EXTENDEDKEY : 0;
    require(events.size() == 2 && events[0].ki.wVk == vk && events[0].ki.wScan == scan && events[0].ki.dwFlags == extended, "A key without a scan code was sent without one, or lost its virtual key");
    require(events[1].ki.wVk == vk && events[1].ki.wScan == scan && events[1].ki.dwFlags == (extended | KEYEVENTF_KEYUP), "The release of a filled-in key differs from its press");
    if (vk == 'A' || vk == VK_LEFT || vk == VK_DIVIDE) require(filled && scan && (vk == 'A' || (events[0].ki.dwFlags & KEYEVENTF_EXTENDEDKEY)), "A letter, arrow or keypad Divide got no scan code or lost its extended flag");
  }
  // Hangul/Hanja without a scan code stay virtual-key only, as before.
  for (const WORD vk : {WORD(VK_HANGUL), WORD(VK_HANJA)}) {
    events.clear(); input.apply(key(vk, true)); input.apply(key(vk, false));
    require(events.size() == 2 && events[0].ki.wVk == vk && events[0].ki.wScan == 0 && events[0].ki.dwFlags == 0 && events[1].ki.dwFlags == KEYEVENTF_KEYUP, "An IME semantic key got a scan code");
  }
  // A filled-in key released by a reset (focus loss) is released with its scan code.
  events.clear(); input.apply(key('Q', true)); input.reset();
  require(events.size() == 2 && events[0].ki.wScan != 0 && events[1].ki.wVk == 'Q' && events[1].ki.wScan == events[0].ki.wScan && events[1].ki.dwFlags == KEYEVENTF_KEYUP && input.pending() == 0, "A reset released a filled-in key differently");
  // The sender's extended flag stays when the layout's code is not extended (keypad Enter).
  events.clear(); input.apply(key(VK_RETURN, true, 0, true)); input.apply(key(VK_RETURN, false));
  require(events.size() == 2 && events[0].ki.wVk == VK_RETURN && events[0].ki.wScan == (MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC_EX) & 0xff) && events[0].ki.dwFlags == KEYEVENTF_EXTENDEDKEY && events[1].ki.dwFlags == (KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP), "The sender's extended flag was lost");
  // E1 keys (Pause) have no single scan code: they stay virtual-key only.
  if ((MapVirtualKeyW(VK_PAUSE, MAPVK_VK_TO_VSC_EX) >> 8) == 0xe1) {
    events.clear(); input.apply(key(VK_PAUSE, true)); input.apply(key(VK_PAUSE, false));
    require(events.size() == 2 && events[0].ki.wVk == VK_PAUSE && events[0].ki.wScan == 0 && events[0].ki.dwFlags == 0, "Pause got a partial scan code");
  }
  std::cout << "{\"kind\":\"simulated-injector\",\"passed\":true,\"imeSemanticVariants\":6,\"scanCodeFilled\":true,\"shortcuts\":[\"Win\",\"Alt+Tab\",\"Ctrl+Shift+Esc\"],\"repeat\":true,\"matchingRelease\":true,\"failedReleaseRetry\":true,\"mouseReleaseRetry\":true,\"secureAttentionBlocked\":true,\"osKeystrokesEmitted\":false}\n";
}

void idleReleaseRetry() {
  std::atomic<unsigned> calls{0}, releases{0};
  std::atomic<bool> deny{true};
  const auto inject = [&](INPUT& event) -> UINT {
    ++calls;
    if ((event.type == INPUT_KEYBOARD && (event.ki.dwFlags & KEYEVENTF_KEYUP)) ||
        (event.type == INPUT_MOUSE && (event.mi.dwFlags & MOUSEEVENTF_LEFTUP))) {
      ++releases;
      return deny ? 0U : 1U;
    }
    return 1U;
  };
  const auto waitFor = [&](auto predicate) {
    // Generous ceiling for a loaded machine; returns as soon as it holds.
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (!predicate() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    require(predicate(), "Idle release retry did not make progress without a new pipe/input message");
  };
  {
    RemoteInput input(inject, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    require(calls == 0, "Idle release worker injected input with no failed release");
    input.apply(key(VK_LCONTROL, true, 0x1d));
    input.apply(key(VK_LCONTROL, false));
    waitFor([&] { return releases >= 3; });
    require(input.pending() == 1, "Denied release was lost before recovery");
    deny = false;
    waitFor([&] { return input.pending() == 0; });
    const auto settled = calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    require(calls == settled, "Recovered release worker continued injecting");
    deny = true; releases = 0;
    input.apply(JsonParser::parse(R"({"event":"button","button":"left","down":true})"));
    input.reset();
    waitFor([&] { return releases >= 3; });
    deny = false;
    waitFor([&] { return input.pending() == 0; });
    deny = true;
    input.apply(key('A', true, 0x1e)); input.reset();
    require(input.pending() == 1, "Shutdown fixture did not retain a denied release");
  }
  const auto stopped = calls.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  require(calls == stopped, "Release worker survived its input owner");
  std::cout << "{\"kind\":\"idle-release-retry\",\"passed\":true,\"newInputRequired\":false,\"keyboardAndMouse\":true,\"stopsAfterRecovery\":true,\"ownerLifetime\":true,\"osKeystrokesEmitted\":false}\n";
}

void concurrentReleaseRetry() {
  std::atomic<bool> denyA{false}, done{false};
  std::atomic<unsigned> bReleases{0};
  RemoteInput input([&](INPUT& event) -> UINT {
    if (event.type == INPUT_KEYBOARD && (event.ki.dwFlags & KEYEVENTF_KEYUP)) {
      if (event.ki.wVk == 'A' && denyA) return 0;
      if (event.ki.wVk == 'B') ++bReleases;
    }
    return 1;
  });
  std::thread retry([&] { while (!done) { input.retry(); SwitchToThread(); } });
  bool prematureRelease = false;
  for (unsigned round = 0; round < 1000 && !prematureRelease; ++round) {
    denyA = true; input.apply(key('A', true)); input.apply(key('A', false));
    denyA = false; input.retry();
    const auto before = bReleases.load();
    input.apply(key('B', true));
    for (int i = 0; i < 8; ++i) SwitchToThread();
    prematureRelease = input.pending() != 1 || bReleases != before;
    input.apply(key('B', false));
  }
  done = true; retry.join(); input.reset();
  require(!prematureRelease, "An older retry released a newly accepted key press");
  std::cout << "{\"kind\":\"concurrent-release-retry\",\"passed\":true,\"rounds\":1000,\"newPressPreserved\":true,\"osKeystrokesEmitted\":false}\n";
}

// Explicit opt-in OS test: creates only our own controls, never types into or
// reads another application. It changes keyboard/IME state only on this thread.
class OwnWindowTest {
 public:
  OwnWindowTest() {
    for (int vk : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN}) require(!(GetAsyncKeyState(vk) & 0x8000), "Release physical modifiers before the own-window OS test");
    originalLayout_ = GetKeyboardLayout(0);
    WNDCLASSW type{}; type.lpfnWndProc = DefWindowProcW; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"MultitapHostInputOwnWindowTest"; type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&type);
    window_ = CreateWindowExW(0, type.lpszClassName, L"Multitap input QA - own test controls only", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 570, 200, nullptr, nullptr, type.hInstance, nullptr);
    require(window_ != nullptr, "Cannot create own QA window");
    edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, 25, 510, 35, window_, nullptr, type.hInstance, nullptr);
    second_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, 80, 510, 35, window_, nullptr, type.hInstance, nullptr);
    require(edit_ && second_, "Cannot create own QA edit controls");
    SetWindowLongPtrW(edit_, GWLP_USERDATA, LONG_PTR(this)); oldEdit_ = WNDPROC(SetWindowLongPtrW(edit_, GWLP_WNDPROC, LONG_PTR(editProcedure)));
    ShowWindow(window_, SW_SHOW); SetForegroundWindow(window_); SetFocus(edit_);
    std::cout << "Waiting up to 60 seconds for foreground: Multitap input QA - own test controls only\n" << std::flush;
    const auto focusDeadline = Clock::now() + std::chrono::seconds(60);
    while (IsWindow(window_) && GetForegroundWindow() != window_ && Clock::now() < focusDeadline) pump(50);
    if (GetForegroundWindow() == window_) SetFocus(edit_);
    pump(200);
    focused();
  }
  ~OwnWindowTest() { input_.reset(); if (originalLayout_) ActivateKeyboardLayout(originalLayout_, 0); if (window_ && IsWindow(window_)) DestroyWindow(window_); }
  void run() {
    int count = GetKeyboardLayoutList(0, nullptr); std::vector<HKL> layouts(std::max(0, count)); if (count) GetKeyboardLayoutList(count, layouts.data());
    HKL korean = nullptr;
    for (const auto layout : layouts) if (LOWORD(ULONG_PTR(layout)) == MAKELANGID(LANG_KOREAN, SUBLANG_DEFAULT)) { korean = layout; break; }
    // First use a harmless function key: repeat is verified from real WM_KEYDOWN
    // counts rather than edit text, so it is independent of the current IME mode.
    for (int i = 0; i < 3; i++) send(VK_F8, true, 0x42);
    send(VK_F8, false); require(f8Down_ == 3 && f8Up_ == 1 && input_.pending() == 0, "Own-window OS repeat/key-up failed");
    send(VK_LSHIFT, true, 0x2a); SetFocus(second_); input_.reset(); pump(100); focused();
    require(input_.pending() == 0 && !(GetAsyncKeyState(VK_LSHIFT) & 0x8000), "Own-window focus-transfer reset left Shift held");
    SetFocus(edit_);
    if (!korean) { std::cout << "{\"kind\":\"own-window-os\",\"repeat\":true,\"focusTransferRelease\":true,\"koreanIme\":\"not-tested-no-loaded-Korean-layout\",\"physicalKeyboard\":false}\n"; return; }
    require(ActivateKeyboardLayout(korean, 0) != nullptr, "Cannot activate an already loaded Korean layout for this QA thread"); pump(300); focused();
    bool toggled = false;
    if (HIMC context = ImmGetContext(edit_)) { const bool initial = ImmGetOpenStatus(context) != FALSE; ImmReleaseContext(edit_, context); if (!initial) { send(VK_HANGUL, true, 0xf1, true); send(VK_HANGUL, false, 0xf1, true); toggled = true; } }
    const struct { WORD vk, scan; } sequence[] = {{'G',0x22},{'K',0x25},{'S',0x1f},{'R',0x13},{'M',0x32},{'F',0x21},{VK_SPACE,0x39}};
    for (const auto [vk, scan] : sequence) { send(vk, true, scan); send(vk, false, scan); }
    pump(400);
    const int size = GetWindowTextLengthW(edit_); std::wstring text(size + 1, L'\0'); GetWindowTextW(edit_, text.data(), size + 1); text.resize(size);
    const bool composed = text.find(L"한글") != std::wstring::npos;
    // Start an actual convertible syllable at the caret (rather than a trailing
    // space), then request Hanja only in this test's IME context.
    SetWindowTextW(edit_, L"");
    const struct { WORD vk, scan; } hanjaSequence[] = {{'G',0x22},{'K',0x25},{'S',0x1f}};
    for (const auto [vk, scan] : hanjaSequence) { send(vk, true, scan); send(vk, false, scan); }
    send(VK_HANJA, true, 0xf2, true); send(VK_HANJA, false, 0xf2, true); pump(200);
    if (HIMC context = ImmGetContext(edit_)) { DWORD lists = 0; ImmGetCandidateListCountW(context, &lists); candidate_ = candidate_ || lists > 0; ImmReleaseContext(edit_, context); }
    send(VK_ESCAPE, true, 1); send(VK_ESCAPE, false, 1);
    std::cout << "{\"kind\":\"own-window-os\",\"repeat\":true,\"focusTransferRelease\":true,\"loadedKoreanLayout\":true,\"hangulToggleSent\":" << (toggled ? "true" : "false") << ",\"hangulComposed\":" << (composed ? "true" : "false") << ",\"imeCompositionMessages\":" << composition_ << ",\"hanjaKeyRequested\":true,\"hanjaCandidateObserved\":" << (candidate_ ? "true" : "false") << ",\"physicalKeyboard\":false}\n";
    if (!composed) throw std::runtime_error("Real OS Korean IME composition was not observed; semantic structure tests alone do not establish Hangul support");
  }
 private:
  HWND window_ = nullptr, edit_ = nullptr, second_ = nullptr; WNDPROC oldEdit_ = nullptr; HKL originalLayout_ = nullptr;
  RemoteInput input_; int f8Down_ = 0, f8Up_ = 0, composition_ = 0; bool candidate_ = false;
  void focused() { require(IsWindow(window_) && GetForegroundWindow() == window_ && (GetFocus() == edit_ || GetFocus() == second_), "Own QA window lost focus; stopped before further injection"); }
  void pump(int milliseconds) { const auto end = Clock::now() + std::chrono::milliseconds(milliseconds); do { MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } std::this_thread::sleep_for(std::chrono::milliseconds(2)); } while (Clock::now() < end); }
  void send(WORD vk, bool down, WORD scan = 0, bool extended = false) { focused(); input_.apply(key(vk, down, scan, extended)); pump(40); }
  static LRESULT CALLBACK editProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* test = reinterpret_cast<OwnWindowTest*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_KEYDOWN && wParam == VK_F8) test->f8Down_++;
    if (message == WM_KEYUP && wParam == VK_F8) test->f8Up_++;
    if (message == WM_IME_COMPOSITION) test->composition_++;
    if (message == WM_IME_NOTIFY && wParam == IMN_OPENCANDIDATE) test->candidate_ = true;
    return CallWindowProcW(test->oldEdit_, window, message, wParam, lParam);
  }
};
}
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc == 1) { simulated(); idleReleaseRetry(); concurrentReleaseRetry(); return 0; }
    if (argc == 2 && std::wstring(argv[1]) == L"--own-window-os") { check(OleInitialize(nullptr), "Own-window COM/IME initialization"); try { OwnWindowTest test; test.run(); } catch (...) { OleUninitialize(); throw; } OleUninitialize(); return 0; }
    std::cerr << "Use no argument for simulation or --own-window-os for the explicit native EDIT/IME test.\n"; return 2;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
