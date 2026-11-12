#pragma once
// hostAction 275 (backlog #69): a Host process that ended unexpectedly. When
// the service "terminated unexpectedly" (Defender had quarantined its exe, in
// a field case), this is what says why and after how long. One fixed
// record per unexpected end, history only (never readiness authority):
//   stage      which process: 1 the boot service (its previous run, from the run
//              marker), 2 the session worker of a stream, 3 another interactive
//              worker (display probe, display operation, recovery)
//   cause      1 it exited and `win32` is its exit code or NTSTATUS, 2 it ended
//              without a code (killed, or terminated before it could note one),
//              3 Windows restarted while it ran (power loss, reset, bug check)
//   win32      the exit code or NTSTATUS (cause 1 only)
//   runSeconds how long it had run (the service: at least, from its last alive mark)
// No path, name, PID or text is recorded.
#include "operational_log.hpp"
#include "worker_exit_fence.hpp"
#include <atomic>
#include <cstdint>
#include <optional>

namespace host {
inline constexpr uint32_t processExitAction = 275;
enum class ExitedProcess : uint32_t { service = 1, sessionWorker = 2, worker = 3 };
enum class ProcessExitCause : uint32_t { exited = 1, noCode = 2, windowsRestarted = 3 };
struct ProcessExitRecord {
  ExitedProcess process = ExitedProcess::service; ProcessExitCause cause = ProcessExitCause::noCode;
  std::optional<uint32_t> code; uint64_t runSeconds = 0;
};
// An NTSTATUS error (access violation 0xC0000005, fail-fast 0xC0000409, heap
// corruption 0xC0000374, ...): the process crashed rather than returned.
inline bool processExitCrashCode(uint32_t code) noexcept { return code >= 0xC0000000u; }
template<class Sink> void emitProcessExit(const ProcessExitRecord& record, Sink&& sink) noexcept {
  using Values = std::initializer_list<std::pair<const char*, int64_t>>;
  const auto stage = int64_t(record.process), cause = int64_t(record.cause), run = int64_t(std::min<uint64_t>(record.runSeconds, 4294967295ull));
  try {
    if (record.cause == ProcessExitCause::exited && record.code) sink(Values{{"action", processExitAction}, {"stage", stage}, {"cause", cause}, {"win32", int64_t(*record.code)}, {"runSeconds", run}, {"ok", false}});
    else sink(Values{{"action", processExitAction}, {"stage", stage}, {"cause", cause}, {"runSeconds", run}, {"ok", false}});
  } catch (...) {}
}
inline void recordProcessExit(const ProcessExitRecord& record) noexcept { emitProcessExit(record, [](auto values) { recordHostEvent("hostAction", values); }); }
// Any other interactive worker (probe, display operation, recovery) is short and
// returns by itself: only a crash it ended with by itself is unexpected.
inline std::optional<ProcessExitRecord> workerCrashExit(const OwnedWorkerExit& exit) noexcept {
  if (!exit.observed || !exit.ownExit || !processExitCrashCode(exit.code)) return std::nullopt;
  return ProcessExitRecord{ExitedProcess::worker, ProcessExitCause::exited, exit.code, exit.runMs / 1000};
}
// A stream's session worker ended unexpectedly when it ended by itself (not by
// the Host's job close) while no stop was asked for, or when it crashed even
// while stopping. A worker that is still running or was ended by the Host is
// covered by the relay record (hostAction 260) and the quiescence fence.
inline std::optional<ProcessExitRecord> sessionWorkerExit(const OwnedWorkerExit& exit, bool stopRequested) noexcept {
  if (!exit.observed || !exit.ownExit || (stopRequested && !processExitCrashCode(exit.code))) return std::nullopt;
  return ProcessExitRecord{ExitedProcess::sessionWorker, ProcessExitCause::exited, exit.code, exit.runMs / 1000};
}

// The boot service's run marker: one fixed 64-byte record in the protected
// private folder, open for the service's whole run. A clean stop, a requested
// stop or Windows' shutdown control leaves no record behind; anything else is
// found by the next run. Every write is positional and allocation-free, so the
// unhandled-exception filter can note the crash code from a failing thread.
struct ServiceRunState {
  uint32_t magic = 0, version = 0, state = 0, codeKnown = 0, code = 0, reserved = 0;
  uint64_t startUtc = 0, bootUtc = 0, aliveUtc = 0, aliveTick = 0, reserved2 = 0;
};
static_assert(sizeof(ServiceRunState) == 64);
inline constexpr uint32_t serviceRunMagic = 0x31525654;  // "TVR1"
enum ServiceRunPhase : uint32_t { serviceRunStopped = 0, serviceRunRunning = 1, serviceRunStopping = 2, serviceRunFailed = 3 };
// Two boots are told apart by the tick count (it restarts at boot) and by the
// boot time it implies (wall clock minus tick count, within two minutes).
inline bool serviceRunSameBoot(const ServiceRunState& prior, uint64_t nowUtc, uint64_t nowTick) noexcept {
  if (nowTick < prior.aliveTick || nowUtc < nowTick * 10000) return false;
  const uint64_t boot = nowUtc - nowTick * 10000, window = 120ull * 10000000ull;
  return (boot > prior.bootUtc ? boot - prior.bootUtc : prior.bootUtc - boot) <= window;
}
// What the previous run's marker says, if it ended unexpectedly.
inline std::optional<ProcessExitRecord> previousServiceRun(const ServiceRunState& prior, uint64_t nowUtc, uint64_t nowTick) noexcept {
  if (prior.magic != serviceRunMagic || prior.version != 1) return std::nullopt;
  // A requested stop that then crashed still counts; one that was merely slow does not.
  const bool crashedWhileStopping = prior.state == serviceRunStopping && prior.codeKnown && processExitCrashCode(prior.code);
  if (prior.state != serviceRunRunning && prior.state != serviceRunFailed && !crashedWhileStopping) return std::nullopt;
  ProcessExitRecord record; record.process = ExitedProcess::service;
  record.runSeconds = prior.aliveUtc > prior.startUtc ? (prior.aliveUtc - prior.startUtc) / 10000000ull : 0;
  if (prior.codeKnown) { record.cause = ProcessExitCause::exited; record.code = prior.code; }
  else record.cause = serviceRunSameBoot(prior, nowUtc, nowTick) ? ProcessExitCause::noCode : ProcessExitCause::windowsRestarted;
  return record;
}
inline uint64_t serviceRunUtcNow() noexcept { FILETIME now{}; GetSystemTimeAsFileTime(&now); return (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime; }
class ServiceRunMarker {
 public:
  struct Clock { uint64_t (*utc)() = serviceRunUtcNow; uint64_t (*tick)() = [] { return uint64_t(GetTickCount64()); }; };
  // Opens (or creates) the marker; `previous` is the last run's ending, if it was unexpected.
  explicit ServiceRunMarker(const std::filesystem::path& path, Clock clock = {}) noexcept : clock_(clock) {
    try {
      file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      BY_HANDLE_FILE_INFORMATION info{};
      if (file_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file_, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || info.nNumberOfLinks != 1) { close(); return; }
      ServiceRunState prior; DWORD read = 0; OVERLAPPED at{};
      const uint64_t utc = clock_.utc(), tick = clock_.tick();
      if (ReadFile(file_, &prior, sizeof(prior), &read, &at) && read == sizeof(prior)) previous_ = previousServiceRun(prior, utc, tick);
      ServiceRunState current; current.magic = serviceRunMagic; current.version = 1; current.state = serviceRunRunning;
      current.startUtc = current.aliveUtc = utc; current.bootUtc = utc - std::min(utc, tick * 10000); current.aliveTick = tick;
      if (!writeAt(0, &current, sizeof(current))) { close(); return; }
      aliveTick_ = tick;
    } catch (...) { close(); }
  }
  ~ServiceRunMarker() { close(); }
  ServiceRunMarker(const ServiceRunMarker&) = delete; ServiceRunMarker& operator=(const ServiceRunMarker&) = delete;
  bool open() const noexcept { return file_ != INVALID_HANDLE_VALUE; }
  const std::optional<ProcessExitRecord>& previous() const noexcept { return previous_; }
  // At most every 15 s: how long this run lasted, if it then ends without a word.
  void alive() noexcept {
    const uint64_t tick = clock_.tick(); if (!open() || tick - aliveTick_ < 15000) return;
    aliveTick_ = tick; const uint64_t mark[2]{clock_.utc(), tick}; writeAt(offsetof(ServiceRunState, aliveUtc), mark, sizeof(mark));
  }
  void stopping() noexcept { phase(serviceRunStopping); }
  void stopped() noexcept { phase(serviceRunStopped); }
  // The service stops with an error (SCM restarts it, failureflag 1).
  void failed(uint32_t code) noexcept { note(code); phase(serviceRunFailed); }
  // From the unhandled-exception filter of a failing thread.
  void crashed(uint32_t code) noexcept { if (!open()) return; const uint64_t mark[2]{clock_.utc(), clock_.tick()}; writeAt(offsetof(ServiceRunState, aliveUtc), mark, sizeof(mark)); note(code); }
 private:
  HANDLE file_ = INVALID_HANDLE_VALUE; Clock clock_; uint64_t aliveTick_ = 0; std::optional<ProcessExitRecord> previous_;
  bool writeAt(uint64_t offset, const void* data, DWORD size) noexcept {
    OVERLAPPED at{}; at.Offset = DWORD(offset); at.OffsetHigh = DWORD(offset >> 32); DWORD wrote = 0;
    return file_ != INVALID_HANDLE_VALUE && WriteFile(file_, data, size, &wrote, &at) && wrote == size;
  }
  void note(uint32_t code) noexcept { const uint32_t value[2]{1, code}; writeAt(offsetof(ServiceRunState, codeKnown), value, sizeof(value)); }
  void phase(uint32_t state) noexcept { writeAt(offsetof(ServiceRunState, state), &state, sizeof(state)); }
  void close() noexcept { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
};
// The marker the service control handler and the crash filter reach.
inline std::atomic<ServiceRunMarker*> serviceRunMarker{nullptr};
inline LPTOP_LEVEL_EXCEPTION_FILTER& serviceRunPreviousFilter() { static LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr; return previous; }
inline LONG WINAPI serviceRunCrashFilter(EXCEPTION_POINTERS* info) {
  if (auto* marker = serviceRunMarker.load()) marker->crashed(info && info->ExceptionRecord ? uint32_t(info->ExceptionRecord->ExceptionCode) : 0u);
  const auto previous = serviceRunPreviousFilter();
  return previous ? previous(info) : EXCEPTION_CONTINUE_SEARCH;
}
}
