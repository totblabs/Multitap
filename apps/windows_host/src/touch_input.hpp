#pragma once
// touch-input-v1 (docs/remote-session-protocol.md "Touch input"): the touch
// contacts of a client's touch screen (the Android app's direct touch mode),
// injected as Windows touch input so applications get real pans, pinch-zoom
// and press-and-hold instead of an emulated mouse.
//
// Injection uses InitializeTouchInjection/InjectTouchInput (Windows 8+), not
// a CreateSyntheticPointerDevice/InjectSyntheticPointerInput device: its
// coordinates are documented as ordinary screen coordinates within the
// desktop (the synthetic device's are documented "relative to the top left of
// the virtual screen", which differs whenever an owned output sits left of or
// above the primary), and its frame, flag, cancel and timing rules are
// documented. Remote desktop hosts (e.g. Chrome Remote Desktop's Windows host)
// use the same call. Its context belongs to the process, i.e. to this
// per-session interactive worker, and is created on the first touch frame of
// a granted session. Like SendInput it injects into the worker's own desktop
// with the worker's (linked, elevated) token, so elevated windows accept it
// the same way as the mouse; the secure desktop is never targeted.
//
// Windows rules followed here (InjectTouchInput remarks):
// - every injection lists all active contacts; a new contact is DOWN, a held
//   one UPDATE, an ending one UP (UP|CANCELED for a cancel);
// - UP must repeat the contact's previous position, so a client "up" at a new
//   position is preceded by an UPDATE there;
// - a held contact needs repeated frames or Windows cancels it, so held
//   contacts are refreshed every 50 ms while no client frame arrives;
// - ERROR_NOT_READY (calls < 0.1 ms apart) is retried.
// Any other failure (desktop switch, display change, refused frame) cancels
// and forgets every contact. release() (input.reset: focus loss, disconnect,
// session end, resize, worker stop) does the same. A contact Windows still
// holds after the injector stops refreshing it, or after the worker process
// ends, is cancelled by Windows itself.
#include "support.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace host {
inline constexpr const char* touchInputProtocol = "touch-input-v1";
// Contacts per frame and held at once (the injection context's maximum).
inline constexpr size_t touchInputMaxContacts = 10;
// Refresh period of held contacts, and how long contacts may be held without
// any client frame before they are cancelled (a stuck client cannot hold a
// remote contact forever).
inline constexpr std::chrono::milliseconds touchKeepAlivePeriod{50};
inline constexpr std::chrono::seconds touchHoldLimit{30};

// session.start of an ordinary stream session whose broker granted it
// (independent outputs keep pointer/key input only).
inline bool touchInputSessionRequested(const Json& start) {
  return start.get("type").string() == "session.start" && start.get("mode").string() == "stream" &&
         start.get("touchInputProtocol").string() == touchInputProtocol && start.get("mediaProtocol").string().empty();
}

enum class TouchPhase { down, move, up, cancel };
struct TouchContactInput {
  uint32_t id = 0;
  TouchPhase phase = TouchPhase::move;
  double x = 0, y = 0;  // 0..1 over the streamed source rectangle
  std::optional<double> pressure;  // 0..1
  std::optional<double> width, height;  // fractions of the source size, (0, 1]
};

// The contacts of one valid {"type":"input","event":"touch","contacts":[...]}
// message (an optional sessionId added by the service is the only other
// field). Refused as a whole: 0 or more than 10 contacts, duplicate ids, ids
// outside 0..2^31-1, unknown phases, coordinates or pressure outside 0..1,
// a size without both w and h (each > 0 and <= 1) or any other field.
inline std::optional<std::vector<TouchContactInput>> touchInputFrame(const Json& message) {
  const auto* fields = std::get_if<Json::Object>(&message.value);
  if (!fields || message.get("type").string() != "input" || message.get("event").string() != "touch") return {};
  for (const auto& [key, item] : *fields) {
    (void)item;
    if (key != "type" && key != "event" && key != "contacts" && key != "sessionId") return {};
  }
  const auto* contacts = std::get_if<Json::Array>(&message.get("contacts").value);
  if (!contacts || contacts->empty() || contacts->size() > touchInputMaxContacts) return {};
  const auto unit = [](const Json& value) -> std::optional<double> {
    const auto* number = std::get_if<double>(&value.value);
    if (!number || !std::isfinite(*number) || *number < 0 || *number > 1) return {};
    return *number;
  };
  std::vector<TouchContactInput> frame;
  for (const auto& item : *contacts) {
    const auto* contact = std::get_if<Json::Object>(&item.value);
    if (!contact) return {};
    for (const auto& [key, value] : *contact) {
      (void)value;
      if (key != "id" && key != "phase" && key != "x" && key != "y" && key != "pressure" && key != "w" && key != "h") return {};
    }
    TouchContactInput parsed;
    const auto* id = std::get_if<double>(&item.get("id").value);
    if (!id || *id < 0 || *id > 2147483647.0 || std::floor(*id) != *id) return {};
    parsed.id = uint32_t(*id);
    const auto* phase = std::get_if<std::string>(&item.get("phase").value);
    if (!phase) return {};
    if (*phase == "down") parsed.phase = TouchPhase::down;
    else if (*phase == "move") parsed.phase = TouchPhase::move;
    else if (*phase == "up") parsed.phase = TouchPhase::up;
    else if (*phase == "cancel") parsed.phase = TouchPhase::cancel;
    else return {};
    const auto x = unit(item.get("x")), y = unit(item.get("y"));
    if (!x || !y) return {};
    parsed.x = *x; parsed.y = *y;
    if (contact->contains("pressure")) { parsed.pressure = unit(item.get("pressure")); if (!parsed.pressure) return {}; }
    if (contact->contains("w") != contact->contains("h")) return {};
    if (contact->contains("w")) {
      parsed.width = unit(item.get("w")); parsed.height = unit(item.get("h"));
      if (!parsed.width || !parsed.height || *parsed.width <= 0 || *parsed.height <= 0) return {};
    }
    for (const auto& previous : frame) if (previous.id == parsed.id) return {};
    frame.push_back(parsed);
  }
  return frame;
}

