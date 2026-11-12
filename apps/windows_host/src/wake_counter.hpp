#pragma once
// Measurement only (tests and MultitapHostWakeProbe): context switches, CPU time,
// handles and private bytes per process and thread, from one
// NtQuerySystemInformation(SystemProcessInformation) snapshot. Read-only: no
// process or thread handle is opened, nothing is changed. A thread's context
// switch count rises each time it is scheduled, so its delta over an idle window
// is the number of times it woke up.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace host::wake {
struct ThreadSample { uint32_t tid = 0; uint32_t switches = 0; uint64_t cpu100ns = 0, created = 0; uintptr_t start = 0; int32_t priority = 0, basePriority = 0; uint32_t state = 0, waitReason = 0; };
struct ProcessSample {
  uint32_t pid = 0, parent = 0, handles = 0, threadCount = 0; std::wstring image;
  uint64_t privateBytes = 0, workingSet = 0, cpu100ns = 0, created = 0;
  std::vector<ThreadSample> threads;
};
namespace detail {
// Layouts of SYSTEM_PROCESS_INFORMATION / SYSTEM_THREAD_INFORMATION (winternl.h
// names the fields used here Reserved*; this is the documented-by-use layout).
struct Text { USHORT length, maximum; PWSTR buffer; };
struct Thread {
  LARGE_INTEGER kernel, user, create; ULONG waitTime; PVOID start; HANDLE process, thread;
  LONG priority, basePriority; ULONG contextSwitches, state, waitReason;
};
// SystemExtendedProcessInformation: each thread entry also carries its Win32
// start address (the thread procedure, e.g. the CRT's thread start).
struct ExtendedThread { Thread thread; PVOID stackBase, stackLimit, win32Start, teb; ULONG_PTR reserved[3]; };
struct Process {
  ULONG next, threads; LARGE_INTEGER workingSetPrivate; ULONG hardFaults, threadsHigh; ULONGLONG cycles;
  LARGE_INTEGER create, user, kernel; Text image; LONG basePriority; HANDLE pid, parent; ULONG handles, session; ULONG_PTR key;
  SIZE_T peakVirtual, virtualSize; ULONG pageFaults; SIZE_T peakWorkingSet, workingSet, quotaPeakPaged, quotaPaged, quotaPeakNonPaged, quotaNonPaged, pagefile, peakPagefile, privatePages;
  LARGE_INTEGER io[6];
};
using Query = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
}
inline std::vector<ProcessSample> sampleProcesses(bool extended = false) {
  static const auto query = reinterpret_cast<detail::Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
  if (!query) throw std::runtime_error("NtQuerySystemInformation unavailable");
  std::vector<uint8_t> buffer(1 << 20);
  for (;;) {
    ULONG needed = 0;
    const LONG status = query(extended ? 57 /* SystemExtendedProcessInformation */ : 5 /* SystemProcessInformation */, buffer.data(), ULONG(buffer.size()), &needed);
    if (status == LONG(0xC0000004) /* STATUS_INFO_LENGTH_MISMATCH */) { buffer.resize(std::max<size_t>(buffer.size() * 2, needed + 65536)); continue; }
    if (status < 0) throw std::runtime_error("Process snapshot failed");
    break;
  }
  std::vector<ProcessSample> result;
  for (size_t offset = 0;;) {
    const auto* process = reinterpret_cast<const detail::Process*>(buffer.data() + offset);
    ProcessSample sample;
    sample.pid = uint32_t(reinterpret_cast<uintptr_t>(process->pid)); sample.parent = uint32_t(reinterpret_cast<uintptr_t>(process->parent));
    sample.handles = process->handles; sample.threadCount = process->threads;
    if (process->image.buffer) sample.image.assign(process->image.buffer, process->image.length / sizeof(wchar_t));
    sample.privateBytes = process->privatePages; sample.workingSet = process->workingSet;
    sample.cpu100ns = uint64_t(process->user.QuadPart) + uint64_t(process->kernel.QuadPart); sample.created = uint64_t(process->create.QuadPart);
    const auto* threads = reinterpret_cast<const detail::Thread*>(process + 1);
    const auto* wide = reinterpret_cast<const detail::ExtendedThread*>(process + 1);
    for (ULONG i = 0; i < process->threads; ++i) {
      const auto& thread = extended ? wide[i].thread : threads[i];
      sample.threads.push_back({uint32_t(reinterpret_cast<uintptr_t>(thread.thread)), thread.contextSwitches, uint64_t(thread.user.QuadPart) + uint64_t(thread.kernel.QuadPart),
        uint64_t(thread.create.QuadPart), extended ? reinterpret_cast<uintptr_t>(wide[i].win32Start) : 0, thread.priority, thread.basePriority, thread.state, thread.waitReason});
    }
    result.push_back(std::move(sample));
    if (!process->next) break;
    offset += process->next;
  }
  return result;
}
inline ProcessSample sampleProcess(uint32_t pid) {
  for (auto& process : sampleProcesses()) if (process.pid == pid) return process;
  throw std::runtime_error("Process not found");
}
inline std::set<uint32_t> currentThreads() {
  std::set<uint32_t> ids; for (const auto& thread : sampleProcess(GetCurrentProcessId()).threads) ids.insert(thread.tid); return ids;
}
// Wakeups of this process's threads that are not in `excluded` (threads that
// existed before the measured component started) between two snapshots.
inline uint64_t wakeups(const ProcessSample& before, const ProcessSample& after, const std::set<uint32_t>& excluded) {
  std::map<uint32_t, uint32_t> earlier; for (const auto& thread : before.threads) earlier[thread.tid] = thread.switches;
  uint64_t total = 0;
  for (const auto& thread : after.threads) {
    if (excluded.count(thread.tid)) continue;
    const auto found = earlier.find(thread.tid);
    total += found == earlier.end() ? thread.switches : thread.switches - found->second;
  }
  return total;
}
}
