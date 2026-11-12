#pragma once
#include <atomic>

namespace host {
// Recovery runs outside the service loop. Its transitions must remain visible
// even if the loop consumes ordinary display invalidation before recovery exits.
class RecoveryReadiness {
 public:
  operator bool() const noexcept { return running_.load(); }
  RecoveryReadiness& operator=(bool running) noexcept {
    if (running_.exchange(running) != running) changed_ = true;
    return *this;
  }
  bool takeChanged() noexcept { return changed_.exchange(false); }
 private:
  std::atomic<bool> running_{false}, changed_{false};
};
}
