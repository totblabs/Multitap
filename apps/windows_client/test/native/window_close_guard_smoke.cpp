// The close guard's allowance covers every WM_CLOSE of one confirmed close:
// the runner's own and the engine's re-dispatched one after the framework's
// exit answer (the exit question used to appear twice).
#include "window_close_guard.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void requireAt(bool ok, int line) {
  if (!ok) throw std::runtime_error("Window close guard QA failed at line " + std::to_string(line));
}
#define require(ok) requireAt((ok), __LINE__)
}  // namespace

int main() {
  try {
    using Clock = multitap::WindowCloseGuard::Clock;
    const auto t0 = Clock::now();
    multitap::WindowCloseGuard guard;
    require(!guard.BlocksClose(t0));  // nothing to protect yet
    guard.Update(false, true);
    require(guard.BlocksClose(t0));  // the app asks first
    require(guard.RequestClose(t0));
    // Ours, then the engine's re-dispatched close: both pass.
    require(!guard.BlocksClose(t0));
    require(!guard.BlocksClose(t0 + std::chrono::milliseconds(300)));
    // A later, separate close asks again.
    require(guard.BlocksClose(t0 + std::chrono::seconds(6)));
    // Busy work refuses and withdraws an allowance.
    require(guard.RequestClose(t0));
    guard.Update(true, true);
    require(guard.BlocksClose(t0));
    require(!guard.RequestClose(t0));
    guard.Update(false, false);
    require(!guard.BlocksClose(t0));
    std::cout << "{\"passed\":true,\"windowCloseGuard\":true}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