// The pixel a normalized position names in the streamed source rectangle
// (screen coordinates; the same mapping as the mouse path). The area must be
// at least 2x2 and lie inside the desktop.
inline std::optional<POINT> touchInputPoint(double x, double y, const RECT& area) {
  const int64_t width = int64_t(area.right) - area.left, height = int64_t(area.bottom) - area.top;
  if (width < 2 || height < 2 || !std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1) return {};
  return POINT{LONG(area.left + std::llround(x * double(width - 1))), LONG(area.top + std::llround(y * double(height - 1)))};
}

// The injection calls, substitutable so tests emit no OS input. initialize:
// InitializeTouchInjection; inject: InjectTouchInput, returning 0 or the
// Win32 error. An empty backend refuses every frame.
struct TouchInjectionBackend {
  std::function<bool()> initialize;
  std::function<DWORD(const std::vector<POINTER_TOUCH_INFO>&)> inject;
  explicit operator bool() const { return initialize && inject; }
};
inline TouchInjectionBackend nativeTouchInjection() {
  return {[] { return InitializeTouchInjection(UINT32(touchInputMaxContacts), TOUCH_FEEDBACK_DEFAULT) != FALSE; },
          [](const std::vector<POINTER_TOUCH_INFO>& frame) -> DWORD {
            if (frame.empty() || frame.size() > touchInputMaxContacts) return ERROR_INVALID_PARAMETER;
            return InjectTouchInput(UINT32(frame.size()), frame.data()) ? DWORD(0) : GetLastError();
          }};
}

// The held contacts of one worker and their injection. Thread-safe; the
// optional keep-alive thread starts with the first held contact.
class RemoteTouch {
 public:
  using TimePoint = std::chrono::steady_clock::time_point;
  struct Stats { uint64_t frames = 0, injections = 0, failures = 0, keepAlives = 0; DWORD lastError = 0; };
  RemoteTouch(TouchInjectionBackend backend, std::function<RECT()> bounds, bool automaticKeepAlive)
      : backend_(std::move(backend)), bounds_(std::move(bounds)), automatic_(automaticKeepAlive) {}
  ~RemoteTouch() {
    { std::lock_guard lock(mutex_); closing_ = true; releaseLocked(); }
    wake_.notify_all();
    if (keeper_.joinable()) keeper_.join();
  }
  RemoteTouch(const RemoteTouch&) = delete;
  RemoteTouch& operator=(const RemoteTouch&) = delete;

