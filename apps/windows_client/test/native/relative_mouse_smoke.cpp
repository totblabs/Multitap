// relative-mouse-v1 without touching the real cursor or mouse: the delta summing
// is pure, and the capture runs on an actual hidden HWND with ClipCursor, raw
// input and the foreground replaced by recorded fakes.
#include "relative_mouse.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks = 0;
void require(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
  ++checks;
}
using multitap::RelativeDeltas;
using multitap::RelativeMouse;

void paced() {
  RelativeDeltas deltas;
  require(!deltas.Take(0) && !deltas.Pending(), "Nothing pending produced a message");
  deltas.Add(5, -2);
  auto step = deltas.Take(1000);
  require(step && step->x == 5 && step->y == -2 && !deltas.Pending(), "The first delta did not go out at once");
  deltas.Add(1, 1);
  require(!deltas.Take(4999) && deltas.Pending(), "A second message came within 4 ms");
  deltas.Add(2, 3);
  step = deltas.Take(5000);
  require(step && step->x == 3 && step->y == 4, "Deltas within 4 ms were not summed");
}
void zeroNeverSent() {
  RelativeDeltas deltas;
  deltas.Add(7, -7);
  deltas.Add(-7, 7);
  require(!deltas.Pending() && !deltas.Take(10000), "Cancelled movement sent a zero message");
}
void bounds() {
  RelativeDeltas deltas;
  deltas.Add(50000, -20000);
  auto step = deltas.Take(0);
  require(step && step->x == 16384 && step->y == -16384, "A message left the wire bounds");
  step = deltas.Take(4000);
  require(step && step->x == 16384 && step->y == -3616, "The remainder of a large burst was not next");
  step = deltas.Take(8000);
  require(step && step->x == 16384 && step->y == 0, "A remainder on one axis was lost");
  step = deltas.Take(12000);
  require(step && step->x == 848 && step->y == 0 && !deltas.Pending(), "A large burst did not drain exactly");
  require(!deltas.Take(16000), "A drained burst sent another message");
}
void slowMovementKept() {
  RelativeDeltas deltas;
  int64_t sentX = 0, sentY = 0;
  int messages = 0;
  // A 1000 Hz mouse for one second, one count per report.
  for (int64_t now = 0; now < 1000000; now += 1000) {
    deltas.Add(1, now % 2000 ? 0 : -1);
    if (const auto step = deltas.Take(now)) {
      sentX += step->x;
      sentY += step->y;
      ++messages;
    }
  }
  require(messages >= 200 && messages <= 251, "A 1000 Hz mouse was not paced to 4 ms");
  deltas.Add(0, 0);
  if (const auto step = deltas.Take(2000000)) {
    sentX += step->x;
    sentY += step->y;
  }
  require(sentX == 1000 && sentY == -500 && !deltas.Pending(), "Slow movement was lost");
  // An 8000 Hz mouse, never more than the pace either.
  RelativeDeltas fast;
  messages = 0;
  for (int64_t now = 0; now < 1000000; now += 125) {
    fast.Add(3, 0);
    messages += fast.Take(now).has_value();
  }
  require(messages <= 251, "An 8000 Hz mouse exceeded one message per 4 ms");
}

