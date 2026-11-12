#pragma once
// relative-mouse-v1 (docs/remote-session-protocol.md): while the
// mouse-lock capture holds the mouse, the pointer stays inside the picture and
// raw mouse deltas travel instead of positions. Dart decides when
// (data/relative_mouse.dart); this class owns the clip and WM_INPUT on the root
// window's message thread and ends capture by itself whenever the window stops
// owning the pointer. ASCII only. The picture's Flutter cursor is `none` and
// the cursor presenter shows the remote cursor (or nothing when it is hidden)
// where the local pointer is and moves it to each position the remote reports,
// so there is no ShowCursor counter here to restore. With cursor-echo-v1 the
// engagement is pinned: the pointer is held on one pixel that the presenter
// moves to each echoed VM position (Pin), so what is shown is the VM's cursor.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

#include "pointer_confinement.h"

namespace multitap {

// Posted by the keyboard hook to its root window for Ctrl+Alt+Shift+Z.
constexpr UINT kRelativeMouseToggleMessage = WM_APP + 187;

// Sums raw deltas into wire messages: at most one per 4 ms, each axis within
// +-16384, never both zero. What a message cannot carry yet stays for the next,
// so slow movement is not lost and a fast mouse cannot flood the socket.
class RelativeDeltas {
 public:
  static constexpr int64_t kLimit = 16384, kIntervalUs = 4000;
  void Add(int64_t dx, int64_t dy) {
    dx_ += dx;
    dy_ += dy;
  }
  void Clear() { dx_ = dy_ = 0; }
  bool Pending() const { return dx_ != 0 || dy_ != 0; }
  std::optional<POINT> Take(int64_t now_us) {
    if (!Pending() || (sent_ && now_us - last_us_ < kIntervalUs)) return std::nullopt;
    const int64_t x = std::clamp(dx_, -kLimit, kLimit), y = std::clamp(dy_, -kLimit, kLimit);
    dx_ -= x;
    dy_ -= y;
    sent_ = true;
    last_us_ = now_us;
    return POINT{LONG(x), LONG(y)};
  }

 private:
  int64_t dx_ = 0, dy_ = 0, last_us_ = 0;
  bool sent_ = false;
};

// Operating-system calls, replaceable by the native test.
struct RelativeMouseOs {
  BOOL(WINAPI* clip)(const RECT*) = ::ClipCursor;
  BOOL(WINAPI* clipped)(LPRECT) = ::GetClipCursor;
  BOOL(WINAPI* cursor)(LPPOINT) = ::GetCursorPos;
  HWND(WINAPI* foreground)() = ::GetForegroundWindow;
  HWND(WINAPI* window)(POINT) = ::WindowFromPoint;
  BOOL(WINAPI* devices)(PCRAWINPUTDEVICE, UINT, UINT) = ::RegisterRawInputDevices;
  UINT(WINAPI* raw)(HRAWINPUT, UINT, LPVOID, PUINT, UINT) = ::GetRawInputData;
  int64_t (*now_us)() = [] {
    return int64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count());
  };
};

// Where raw input goes instead of the deltas for one engagement: the game
// mouse's virtual USB device (game_mouse_usb.h). [input] gets each relative
// event, [tick] the timer (false: the device stopped taking input, capture
// ends as "lost"), [released] the end of capture so nothing stays pressed.
struct RelativeMouseSink {
  std::function<void(const RAWMOUSE&, HANDLE)> input;
  std::function<bool()> tick;
  std::function<void()> released;
};

