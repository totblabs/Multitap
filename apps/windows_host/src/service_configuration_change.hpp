#pragma once
#include <atomic>
#include <cstdint>
#include <utility>

namespace host {
inline constexpr uint32_t hostServiceReloadControl = 128;
inline constexpr uint32_t hostServiceCapabilitiesControl = 129;

// SCM settings notifications are distinct from WTS session transitions. Saving
// an account only requests a fresh observation/publication; it must not stop an
// active session, invalidate its display or reload the broker connection.
class ServiceConfigurationChanges {
 public:
  bool request(uint32_t control) noexcept {
    if (control == hostServiceReloadControl) reload_ = true;
    else if (control == hostServiceCapabilitiesControl) capabilities_ = true;
    else return false;
    return true;
  }
  template<class Reload, class Sessions>
  void processSessionChanges(bool haveSessionChanges, Reload&& reload, Sessions&& sessions) {
    if (reload_.exchange(false)) std::forward<Reload>(reload)();
    else if (haveSessionChanges) std::forward<Sessions>(sessions)();
  }
  bool takeCapabilitiesRequest() noexcept { return capabilities_.exchange(false); }
 private:
  std::atomic<bool> reload_{false}, capabilities_{false};
};

// A committed settings write stays acknowledged even if its follow-up SCM
// notification fails (old/stopped Host, access error). The regular heartbeat
// still reads fresh state. A failed write never sends a notification.
template<class Apply, class Refresh>
bool applyThenRequestCapabilities(Apply&& apply, Refresh&& refresh) {
  std::forward<Apply>(apply)();
  try { return std::forward<Refresh>(refresh)(); }
  catch (...) { return false; }
}
}