  // One validated client frame, mapped onto `area` (the streamed source
  // rectangle in screen coordinates). False when refused: nothing of it was
  // injected (bad sequence, too many contacts, no area, no backend), or an
  // injection failed (then every contact was cancelled and forgotten).
  // move/up/cancel of a contact this worker does not hold (released by a
  // reset the client had not seen yet) are skipped.
  bool apply(const std::vector<TouchContactInput>& frame, const RECT& area, TimePoint now) {
    std::lock_guard lock(mutex_);
    if (closing_ || !backend_ || frame.empty() || frame.size() > touchInputMaxContacts) return false;
    ++stats_.frames;
    if (!areaInsideDesktop(area)) return false;
    struct Planned { const TouchContactInput* contact; int slot; POINT at; };
    std::vector<Planned> downs, moves, ends;
    const size_t holding = active();
    for (const auto& contact : frame) {
      const int slot = find(contact.id);
      if (contact.phase == TouchPhase::down) {
        if (slot >= 0) return false;  // already down: a client sequencing error
      } else if (slot < 0) continue;  // released already
      const auto at = touchInputPoint(contact.x, contact.y, area);
      if (!at) return false;
      if (contact.phase == TouchPhase::down) downs.push_back({&contact, -1, *at});
      else if (contact.phase == TouchPhase::move) moves.push_back({&contact, slot, *at});
      else ends.push_back({&contact, slot, *at});
    }
    if (holding - ends.size() + downs.size() > touchInputMaxContacts) return false;
    if (downs.empty() && moves.empty() && ends.empty()) return true;
    if (!initialized_) {
      if (!backend_.initialize()) { stats_.lastError = GetLastError(); ++stats_.failures; return false; }
      initialized_ = true;
    }
    lastClient_ = now;
    // 1. Updates: every move (a new position, pressure or size, or only "still
    //    here"), and an "up" at a new position (UP must repeat the previous
    //    UPDATE's position). A cancel ends where the contact was.
    bool update = !moves.empty();
    for (const auto& item : moves) (void)updateSlot(slots_[size_t(item.slot)], *item.contact, item.at, area);
    for (const auto& item : ends) if (item.contact->phase == TouchPhase::up) update |= updateSlot(slots_[size_t(item.slot)], *item.contact, item.at, area);
    if (update && !inject(frameLocked({}), now)) return false;
    // 2. Ends: UP (or UP|CANCELED), every other held contact UPDATE.
    if (!ends.empty()) {
      std::array<DWORD, touchInputMaxContacts> flags{};
      for (const auto& item : ends) flags[size_t(item.slot)] = item.contact->phase == TouchPhase::up ? POINTER_FLAG_UP : POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
      if (!inject(frameLocked(flags), now)) return false;
      for (const auto& item : ends) slots_[size_t(item.slot)] = {};
    }
    // 3. New contacts: DOWN, every other held contact UPDATE.
    if (!downs.empty()) {
      std::array<DWORD, touchInputMaxContacts> flags{};
      for (auto& item : downs) {
        item.slot = freeSlot();
        auto& slot = slots_[size_t(item.slot)];
        slot = {}; slot.active = true; slot.client = item.contact->id; slot.at = item.at;
        (void)updateSlot(slot, *item.contact, item.at, area);
        flags[size_t(item.slot)] = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
      }
      if (!inject(frameLocked(flags), now)) return false;
      startKeeperLocked();
    }
    return true;
  }
  // Re-injects held contacts when no frame was injected for a keep-alive
  // period, and cancels them after the hold limit without client frames.
  // Called by the keep-alive thread (or directly by tests).
  void tick(TimePoint now) {
    std::lock_guard lock(mutex_);
    if (!active()) return;
    if (now - lastClient_ >= touchHoldLimit) { releaseLocked(); return; }
    if (now - lastInjected_ < touchKeepAlivePeriod) return;
    ++stats_.keepAlives;
    (void)inject(frameLocked({}), now);
  }
  // Cancels and forgets every held contact (input.reset and every stop path).
  void release() { std::lock_guard lock(mutex_); releaseLocked(); }
  size_t held() { std::lock_guard lock(mutex_); return active(); }
  Stats stats() { std::lock_guard lock(mutex_); return stats_; }

