#include "resource_sampler.hpp"
#include <iostream>
#include <stdexcept>
using namespace host;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Fake { CpuTimes times; MemoryStatus memory{8000, 3000}; bool timesOk = true, memoryOk = true;
  ResourceSampler sampler() { return ResourceSampler([this](CpuTimes& o) { o = times; return timesOk; }, [this](MemoryStatus& o) { o = memory; return memoryOk; }, 4); } };
void cpu() {
  Fake f; auto s = f.sampler();
  require(!s.Sample(), "first sample has no CPU baseline");
  f.times = {750, 1000, 0};  // kernel (incl. idle) 1000, idle 750 -> 25 % busy
  auto r = s.Sample(); require(r && std::abs(r->cpuPercent - 25.0) < 1e-9 && r->cpuCount == 4, "25 % busy");
  f.times = {750 + 100, 1000 + 100, 0};  // all idle
  r = s.Sample(); require(r && r->cpuPercent == 0.0, "idle interval is 0 %");
  f.times = {850, 1100 + 100, 100};  // kernel+user 200, idle 0 -> 100 %
  r = s.Sample(); require(r && r->cpuPercent == 100.0, "fully busy is 100 %");
  r = s.Sample(); require(r && r->cpuPercent == 100.0, "no elapsed time keeps the last value");
  f.times = {10, 20, 0};  // counters went backwards
  require(!s.Sample(), "counter reset restarts the baseline");
  f.times = {10 + 50, 20 + 100, 0}; r = s.Sample(); require(r && std::abs(r->cpuPercent - 50.0) < 1e-9, "baseline restarted");
  f.timesOk = false; require(!s.Sample(), "failing time source gives nothing");
}
void memory() {
  Fake f; auto s = f.sampler(); s.Sample(); f.times = {1, 2, 0};
  auto r = s.Sample(); require(r && r->memoryTotal == 8000 && r->memoryUsed == 5000 && std::abs(r->memoryPercent() - 62.5) < 1e-9, "used is total minus available");
  f.memory = {8000, 9000}; r = s.Sample(); require(!r || r->memoryUsed == 0, "available above total clamps to 0 used");
  f.memory = {0, 0}; f.times = {5, 6, 0}; require(!s.Sample(), "zero total is rejected");
  f.memory = {8000, 3000}; f.memoryOk = false; f.times = {9, 10, 0}; require(!s.Sample(), "failing memory source gives nothing");
}
void policy() {
  using C = ResourceReportPolicy::Clock; const auto t0 = C::time_point{} + std::chrono::seconds(100);
  ResourceReportPolicy p; ResourceReading a; a.cpuPercent = 10; a.memoryTotal = 1000; a.memoryUsed = 500; a.cpuCount = 2;
  require(!p.EarlyDue(a, t0), "nothing sent yet: the regular heartbeat goes first");
  p.Sent(a, t0);
  ResourceReading b = a; b.cpuPercent = 14;
  require(!p.EarlyDue(b, t0 + std::chrono::seconds(10)), "4 points is not meaningful");
  b.cpuPercent = 15; require(p.EarlyDue(b, t0 + std::chrono::seconds(4)), "5 points CPU after 3 s sends early");
  require(!p.EarlyDue(b, t0 + std::chrono::seconds(2)), "never more often than every 3 s");
  b = a; b.memoryUsed = 440; require(p.EarlyDue(b, t0 + std::chrono::seconds(5)), "memory moved 6 points");
  p.Reset(); require(!p.EarlyDue(b, t0 + std::chrono::seconds(50)), "reset forgets what was sent");
}
void json() {
  ResourceReading r; r.cpuPercent = 12.34; r.cpuCount = 8; r.memoryUsed = 5; r.memoryTotal = 10;
  require(ResourcesJson(r) == "{\"cpuPercent\":12.3,\"cpuCount\":8,\"memoryUsedBytes\":5,\"memoryTotalBytes\":10}", "wire shape");
  r.memoryTotal = 17179869184ull; r.memoryUsed = 12884901888ull; require(ResourcesJson(r).find("12884901888") != std::string::npos, "64-bit bytes");
}
// host-resources-v2: uptime from a substituted reader, on the wire only when negotiated.
void uptime() {
  require(awakeSeconds(36000000000ull, 0) == 3600ull, "never woke: seconds since boot");
  require(awakeSeconds(36000000000ull, 35400000000ull) == 60ull, "seconds since the last wake, not since boot");
  require(awakeSeconds(36000000000ull, 36000000000ull) == 0ull, "woke just now");
  require(!awakeSeconds(1000, 2000), "a wake after now is no reading");
  Fake f; std::optional<uint64_t> up = 90061;
  ResourceSampler s([&f](CpuTimes& o) { o = f.times; return true; }, [&f](MemoryStatus& o) { o = f.memory; return true; }, 4, [&up] { return up; });
  s.Sample(); f.times = {1, 2, 0};
  auto r = s.Sample(); require(r && r->uptimeSeconds == 90061ull, "the reader's uptime is in the reading");
  require(ResourcesJson(*r, true) == "{\"cpuPercent\":50.0,\"cpuCount\":4,\"memoryUsedBytes\":5000,\"memoryTotalBytes\":8000,\"uptimeSeconds\":90061}", "negotiated wire shape");
  require(ResourcesJson(*r) == "{\"cpuPercent\":50.0,\"cpuCount\":4,\"memoryUsedBytes\":5000,\"memoryTotalBytes\":8000}", "a released broker never gets uptime");
  up.reset(); f.times = {2, 4, 0}; r = s.Sample();
  require(r && !r->uptimeSeconds && ResourcesJson(*r, true).find("uptime") == std::string::npos, "an unreadable uptime is left out, CPU and memory still go");
  Fake g; auto t = g.sampler(); t.Sample(); g.times = {1, 2, 0}; const auto x = t.Sample(); require(x && !x->uptimeSeconds, "no reader, no uptime");
}
void real() {
  auto s = ResourceSampler::System(); require(!s.Sample(), "system baseline");
  Sleep(300); auto r = s.Sample(); require(r && r->cpuCount >= 1 && r->memoryTotal > 0 && r->memoryUsed <= r->memoryTotal && r->cpuPercent >= 0 && r->cpuPercent <= 100, "real system numbers are sane");
  ULONGLONG lastWake = 0, lastSleep = 0;
  CallNtPowerInformation(LastWakeTime, nullptr, 0, &lastWake, sizeof(lastWake)); CallNtPowerInformation(LastSleepTime, nullptr, 0, &lastSleep, sizeof(lastSleep));
  std::cout << "real: uptime " << (r->uptimeSeconds ? std::to_string(*r->uptimeSeconds) : std::string("none")) << " s, tick " << GetTickCount64() / 1000 << " s, last wake " << lastWake / 10000000ull << " s, last sleep " << lastSleep / 10000000ull << " s\n";
  require(r->uptimeSeconds && *r->uptimeSeconds <= GetTickCount64() / 1000 + 1, "real uptime is read and never longer than since boot");
}
}
int main() { try { cpu(); memory(); policy(); json(); uptime(); real(); std::cout << "Host resource sampler: all checks passed.\n"; return 0; } catch (const std::exception& e) { std::cerr << "Resource sampler test failed: " << e.what() << '\n'; return 1; } }