// Recorded fakes.
HWND os_foreground = nullptr, os_under = nullptr;
POINT os_cursor{};
RECT os_clip{};
bool clip_allowed = true, devices_allowed = true;
int64_t os_now = 0;
std::vector<RAWINPUTDEVICE> device_calls;
std::vector<std::pair<bool, RECT>> clip_calls;  // (released?, rect)
RECT screen() { return RECT{-1920, 0, 3840, 2160}; }
bool unclipped() {
  const RECT whole = screen();
  return EqualRect(&os_clip, &whole) != FALSE;
}
BOOL WINAPI FakeClip(const RECT* rect) {
  if (rect && !clip_allowed) return FALSE;
  clip_calls.emplace_back(rect == nullptr, rect ? *rect : RECT{});
  os_clip = rect ? *rect : screen();
  return TRUE;
}
BOOL WINAPI FakeClipped(LPRECT rect) { *rect = os_clip; return TRUE; }
BOOL WINAPI FakeCursor(LPPOINT point) { *point = os_cursor; return TRUE; }
HWND WINAPI FakeForeground() { return os_foreground; }
HWND WINAPI FakeWindow(POINT) { return os_under; }
BOOL WINAPI FakeDevices(PCRAWINPUTDEVICE devices, UINT count, UINT) {
  require(count == 1, "One raw device expected");
  if (!(devices->dwFlags & RIDEV_REMOVE) && !devices_allowed) return FALSE;
  device_calls.push_back(*devices);
  return TRUE;
}
// The HRAWINPUT of a test message is a pointer to its report.
UINT WINAPI FakeRaw(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT) {
  require(command == RID_INPUT && data && size && *size >= sizeof(RAWINPUT), "Raw input was read badly");
  *static_cast<RAWINPUT*>(data) = *reinterpret_cast<const RAWINPUT*>(input);
  return sizeof(RAWINPUT);
}
int64_t FakeNow() { return os_now; }
multitap::RelativeMouseOs fakes() {
  multitap::RelativeMouseOs os;
  os.clip = FakeClip;
  os.clipped = FakeClipped;
  os.cursor = FakeCursor;
  os.foreground = FakeForeground;
  os.window = FakeWindow;
  os.devices = FakeDevices;
  os.raw = FakeRaw;
  os.now_us = FakeNow;
  return os;
}
RAWINPUT report(LONG x, LONG y, USHORT flags = MOUSE_MOVE_RELATIVE) {
  RAWINPUT raw{};
  raw.header.dwType = RIM_TYPEMOUSE;
  raw.data.mouse.usFlags = flags;
  raw.data.mouse.lLastX = x;
  raw.data.mouse.lLastY = y;
  return raw;
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  return DefWindowProcW(window, message, wparam, lparam);
}
struct Fixture {
  HWND root = nullptr, view = nullptr, other = nullptr;
  std::vector<std::pair<int, int>> moves;
  std::vector<std::string> ended;
  void Reset() {
    moves.clear();
    ended.clear();
    device_calls.clear();
    clip_calls.clear();
    clip_allowed = devices_allowed = true;
    os_foreground = root;
    os_under = view;
    os_cursor = POINT{640, 360};
    os_clip = screen();
    os_now = 0;
  }
  void Bind(RelativeMouse& mouse) {
    mouse.Bind([this](int dx, int dy) { moves.emplace_back(dx, dy); },
               [this](const char* reason) { ended.emplace_back(reason); });
  }
  void Released(RelativeMouse& mouse, const char* reason) const {
    require(!mouse.engaged(), reason);
    require(!clip_calls.empty() && clip_calls.back().first && unclipped(), reason);
    require(!device_calls.empty() && (device_calls.back().dwFlags & RIDEV_REMOVE), reason);
    require(multitap::ClippedPointerWindows().load() == 0, reason);
  }
};
void input(RelativeMouse& mouse, HWND window, RAWINPUT raw) {
  mouse.HandleMessage(window, WM_INPUT, RIM_INPUT, reinterpret_cast<LPARAM>(&raw));
}
void engaging(Fixture& f) {
  f.Reset();
  RelativeMouse mouse(fakes());
  f.Bind(mouse);
  require(!mouse.Engage(nullptr, f.view) && !mouse.Engage(f.root, nullptr), "A missing window engaged");
  os_foreground = f.other;
  require(!mouse.Engage(f.root, f.view), "A background window engaged");
  os_foreground = nullptr;
  require(!mouse.Engage(f.root, f.view), "An unknown foreground engaged");
  os_foreground = f.root;
  os_under = f.other;
  require(!mouse.Engage(f.root, f.view), "A pointer off the picture engaged");
  os_under = f.view;
  mouse.HandleMessage(f.root, WM_ENTERSIZEMOVE, 0, 0);
  require(!mouse.Engage(f.root, f.view), "A move/size loop engaged");
  mouse.HandleMessage(f.root, WM_EXITSIZEMOVE, 0, 0);
  devices_allowed = false;
  require(!mouse.Engage(f.root, f.view) && clip_calls.empty(), "A missing raw registration clipped the pointer");
  devices_allowed = true;
  clip_allowed = false;
  require(!mouse.Engage(f.root, f.view), "A refused clip engaged");
  require(!device_calls.empty() && (device_calls.back().dwFlags & RIDEV_REMOVE), "A refused clip left raw input registered");
  clip_allowed = true;
  require(multitap::ClippedPointerWindows().load() == 0 && !mouse.engaged(), "A refused engage stayed");
  device_calls.clear();
  require(mouse.Engage(f.root, f.view) && mouse.engaged(), "A valid engage failed");
  require(mouse.Engage(f.root, f.view) && device_calls.size() == 1, "A repeated engage registered again");
  require(device_calls[0].usUsagePage == 0x01 && device_calls[0].usUsage == 0x02 && device_calls[0].hwndTarget == f.root, "Raw input is not the root window's mouse");
  // Legacy messages stay on: buttons and the wheel still reach Flutter.
  require(device_calls[0].dwFlags == 0, "Raw input changed the legacy mouse messages");
  const RECT parked{640, 360, 641, 361};
  require(EqualRect(&clip_calls.back().second, &parked) != FALSE, "The pointer was not parked where it stood");
  require(multitap::ClippedPointerWindows().load() == 1, "The crash release does not know about this clip");
  mouse.Release();
  f.Released(mouse, "Release left the pointer captured");
  require(f.ended.empty(), "A requested release reported itself");
  const auto calls = clip_calls.size();
  mouse.Release();
  require(clip_calls.size() == calls, "A second release touched the clip");
}
void sending(Fixture& f) {
  f.Reset();
  RelativeMouse mouse(fakes());
  f.Bind(mouse);
  require(mouse.Engage(f.root, f.view), "Engage failed");
  input(mouse, f.root, report(3, -1));
  require(f.moves.size() == 1 && f.moves[0] == std::pair{3, -1}, "The first report was not sent");
  os_now = 1000;
  input(mouse, f.root, report(2, 0));
  os_now = 2000;
  input(mouse, f.root, report(1, 1));
  require(f.moves.size() == 1, "Reports within 4 ms were each sent");
  // No more reports: the timer sends what waits.
  os_now = 4000;
  require(mouse.HandleMessage(f.root, WM_TIMER, RelativeMouse::kTimer, 0) && f.moves.size() == 2 && f.moves[1] == std::pair{3, 1}, "The remainder was not sent by the timer");
  require(!mouse.HandleMessage(f.root, WM_TIMER, RelativeMouse::kTimer + 1, 0), "Another timer was taken");
  RAWINPUT keyboard{};
  keyboard.header.dwType = RIM_TYPEKEYBOARD;
  input(mouse, f.root, keyboard);
  input(mouse, f.other, report(9, 9));
  os_now = 9000;
  mouse.HandleMessage(f.root, WM_TIMER, RelativeMouse::kTimer, 0);
  require(f.moves.size() == 2, "A foreign report moved the remote");
  // WM_INPUT stays unhandled: DefWindowProc owns its cleanup.
  RAWINPUT raw = report(1, 1);
  require(!mouse.HandleMessage(f.root, WM_INPUT, RIM_INPUT, reinterpret_cast<LPARAM>(&raw)), "WM_INPUT was swallowed");
  mouse.Release();
  input(mouse, f.root, report(5, 5));
  require(f.moves.size() == 3, "A released capture sent movement");
}
void endings(Fixture& f) {
  const struct { UINT message; WPARAM wparam; const char* reason; const char* failure; } cases[] = {
      {WM_ACTIVATE, WA_INACTIVE, "focus", "Deactivation left the pointer captured"},
      {WM_ACTIVATEAPP, FALSE, "focus", "App deactivation left the pointer captured"},
      {WM_ENTERSIZEMOVE, 0, "focus", "A move/size loop left the pointer captured"},
      {WM_ENTERMENULOOP, 0, "focus", "A menu loop left the pointer captured"},
      {WM_SIZE, SIZE_MINIMIZED, "focus", "Minimize left the pointer captured"},
      {WM_DISPLAYCHANGE, 0, "lost", "A display change left the pointer captured"},
      {WM_DPICHANGED, 0, "lost", "A DPI change left the pointer captured"},
  };
  for (const auto& item : cases) {
    f.Reset();
    RelativeMouse mouse(fakes());
    f.Bind(mouse);
    require(mouse.Engage(f.root, f.view), "Engage failed");
    mouse.HandleMessage(f.other, item.message, item.wparam, 0);
    require(mouse.engaged(), "Another window's message ended capture");
    mouse.HandleMessage(f.root, item.message, item.wparam, 0);
    f.Released(mouse, item.failure);
    require(f.ended == std::vector<std::string>{item.reason}, item.failure);
  }
  for (const UINT message : {WM_DESTROY, WM_NCDESTROY}) {
    f.Reset();
    RelativeMouse mouse(fakes());
    f.Bind(mouse);
    require(mouse.Engage(f.root, f.view), "Engage failed");
    mouse.HandleMessage(f.root, message, 0, 0);
    f.Released(mouse, "Window destruction left the pointer captured");
  }
  // These stay captured.
  f.Reset();
  RelativeMouse mouse(fakes());
  f.Bind(mouse);
  require(mouse.Engage(f.root, f.view), "Engage failed");
  mouse.HandleMessage(f.root, WM_ACTIVATE, WA_ACTIVE, 0);
  mouse.HandleMessage(f.root, WM_ACTIVATEAPP, TRUE, 0);
  mouse.HandleMessage(f.root, WM_SIZE, SIZE_RESTORED, 0);
  require(mouse.engaged() && f.ended.empty(), "An active window lost capture");
  // The foreground moved without a deactivation message.
  os_foreground = f.other;
  mouse.HandleMessage(f.root, WM_TIMER, RelativeMouse::kTimer, 0);
  f.Released(mouse, "A background window kept the pointer");
  require(f.ended == std::vector<std::string>{"focus"}, "The moved foreground was not reported");
  // Windows or another program dropped the clip (secure desktop, display change).
  f.Reset();
  require(mouse.Engage(f.root, f.view), "Engage failed");
  os_clip = screen();
  mouse.HandleMessage(f.root, WM_TIMER, RelativeMouse::kTimer, 0);
  f.Released(mouse, "A dropped clip was trusted");
  require(f.ended == std::vector<std::string>{"lost"}, "The dropped clip was not reported");
  // A pointer that reports positions has no deltas to send.
  f.Reset();
  require(mouse.Engage(f.root, f.view), "Engage failed");
  input(mouse, f.root, report(30000, 30000, MOUSE_MOVE_ABSOLUTE));
  f.Released(mouse, "An absolute device left the pointer captured");
  require(f.ended == std::vector<std::string>{"device"} && f.moves.empty(), "An absolute device moved the remote");
  // End after a release says nothing.
  f.ended.clear();
  mouse.End("focus");
  require(f.ended.empty(), "A released capture reported an end");
}
// 게임 마우스 hands the mouse over with the remote cursor visible: the pointer
// is held inside the picture (it keeps moving there), not parked.
void pictureClip(Fixture& f) {
  f.Reset();
  RelativeMouse mouse(fakes());
  f.Bind(mouse);
  // The picture of a window at (200, 150) at 144 dpi, as the runner computes it.
  const auto area = multitap::ConfinementRect({50, 20, 400, 300}, 144, POINT{200, 150}, SIZE{900, 700});
  require(area && area->left == 278 && area->top == 183 && area->right == 872 && area->bottom == 627, "The picture rectangle is not the picture on its monitor");
  os_cursor = POINT{640, 360};
  require(mouse.Engage(f.root, f.view, &*area), "A picture engage failed");
  require(EqualRect(&clip_calls.back().second, &*area) != FALSE, "The pointer was not held in the picture");
  require(mouse.HandleMessage(f.root, WM_TIMER, RelativeMouse::kTimer, 0) && mouse.engaged(), "The picture clip was taken as lost");
  // The window did not move: a size or move message keeps the clip.
  mouse.HandleMessage(f.root, WM_MOVE, 0, 0);
  mouse.HandleMessage(f.root, WM_SIZE, SIZE_RESTORED, 0);
  require(mouse.engaged() && f.ended.empty(), "An unchanged window lost the mouse");
  // It moved: the picture is elsewhere now.
  SetWindowPos(f.root, nullptr, 260, 150, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  mouse.HandleMessage(f.root, WM_MOVE, 0, 0);
  f.Released(mouse, "A moved window kept the old clip");
  require(f.ended == std::vector<std::string>{"lost"}, "The move was not reported");
  // Resized as well.
  f.ended.clear();
  require(mouse.Engage(f.root, f.view, &*area), "engage");
  SetWindowPos(f.root, nullptr, 0, 0, 880, 680, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  mouse.HandleMessage(f.root, WM_SIZE, SIZE_RESTORED, 0);
  f.Released(mouse, "A resized window kept the old clip");
  require(f.ended == std::vector<std::string>{"lost"}, "The resize was not reported");
  SetWindowPos(f.root, nullptr, 200, 150, 900, 700, SWP_NOZORDER | SWP_NOACTIVATE);
  // A pointer outside the given picture (or none given) is parked where it stands.
  const RECT elsewhere{0, 0, 100, 100};
  require(mouse.Engage(f.root, f.view, &elsewhere), "engage");
  const RECT parked{640, 360, 641, 361};
  require(EqualRect(&clip_calls.back().second, &parked) != FALSE, "A picture away from the pointer was used");
  mouse.Release();
}
// cursor-echo-v1: a pinned engagement parks the pointer even with a picture,
// then holds it on each pixel Pin names (the echoed VM position), which the
// timer check accepts as its own clip. Raw input keeps flowing meanwhile.
void pinned(Fixture& f) {
  f.Reset();
  RelativeMouse mouse(fakes());
  f.Bind(mouse);
  require(!mouse.Pin(POINT{10, 10}) && clip_calls.empty(), "Pin moved a pointer that is not engaged");
  const RECT picture{200, 150, 1100, 850};
  require(mouse.Engage(f.root, f.view, &picture, true) && mouse.pinned(), "A pinned engage failed");
  const RECT parked{640, 360, 641, 361};
  require(EqualRect(&clip_calls.back().second, &parked) != FALSE, "A pinned engage used the picture instead of parking");
  require(mouse.Pin(POINT{700, 420}), "Pin refused");
  const RECT moved{700, 420, 701, 421};
  require(EqualRect(&clip_calls.back().second, &moved) != FALSE && EqualRect(&os_clip, &moved) != FALSE, "Pin did not hold the pointer on the echoed pixel");
  const auto calls = clip_calls.size();
  require(mouse.Pin(POINT{700, 420}) && clip_calls.size() == calls, "An unchanged pin touched the clip again");
  require(mouse.HandleMessage(f.root, WM_TIMER, RelativeMouse::kTimer, 0) && mouse.engaged() && f.ended.empty(), "A pinned clip was taken as lost");
  input(mouse, f.root, report(4, -2));
  require(f.moves.size() == 1 && f.moves[0] == std::pair{4, -2}, "Raw movement stopped while pinned");
  require(mouse.pins() == 1, "Pin counter");
  // Windows refusing a pin leaves the last one (and the capture) as it was.
  clip_allowed = false;
  require(!mouse.Pin(POINT{710, 430}) && EqualRect(&os_clip, &moved) != FALSE && mouse.engaged(), "A refused pin changed the clip");
  clip_allowed = true;
  // A repeated engage of another kind is refused; release unpins.
  require(!mouse.Engage(f.root, f.view, &picture, false) && mouse.pinned(), "An unpinned engage replaced a pinned one");
  mouse.Release();
  f.Released(mouse, "Release left a pinned pointer clipped");
  require(!mouse.pinned() && !mouse.Pin(POINT{1, 1}), "A released capture still pinned");
  // Not pinned: Pin never moves the clip (positions warp as before).
  require(mouse.Engage(f.root, f.view, &picture) && !mouse.pinned(), "engage");
  require(EqualRect(&clip_calls.back().second, &picture) != FALSE, "An unpinned engage no longer used the picture");
  require(!mouse.Pin(POINT{700, 420}) && EqualRect(&os_clip, &picture) != FALSE, "Pin moved an unpinned capture");
  mouse.Release();
}
// cursor-compose-v1: a concealed engagement parks the pointer even with a
// picture (and even when pin was asked too), never moves it, keeps raw input
// flowing, and release ends it like any other.
void concealed(Fixture& f) {
  f.Reset();
  RelativeMouse mouse(fakes());
  f.Bind(mouse);
  const RECT picture{200, 150, 1100, 850};
  require(mouse.Engage(f.root, f.view, &picture, true, true) && mouse.concealed() && !mouse.pinned(), "A concealed engage failed or pinned");
  const RECT parked{640, 360, 641, 361};
  require(EqualRect(&clip_calls.back().second, &parked) != FALSE, "A concealed engage used the picture instead of parking");
  const auto calls = clip_calls.size();
  require(!mouse.Pin(POINT{700, 420}) && clip_calls.size() == calls, "A concealed pointer was moved");
  require(mouse.HandleMessage(f.root, WM_TIMER, RelativeMouse::kTimer, 0) && mouse.engaged() && f.ended.empty(), "A concealed park was taken as lost");
  input(mouse, f.root, report(-3, 5));
  require(f.moves.size() == 1 && f.moves[0] == std::pair{-3, 5}, "Raw movement stopped while concealed");
  require(!mouse.Engage(f.root, f.view, &picture, true, false) && mouse.concealed(), "A pinned engage replaced a concealed one");
  mouse.Release();
  f.Released(mouse, "Release left a concealed pointer clipped");
  require(!mouse.concealed(), "A released capture still concealed");
  require(mouse.Engage(f.root, f.view, &picture) && !mouse.concealed(), "An ordinary engage was concealed");
  mouse.Release();
}
void destructor(Fixture& f) {
  f.Reset();
  {
    RelativeMouse mouse(fakes());
    f.Bind(mouse);
    require(mouse.Engage(f.root, f.view), "Engage failed");
  }
  require(unclipped() && clip_calls.back().first, "Destruction left the pointer clipped");
  require(multitap::ClippedPointerWindows().load() == 0, "The crash count leaked");
  require(f.ended.empty(), "Destruction called back");
}
void oneCapture(Fixture& f) {
  f.Reset();
  RelativeMouse first(fakes()), second(fakes());
  f.Bind(first);
  f.Bind(second);
  require(first.Engage(f.root, f.view), "First engage failed");
  require(second.Engage(f.root, f.view) && !first.engaged(), "Raw input was claimed twice");
  require(f.ended == std::vector<std::string>{"focus"}, "The replaced capture was not told");
  require(multitap::ClippedPointerWindows().load() == 1, "The crash count drifted");
  second.Release();
}
void machine() {
  WNDCLASSW type{};
  type.lpfnWndProc = Procedure;
  type.hInstance = GetModuleHandleW(nullptr);
  type.lpszClassName = L"MultitapRelativeMouseQA";
  require(RegisterClassW(&type) != 0, "QA window class registration failed");
  Fixture f;
  // Never shown, never activated: the foreground is the fake's answer.
  f.root = CreateWindowExW(0, type.lpszClassName, L"Multitap relative QA", WS_OVERLAPPEDWINDOW, 200, 150, 900, 700, nullptr, nullptr, type.hInstance, nullptr);
  f.other = CreateWindowExW(0, type.lpszClassName, L"Multitap relative QA peer", WS_OVERLAPPEDWINDOW, 0, 0, 300, 200, nullptr, nullptr, type.hInstance, nullptr);
  f.view = CreateWindowExW(0, type.lpszClassName, L"", WS_CHILD, 0, 0, 900, 700, f.root, nullptr, type.hInstance, nullptr);
  require(f.root && f.other && f.view && !IsWindowVisible(f.root), "Hidden QA windows were not created");
  engaging(f);
  sending(f);
  endings(f);
  pictureClip(f);
  pinned(f);
  concealed(f);
  destructor(f);
  oneCapture(f);
  DestroyWindow(f.other);
  DestroyWindow(f.root);
  UnregisterClassW(type.lpszClassName, type.hInstance);
}
}  // namespace

int main() {
  try {
    RECT before{};
    GetClipCursor(&before);
    paced();
    zeroNeverSent();
    bounds();
    slowMovementKept();
    machine();
    RECT after{};
    GetClipCursor(&after);
    require(EqualRect(&before, &after) != FALSE, "The real cursor clip changed during QA");
    std::cout << "{\"passed\":true,\"checks\":" << checks
              << ",\"realClipCursorCalled\":false,\"actualHiddenWindow\":true,\"windowShown\":false,"
                 "\"physicalInput\":false,\"realSessionVerified\":false}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
