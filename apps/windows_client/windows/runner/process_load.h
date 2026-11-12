#pragma once
// This client's own resource cost (CPU of the process and of the PC, GPU
// video-decode engine of the process), sampled on a private timer thread so
// no media thread or Stats() call ever waits on a system query. Values are
// tenths of a percent (0..1000); -1 means "not measured / not available".
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <thread>
#include <vector>

namespace multitap {

// Pure math, kept separate so it can be tested with fake counters.
struct ProcessTimes { uint64_t kernel = 0, user = 0; };           // 100 ns units
struct SystemTimes { uint64_t idle = 0, kernel = 0, user = 0; };  // kernel includes idle

inline int ClampTenths(double percent) {
  if (!(percent >= 0)) return 0;
  const long v = static_cast<long>(percent * 10 + 0.5);
  return static_cast<int>(std::min(1000L, v));
}

// CPU time used by the process between two readings, as a share of the whole
// machine (wall time x logical CPUs). -1 when the interval is unusable.
inline int ProcessCpuPct10(const ProcessTimes& before, const ProcessTimes& after, uint64_t wall_100ns, unsigned cpus) {
  if (wall_100ns == 0 || cpus == 0 || after.kernel < before.kernel || after.user < before.user) return -1;
  const double used = double(after.kernel - before.kernel) + double(after.user - before.user);
  return ClampTenths(used * 100.0 / (double(wall_100ns) * cpus));
}

// Busy share of all logical CPUs between two GetSystemTimes readings.
inline int SystemCpuPct10(const SystemTimes& before, const SystemTimes& after) {
  if (after.kernel < before.kernel || after.user < before.user || after.idle < before.idle) return -1;
  const double total = double(after.kernel - before.kernel) + double(after.user - before.user);
  const double idle = double(after.idle - before.idle);
  if (total <= 0 || idle > total) return -1;
  return ClampTenths((total - idle) * 100.0 / total);
}

struct LoadSnapshot { int app_cpu = -1, system_cpu = -1, gpu_decode = -1, gpu_3d = -1; };

class ProcessLoadSampler {
 public:
  static ProcessLoadSampler& Instance() { static ProcessLoadSampler sampler; return sampler; }
  // Starts the timer thread on first use and returns the latest values.
  LoadSnapshot Latest() {
    std::call_once(started_, [this] { thread_ = std::thread([this] { Run(); }); });
    return {app_cpu_.load(), system_cpu_.load(), gpu_decode_.load(), gpu_3d_.load()};
  }
  ~ProcessLoadSampler() {
    { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

 private:
  ProcessLoadSampler() = default;
  static uint64_t U64(const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
  static bool ReadProcess(ProcessTimes& out) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return false;
    out = {U64(k), U64(u)};
    return true;
  }
  static bool ReadSystem(SystemTimes& out) {
    FILETIME i, k, u;
    if (!GetSystemTimes(&i, &k, &u)) return false;
    out = {U64(i), U64(k), U64(u)};
    return true;
  }

  // PDH "GPU Engine" counters of this process. Any failure leaves the value
  // at -1 (unknown); the query is rebuilt when no instance was seen so a
  // decoder started after the first sample is still found.
  struct Gpu {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER decode = nullptr, render = nullptr;
    bool primed = false;
    int misses = 0;
    void Close() { if (query) PdhCloseQuery(query); query = nullptr; decode = render = nullptr; primed = false; }
    bool Open() {
      Close();
      if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) { query = nullptr; return false; }
      const DWORD pid = GetCurrentProcessId();
      wchar_t path[160];
      swprintf_s(path, L"\\GPU Engine(pid_%lu*engtype_VideoDecode)\\Utilization Percentage", pid);
      const bool d = PdhAddEnglishCounterW(query, path, 0, &decode) == ERROR_SUCCESS;
      swprintf_s(path, L"\\GPU Engine(pid_%lu*engtype_3D)\\Utilization Percentage", pid);
      const bool r = PdhAddEnglishCounterW(query, path, 0, &render) == ERROR_SUCCESS;
      if (!d) decode = nullptr;
      if (!r) render = nullptr;
      if (!d && !r) { Close(); return false; }
      return true;
    }
    // Sum of the instances; -1 when the counter has none (or is unreadable).
    static int Sum(PDH_HCOUNTER counter) {
      if (!counter) return -1;
      DWORD bytes = 0, count = 0;
      PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
      if (status != PDH_MORE_DATA || bytes == 0) return -1;
      std::vector<char> buffer(bytes);
      auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
      if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS || count == 0) return -1;
      double total = 0;
      for (DWORD i = 0; i < count; ++i)
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
          total += items[i].FmtValue.doubleValue;
      return ClampTenths(total);
    }
    void Sample(int& decode_out, int& render_out) {
      decode_out = render_out = -1;
      if (!query && !Open()) return;
      if (PdhCollectQueryData(query) != ERROR_SUCCESS) { Close(); return; }
      if (!primed) { primed = true; return; }  // a rate needs two readings
      decode_out = Sum(decode);
      render_out = Sum(render);
      if (decode_out < 0 && render_out < 0) {
        if (++misses >= 5) { misses = 0; Close(); }  // look the instances up afresh
      } else {
        misses = 0;
      }
    }
  };

  void Run() {
    ProcessTimes process_before{}, process_after{};
    SystemTimes system_before{}, system_after{};
    bool have = ReadProcess(process_before) && ReadSystem(system_before);
    auto last = std::chrono::steady_clock::now();
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    unsigned cpus = std::max<unsigned>(1, info.dwNumberOfProcessors);
    Gpu gpu;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_; })) break;
      }
      const auto now = std::chrono::steady_clock::now();
      const uint64_t wall = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count() / 100);
      last = now;
      if (have && ReadProcess(process_after) && ReadSystem(system_after)) {
        app_cpu_ = ProcessCpuPct10(process_before, process_after, wall, cpus);
        system_cpu_ = SystemCpuPct10(system_before, system_after);
        process_before = process_after;
        system_before = system_after;
      } else {
        have = ReadProcess(process_before) && ReadSystem(system_before);
        app_cpu_ = system_cpu_ = -1;
      }
      int decode = -1, render = -1;
      gpu.Sample(decode, render);
      gpu_decode_ = decode;
      gpu_3d_ = render;
    }
    gpu.Close();
  }

  std::once_flag started_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  std::atomic<int> app_cpu_{-1}, system_cpu_{-1}, gpu_decode_{-1}, gpu_3d_{-1};
};

}  // namespace multitap
