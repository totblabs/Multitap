// stream_gpu_priority.hpp: the class the streaming worker asks for (REALTIME
// without HAGS, HIGH with it or unknown, HIGH when REALTIME is refused), what
// it leaves alone, what it restores, and the fixed diagnostic. Substituted
// access; the real one is only read (this process's class), never changed.
#include "stream_gpu_priority.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace host;
namespace {
unsigned checks = 0;
void require(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
struct Fake {
  std::optional<bool> hags = false;
  bool privilege = true;
  int current = StreamGpuPriority::normal;
  std::vector<int> refused;
  std::vector<std::string> calls;
  GpuPriorityAccess access() {
    GpuPriorityAccess value;
    value.hags = [this](uint64_t luid) { calls.push_back("hags:" + std::to_string(luid)); return hags; };
    value.privilege = [this] { calls.push_back("privilege"); return privilege; };
    value.get = [this]() -> std::optional<int> { return current; };
    value.set = [this](int wanted) {
      calls.push_back("set:" + std::to_string(wanted));
      for (const int no : refused) if (no == wanted) return false;
      current = wanted; return true;
    };
    return value;
  }
};
constexpr uint64_t adapter = 0x0000000100002a3bull;

void noAdapterChangesNothing() {
  Fake fake;
  { StreamGpuPriority priority(std::nullopt, fake.access()); require(!priority.attempted() && !priority.applied() && priority.diagnostic(false) == 0, "A GPU-less capture was raised"); }
  require(fake.calls.empty() && fake.current == StreamGpuPriority::normal, "A GPU-less capture touched the GPU scheduling");
}

void classFollowsHardwareScheduling() {
  Fake off;
  {
    StreamGpuPriority priority(adapter, off.access());
    require(priority.applied() == StreamGpuPriority::realtime && off.current == StreamGpuPriority::realtime, "Without HAGS the class is not REALTIME");
    require(off.calls.front() == "hags:" + std::to_string(adapter), "HAGS was not read for the capture's adapter");
    require(priority.diagnostic(true) == (5u | 5u << 4 | 0x100u | 0x400u | 0x800u), "Diagnostic of REALTIME without HAGS");
  }
  require(off.current == StreamGpuPriority::normal, "The previous class did not come back");
  Fake on; on.hags = true;
  { StreamGpuPriority priority(adapter, on.access()); require(priority.applied() == StreamGpuPriority::high && priority.diagnostic(false) == (4u | 4u << 4 | 0x300u | 0x400u), "With HAGS the class is not HIGH"); }
  require(on.current == StreamGpuPriority::normal, "HIGH was not undone");
  Fake unknown; unknown.hags = std::nullopt;
  { StreamGpuPriority priority(adapter, unknown.access()); require(priority.applied() == StreamGpuPriority::high && (priority.diagnostic(false) & 0x300u) == 0, "An unreadable HAGS state did not take the safer HIGH"); }
}

void refusalsFailSoft() {
  Fake fake; fake.refused = {StreamGpuPriority::realtime}; fake.privilege = false;
  {
    StreamGpuPriority priority(adapter, fake.access());
    require(priority.applied() == StreamGpuPriority::high && fake.calls.back() == "set:4", "A refused REALTIME did not fall back to HIGH");
    require((priority.diagnostic(false) & 0x5ffu) == (5u | 4u << 4 | 0x100u), "Diagnostic of the HIGH fallback without the privilege");
  }
  Fake none; none.refused = {StreamGpuPriority::realtime, StreamGpuPriority::high};
  { StreamGpuPriority priority(adapter, none.access()); require(!priority.applied() && (priority.diagnostic(false) >> 4 & 15) == 15, "A refused class claimed to apply"); }
  require(none.current == StreamGpuPriority::normal && none.calls.back() == "set:4", "A refused class was restored or retried");
  GpuPriorityAccess throwing; throwing.hags = [](uint64_t) -> std::optional<bool> { throw std::runtime_error("x"); };
  { StreamGpuPriority priority(adapter, throwing); require(priority.attempted() && !priority.applied(), "A failing read was not soft"); }
}

void leavesHigherAndForeignClassesAlone() {
  Fake high; high.hags = true; high.current = StreamGpuPriority::high;
  { StreamGpuPriority priority(adapter, high.access()); require(!priority.applied() && (priority.diagnostic(false) & 0x1000u), "A class already high enough was set again"); }
  require(high.current == StreamGpuPriority::high && high.calls.back() != "set:2", "An untouched class was restored");
  Fake changed;
  { StreamGpuPriority priority(adapter, changed.access()); changed.current = StreamGpuPriority::high; }
  require(changed.current == StreamGpuPriority::high, "A class changed by someone else meanwhile was overwritten");
}

void realAccessReadsOnly() {
  // This process's own class through gdi32 (read, never set here).
  const auto value = windowsGpuPriorityAccess().get();
  require(!value || (*value >= 0 && *value <= 5), "The real class is out of range");
}
}

int main() {
  const struct { const char* name; void (*run)(); } cases[] = {{"no-adapter", noAdapterChangesNothing}, {"class-follows-hags", classFollowsHardwareScheduling},
    {"refusals-fail-soft", refusalsFailSoft}, {"leaves-higher-and-foreign", leavesHigherAndForeignClassesAlone}, {"real-access-read-only", realAccessReadsOnly}};
  unsigned passed = 0, failed = 0;
  for (const auto& value : cases) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
  std::cout << "{\"kind\":\"stream-gpu-priority\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << ",\"gpuSchedulingChanged\":false}\n";
  return failed ? 1 : 0;
}
