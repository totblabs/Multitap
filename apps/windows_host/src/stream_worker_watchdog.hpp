#pragma once
#include <chrono>
#include <cstdint>

namespace host {
// Called after packet handling, before a handled control packet can continue
// the receive loop. Actual video and verified resize completion retain their
// existing clock refresh. Startup preparation and stop ACKs stay outside.
inline bool streamWorkerVideoAlive(std::chrono::steady_clock::time_point lastVideo,
                                  std::chrono::steady_clock::time_point now,
                                  int64_t resizeDeadline, int64_t monotonicMillis) {
  return now - lastVideo <= std::chrono::seconds(15) || resizeDeadline > monotonicMillis;
}
}
