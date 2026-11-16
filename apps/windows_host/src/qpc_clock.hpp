#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace host {
// System-wide monotonic microseconds from QueryPerformanceCounter. QPC values
// are comparable between processes and threads on one machine: the Session0
// service's receive stamp, the worker's injection stamp (hostMonotonicUs,
// input_latency.hpp) and a capture's frame arrival stamp share this clock.
inline uint64_t qpcMicroseconds() {
  static const int64_t frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart > 0 ? value.QuadPart : 1; }();
  LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
  const auto ticks = uint64_t(std::max<int64_t>(0, now.QuadPart));
  return ticks / uint64_t(frequency) * 1000000 + ticks % uint64_t(frequency) * 1000000 / uint64_t(frequency);
}
}
