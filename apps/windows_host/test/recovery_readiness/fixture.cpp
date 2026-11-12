// Production notification helper only. No service, SDK, display, process or
// Windows API is included or invoked. Interleavings are replayed synchronously.
#include "../../src/recovery_readiness.hpp"
#include <atomic>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
int checks = 0;
void require(bool result, const char* message) {
  ++checks;
  if (!result) throw std::runtime_error(message);
}
struct ServiceLoop {
  host::RecoveryReadiness recovering;
  std::atomic<bool> displayChanged{false};
  std::optional<bool> announce() {
    const bool ordinary = displayChanged.exchange(false);
    if (!recovering.takeChanged() && !ordinary) return {};
    return bool(recovering); // true advertises maintenance, false is idle.
  }
};
void run() {
  ServiceLoop a, b;
  require(!a.recovering && !a.announce(), "Idle service manufactured maintenance");
  a.recovering = true;
  require(bool(a.recovering), "Beginning recovery did not close admission");
  require(a.announce() == true, "Recovery begin did not publish maintenance");
  require(!a.announce(), "Unchanged recovery flooded announcements");
  require(!b.recovering && !b.announce(), "One VM's recovery affected another VM");
  a.recovering = true;
  require(!a.announce(), "Repeated begin manufactured a transition");

  // The restore routine invalidates its display before the worker wrapper
  // clears its running flag. Consuming that invalidation must not swallow the
  // separate transition to idle, or the broker can retain maintenance forever.
  a.displayChanged = true;
  require(a.announce() == true, "An in-flight restore advertised idle");
  require(!a.announce(), "The ordinary invalidation was not consumed");
  a.recovering = false;
  require(!a.recovering, "Worker exit left admission busy");
  require(a.announce() == false, "Worker exit after invalidation lost its idle announcement");
  require(!a.announce(), "Idle transition flooded announcements");

  // A failed worker launch can begin and unwind before the next service tick.
  // Publish the final idle state once; never leave a false busy latch.
  a.recovering = true;
  a.recovering = false;
  require(a.announce() == false, "Failed launch left stale maintenance");
  require(!a.announce(), "Failed launch did not coalesce its changes");

  // An ordinary invalidation and an independent recovery transition are both
  // consumed, so neither repeatedly masks the other at later ticks.
  b.recovering = true;
  b.displayChanged = true;
  require(b.announce() == true, "Combined change did not advertise busy");
  require(!b.announce(), "Combined change left an old notification pending");
  require(!a.recovering && !a.announce(), "Another VM reopened an unrelated gate");
  b.recovering = false;
  require(b.announce() == false, "Second VM did not reopen independently");
  for (int i = 0; i < 64; ++i) b.recovering = false;
  require(!b.announce(), "Unchanged idle state generated work");
}
}
int main() {
  try {
    run();
    std::cout << "PASS " << checks << " production recovery-readiness checks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << checks << ": " << error.what() << '\n';
    return 1;
  }
}