class RelativeMouse {
 public:
  static constexpr UINT_PTR kTimer = 19043;
  // Sends what WM_INPUT left waiting and checks that the clip still holds.
  static constexpr UINT kTimerMs = USER_TIMER_MINIMUM;
  // |ended| runs after the clip is gone, for capture ended here: "focus",
  // "lost" (the clip is not ours any more) or "device" (no deltas exist).
  using Move = std::function<void(int dx, int dy)>;
  using Ended = std::function<void(const char* reason)>;
  explicit RelativeMouse(RelativeMouseOs os = {}) : os_(os) {}
  ~RelativeMouse() { Release(); }
  RelativeMouse(const RelativeMouse&) = delete;
  RelativeMouse& operator=(const RelativeMouse&) = delete;
  void Bind(Move move, Ended ended) {
    move_ = std::move(move);
    ended_ = std::move(ended);
  }
  // Before Engage, for that engagement only; ignored while engaged.
  void Route(RelativeMouseSink sink) {
    if (!engaged_) sink_ = std::move(sink);
  }
  bool routed() const { return engaged_ && sink_.input != nullptr; }

  // The pointer must be on |view| of the foreground, unminimized |root|.
  // Anything else stays absolute. |area|: the picture in screen pixels; the
  // pointer is held inside it (a visible remote cursor keeps moving there),
  // or parked where it stands when there is none or the pointer is outside.
  // |pinned| (cursor-echo-v1): the pointer is parked where it stands and then
  // moved only by Pin, to each position the VM reports; |area| is not used.
  // |concealed| (cursor-compose-v1): parked where it stands, never moved, and
  // the cursor presenter shows nothing (concealed()); the Host draws the VM's
  // cursor into the video. Raw Input keeps reporting this PC's motion.
  bool Engage(HWND root, HWND view, const RECT* area = nullptr, bool pinned = false, bool concealed = false) {
    if (concealed) pinned = false;
    if (engaged_) return root == root_ && pinned == pinned_ && concealed == concealed_;
    if (!root || !view || modal_ || IsIconic(root)) return false;
    const HWND foreground = os_.foreground();
    POINT pointer{};
    if (!foreground || GetAncestor(foreground, GA_ROOT) != root) return false;
    if (!os_.cursor(&pointer) || os_.window(pointer) != view) return false;
    // One raw input registration per process: never two captures at once.
    if (Active() && Active() != this) Active()->End("focus");
    const RAWINPUTDEVICE mouse{0x01, 0x02, 0, root};
    if (!os_.devices(&mouse, 1, sizeof(mouse))) return false;
    const RECT parked{pointer.x, pointer.y, pointer.x + 1, pointer.y + 1};
    const RECT clip = !pinned && !concealed && area && PtInRect(area, pointer) ? *area : parked;
    if (!os_.clip(&clip)) {
      Unregister();
      return false;
    }
    RECT now{};
    held_ = os_.clipped(&now) ? now : clip;
    frame_ = Frame(root);
    InstallPointerCrashRelease();
    ++ClippedPointerWindows();
    engaged_ = true;
    pinned_ = pinned;
    concealed_ = concealed;
    root_ = root;
    Active() = this;
    deltas_.Clear();
    timer_ = SetTimer(root, kTimer, kTimerMs, nullptr) != 0;
    return true;
  }
  // cursor-echo-v1, a pinned engagement only: the pointer stands on |point|
  // (screen pixels; the VM's cursor mapped into the picture) and this PC's own
  // motion cannot move it until the next one (Raw Input still reports it).
  // False when not pinned or Windows refused the clip.
  bool Pin(POINT point) {
    if (!engaged_ || !pinned_) return false;
    const RECT clip{point.x, point.y, point.x + 1, point.y + 1};
    if (EqualRect(&clip, &held_)) return true;
    if (!os_.clip(&clip)) return false;
    RECT now{};
    held_ = os_.clipped(&now) ? now : clip;
    ++pins_;
    return true;
  }
  bool pinned() const { return engaged_ && pinned_; }
  // cursor-compose-v1: engaged with the cursor in the video (nothing shown here).
  bool concealed() const { return engaged_ && concealed_; }
  uint64_t pins() const { return pins_; }
  // Dart asked; nothing is reported back. Safe on every path, repeatedly.
  void Release() {
    auto released = std::move(sink_.released);
    sink_ = {};
    if (!engaged_) return;
    engaged_ = false;
    pinned_ = false;
    concealed_ = false;
    --ClippedPointerWindows();
    os_.clip(nullptr);
    if (timer_ && root_ && IsWindow(root_)) KillTimer(root_, kTimer);
    timer_ = false;
    Unregister();
    if (Active() == this) Active() = nullptr;
    deltas_.Clear();
    root_ = nullptr;
    if (released) released();
  }
  // Native code decided: the same release, reported.
  void End(const char* reason) {
    if (!engaged_) return;
    Release();
    if (ended_) ended_(reason);
  }
  bool engaged() const { return engaged_; }

