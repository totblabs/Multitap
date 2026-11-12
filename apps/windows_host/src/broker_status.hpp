#pragma once
#include "support.hpp"
#include <mutex>
#include <thread>

namespace host {
enum class BrokerPhase { WaitingRegistration, Connecting, AwaitingAck, Online, Reconnecting, Stale, Stopped, Unknown };
inline const char* brokerPhaseName(BrokerPhase phase) {
  switch (phase) {
    case BrokerPhase::WaitingRegistration: return "waiting_registration";
    case BrokerPhase::Connecting: return "connecting";
    case BrokerPhase::AwaitingAck: return "awaiting_ack";
    case BrokerPhase::Online: return "online";
    case BrokerPhase::Reconnecting: return "reconnecting";
    case BrokerPhase::Stale: return "stale";
    case BrokerPhase::Stopped: return "stopped";
    default: return "unknown";
  }
}
inline bool brokerSafeReason(const std::string& reason) {
  static const std::set<std::string> allowed{"", "unregistered", "credentials_unavailable", "network_unavailable", "tls_failed", "authorization_failed", "server_unavailable", "protocol_rejected", "transport_closed", "send_failed", "heartbeat_timeout", "clock_changed", "status_unavailable", "service_stopped", "service_unknown"};
  return allowed.count(reason) != 0;
}
inline uint64_t brokerUtcMilliseconds() {
  FILETIME time{}; GetSystemTimeAsFileTime(&time);
  ULARGE_INTEGER raw{}; raw.LowPart = time.dwLowDateTime; raw.HighPart = time.dwHighDateTime;
  constexpr uint64_t epoch = 116444736000000000ULL;
  return raw.QuadPart >= epoch ? (raw.QuadPart - epoch) / 10000 : 0;
}
struct BrokerEvidence {
  BrokerPhase phase = BrokerPhase::WaitingRegistration;
  std::string reason = "unregistered";
  uint64_t readyUtc = 0, heartbeatUtc = 0, verifiedTick = 0, heartbeatTick = 0, attemptTick = 0;
};
inline BrokerEvidence freshBrokerEvidence(BrokerEvidence value, uint64_t now) {
  if (value.phase == BrokerPhase::Online) {
    if (!value.verifiedTick || now < value.verifiedTick) { value.phase = BrokerPhase::Stale; value.reason = "clock_changed"; }
    else if (now - value.verifiedTick > 30000) { value.phase = BrokerPhase::Stale; value.reason = "heartbeat_timeout"; }
  } else if (value.phase == BrokerPhase::AwaitingAck && (now < value.attemptTick || now - value.attemptTick > 30000)) {
    value.phase = BrokerPhase::Stale; value.reason = now < value.attemptTick ? "clock_changed" : "heartbeat_timeout";
  }
  return value;
}
inline std::string brokerStatusJson(BrokerEvidence value, uint64_t now = GetTickCount64()) {
  value = freshBrokerEvidence(std::move(value), now);
  const auto stamp = [](uint64_t n) { return n ? std::to_string(n) : std::string("null"); };
  const auto age = value.heartbeatTick && now >= value.heartbeatTick ? std::to_string((now - value.heartbeatTick) / 1000) : std::string("null");
  return "{\"state\":" + jsonQuote(brokerPhaseName(value.phase)) + ",\"online\":" + (value.phase == BrokerPhase::Online ? "true" : "false") + ",\"lastReadyAt\":" + stamp(value.readyUtc) + ",\"lastHeartbeatAt\":" + stamp(value.heartbeatUtc) + ",\"heartbeatAgeSeconds\":" + age + ",\"failureReason\":" + jsonQuote(brokerSafeReason(value.reason) ? value.reason : "status_unavailable") + '}';
}
class BrokerConnectionStatus {
 public:
  uint64_t begin(uint64_t tick = GetTickCount64()) {
    std::lock_guard lock(mutex_);
    if (value_.phase == BrokerPhase::Stopped || value_.phase == BrokerPhase::WaitingRegistration) value_.reason.clear();
    ++generation_; upgraded_ = false; readyForGeneration_ = false; value_.phase = BrokerPhase::Connecting; value_.attemptTick = tick; value_.verifiedTick = 0; return generation_;
  }
  void upgraded(uint64_t generation, uint64_t tick = GetTickCount64()) {
    std::lock_guard lock(mutex_); if (generation != generation_) return;
    upgraded_ = true; value_.phase = BrokerPhase::AwaitingAck; value_.attemptTick = tick;
  }
  void acknowledged(uint64_t generation, bool heartbeat, uint64_t tick = GetTickCount64(), uint64_t utc = brokerUtcMilliseconds()) {
    std::lock_guard lock(mutex_); if (generation != generation_ || !upgraded_) return;
    if (!readyForGeneration_ || !heartbeat) value_.readyUtc = utc;
    readyForGeneration_ = true;
    if (heartbeat) { value_.heartbeatUtc = utc; value_.heartbeatTick = tick; }
    value_.verifiedTick = tick; value_.phase = BrokerPhase::Online; value_.reason.clear();
  }
  void disconnected(uint64_t generation, const std::string& reason) {
    std::lock_guard lock(mutex_); if (generation != generation_ || value_.phase == BrokerPhase::Stopped) return;
    if (value_.phase != BrokerPhase::Reconnecting) value_.reason = brokerSafeReason(reason) ? reason : "network_unavailable";
    upgraded_ = false; value_.verifiedTick = 0; value_.phase = BrokerPhase::Reconnecting;
  }
  void waiting(const std::string& reason = "unregistered") {
    std::lock_guard lock(mutex_); ++generation_; upgraded_ = false; value_ = {}; value_.reason = brokerSafeReason(reason) ? reason : "credentials_unavailable";
  }
  void stop() {
    std::lock_guard lock(mutex_); ++generation_; upgraded_ = false; value_.phase = BrokerPhase::Stopped; value_.verifiedTick = 0; value_.reason = "service_stopped";
  }
  BrokerEvidence evidence() const { std::lock_guard lock(mutex_); return value_; }
  bool expired(uint64_t generation, uint64_t tick = GetTickCount64()) const {
    std::lock_guard lock(mutex_); return generation == generation_ && freshBrokerEvidence(value_, tick).phase == BrokerPhase::Stale;
  }
 private:
  mutable std::mutex mutex_; BrokerEvidence value_; uint64_t generation_ = 0; bool upgraded_ = false, readyForGeneration_ = false;
};
struct BrokerHandle {
  HANDLE value = nullptr;
  explicit BrokerHandle(HANDLE handle = nullptr) : value(handle) {}
  ~BrokerHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  BrokerHandle(const BrokerHandle&) = delete;
};
inline std::wstring brokerStatusPipeName(DWORD pid) { return L"\\\\.\\pipe\\MultitapHost.BrokerStatus." + std::to_wstring(pid); }
inline uint64_t brokerProcessCreation(HANDLE process) {
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return 0;
  ULARGE_INTEGER time{}; time.LowPart = creation.dwLowDateTime; time.HighPart = creation.dwHighDateTime; return time.QuadPart;
}
inline bool brokerSystemProcess(HANDLE process) {
  HANDLE raw = nullptr; if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return false; BrokerHandle token(raw);
  DWORD size = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &size); if (!size || size > 16384) return false;
  std::vector<uint8_t> buffer(size);
  return GetTokenInformation(token.value, TokenUser, buffer.data(), size, &size) && IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, WinLocalSystemSid);
}
inline bool brokerProcessActive(HANDLE process) {
  DWORD code=0;return GetExitCodeProcess(process,&code)&&code==STILL_ACTIVE;
}
inline std::wstring parseBrokerServiceExecutable(DWORD type,const std::wstring& account,const std::wstring& command) {
  const std::wstring tail=L"\" --service";
  if(type!=SERVICE_WIN32_OWN_PROCESS||(_wcsicmp(account.c_str(),L"LocalSystem")!=0&&_wcsicmp(account.c_str(),L"NT AUTHORITY\\SYSTEM")!=0)||command.size()<=tail.size()+1||command.front()!=L'"'||!command.ends_with(tail))throw std::runtime_error("Local broker service identity unavailable");
  const auto executable=command.substr(1,command.size()-tail.size()-1);const std::filesystem::path path(executable);
  if(executable.find_first_of(L"\"\r\n\0",0,4)!=std::wstring::npos||executable.size()<4||executable[1]!=L':'||executable[2]!=L'\\'||!path.is_absolute()||path.lexically_normal()!=path||_wcsicmp(path.filename().c_str(),L"MultitapHost.exe")!=0)throw std::runtime_error("Local broker service executable unavailable");
  return executable;
}
inline std::wstring brokerServiceExecutable(SC_HANDLE service) {
  std::vector<uint8_t> bytes(8192);DWORD required=0;
  if(!QueryServiceConfigW(service,reinterpret_cast<QUERY_SERVICE_CONFIGW*>(bytes.data()),DWORD(bytes.size()),&required))throw std::runtime_error("Local broker service configuration unavailable");
  const auto* config=reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(bytes.data());
  if(!config->lpServiceStartName||!config->lpBinaryPathName)throw std::runtime_error("Local broker service configuration unavailable");
  const auto executable=parseBrokerServiceExecutable(config->dwServiceType,config->lpServiceStartName,config->lpBinaryPathName);
  const auto root=programFilesFolder()/L"Multitap"/L"Host";const auto boundary=root.wstring()+L"\\";
  if(executable.size()<=boundary.size()||_wcsnicmp(executable.c_str(),boundary.c_str(),boundary.size())!=0)throw std::runtime_error("Local broker service installation unavailable");
  for(auto path=std::filesystem::path(executable);;path=path.parent_path()){
    // The executable and its folders up to Multitap\Host, as either installer leaves them (support.hpp).
    if(!trustedInstalledPath(path))throw std::runtime_error("Local broker service installation permissions unavailable");
    if(_wcsicmp(path.c_str(),root.c_str())==0)break;
    if(path==path.parent_path())throw std::runtime_error("Local broker service installation boundary unavailable");
  }
  return executable;
}
inline bool brokerProcessExecutable(HANDLE process,const std::wstring& expected) {
  std::wstring image(32768,L'\0');DWORD size=DWORD(image.size());
  return QueryFullProcessImageNameW(process,0,image.data(),&size)&&size&&_wcsicmp(image.c_str(),expected.c_str())==0;
}
inline bool brokerPipeTransfer(HANDLE pipe, bool write, void* bytes, DWORD size, const std::atomic<bool>* stopped = nullptr) {
  auto* at = static_cast<uint8_t*>(bytes); const uint64_t begun = GetTickCount64();
  while (size) {
    BrokerHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!event.value) return false;
    OVERLAPPED operation{}; operation.hEvent = event.value; DWORD count = 0;
    BOOL ok = write ? WriteFile(pipe, at, size, &count, &operation) : ReadFile(pipe, at, size, &count, &operation);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
      for (;;) {
        const DWORD wait = WaitForSingleObject(event.value, 25);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_TIMEOUT || (stopped && *stopped) || GetTickCount64() - begun > 750) {
          CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &count, TRUE); return false;
        }
      }
      ok = GetOverlappedResult(pipe, &operation, &count, FALSE);
    }
    if (!ok || !count || count > size) return false;
    at += count; size -= count;
  }
  return true;
}
inline std::string brokerWire(const BrokerEvidence& value, uint64_t sample = GetTickCount64()) {
  return "{\"version\":1,\"phase\":" + jsonQuote(brokerPhaseName(value.phase)) + ",\"reason\":" + jsonQuote(brokerSafeReason(value.reason) ? value.reason : "status_unavailable") + ",\"readyUtc\":" + std::to_string(value.readyUtc) + ",\"heartbeatUtc\":" + std::to_string(value.heartbeatUtc) + ",\"verifiedTick\":" + std::to_string(value.verifiedTick) + ",\"heartbeatTick\":" + std::to_string(value.heartbeatTick) + ",\"attemptTick\":" + std::to_string(value.attemptTick) + ",\"sampleTick\":" + std::to_string(sample) + '}';
}
inline BrokerEvidence parseBrokerWire(const std::string& bytes, uint64_t now = GetTickCount64()) {
  if (bytes.empty() || bytes.size() > 2048) throw std::runtime_error("Invalid local broker status");
  const auto json = JsonParser::parse(bytes); if (json.get("version").number() != 1) throw std::runtime_error("Invalid local broker status");
  const auto number = [&](const char* name) {
    const auto n = json.get(name).number(-1); if (n < 0 || n > 9007199254740991.0 || std::floor(n) != n) throw std::runtime_error("Invalid local broker status"); return uint64_t(n);
  };
  const auto sample = number("sampleTick"); if (now < sample || now - sample > 2000) throw std::runtime_error("Stale local broker status");
  BrokerEvidence value; const auto phase = json.get("phase").string(); bool found = false;
  for (const auto item : {BrokerPhase::WaitingRegistration, BrokerPhase::Connecting, BrokerPhase::AwaitingAck, BrokerPhase::Online, BrokerPhase::Reconnecting, BrokerPhase::Stale, BrokerPhase::Stopped, BrokerPhase::Unknown}) if (phase == brokerPhaseName(item)) { value.phase = item; found = true; break; }
  value.reason = json.get("reason").string(); if (!found || !brokerSafeReason(value.reason)) throw std::runtime_error("Invalid local broker status");
  value.readyUtc = number("readyUtc"); value.heartbeatUtc = number("heartbeatUtc"); value.verifiedTick = number("verifiedTick"); value.heartbeatTick = number("heartbeatTick"); value.attemptTick = number("attemptTick");
  if (value.verifiedTick > sample || value.heartbeatTick > sample || value.attemptTick > sample || (value.phase == BrokerPhase::Online && (!value.readyUtc || !value.verifiedTick))) throw std::runtime_error("Invalid local broker evidence");
  return freshBrokerEvidence(std::move(value), now);
}
// No files or registry values hold this status. The boot service owns the
// endpoint; its lifetime and monotonic ACK evidence determine freshness.
// Waits for a pipe client or `stop`, without a timeout: an idle endpoint never
// wakes its thread. (Both servers used to poll every 25 or 50 ms: about 2,900
// wakeups a minute in an idle Host service, measured.)
// A null `stop` keeps the earlier 25 ms polling of `stopped`.
inline DWORD waitPipeClient(HANDLE connected, HANDLE stop, const std::atomic<bool>& stopped) noexcept {
  if (stop) { const HANDLE events[] = {connected, stop}; return WaitForMultipleObjects(2, events, FALSE, INFINITE); }
  DWORD wait = WAIT_TIMEOUT; while (!stopped && (wait = WaitForSingleObject(connected, 25)) == WAIT_TIMEOUT) {}
  return wait;
}
class BrokerStatusServer {
 public:
  explicit BrokerStatusServer(BrokerConnectionStatus& status, std::wstring endpoint = {}, std::wstring testSecurity = {}) : status_(status), endpoint_(endpoint.empty() ? brokerStatusPipeName(GetCurrentProcessId()) : std::move(endpoint)), security_(testSecurity.empty() ? L"D:P(A;;GA;;;SY)(A;;GA;;;BA)" : std::move(testSecurity)) {
    worker_ = std::thread([this] { run(); });
  }
  ~BrokerStatusServer() { stopped_ = true; if (stop_.value) SetEvent(stop_.value); if (worker_.joinable()) worker_.join(); }
  BrokerStatusServer(const BrokerStatusServer&) = delete;
 private:
  BrokerConnectionStatus& status_; std::wstring endpoint_, security_; std::atomic<bool> stopped_{false};
  BrokerHandle stop_{CreateEventW(nullptr, TRUE, FALSE, nullptr)}; std::thread worker_;
  void run() noexcept {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(security_.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return;
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    BrokerHandle pipe(CreateNamedPipeW(endpoint_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, &security)); LocalFree(descriptor);
    if (pipe.value == INVALID_HANDLE_VALUE) return;
    while (!stopped_) {
      BrokerHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!event.value) return;
      OVERLAPPED operation{}; operation.hEvent = event.value; BOOL connected = ConnectNamedPipe(pipe.value, &operation); const DWORD error = GetLastError();
      if (!connected && error == ERROR_IO_PENDING) {
        DWORD count = 0;
        const DWORD waited = waitPipeClient(event.value, stop_.value, stopped_);
        if (stopped_ || waited != WAIT_OBJECT_0) { CancelIoEx(pipe.value, &operation); GetOverlappedResult(pipe.value, &operation, &count, TRUE); return; }
        connected = GetOverlappedResult(pipe.value, &operation, &count, FALSE);
      } else if (!connected && error == ERROR_PIPE_CONNECTED) connected = TRUE;
      if (connected && !stopped_) {
        try { auto bytes = brokerWire(status_.evidence()); DWORD length = DWORD(bytes.size()); if (brokerPipeTransfer(pipe.value, true, &length, sizeof(length), &stopped_) && brokerPipeTransfer(pipe.value, true, bytes.data(), length, &stopped_)) {uint8_t ack=0;brokerPipeTransfer(pipe.value,false,&ack,1,&stopped_);} } catch (...) {}
      }
      DisconnectNamedPipe(pipe.value);
      // Closing the server handle ends this connection without waiting for a
      // client to acknowledge/flush, so a stuck UI cannot stop the service.
    }
  }
};
inline BrokerEvidence readBrokerStatusPipe(const std::wstring& endpoint, DWORD expectedPid, uint64_t expectedCreation, bool requireSystem = true) {
  BrokerHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, expectedPid));
  if (!process.value || !expectedPid || !expectedCreation || brokerProcessCreation(process.value) != expectedCreation || !brokerProcessActive(process.value) || (requireSystem && !brokerSystemProcess(process.value))) throw std::runtime_error("Local broker status peer is unavailable");
  if (!WaitNamedPipeW(endpoint.c_str(), 300)) throw std::runtime_error("Local broker status is unavailable");
  BrokerHandle pipe(CreateFileW(endpoint.c_str(), GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
  if (pipe.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Local broker status is unavailable");
  ULONG peer = 0; if (!GetNamedPipeServerProcessId(pipe.value, &peer) || peer != expectedPid) throw std::runtime_error("Local broker status peer changed");
  DWORD length = 0; if (!brokerPipeTransfer(pipe.value, false, &length, sizeof(length)) || !length || length > 2048) throw std::runtime_error("Local broker status is unavailable");
  std::string bytes(length, 0); if (!brokerPipeTransfer(pipe.value, false, bytes.data(), length) || !brokerProcessActive(process.value)) throw std::runtime_error("Local broker status process exited");
  auto value=parseBrokerWire(bytes);uint8_t ack=1;brokerPipeTransfer(pipe.value,true,&ack,1);return value;
}
inline BrokerEvidence unavailableBrokerStatus(BrokerPhase phase, const char* reason) { BrokerEvidence value; value.phase = phase; value.reason = reason; return value; }
inline std::string publicBrokerStatus() {
  try {
    SC_HANDLE rawManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!rawManager) return brokerStatusJson(unavailableBrokerStatus(BrokerPhase::Unknown, "service_unknown"));
    struct Scm { SC_HANDLE h; ~Scm() { if (h) CloseServiceHandle(h); } } manager{rawManager}, service{OpenServiceW(rawManager, L"MultitapHost", SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG)};
    if (!service.h) return brokerStatusJson(unavailableBrokerStatus(GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? BrokerPhase::Stopped : BrokerPhase::Unknown, GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? "service_stopped" : "service_unknown"));
    SERVICE_STATUS_PROCESS before{}; DWORD count = 0;
    if (!QueryServiceStatusEx(service.h, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&before), sizeof(before), &count)) return brokerStatusJson(unavailableBrokerStatus(BrokerPhase::Unknown, "service_unknown"));
    if (before.dwCurrentState != SERVICE_RUNNING || !before.dwProcessId) return brokerStatusJson(unavailableBrokerStatus(BrokerPhase::Stopped, "service_stopped"));
    const auto executable=brokerServiceExecutable(service.h);
    BrokerHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, before.dwProcessId));
    if (!process.value) throw std::runtime_error("Local service identity unavailable");
    const auto creation = brokerProcessCreation(process.value);
    if(!brokerProcessExecutable(process.value,executable))throw std::runtime_error("Local service executable changed");
    // SCM proves the protected own-process LocalSystem installation. Binding
    // its current PID, creation time, actual image and pipe server PID avoids
    // opening another account's token or enabling SeDebugPrivilege in the UI.
    const auto value = readBrokerStatusPipe(brokerStatusPipeName(before.dwProcessId), before.dwProcessId, creation,false);
    SERVICE_STATUS_PROCESS after{};
    if (!QueryServiceStatusEx(service.h, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&after), sizeof(after), &count) || after.dwCurrentState != SERVICE_RUNNING || after.dwProcessId != before.dwProcessId || !brokerProcessActive(process.value)||brokerServiceExecutable(service.h)!=executable||!brokerProcessExecutable(process.value,executable)) throw std::runtime_error("Local service changed");
    return brokerStatusJson(value);
  } catch (...) { return brokerStatusJson(unavailableBrokerStatus(BrokerPhase::Unknown, "status_unavailable")); }
}
}
