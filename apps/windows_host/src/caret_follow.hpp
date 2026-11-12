#pragma once
// caret-v1 (docs/remote-session-protocol.md "Caret follow"): while a client's
// soft keyboard is open (the Android app) it sends {"type":"caret.watch",
// "on":true}; the interactive worker then reports where the text caret of the
// focused application is, in pixels of the streamed output, so the client can
// keep the field being typed in above its keyboard.
//
// Sources (all in the user session, out-of-context WinEvent hooks): caret
// location changes (EVENT_OBJECT_LOCATIONCHANGE with OBJID_CARET) read through
// IAccessible::accLocation, and the foreground thread's GUITHREADINFO caret
// (ClientToScreen) as a fallback and on focus/foreground changes. Applications
// that publish neither (many browser/Qt/custom-drawn editors) report "no
// caret"; the client then follows taps. A UI Automation TextPattern2 caret is
// not read (a cross-process call per update; left for later if needed).
//
// Privacy: the report is {type, sessionId, has, x, y, w, h, sw, sh} only, in
// numbers. No text, window title, class or process name is read or sent.
#include "support.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <oleacc.h>
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace host {
inline constexpr const char* caretProtocol = "caret-v1";
// At most ~15 reports per second; the newest change is held and sent when the
// interval has passed.
inline constexpr std::chrono::milliseconds caretMinInterval{66};

// session.start of an ordinary stream session whose broker granted it.
inline bool caretSessionRequested(const Json& start) {
  return start.get("type").string() == "session.start" && start.get("mode").string() == "stream" &&
         start.get("caretProtocol").string() == caretProtocol && start.get("mediaProtocol").string().empty();
}

// {"type":"caret.watch","on":bool[,"sessionId":...]}; nothing else.
inline std::optional<bool> caretWatchRequest(const Json& message) {
  const auto* fields = std::get_if<Json::Object>(&message.value);
  if (!fields || message.get("type").string() != "caret.watch") return {};
  for (const auto& [key, item] : *fields) { (void)item; if (key != "type" && key != "on" && key != "sessionId") return {}; }
  const auto* on = std::get_if<bool>(&message.get("on").value);
  if (!on) return {};
  return *on;
}

// One report in pixels of the streamed output (origin = its top-left).
struct CaretReport {
  bool has = false;
  int x = 0, y = 0, w = 0, h = 0, sw = 0, sh = 0;
  bool operator==(const CaretReport&) const = default;
};

// Maps a caret rectangle in screen pixels into the streamed output `area`
// (the same screen-pixel rectangle the input path maps normalized pointer
// positions onto, in reverse). A caret whose anchor (top-left) lies outside
// the area is dropped (nullopt); a caret partly outside is clipped. The
// rectangle is at least 1x1 and the output at least 2x2.
inline std::optional<CaretReport> caretToOutput(RECT caret, const RECT& area) {
  const int64_t sw = int64_t(area.right) - area.left, sh = int64_t(area.bottom) - area.top;
  if (sw < 2 || sh < 2 || sw > 65535 || sh > 65535) return {};
  if (caret.right < caret.left) std::swap(caret.left, caret.right);
  if (caret.bottom < caret.top) std::swap(caret.top, caret.bottom);
  if (caret.left < area.left || caret.left >= area.right || caret.top < area.top || caret.top >= area.bottom) return {};
  if (caret.right <= caret.left) caret.right = caret.left + 1;
  if (caret.bottom <= caret.top) caret.bottom = caret.top + 1;
  caret.right = std::min<LONG>(caret.right, area.right);
  caret.bottom = std::min<LONG>(caret.bottom, area.bottom);
  CaretReport report;
  report.has = true;
  report.x = caret.left - area.left; report.y = caret.top - area.top;
  report.w = caret.right - caret.left; report.h = caret.bottom - caret.top;
  report.sw = int(sw); report.sh = int(sh);
  return report;
}

// The fixed message for one report.
inline std::string caretMessage(const std::string& sessionId, const CaretReport& r) {
  std::string out = "{\"type\":\"caret\",\"sessionId\":" + jsonQuote(sessionId) + ",\"has\":" + (r.has ? "true" : "false");
  if (r.has) out += ",\"x\":" + std::to_string(r.x) + ",\"y\":" + std::to_string(r.y) + ",\"w\":" + std::to_string(r.w) + ",\"h\":" + std::to_string(r.h) + ",\"sw\":" + std::to_string(r.sw) + ",\"sh\":" + std::to_string(r.sh);
  return out + '}';
}

// Send only on change, at most once per interval, newest value wins. Not
// thread safe (the tracker thread owns it).
class CaretThrottle {
 public:
  using Clock = std::chrono::steady_clock;
  explicit CaretThrottle(std::chrono::milliseconds interval = caretMinInterval) : interval_(interval) {}
  // A new observation; returns the report to send now, if any.
  std::optional<CaretReport> offer(const CaretReport& report, Clock::time_point now) {
    wanted_ = report;
    return due(now);
  }
  // Called on timer ticks: the held change once the interval has passed.
  std::optional<CaretReport> due(Clock::time_point now) {
    if (!wanted_) return {};
    if (sent_ && *sent_ == *wanted_) { wanted_.reset(); return {}; }
    if (sentAt_ && now - *sentAt_ < interval_) return {};
    sent_ = wanted_; sentAt_ = now; wanted_.reset();
    return sent_;
  }
  bool pending() const { return wanted_.has_value() && !(sent_ && *sent_ == *wanted_); }
  // Time until a held change may be sent (zero when none or already due).
  std::chrono::milliseconds wait(Clock::time_point now) const {
    if (!pending() || !sentAt_) return std::chrono::milliseconds(0);
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(interval_ - (now - *sentAt_));
    return left.count() > 0 ? left : std::chrono::milliseconds(0);
  }
  void reset() { wanted_.reset(); sent_.reset(); sentAt_.reset(); }
 private:
  std::chrono::milliseconds interval_;
  std::optional<CaretReport> wanted_, sent_;
  std::optional<Clock::time_point> sentAt_;
};

// The user-session tracker: one thread with a message loop for the WinEvent
// hooks, alive only while the client watches.
//
// The accessibility queries (AccessibleObjectFromEvent / accLocation) are cross-process COM calls into the focused
// application and can block for as long as that application is hung. Stopping must not wait for it: everything the
// thread touches lives in shared_ptr state that the thread co-owns, so watch(false) waits a bounded time and then
// abandons a thread that is still inside such a call (it exits on its own when the call returns, without emitting).
class CaretTracker {
 public:
  using Emit = std::function<void(const std::string&)>;
  explicit CaretTracker(Emit emit, std::chrono::milliseconds stopTimeout = std::chrono::milliseconds(1500))
      : config_(std::make_shared<Config>()), stopTimeout_(stopTimeout) { config_->emit = std::move(emit); }
  ~CaretTracker() { watch(false); }
  CaretTracker(const CaretTracker&) = delete;
  CaretTracker& operator=(const CaretTracker&) = delete;

  // The streamed output (screen pixels) and session the reports belong to.
  void configure(const std::string& sessionId, const RECT& area) {
    std::lock_guard lock(config_->mutex);
    config_->sessionId = sessionId; config_->area = area;
  }
  bool watching() const { return running_.load(); }
  // How many tracker threads were abandoned because a cross-process call did not return in time (diagnostics, tests).
  static int abandoned() { return abandoned_.load(); }
  void watch(bool on) {
    std::lock_guard lifecycle(lifecycle_);
    if (on == running_.load()) return;
    if (on) {
      auto state = std::make_shared<Run>();
      try { thread_ = std::thread([config = config_, state] { run(config, state); }); } catch (...) { return; }
      run_ = std::move(state); running_ = true;
    } else {
      const auto state = std::move(run_);
      running_ = false;
      if (!state) return;
      state->stop = true;
      const DWORD id = state->threadId.load();
      if (id) PostThreadMessageW(id, WM_QUIT, 0, 0);
      bool finished;
      { std::unique_lock lock(state->doneMutex); finished = state->doneCv.wait_for(lock, stopTimeout_, [&] { return state->done; }); }
      if (finished) { if (thread_.joinable()) thread_.join(); }
      else { ++abandoned_; if (thread_.joinable()) thread_.detach(); }
    }
  }

 private:
  struct Config { std::mutex mutex; std::string sessionId; RECT area{}; Emit emit; };
  struct Event { std::atomic<bool> dirty{false}; std::atomic<HWND> window{nullptr}; std::atomic<LONG> child{0}; std::atomic<bool> reset{false}; };
  struct Run {
    std::atomic<bool> stop{false};
    std::atomic<DWORD> threadId{0};
    Event event;
    std::mutex doneMutex; std::condition_variable doneCv; bool done = false;
  };
  static inline std::atomic<Event*> active_{nullptr};
  static inline std::atomic<int> abandoned_{0};

  static void CALLBACK hook(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD) {
    auto* self = active_.load();
    if (!self) return;
    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
      if (object != OBJID_CARET) return;
      self->window = window; self->child = child; self->dirty = true;
    } else {
      // Focus or foreground change: the old caret is gone until a new one shows.
      self->reset = true; self->dirty = true;
    }
  }

  // The caret rectangle in screen pixels, or none.
  static std::optional<RECT> fromAccessible(HWND window, LONG child) {
    if (!window) return {};
    IAccessible* accessible = nullptr; VARIANT id; VariantInit(&id);
    if (FAILED(AccessibleObjectFromEvent(window, DWORD(OBJID_CARET), DWORD(child), &accessible, &id)) || !accessible) return {};
    LONG left = 0, top = 0, width = 0, height = 0;
    const HRESULT hr = accessible->accLocation(&left, &top, &width, &height, id);
    accessible->Release(); VariantClear(&id);
    if (FAILED(hr) || width < 0 || height < 0 || (width == 0 && height == 0)) return {};
    return RECT{left, top, LONG(left + width), LONG(top + height)};
  }
  static std::optional<RECT> fromThreadInfo() {
    GUITHREADINFO info{sizeof(info)};
    if (!GetGUIThreadInfo(0, &info) || !info.hwndCaret) return {};
    POINT a{info.rcCaret.left, info.rcCaret.top}, b{info.rcCaret.right, info.rcCaret.bottom};
    if (!ClientToScreen(info.hwndCaret, &a) || !ClientToScreen(info.hwndCaret, &b)) return {};
    if (info.rcCaret.right == info.rcCaret.left && info.rcCaret.bottom == info.rcCaret.top) return {};
    return RECT{a.x, a.y, b.x, b.y};
  }

  // Test seam: runs in place of fromAccessible when set (a stand-in for a call into a hung application).
  static inline std::function<std::optional<RECT>(HWND, LONG)> accessibleOverride_;
  static std::optional<RECT> query(HWND window, LONG child) {
    if (accessibleOverride_) return accessibleOverride_(window, child);
    return fromAccessible(window, child);
  }

  static void run(const std::shared_ptr<Config>& config, const std::shared_ptr<Run>& state) {
    state->threadId = GetCurrentThreadId();
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    MSG pump; PeekMessageW(&pump, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create the queue
    Event* mine = &state->event;
    active_ = mine;
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    HWINEVENTHOOK hooks[3] = {
        SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, hook, 0, 0, flags),
        SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, hook, 0, 0, flags),
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, hook, 0, 0, flags)};
    CaretThrottle throttle;
    auto lastPoll = CaretThrottle::Clock::now() - std::chrono::seconds(1);
    state->event.reset = true; state->event.dirty = true;  // initial evaluation
    while (!state->stop) {
      const auto now = CaretThrottle::Clock::now();
      RECT area; std::string session;
      { std::lock_guard lock(config->mutex); area = config->area; session = config->sessionId; }
      std::optional<CaretReport> observed;
      if (state->event.dirty.exchange(false)) {
        const bool reset = state->event.reset.exchange(false);
        std::optional<RECT> rect = reset ? std::optional<RECT>{} : query(state->event.window.load(), state->event.child.load());
        if (state->stop) break;  // stopped while inside the (possibly long) call: report nothing
        if (!rect) rect = fromThreadInfo();
        observed = rect ? caretToOutput(*rect, area).value_or(CaretReport{}) : CaretReport{};
        lastPoll = now;
      } else if (now - lastPoll >= std::chrono::milliseconds(250)) {
        // Classic carets that raise no event while a window scrolls: a cheap
        // poll; a missing caret here never erases an event-derived one.
        lastPoll = now;
        if (const auto rect = fromThreadInfo()) observed = caretToOutput(*rect, area).value_or(CaretReport{});
      }
      std::optional<CaretReport> send;
      if (observed) { send = throttle.offer(*observed, now); } else send = throttle.due(now);
      if (send && config->emit && !state->stop) { try { config->emit(caretMessage(session, *send)); } catch (...) { } }
      const auto wait = throttle.pending() ? std::max<DWORD>(1, DWORD(throttle.wait(now).count())) : DWORD(40);
      MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      while (PeekMessageW(&pump, nullptr, 0, 0, PM_REMOVE)) {
        if (pump.message == WM_QUIT) state->stop = true;
        else { TranslateMessage(&pump); DispatchMessageW(&pump); }
      }
    }
    for (auto h : hooks) if (h) UnhookWinEvent(h);
    active_.compare_exchange_strong(mine, nullptr);  // a newer tracker's registration stays
    if (com) CoUninitialize();
    state->threadId = 0;
    { std::lock_guard lock(state->doneMutex); state->done = true; }
    state->doneCv.notify_all();
  }

  std::shared_ptr<Config> config_;
  std::chrono::milliseconds stopTimeout_;
  std::mutex lifecycle_;
  std::shared_ptr<Run> run_;
  std::thread thread_;
  std::atomic<bool> running_{false};
 public:
  // Tests only: replaces the accessibility query (nullptr restores the real one).
  static void setAccessibleQueryForTest(std::function<std::optional<RECT>(HWND, LONG)> query) { accessibleOverride_ = std::move(query); }
  // Tests only: raises a caret location event for the running tracker as the WinEvent hook would.
  static bool raiseLocationEventForTest() {
    auto* event = active_.load(); if (!event) return false;
    event->window = reinterpret_cast<HWND>(static_cast<uintptr_t>(1)); event->child = 0; event->dirty = true; return true;
  }
};}  // namespace host
