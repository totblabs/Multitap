#ifndef MULTITAP_WINDOW_CLOSE_GUARD_H_
#define MULTITAP_WINDOW_CLOSE_GUARD_H_

#include <chrono>

namespace multitap {
// Local window lifetime only. It never cancels a host operation or stops a service.
class WindowCloseGuard {
 public:
  using Clock = std::chrono::steady_clock;
  void Update(bool busy, bool dirty) {
    busy_ = busy; dirty_ = dirty;
    if (busy) allowed_until_ = {};
  }
  // A close the app allowed passes every WM_CLOSE of that close for a few
  // seconds: ours, and the engine's re-dispatched one after the framework
  // answered its exit request (a single-use allowance would be spent on the
  // first and the exit question would come back once more).
  bool RequestClose(Clock::time_point now = Clock::now()) {
    if (busy_) return false;
    allowed_until_ = now + std::chrono::seconds(5);
    return true;
  }
  bool BlocksClose(Clock::time_point now = Clock::now()) const {
    if (now < allowed_until_) return false;
    return busy_ || dirty_;
  }
  // The app asks before closing (its own confirmation already ran).
  bool Dirty() const { return dirty_; }
 private:
  bool busy_ = false, dirty_ = false;
  Clock::time_point allowed_until_{};
};
}
#endif