  // Call first from the root window procedure. Returns true only for its own
  // timer: WM_INPUT still needs DefWindowProc for its cleanup.
  bool HandleMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_ENTERSIZEMOVE: case WM_ENTERMENULOOP:
        modal_ = true;
        break;
      case WM_EXITSIZEMOVE: case WM_EXITMENULOOP:
        modal_ = false;
        break;
    }
    if (!engaged_ || window != root_) return false;
    switch (message) {
      case WM_INPUT:
        Input(reinterpret_cast<HRAWINPUT>(lparam));
        break;
      case WM_TIMER:
        if (wparam != kTimer) return false;
        Tick();
        return true;
      case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) End("focus");
        break;
      case WM_ACTIVATEAPP:
        if (!wparam) End("focus");
        break;
      case WM_ENTERSIZEMOVE: case WM_ENTERMENULOOP:
        End("focus");
        break;
      case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) End("focus");
        else if (Moved()) End("lost");
        break;
      // The picture moved: the clip no longer matches it.
      case WM_MOVE:
        if (Moved()) End("lost");
        break;
      case WM_DISPLAYCHANGE: case WM_DPICHANGED:
        End("lost");
        break;
      case WM_DESTROY: case WM_NCDESTROY:
        Release();
        break;
    }
    return false;
  }

 private:
  static RelativeMouse*& Active() {
    static RelativeMouse* active = nullptr;
    return active;
  }
  void Unregister() {
    const RAWINPUTDEVICE mouse{0x01, 0x02, RIDEV_REMOVE, nullptr};
    os_.devices(&mouse, 1, sizeof(mouse));
  }
  void Input(HRAWINPUT input) {
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    if (os_.raw(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) ||
        raw.header.dwType != RIM_TYPEMOUSE) {
      return;
    }
    // A tablet or a VM's pointer reports positions, so nothing could move.
    if (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
      End("device");
      return;
    }
    if (sink_.input) {
      sink_.input(raw.data.mouse, raw.header.hDevice);
      return;
    }
    deltas_.Add(raw.data.mouse.lLastX, raw.data.mouse.lLastY);
    Flush();
  }
  void Flush() {
    const auto step = deltas_.Take(os_.now_us());
    if (step && move_) move_(int(step->x), int(step->y));
  }
  void Tick() {
    RECT now{};
    const HWND foreground = os_.foreground();
    if (!foreground || GetAncestor(foreground, GA_ROOT) != root_) {
      End("focus");
    } else if (!os_.clipped(&now) || !EqualRect(&now, &held_)) {
      End("lost");
    } else if (sink_.tick && !sink_.tick()) {
      End("lost");
    } else {
      Flush();
    }
  }

  // The client area in screen pixels: where the picture (and the clip) is.
  static RECT Frame(HWND root) {
    RECT client{};
    POINT origin{};
    if (!GetClientRect(root, &client) || !ClientToScreen(root, &origin)) return RECT{};
    return RECT{origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
  }
  bool Moved() const {
    const RECT now = Frame(root_);
    return !EqualRect(&now, &frame_);
  }

  RelativeMouseOs os_;
  Move move_;
  Ended ended_;
  RelativeMouseSink sink_;
  RelativeDeltas deltas_;
  HWND root_ = nullptr;
  RECT held_{}, frame_{};
  bool engaged_ = false, pinned_ = false, concealed_ = false, timer_ = false, modal_ = false;
  uint64_t pins_ = 0;
};

}  // namespace multitap
