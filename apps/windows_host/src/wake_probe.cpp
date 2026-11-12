// Measurement tool: wakeups (context switches), CPU time, handles and private
// bytes of every running process with the given image name over a window.
//   MultitapHostWakeProbe.exe MultitapHost.exe 60
// Read-only (one process snapshot at each end); it opens no process, so it can
// observe the boot service without touching it. Each thread also shows when it
// was created (ms after its process) and the module its procedure starts in
// (system DLLs share their base address across processes until a restart).
#include "wake_counter.hpp"
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <map>

namespace {
std::string startModule(uintptr_t address) {
  if (!address) return "unknown";
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module) || !module) {
    char text[32]; std::snprintf(text, sizeof(text), "0x%llx", (unsigned long long)address); return text;
  }
  wchar_t path[MAX_PATH]{}; GetModuleFileNameW(module, path, MAX_PATH);
  const wchar_t* name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
  char text[160]; std::snprintf(text, sizeof(text), "%ls+0x%llx", name, (unsigned long long)(address - reinterpret_cast<uintptr_t>(module))); return text;
}

// Timeline of one thread (third argument, a thread ID): every 20 ms snapshot, one
// line per change of its context switch count, so each burst (a heartbeat's
// capability report) shows as its own row with its wakeups and CPU time.
int timeline(const std::wstring& image, int seconds, uint32_t tid) {
  const auto find = [&](uint32_t& switches, uint64_t& cpu) {
    for (const auto& process : host::wake::sampleProcesses()) if (_wcsicmp(process.image.c_str(), image.c_str()) == 0)
      for (const auto& thread : process.threads) if (thread.tid == tid) { switches = thread.switches; cpu = thread.cpu100ns; return true; }
    return false; };
  uint32_t last = 0; uint64_t lastCpu = 0; if (!find(last, lastCpu)) { std::fprintf(stderr, "thread %u not found\n", tid); return 1; }
  const auto began = GetTickCount64(); uint64_t total = 0;
  while (GetTickCount64() - began < uint64_t(seconds) * 1000) {
    Sleep(20); uint32_t now = 0; uint64_t cpu = 0; if (!find(now, cpu)) break;
    if (now != last) { std::printf("%llu ms: +%u wakeups, %.3f ms CPU\n", (unsigned long long)(GetTickCount64() - began), now - last, double(cpu - lastCpu) / 10000.0); total += now - last; last = now; lastCpu = cpu; }
  }
  std::printf("total %llu wakeups in %d s\n", (unsigned long long)total, seconds);
  return 0;
}
}

int wmain(int argc, wchar_t** argv) {
  if (argc != 3 && argc != 4) { std::fwprintf(stderr, L"usage: MultitapHostWakeProbe <image.exe> <seconds> [thread id: timeline]\n"); return 2; }
  const std::wstring image = argv[1]; const int seconds = std::clamp(_wtoi(argv[2]), 1, 3600);
  if (argc == 4) try { return timeline(image, seconds, uint32_t(_wtoi(argv[3]))); } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
  // Libraries the Host links, so a thread procedure inside one can be named.
  for (const auto* name : {L"winhttp.dll", L"webio.dll", L"pdh.dll", L"rpcrt4.dll", L"combase.dll", L"ole32.dll", L"ws2_32.dll", L"mswsock.dll", L"wtsapi32.dll", L"dxgi.dll", L"d3d11.dll",
         L"mfplat.dll", L"user32.dll", L"sechost.dll", L"iphlpapi.dll", L"setupapi.dll", L"cfgmgr32.dll", L"userenv.dll", L"crypt32.dll", L"bcrypt.dll", L"dnsapi.dll", L"nsi.dll", L"msvcp140.dll", L"vcruntime140.dll", L"ucrtbase.dll", L"powrprof.dll", L"winsta.dll"})
    LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  try {
    bool extended = true;
    const auto pick = [&] {
      std::vector<host::wake::ProcessSample> all;
      try { all = host::wake::sampleProcesses(extended); } catch (...) { extended = false; all = host::wake::sampleProcesses(false); }
      std::map<uint32_t, host::wake::ProcessSample> found; for (auto& process : all) if (_wcsicmp(process.image.c_str(), image.c_str()) == 0) found[process.pid] = std::move(process); return found; };
    const auto before = pick();
    const auto began = GetTickCount64();
    // What each thread waits on (KWAIT_REASON while Waiting), sampled 5 times a second:
    // 4/11 Sleep, 6/13 a handle wait, 15 an I/O completion queue, 37 a condition variable or SRW lock.
    std::map<uint32_t, std::map<uint32_t, uint32_t>> reasons;
    while (GetTickCount64() - began < uint64_t(seconds) * 1000) {
      Sleep(200);
      for (const auto& [pid, process] : pick()) for (const auto& thread : process.threads) ++reasons[thread.tid][thread.state == 5 ? thread.waitReason : 100 + thread.state];
    }
    const auto after = pick();
    const double minutes = double(GetTickCount64() - began) / 60000.0;
    for (const auto& [pid, now] : after) {
      const auto old = before.find(pid); if (old == before.end()) continue;
      std::map<uint32_t, std::pair<uint32_t, uint64_t>> earlier; for (const auto& thread : old->second.threads) earlier[thread.tid] = {thread.switches, thread.cpu100ns};
      struct Row { uint32_t tid; uint64_t switches, cpu; int64_t createdMs; uintptr_t start; };
      std::vector<Row> rows; uint64_t total = 0;
      for (const auto& thread : now.threads) {
        const auto e = earlier.find(thread.tid);
        const Row row{thread.tid, e == earlier.end() ? thread.switches : thread.switches - e->second.first, e == earlier.end() ? thread.cpu100ns : thread.cpu100ns - e->second.second,
          (int64_t(thread.created) - int64_t(now.created)) / 10000, thread.start};
        total += row.switches; rows.push_back(row);
      }
      std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.createdMs < b.createdMs; });
      std::printf("{\"pid\":%u,\"parent\":%u,\"windowMinutes\":%.2f,\"threads\":%u,\"handles\":%u,\"privateBytes\":%llu,\"workingSet\":%llu,\"cpuMsPerMinute\":%.2f,\"wakeupsPerMinute\":%.1f,\"perThread\":[\n",
        pid, now.parent, minutes, now.threadCount, now.handles, (unsigned long long)now.privateBytes, (unsigned long long)now.workingSet,
        double(now.cpu100ns - old->second.cpu100ns) / 10000.0 / minutes, double(total) / minutes);
      for (size_t i = 0; i < rows.size(); ++i) {
        int32_t priority = 0; for (const auto& thread : now.threads) if (thread.tid == rows[i].tid) priority = thread.priority;
        std::string waits; for (const auto& [reason, count] : reasons[rows[i].tid]) waits += (waits.empty() ? "" : ",") + std::to_string(reason) + ":" + std::to_string(count);
        std::printf("%s {\"tid\":%u,\"createdMs\":%lld,\"start\":\"%s\",\"priority\":%d,\"waits\":\"%s\",\"wakeupsPerMinute\":%.1f,\"cpuMsPerMinute\":%.2f}\n", i ? "," : "", rows[i].tid, (long long)rows[i].createdMs, startModule(rows[i].start).c_str(), priority, waits.c_str(), double(rows[i].switches) / minutes, double(rows[i].cpu) / 10000.0 / minutes);
      }
      std::printf("]}\n");
    }
    return 0;
  } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
