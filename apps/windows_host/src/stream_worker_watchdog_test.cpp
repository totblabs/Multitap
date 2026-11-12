#include "stream_worker_watchdog.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point at(int64_t millis) { return Clock::time_point(std::chrono::milliseconds(millis)); }
unsigned checks = 0;
void require(bool accepted, const char* message) { if (!accepted) throw std::runtime_error(message); ++checks; }
}
int main() {
  try {
    // These handlers used to continue before the shared no-video check.
    auto last = at(0);
    unsigned dispatched = 0;
    for (const int64_t now : std::array<int64_t, 6>{1000, 3000, 7000, 12000, 15000, 15001}) {
      if (!host::streamWorkerVideoAlive(last, at(now), 0, now)) break;
      ++dispatched;  // A handled control packet cannot bypass the common gate.
    }
    require(dispatched == 5, "Control traffic starved the video watchdog or changed its grace");
    require(last == at(0), "Control traffic refreshed the video clock");
    for (int64_t now = 1000; now <= 120000; now += 1000) {
      last = at(now);  // The service's actual video handler owns this refresh.
      require(host::streamWorkerVideoAlive(last, at(now), 0, now), "Healthy video was stopped");
    }
    require(last == at(120000), "Actual video did not refresh its clock");
    // Preserve the old behavior: an actual frame received by this iteration
    // refreshes the clock before the watchdog judges that packet.
    last = at(16000);
    require(host::streamWorkerVideoAlive(last, at(16000), 0, 16000), "A received video frame was judged before its refresh");
    require(host::streamWorkerVideoAlive(last, at(31000), 0, 31000), "The exact 15-second boundary changed");
    require(!host::streamWorkerVideoAlive(last, at(31001), 0, 31001), "Audio/cursor/control traffic kept stale video alive");
    last = at(0);
    require(host::streamWorkerVideoAlive(last, at(30000), 90000, 30000), "An active resize lost its separate deadline");
    require(last == at(0), "A resize exemption invented a video frame");
    require(!host::streamWorkerVideoAlive(last, at(90000), 90000, 90000), "An expired resize extended the video exemption");
    // resizePacket/trackResizePacket retain their explicit clock refresh when
    // the verified resize completes; the watchdog does not replace that proof.
    last = at(90000);
    require(host::streamWorkerVideoAlive(last, at(104999), 0, 104999), "A verified resize lost its restart grace");
    require(!host::streamWorkerVideoAlive(last, at(105001), 0, 105001), "Resize completion granted an unlimited grace");
    std::cout << "{\"kind\":\"worker-video-watchdog\",\"checks\":" << checks
              << ",\"fakeClock\":true,\"runtimeWorker\":false,\"vmAccess\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