 private:
  struct Slot {
    bool active = false; uint32_t client = 0; POINT at{};
    std::optional<UINT32> pressure; std::optional<RECT> contact;
  };
  size_t active() const { size_t count = 0; for (const auto& slot : slots_) count += slot.active ? 1 : 0; return count; }
  int find(uint32_t client) const { for (size_t n = 0; n < slots_.size(); ++n) if (slots_[n].active && slots_[n].client == client) return int(n); return -1; }
  int freeSlot() const { for (size_t n = 0; n < slots_.size(); ++n) if (!slots_[n].active) return int(n); return -1; }
  bool areaInsideDesktop(const RECT& area) const {
    if (int64_t(area.right) - area.left < 2 || int64_t(area.bottom) - area.top < 2 || !bounds_) return false;
    RECT desktop{};
    try { desktop = bounds_(); } catch (...) { return false; }
    return area.left >= desktop.left && area.top >= desktop.top && area.right <= desktop.right && area.bottom <= desktop.bottom;
  }
  // New position, pressure and contact rectangle of one held contact; true
  // when its position changed.
  static bool updateSlot(Slot& slot, const TouchContactInput& contact, POINT at, const RECT& area) {
    const bool moved = slot.at.x != at.x || slot.at.y != at.y;
    slot.at = at;
    if (contact.pressure) slot.pressure = UINT32(std::lround(*contact.pressure * 1024));
    if (contact.width && contact.height) {
      const LONG halfWidth = std::max<LONG>(1, LONG(std::lround(*contact.width * double(area.right - area.left) / 2)));
      const LONG halfHeight = std::max<LONG>(1, LONG(std::lround(*contact.height * double(area.bottom - area.top) / 2)));
      slot.contact = RECT{std::max(area.left, at.x - halfWidth), std::max(area.top, at.y - halfHeight),
                          std::min(area.right, at.x + halfWidth + 1), std::min(area.bottom, at.y + halfHeight + 1)};
    } else if (slot.contact) {
      // Keep the previous size around the new position.
      const LONG halfWidth = std::max<LONG>(1, (slot.contact->right - slot.contact->left) / 2), halfHeight = std::max<LONG>(1, (slot.contact->bottom - slot.contact->top) / 2);
      slot.contact = RECT{std::max(area.left, at.x - halfWidth), std::max(area.top, at.y - halfHeight),
                          std::min(area.right, at.x + halfWidth + 1), std::min(area.bottom, at.y + halfHeight + 1)};
    }
    return moved;
  }
  // All held contacts in slot order; flags[slot] overrides the default
  // UPDATE|INRANGE|INCONTACT of that contact.
  std::vector<POINTER_TOUCH_INFO> frameLocked(const std::array<DWORD, touchInputMaxContacts>& flags) const {
    std::vector<POINTER_TOUCH_INFO> frame;
    for (size_t n = 0; n < slots_.size(); ++n) {
      const auto& slot = slots_[n];
      if (!slot.active) continue;
      POINTER_TOUCH_INFO info{};
      info.pointerInfo.pointerType = PT_TOUCH;
      info.pointerInfo.pointerId = UINT32(n);
      info.pointerInfo.ptPixelLocation = slot.at;
      info.pointerInfo.pointerFlags = flags[n] ? flags[n] : POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
      info.touchFlags = TOUCH_FLAG_NONE;
      info.touchMask = TOUCH_MASK_NONE;
      if (slot.pressure) { info.touchMask |= TOUCH_MASK_PRESSURE; info.pressure = std::min<UINT32>(*slot.pressure, 1024); }
      if (slot.contact) { info.touchMask |= TOUCH_MASK_CONTACTAREA; info.rcContact = *slot.contact; }
      frame.push_back(info);
    }
    return frame;
  }
  // One injection; ERROR_NOT_READY is retried. Any other failure cancels
  // and forgets every contact.
  bool inject(const std::vector<POINTER_TOUCH_INFO>& frame, TimePoint now) {
    if (frame.empty()) return true;
    DWORD error = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
      ++stats_.injections;
      error = backend_.inject(frame);
      if (error != ERROR_NOT_READY) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!error) { lastInjected_ = now; return true; }
    ++stats_.failures; stats_.lastError = error;
    releaseLocked();
    return false;
  }
  void releaseLocked() {
    if (active() && backend_ && initialized_) {
      std::vector<POINTER_TOUCH_INFO> cancel;
      for (size_t n = 0; n < slots_.size(); ++n) if (slots_[n].active) {
        POINTER_TOUCH_INFO info{};
        info.pointerInfo.pointerType = PT_TOUCH;
        info.pointerInfo.pointerId = UINT32(n);
        info.pointerInfo.ptPixelLocation = slots_[n].at;
        info.pointerInfo.pointerFlags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
        cancel.push_back(info);
      }
      // Best effort: after a desktop switch or display change Windows has
      // already cancelled them and refuses this frame.
      for (int attempt = 0; attempt < 4; ++attempt) {
        ++stats_.injections;
        if (backend_.inject(cancel) != ERROR_NOT_READY) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    slots_ = {};
  }
  void startKeeperLocked() {
    if (!automatic_ || closing_ || keeper_.joinable()) { wake_.notify_all(); return; }
    keeper_ = std::thread([this] {
      std::unique_lock lock(mutex_);
      while (!closing_) {
        wake_.wait(lock, [this] { return closing_ || active() > 0; });
        if (closing_) break;
        if (wake_.wait_for(lock, touchKeepAlivePeriod, [this] { return closing_; })) break;
        lock.unlock(); tick(std::chrono::steady_clock::now()); lock.lock();
      }
    });
  }
  TouchInjectionBackend backend_; std::function<RECT()> bounds_; bool automatic_ = false;
  std::mutex mutex_; std::condition_variable wake_; std::thread keeper_; bool closing_ = false, initialized_ = false;
  std::array<Slot, touchInputMaxContacts> slots_{};
  TimePoint lastInjected_{}, lastClient_{};
  Stats stats_;
};
}  // namespace host
