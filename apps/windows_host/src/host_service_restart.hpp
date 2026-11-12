#pragma once
#include "broker_status.hpp"
#include "pending_connection.hpp"
#include <algorithm>
#include <cstdint>
#include <winver.h>
#pragma comment(lib, "version.lib")

namespace host::service_restart {
inline constexpr const char* protocol = "service-restart-v1";
enum class State { unknown, stopped, running, pending, other };
enum class Exit { unknown, alive, exited };
enum class Outcome { refused, uncertain, completed };
enum class Stage { preflight, stop, waitExit, start, waitRunning, completed };
struct Process {
  uint32_t pid = 0;
  uint64_t created = 0;
  bool valid() const noexcept { return pid != 0 && created != 0; }
  bool operator==(const Process&) const = default;
};
struct Observation { State state = State::unknown; Process process; uint32_t error = 0; };
struct Admission { bool allowed = false, wasRunning = false; Process old; uint32_t error = ERROR_INVALID_DATA; };
struct Result {
  Outcome outcome = Outcome::refused;
  Stage stage = Stage::preflight;
  uint32_t error = ERROR_INVALID_DATA, firstError = 0, queryErrors = 0;
  bool stopRequested = false, startRequested = false, observedRunning = false;
  bool oldExitRequired = false, exactOldExit = false;
};
struct Limits { uint64_t stopMs = 65000, startMs = 30000, totalMs = 100000; uint32_t pollMs = 250; };
inline bool versionMatches(const VS_FIXEDFILEINFO& value, const std::wstring& expected) {
  if (value.dwSignature != VS_FFI_SIGNATURE || value.dwFileType != VFT_APP ||
      value.dwFileVersionMS != value.dwProductVersionMS || value.dwFileVersionLS != value.dwProductVersionLS ||
      LOWORD(value.dwFileVersionLS) != 0) return false;
  return std::to_wstring(HIWORD(value.dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(value.dwFileVersionMS)) +
    L"." + std::to_wstring(HIWORD(value.dwFileVersionLS)) == expected;
}
// Every operation is mandatory and noexcept: failures are data, including after
// STOP. There is deliberately no UI callback, cancel flag, or consumer lifetime.
struct Access {
  virtual ~Access() = default;
  virtual Admission admit() noexcept = 0;
  virtual bool owned(uint32_t& error) noexcept = 0;
  virtual Observation observe() noexcept = 0;
  virtual uint32_t stop() noexcept = 0;
  virtual Exit oldExit() noexcept = 0;
  virtual uint32_t start() noexcept = 0;
  virtual uint64_t now() noexcept = 0;
  virtual void wait(uint32_t milliseconds) noexcept = 0;
};
inline Result perform(Access& access, Limits limits = {}) noexcept {
  Result result;
  if (!limits.pollMs || limits.pollMs > 250 || !limits.stopMs || !limits.startMs || !limits.totalMs ||
      limits.stopMs > 65000 || limits.startMs > 30000 || limits.totalMs > 100000) return result;
  const auto initial = access.admit();
  if (!initial.allowed || (initial.wasRunning && !initial.old.valid())) {
    result.error = initial.error ? initial.error : ERROR_INVALID_DATA;
    return result;
  }
  result.outcome = Outcome::uncertain;
  result.error = ERROR_TIMEOUT;
  result.oldExitRequired = initial.wasRunning;
  result.exactOldExit = !initial.wasRunning;
  const uint64_t begin = access.now();
  const auto record = [&](uint32_t error) noexcept { if (error && !result.firstError) result.firstError = error; };
  const auto elapsed = [&](uint64_t from, uint64_t budget) noexcept {
    const auto tick = access.now();
    return tick < from || tick - from >= budget;
  };
  uint32_t ownershipError = 0;
  if (!access.owned(ownershipError)) {
    result.outcome = Outcome::refused;
    result.error = ownershipError ? ownershipError : ERROR_ACCESS_DENIED;
    return result;
  }
  if (initial.wasRunning) {
    result.stage = Stage::stop;
    result.stopRequested = true;
    record(access.stop());
    result.stage = Stage::waitExit;
    // Physical exit is independent of QueryServiceStatusEx. A broken status
    // reader must not strand a successfully stopped service without START.
    for (;;) {
      const auto observation = access.observe();
      if (observation.error) { ++result.queryErrors; record(observation.error); }
      if (access.oldExit() == Exit::exited) result.exactOldExit = true;
      // The process handle may signal before SCM publishes STOPPED. Avoid
      // spending our only START while the known service is still stopping.
      // An unreadable status cannot suppress the physically proven recovery.
      if (result.exactOldExit && (observation.state == State::stopped || observation.error ||
          observation.state == State::unknown || (observation.state == State::running && observation.process != initial.old))) break;
      if (elapsed(begin, limits.stopMs) || elapsed(begin, limits.totalMs)) {
        if (result.exactOldExit) break;
        return result;
      }
      access.wait(limits.pollMs);
    }
  }
  // Never start an unproven old lifetime or a service whose configuration has
  // changed. Even if the observation budget expired inside a blocking Win32
  // call, discharge the one START obligation after these ownership fences.
  if (!access.owned(ownershipError)) {
    result.error = ownershipError ? ownershipError : ERROR_ACCESS_DENIED;
    record(result.error);
    return result;
  }
  result.stage = Stage::start;
  result.startRequested = true;
  const uint32_t startError = access.start();
  record(startError);
  const uint64_t startTick = access.now();
  result.stage = Stage::waitRunning;
  for (;;) {
    const auto observation = access.observe();
    if (observation.error) { ++result.queryErrors; record(observation.error); }
    if (observation.state == State::running && !observation.error && observation.process.valid() &&
        (!initial.wasRunning || observation.process != initial.old) &&
        access.owned(ownershipError) && (!initial.wasRunning || access.oldExit() == Exit::exited)) {
      result.observedRunning = true;
      result.outcome = Outcome::completed;
      result.stage = Stage::completed;
      result.error = 0;
      return result;
    }
    if (elapsed(startTick, limits.startMs) || elapsed(begin, limits.totalMs)) {
      result.error = startError && startError != ERROR_SERVICE_ALREADY_RUNNING ? startError : ERROR_TIMEOUT;
      return result;
    }
    access.wait(limits.pollMs);
  }
}
inline const char* outcomeName(Outcome value) noexcept {
  return value == Outcome::completed ? "completed" : value == Outcome::uncertain ? "uncertain" : "refused";
}
inline const char* stageName(Stage value) noexcept {
  switch (value) {
    case Stage::stop: return "stop";
    case Stage::waitExit: return "wait_old_exit";
    case Stage::start: return "start";
    case Stage::waitRunning: return "wait_running";
    case Stage::completed: return "completed";
    default: return "preflight";
  }
}
inline std::string json(const Result& result) {
  const auto boolean = [](bool value) { return value ? "true" : "false"; };
  return std::string("{\"protocol\":\"") + protocol + "\",\"outcome\":\"" + outcomeName(result.outcome) +
    "\",\"stage\":\"" + stageName(result.stage) + "\",\"error\":" + std::to_string(result.error) +
    ",\"firstError\":" + std::to_string(result.firstError) + ",\"queryErrors\":" + std::to_string(result.queryErrors) +
    ",\"stopRequested\":" + boolean(result.stopRequested) + ",\"startRequested\":" + boolean(result.startRequested) +
    ",\"observedRunning\":" + boolean(result.observedRunning) + ",\"oldExitRequired\":" + boolean(result.oldExitRequired) +
    ",\"exactOldExit\":" + boolean(result.exactOldExit) + '}';
}

class NativeAccess final : public Access {
 public:
  Admission admit() noexcept override {
    try {
      requireAdmin();
      lock_ = nativeConnectionLock(L"Global\\MultitapHost.SettingsRestart.V1", 0);
      if (!lock_) return {false, false, {}, ERROR_BUSY};
      manager_ = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
      if (!manager_) return failure(GetLastError());
      service_ = OpenServiceW(manager_, L"MultitapHost", SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_START);
      if (!service_) return failure(GetLastError());
      executable_ = brokerServiceExecutable(service_);
      const auto executable = std::filesystem::path(executable_);
      const auto tool = std::filesystem::path(executablePath());
      if (_wcsicmp(tool.filename().c_str(), L"MultitapHostTool.exe") ||
          _wcsicmp(tool.parent_path().c_str(), executable.parent_path().c_str()) ||
          _wcsicmp(executable.parent_path().filename().c_str(), L"bin") || !trustedMachineFile(tool)) return failure(ERROR_ACCESS_DENIED);
      image_.value = lockImage(executable_);
      tool_.value = lockImage(tool.wstring());
      if (image_.value == INVALID_HANDLE_VALUE || tool_.value == INVALID_HANDLE_VALUE) return failure(ERROR_ACCESS_DENIED);
      if (!pairedVersion(executable_) || !pairedVersion(tool.wstring())) return failure(ERROR_REVISION_MISMATCH);
      if (!config(configStart_)) return failure(ERROR_INVALID_DATA);
      SERVICE_STATUS_PROCESS status{};
      if (!statusOf(status)) return failure(GetLastError());
      if (status.dwCurrentState == SERVICE_STOPPED && status.dwProcessId == 0) {
        admitted_ = true;
        uint32_t error = 0;
        if (!owned(error)) return failure(error);
        return {true, false, {}, 0};
      }
      if (status.dwCurrentState != SERVICE_RUNNING || !status.dwProcessId) return failure(ERROR_SERVICE_CANNOT_ACCEPT_CTRL);
      old_.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, status.dwProcessId);
      if (!old_.value || !brokerProcessExecutable(old_.value, executable_) || !brokerSystemProcess(old_.value) ||
          WaitForSingleObject(old_.value, 0) != WAIT_TIMEOUT) return failure(ERROR_ACCESS_DENIED);
      oldIdentity_ = {status.dwProcessId, brokerProcessCreation(old_.value)};
      SERVICE_STATUS_PROCESS repeated{};
      if (!oldIdentity_.valid() || !statusOf(repeated) || repeated.dwCurrentState != SERVICE_RUNNING || repeated.dwProcessId != oldIdentity_.pid) return failure(ERROR_INVALID_DATA);
      admitted_ = true;
      uint32_t error = 0;
      if (!owned(error)) return failure(error);
      return {true, true, oldIdentity_, 0};
    } catch (...) { return failure(ERROR_ACCESS_DENIED); }
  }
  ~NativeAccess() override {
    if (service_) CloseServiceHandle(service_);
    if (manager_) CloseServiceHandle(manager_);
  }
  bool owned(uint32_t& error) noexcept override {
    try {
      DWORD start = 0;
      const bool safe = admitted_ && config(start) && start == configStart_ &&
        _wcsicmp(brokerServiceExecutable(service_).c_str(), executable_.c_str()) == 0;
      error = safe ? 0 : ERROR_ACCESS_DENIED;
      return safe;
    } catch (...) { error = ERROR_ACCESS_DENIED; return false; }
  }
  Observation observe() noexcept override {
    try {
      SERVICE_STATUS_PROCESS status{};
      if (!statusOf(status)) return {State::unknown, {}, GetLastError()};
      if (status.dwCurrentState == SERVICE_STOPPED && !status.dwProcessId) return {State::stopped, {}, 0};
      if (status.dwCurrentState != SERVICE_RUNNING) return {State::pending, {}, 0};
      if (!status.dwProcessId) return {State::unknown, {}, ERROR_INVALID_DATA};
      BrokerHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, status.dwProcessId));
      if (!process.value || !brokerProcessExecutable(process.value, executable_) || !brokerSystemProcess(process.value) ||
          WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT) return {State::unknown, {}, ERROR_ACCESS_DENIED};
      const Process identity{status.dwProcessId, brokerProcessCreation(process.value)};
      SERVICE_STATUS_PROCESS repeated{};
      uint32_t error = 0;
      if (!identity.valid() || !owned(error) || !statusOf(repeated) || repeated.dwCurrentState != SERVICE_RUNNING ||
          repeated.dwProcessId != identity.pid || WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT) return {State::unknown, {}, ERROR_INVALID_DATA};
      return {State::running, identity, 0};
    } catch (...) { return {State::unknown, {}, ERROR_ACCESS_DENIED}; }
  }
  uint32_t stop() noexcept override {
    SERVICE_STATUS status{};
    return ControlService(service_, SERVICE_CONTROL_STOP, &status) ? 0 : GetLastError();
  }
  Exit oldExit() noexcept override {
    if (!old_.value || !oldIdentity_.valid()) return Exit::unknown;
    const auto state = WaitForSingleObject(old_.value, 0);
    return state == WAIT_OBJECT_0 ? Exit::exited : state == WAIT_TIMEOUT ? Exit::alive : Exit::unknown;
  }
  uint32_t start() noexcept override { return StartServiceW(service_, 0, nullptr) ? 0 : GetLastError(); }
  uint64_t now() noexcept override { return GetTickCount64(); }
  void wait(uint32_t milliseconds) noexcept override { Sleep(milliseconds); }
 private:
  Admission failure(uint32_t error) noexcept { return {false, false, {}, error ? error : ERROR_INVALID_DATA}; }
  static HANDLE lockImage(const std::wstring& path) noexcept {
    // Deny modification/deletion for the whole mutation. Existing protected
    // ancestor validation is performed by brokerServiceExecutable/Tool checks.
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  static bool pairedVersion(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD count = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!count || count > 65536) return false;
    std::vector<unsigned char> bytes(count);
    if (!GetFileVersionInfoW(path.c_str(), 0, count, bytes.data())) return false;
    VS_FIXEDFILEINFO* value = nullptr;
    UINT size = 0;
    return VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&value), &size) && value &&
      size >= sizeof(VS_FIXEDFILEINFO) && versionMatches(*value, MULTITAP_HOST_VERSION);
  }
  bool statusOf(SERVICE_STATUS_PROCESS& status) noexcept {
    DWORD needed = 0;
    return QueryServiceStatusEx(service_, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed) != FALSE;
  }
  bool config(DWORD& start) noexcept {
    alignas(QUERY_SERVICE_CONFIGW) unsigned char bytes[8192]{};
    DWORD needed = 0;
    if (!QueryServiceConfigW(service_, reinterpret_cast<QUERY_SERVICE_CONFIGW*>(bytes), sizeof(bytes), &needed)) return false;
    const auto* value = reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(bytes);
    start = value->dwStartType;
    if (start == SERVICE_DISABLED || !value->lpBinaryPathName || !value->lpServiceStartName) return false;
    try { return _wcsicmp(parseBrokerServiceExecutable(value->dwServiceType, value->lpServiceStartName, value->lpBinaryPathName).c_str(), executable_.c_str()) == 0; }
    catch (...) { return false; }
  }
  ConnectionGuardPtr lock_;
  SC_HANDLE manager_ = nullptr, service_ = nullptr;
  BrokerHandle image_, tool_, old_;
  std::wstring executable_;
  DWORD configStart_ = 0;
  Process oldIdentity_;
  bool admitted_ = false;
};
inline Result native() noexcept {
  NativeAccess access;
  return perform(access);
}
} // namespace host::service_restart
