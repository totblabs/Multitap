#include "process_load.h"
#include <iostream>
using namespace multitap;
static int failures = 0;
static void Expect(const char* name, int got, int want) {
  if (got != want) { ++failures; std::cout << "FAIL " << name << " got=" << got << " want=" << want << "\n"; }
}
int main() {
  // 1 s wall = 10,000,000 units. 4 CPUs.
  Expect("idle process", ProcessCpuPct10({0, 0}, {0, 0}, 10000000, 4), 0);
  // 1 CPU-second of 4 CPU-seconds available = 25.0%.
  Expect("quarter machine", ProcessCpuPct10({100, 200}, {100 + 4000000, 200 + 6000000}, 10000000, 4), 250);
  // Whole machine busy = 100.0%; overshoot (timer skew) clamps.
  Expect("full", ProcessCpuPct10({0, 0}, {20000000, 20000000}, 10000000, 4), 1000);
  Expect("clamped", ProcessCpuPct10({0, 0}, {90000000, 0}, 10000000, 4), 1000);
  Expect("rounding", ProcessCpuPct10({0, 0}, {0, 1234567}, 10000000, 1), 123);
  Expect("no wall time", ProcessCpuPct10({0, 0}, {1, 1}, 0, 4), -1);
  Expect("no cpus", ProcessCpuPct10({0, 0}, {1, 1}, 10, 0), -1);
  Expect("counter went back", ProcessCpuPct10({10, 10}, {5, 10}, 10000000, 4), -1);
  // Kernel time includes idle: 100 total (kernel 70 incl. idle 60, user 30) -> 40% busy.
  Expect("system 40", SystemCpuPct10({1000, 5000, 2000}, {1060, 5070, 2030}), 400);
  Expect("system idle", SystemCpuPct10({0, 0, 0}, {100, 100, 0}), 0);
  Expect("system full", SystemCpuPct10({0, 0, 0}, {0, 50, 50}), 1000);
  Expect("system no delta", SystemCpuPct10({1, 1, 1}, {1, 1, 1}), -1);
  Expect("system backwards", SystemCpuPct10({10, 10, 10}, {5, 20, 20}), -1);
  Expect("system idle above total", SystemCpuPct10({0, 0, 0}, {90, 50, 10}), -1);
  // Real readings must be usable (non-negative) after a short busy loop.
  ProcessTimes p0, p1; SystemTimes s0, s1;
  FILETIME c, e, k, u, i;
  GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
  p0 = {(uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime, (uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime};
  GetSystemTimes(&i, &k, &u);
  s0 = {(uint64_t(i.dwHighDateTime) << 32) | i.dwLowDateTime, (uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime, (uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime};
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  volatile uint64_t spin = 0;
  while (std::chrono::steady_clock::now() < until) spin = spin + 1;
  GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
  p1 = {(uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime, (uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime};
  GetSystemTimes(&i, &k, &u);
  s1 = {(uint64_t(i.dwHighDateTime) << 32) | i.dwLowDateTime, (uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime, (uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime};
  SYSTEM_INFO info; GetSystemInfo(&info);
  const int app = ProcessCpuPct10(p0, p1, 3000000, info.dwNumberOfProcessors), sys = SystemCpuPct10(s0, s1);
  if (app < 0 || sys < 0) { ++failures; std::cout << "FAIL live readings app=" << app << " sys=" << sys << "\n"; }
  // The sampler thread starts, answers without blocking and stops cleanly.
  const auto snap = ProcessLoadSampler::Instance().Latest();
  std::cout << "process load math failures=" << failures << " liveApp10=" << app << " liveSystem10=" << sys
            << " samplerFirst=" << snap.app_cpu << "\n";
  return failures == 0 ? 0 : 1;
}

