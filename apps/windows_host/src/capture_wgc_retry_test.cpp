// capture_wgc_retry.hpp: when a capture left on GDI tries WGC again, and the
// fixed code of a WGC failure. Pure; no capture.
#include "capture_wgc_retry.hpp"
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
using namespace std::chrono_literals;
using Clock = host::WgcRetry::Clock;

void codes() {
  require(host::wgcFailureCode("wgc_hresult_0x80070005") == 0x80070005u, "A WinRT HRESULT was not read");
  require(host::wgcFailureCode("Capture selected monitor (0x887a0004)") == 0x887a0004u, "A checked HRESULT was not read");
  require(host::wgcFailureCode("Capture monitor pixel geometry mismatch") == 2, "A geometry mismatch has its own code");
  require(host::wgcFailureCode("Selected displays are not contiguous") == 3 && host::wgcFailureCode("Selected display rectangle cuts a monitor") == 4, "Topology codes differ");
  require(host::wgcFailureCode("Windows Graphics Capture unavailable") == 5, "An unavailable WGC has its own code");
  require(host::wgcFailureCode("") == 1 && host::wgcFailureCode("something else") == 1 && host::wgcFailureCode("x (0x)") == 1, "Unknown text is 1");
  require(host::wgcFailureCode("wgc_hresult_0x123456789") == 1, "More than eight hex digits is no HRESULT");
  require(host::wgcFailureCode("wgc_hresult_0x0") == 1, "A zero HRESULT is no failure code");
}

void schedule() {
  host::WgcRetry retry; const auto t = Clock::time_point{} + 100s;
  require(!retry.built(false, t) && !retry.onGdi() && !retry.due(t + 1h), "A WGC capture scheduled a retry");
  require(retry.built(true, t), "Entering GDI asked for no record");
  require(retry.onGdi() && !retry.due(t + 499ms) && retry.due(t + 500ms), "The first try is not after 500 ms");
  // Each try that comes back on GDI waits longer, up to 15 s.
  auto now = t + 500ms; std::chrono::milliseconds expected[] = {1000ms, 2000ms, 4000ms, 8000ms, 15000ms, 15000ms, 15000ms};
  for (const auto wait : expected) {
    retry.failed(now);
    require(!retry.due(now + wait - 1ms) && retry.due(now + wait), "A retry step is wrong");
    now += wait;
  }
  // A rebuild on GDI again (a resize) restarts the steps without a second record.
  require(!retry.built(true, now) && retry.due(now + 500ms) && !retry.due(now + 499ms), "A GDI rebuild did not restart the steps, or recorded twice");
  require(!retry.takeRecovered(), "A recovery was reported while still on GDI");
  // Back on WGC: no tries, and how long GDI lasted, once.
  require(!retry.built(false, now + 2s) && !retry.onGdi() && !retry.due(now + 1h), "WGC kept the retries going");
  const auto lasted = retry.takeRecovered();
  require(lasted && *lasted == now + 2s - t && !retry.takeRecovered(), "The GDI period was not reported once");
  // A later GDI period records again.
  require(retry.built(true, now + 10s), "A second GDI period asked for no record");
}
}

int main() {
  try {
    codes(); schedule();
    std::cout << "{\"kind\":\"capture-wgc-retry\",\"checks\":" << checks << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "capture WGC retry check failed: " << error.what() << "\n";
    return 1;
  }
}
