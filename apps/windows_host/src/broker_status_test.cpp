#include "broker_status.hpp"
#include <cstring>
#include <iostream>
using namespace host;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool debugPrivilegeEnabled() {
  HANDLE raw=nullptr;require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw)!=FALSE,"QA token unavailable");BrokerHandle token(raw);
  DWORD size=0;GetTokenInformation(raw,TokenPrivileges,nullptr,0,&size);std::vector<uint8_t> bytes(size);
  require(GetTokenInformation(raw,TokenPrivileges,bytes.data(),size,&size)!=FALSE,"QA privileges unavailable");
  LUID debug{};require(LookupPrivilegeValueW(nullptr,SE_DEBUG_NAME,&debug)!=FALSE,"QA privilege identity unavailable");
  const auto* privileges=reinterpret_cast<const TOKEN_PRIVILEGES*>(bytes.data());
  for(DWORD i=0;i<privileges->PrivilegeCount;i++){const auto& value=privileges->Privileges[i];if(value.Luid.LowPart==debug.LowPart&&value.Luid.HighPart==debug.HighPart)return (value.Attributes&SE_PRIVILEGE_ENABLED)!=0;}
  return false;
}
int observeInstalled(const std::filesystem::path& output) {
  const bool beforePrivilege=debugPrivilegeEnabled();
  SC_HANDLE manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);require(manager!=nullptr,"QA SCM unavailable");
  SC_HANDLE service=OpenServiceW(manager,L"MultitapHost",SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG);if(!service){CloseServiceHandle(manager);throw std::runtime_error("QA installed service unavailable");}
  SERVICE_STATUS_PROCESS status{};DWORD needed=0;const bool queried=QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&needed)!=FALSE;
  std::vector<uint8_t> config(8192);const bool configured=QueryServiceConfigW(service,reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config.data()),DWORD(config.size()),&needed)!=FALSE;
  const auto* values=reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(config.data());
  const bool systemAccount=configured&&values->lpServiceStartName&&_wcsicmp(values->lpServiceStartName,L"LocalSystem")==0;
  CloseServiceHandle(service);CloseServiceHandle(manager);
  require(queried&&status.dwCurrentState==SERVICE_RUNNING&&status.dwProcessId,"QA running service unavailable");
  BrokerHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,status.dwProcessId));
  const DWORD processError=process.value?0:GetLastError();HANDLE token=nullptr;const bool opened=process.value&&OpenProcessToken(process.value,TOKEN_QUERY,&token);const DWORD tokenError=opened?0:GetLastError();if(token)CloseHandle(token);
  BrokerHandle limited(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,status.dwProcessId));const DWORD limitedError=limited.value?0:GetLastError();DWORD exitCode=0;const bool alive=limited.value&&GetExitCodeProcess(limited.value,&exitCode)&&exitCode==STILL_ACTIVE;
  const auto observed=publicBrokerStatus();
  std::ofstream file(output,std::ios::binary);require(bool(file),"QA observation file unavailable");
  file<<"{\"kind\":\"actual-installed-broker-status-observation\",\"debugPrivilegeEnabledBefore\":"<<(beforePrivilege?"true":"false")<<",\"debugPrivilegeEnabledAfter\":"<<(debugPrivilegeEnabled()?"true":"false")<<",\"systemAccountFromScm\":"<<(systemAccount?"true":"false")<<",\"processOpenError\":"<<processError<<",\"tokenOpenError\":"<<tokenError<<",\"limitedProcessOpenError\":"<<limitedError<<",\"limitedProcessAlive\":"<<(alive?"true":"false")<<",\"broker\":"<<observed<<",\"serviceChanged\":false,\"driverChanged\":false,\"privilegesChanged\":false,\"windowsRestartExecuted\":false}\n";require(bool(file),"QA observation write failed");return 0;
}
template<class F> void rejected(F operation) { bool refused = false; try { operation(); } catch (...) { refused = true; } require(refused, "Invalid broker status was accepted"); }
void serviceIdentityPolicy() {
  const std::wstring executable=L"C:\\Program Files\\Multitap\\Host\\0.2.21\\bin\\MultitapHost.exe",command=L"\""+executable+L"\" --service";
  require(parseBrokerServiceExecutable(SERVICE_WIN32_OWN_PROCESS,L"LocalSystem",command)==executable,"LocalSystem own-process service was rejected");
  require(parseBrokerServiceExecutable(SERVICE_WIN32_OWN_PROCESS,L"NT AUTHORITY\\SYSTEM",command)==executable,"System account alias was rejected");
  rejected([&]{parseBrokerServiceExecutable(SERVICE_WIN32_SHARE_PROCESS,L"LocalSystem",command);});
  rejected([&]{parseBrokerServiceExecutable(SERVICE_WIN32_OWN_PROCESS|SERVICE_INTERACTIVE_PROCESS,L"LocalSystem",command);});
  for(const auto* account:{L"",L"LocalService",L"NetworkService",L"Administrator"})rejected([&]{parseBrokerServiceExecutable(SERVICE_WIN32_OWN_PROCESS,account,command);});
  for(const auto& invalid:std::vector<std::wstring>{executable+L" --service",command+L" --extra",L"\"C:\\Other.exe\" --service",L"\"MultitapHost.exe\" --service",L"\"\\\\server\\share\\MultitapHost.exe\" --service",L"\"C:\\temp\\..\\MultitapHost.exe\" --service",L"\"C:\\bad\n\\MultitapHost.exe\" --service",L"\"C:\\bad\"name\\MultitapHost.exe\" --service"})rejected([&]{parseBrokerServiceExecutable(SERVICE_WIN32_OWN_PROCESS,L"LocalSystem",invalid);});
}
std::wstring nonce() { GUID id{}; require(SUCCEEDED(CoCreateGuid(&id)), "QA GUID generation failed"); wchar_t text[40]{}; StringFromGUID2(id, text, 40); return {text + 1, text + 37}; }
std::wstring ownSid() {
  HANDLE raw = nullptr; require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != FALSE, "QA token query failed"); BrokerHandle token(raw);
  DWORD size = 0; GetTokenInformation(raw, TokenUser, nullptr, 0, &size); std::vector<uint8_t> bytes(size);
  require(GetTokenInformation(raw, TokenUser, bytes.data(), size, &size) != FALSE, "QA user query failed");
  LPWSTR sid = nullptr; require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid) != FALSE, "QA SID conversion failed"); std::wstring result(sid); LocalFree(sid); return result;
}
std::wstring executable() { wchar_t path[32768]{}; const DWORD count = GetModuleFileNameW(nullptr, path, 32768); require(count && count < 32768, "QA executable path unavailable"); return {path, count}; }
struct Child {
  HANDLE process = nullptr; DWORD pid = 0; HANDLE stop = nullptr;
  explicit Child(const std::wstring& endpoint) {
    const auto event = L"Local\\MultitapHost.BrokerStatus.QA.Stop." + nonce();
    stop = CreateEventW(nullptr, TRUE, FALSE, event.c_str()); require(stop != nullptr, "QA stop event failed");
    auto command = L"\"" + executable() + L"\" --serve \"" + endpoint + L"\" \"" + event + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) { CloseHandle(stop); stop = nullptr; throw std::runtime_error("QA child launch failed"); }
    process = info.hProcess; pid = info.dwProcessId; CloseHandle(info.hThread);
  }
  void finish() { if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) { SetEvent(stop); require(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0, "Owned QA child did not stop"); } }
  ~Child() { if (process) { if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) { SetEvent(stop); if (WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) { TerminateProcess(process, 42); WaitForSingleObject(process, 1000); } } CloseHandle(process); } if (stop) CloseHandle(stop); }
};
int child(const std::wstring& endpoint, const std::wstring& event) {
  require(endpoint.rfind(L"\\\\.\\pipe\\MultitapHost.BrokerStatus.QA.", 0) == 0 && endpoint.size() < 160 && event.rfind(L"Local\\MultitapHost.BrokerStatus.QA.Stop.", 0) == 0, "QA endpoint boundary failed");
  BrokerHandle stop(OpenEventW(SYNCHRONIZE, FALSE, event.c_str())); require(stop.value != nullptr, "QA parent stop event missing");
  BrokerConnectionStatus status; const auto generation = status.begin(); status.upgraded(generation); status.acknowledged(generation, false); status.acknowledged(generation, true);
  // Only this QA endpoint adds the current caller SID. Production has only
  // System/Administrators. These ACKs are fixtures, not a real broker test.
  BrokerStatusServer server(status, endpoint, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + ownSid() + L")");
  require(WaitForSingleObject(stop.value, 10000) == WAIT_OBJECT_0, "QA parent did not finish"); status.stop(); return 0;
}
void acknowledgementEvidence() {
  BrokerConnectionStatus status; require(status.evidence().phase == BrokerPhase::WaitingRegistration, "Fresh status implied connectivity");
  const auto generation = status.begin(1000); status.acknowledged(generation, true, 1001, 1700000000000);
  require(status.evidence().phase == BrokerPhase::Connecting, "ACK before TLS upgrade was accepted");
  status.upgraded(generation, 1100); require(status.evidence().phase == BrokerPhase::AwaitingAck, "HTTP101 was mistaken for broker acknowledgement");
  status.acknowledged(generation, false, 1200, 1700000000000);
  auto json = JsonParser::parse(brokerStatusJson(status.evidence(), 1300));
  require(json.get("online").boolean() && json.get("lastHeartbeatAt").string().empty() && std::holds_alternative<std::nullptr_t>(json.get("lastHeartbeatAt").value), "Capabilities acknowledgement invented a heartbeat timestamp");
  status.acknowledged(generation, true, 11200, 1700000010000);
  json = JsonParser::parse(brokerStatusJson(status.evidence(), 13200));
  require(json.get("online").boolean() && json.get("heartbeatAgeSeconds").number() == 2 && json.get("lastReadyAt").number() == 1700000000000.0, "Actual heartbeat observation lost its separate timestamps");
}
void staleAndClock() {
  BrokerConnectionStatus status; const auto generation = status.begin(1000); status.upgraded(generation, 1100);
  require(status.expired(generation, 31101), "Missing first ACK never expired");
  status.acknowledged(generation, true, 2000, 1700000000000);
  auto json = JsonParser::parse(brokerStatusJson(status.evidence(), 32001));
  require(!json.get("online").boolean() && json.get("state").string() == "stale" && json.get("failureReason").string() == "heartbeat_timeout", "Old ACK remained online");
  json = JsonParser::parse(brokerStatusJson(status.evidence(), 1999));
  require(!json.get("online").boolean() && json.get("failureReason").string() == "clock_changed", "Monotonic clock rollback remained online");
  status.acknowledged(generation, true, 2100, 1600000000000);
  require(JsonParser::parse(brokerStatusJson(status.evidence(), 2200)).get("online").boolean(), "Wall-clock adjustment broke fresh monotonic network evidence");
}
void generationsAndFailures() {
  BrokerConnectionStatus status; const auto first = status.begin(1000); status.upgraded(first, 1100); status.acknowledged(first, true, 1200, 1700000000000);
  status.disconnected(first, "tls_failed"); status.disconnected(first, "transport_closed");
  require(status.evidence().phase == BrokerPhase::Reconnecting && status.evidence().reason == "tls_failed" && !JsonParser::parse(brokerStatusJson(status.evidence(), 1300)).get("online").boolean(), "Disconnect preserved online or erased the first safe cause");
  const auto second = status.begin(1400); status.upgraded(second, 1500); status.acknowledged(first, true, 1600, 1700000000500); status.disconnected(first, "authorization_failed");
  require(status.evidence().phase == BrokerPhase::AwaitingAck, "A retired connection changed current evidence");
  status.acknowledged(second, true, 1700, 1700000001000); require(status.evidence().reason.empty() && status.evidence().readyUtc == 1700000001000, "Successful broker ACK did not clear current failure or retained an old generation's ready timestamp");
  status.stop(); status.acknowledged(second, true, 1800, 1700000002000);
  require(status.evidence().phase == BrokerPhase::Stopped, "Late ACK resurrected stopped service state");
  status.waiting(); status.acknowledged(second, true, 1900, 1700000003000);
  require(status.evidence().phase == BrokerPhase::WaitingRegistration && !status.evidence().readyUtc && !status.evidence().heartbeatUtc, "Old registration leaked its network evidence into new registration");
  const auto third = status.begin(2000); status.disconnected(third, "PRIVATE_TOKEN=https://private.invalid/password");
  const auto safe = brokerStatusJson(status.evidence(), 2100); require(safe.find("PRIVATE_TOKEN") == std::string::npos && safe.find("private.invalid") == std::string::npos, "Arbitrary failure text escaped the fixed allowlist");
}
void wireValidation() {
  BrokerConnectionStatus status; const auto generation = status.begin(1000); status.upgraded(generation, 1100); status.acknowledged(generation, true, 1200, 1700000000000);
  const auto good = brokerWire(status.evidence(), 1300); require(parseBrokerWire(good, 1400).phase == BrokerPhase::Online, "Valid broker status wire failed");
  rejected([&] { parseBrokerWire(good, 1299); }); rejected([&] { parseBrokerWire(good, 3301); }); rejected([&] { parseBrokerWire(std::string(2049, 'x'), 1400); });
  for (const auto& pair : {std::pair{"\"version\":1", "\"version\":2"}, std::pair{"\"phase\":\"online\"", "\"phase\":\"made_up\""}, std::pair{"\"verifiedTick\":1200", "\"verifiedTick\":1401"}, std::pair{"\"readyUtc\":1700000000000", "\"readyUtc\":0"}, std::pair{"\"reason\":\"\"", "\"reason\":\"raw_private_error\""}, std::pair{"\"heartbeatTick\":1200", "\"heartbeatTick\":-1"}}) {
    auto invalid = good; const auto at = invalid.find(pair.first); require(at != std::string::npos, "QA invalid wire replacement missing"); invalid.replace(at, std::strlen(pair.first), pair.second); rejected([&] { parseBrokerWire(invalid, 1400); });
  }
  const auto publicJson = JsonParser::parse(brokerStatusJson(status.evidence(), 1400));
  const auto* object = std::get_if<Json::Object>(&publicJson.value); require(object && object->size() == 6, "Public status leaked internal PID/clock/proof fields");
}
void concurrentEvidence() {
  BrokerConnectionStatus status; std::atomic<bool> done{false}; std::atomic<bool> valid{true};
  std::thread reader([&] { while (!done) { try { auto value = status.evidence(); auto json = JsonParser::parse(brokerStatusJson(value, 2000)); if (json.get("online").boolean() && (!value.readyUtc || !value.verifiedTick)) valid = false; } catch (...) { valid = false; } } });
  for (int i = 0; i < 5000; ++i) { const auto old = status.begin(1000); status.upgraded(old, 1100); status.acknowledged(old, true, 1200, 1700000000000); status.disconnected(old, "transport_closed"); status.waiting(); status.acknowledged(old, true, 1300, 1700000001000); }
  done = true; reader.join(); require(valid, "Concurrent broker snapshot was torn or mixed generations");
}
struct PipeMeasurements {int64_t medianUs=0,maxUs=0,blockedReaderShutdownUs=0;};
PipeMeasurements ownedProcesses() {
  const auto endpoint = L"\\\\.\\pipe\\MultitapHost.BrokerStatus.QA." + nonce();
  Child first(endpoint); const auto creation = brokerProcessCreation(first.process); require(creation != 0, "QA child creation identity missing");
  BrokerEvidence evidence; bool connected = false;
  for (int i = 0; i < 30 && !connected; ++i) { try { evidence = readBrokerStatusPipe(endpoint, first.pid, creation, false); connected = true; } catch (...) { Sleep(50); } }
  require(connected && evidence.phase == BrokerPhase::Online, "Actual owned child pipe did not return ACK fixture evidence");
  std::vector<int64_t> latency;
  for (int i = 0; i < 8; ++i) {const auto begun=Clock::now();require(readBrokerStatusPipe(endpoint, first.pid, creation, false).phase == BrokerPhase::Online, "Repeated IPC snapshots lost the service endpoint");latency.push_back(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-begun).count());}
  rejected([&] { readBrokerStatusPipe(endpoint, first.pid, creation + 1, false); });
  rejected([&] { readBrokerStatusPipe(endpoint, GetCurrentProcessId(), brokerProcessCreation(GetCurrentProcess()), false); });
  require(!brokerSystemProcess(first.process), "QA is expected to run as its ordinary user, not System");
  rejected([&] { readBrokerStatusPipe(endpoint, first.pid, creation, true); });
  first.finish(); rejected([&] { readBrokerStatusPipe(endpoint, first.pid, creation, false); });
  Child second(endpoint); const auto secondCreation = brokerProcessCreation(second.process); connected = false;
  for (int i = 0; i < 30 && !connected; ++i) { try { evidence = readBrokerStatusPipe(endpoint, second.pid, secondCreation, false); connected = true; } catch (...) { Sleep(50); } }
  require(connected && evidence.phase == BrokerPhase::Online, "Replacement owned QA process could not publish its own status");
  rejected([&] { readBrokerStatusPipe(endpoint, first.pid, creation, false); });
  require(WaitNamedPipeW(endpoint.c_str(),300)!=FALSE,"QA idle reader could not find endpoint");
  BrokerHandle idleReader(CreateFileW(endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));require(idleReader.value!=INVALID_HANDLE_VALUE,"QA idle reader could not connect");
  const auto shutdown=Clock::now();second.finish();
  const auto shutdownUs=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-shutdown).count();require(shutdownUs<2000000,"A stuck UI reader delayed server shutdown");
  std::sort(latency.begin(),latency.end());return {latency[latency.size()/2],latency.back(),shutdownUs};
}
}
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc == 3 && std::wstring(argv[1]) == L"--observe-installed") return observeInstalled(argv[2]);
    if (argc == 4 && std::wstring(argv[1]) == L"--serve") return child(argv[2], argv[3]);
    require(argc == 1, "Unknown QA arguments"); serviceIdentityPolicy();acknowledgementEvidence(); staleAndClock(); generationsAndFailures(); wireValidation(); concurrentEvidence();const auto measured=ownedProcesses();
    std::cout << "{\"kind\":\"host-broker-live-status-qa\",\"groups\":7,\"serviceIdentityCases\":16,\"actualOwnedChildProcesses\":2,\"repeatedProtectedPipeReads\":true,\"pipeBenchmarkReads\":8,\"pipeMedianUs\":"<<measured.medianUs<<",\"pipeMaxUs\":"<<measured.maxUs<<",\"blockedReaderShutdownUs\":"<<measured.blockedReaderShutdownUs<<",\"peerPidAndCreationChecked\":true,\"systemPeerRequirementRefusesOrdinaryQaProcess\":true,\"processExitAndReplacementInvalidateOldStatus\":true,\"ackGenerationAndMonotonicFreshness\":true,\"ackEventsAreFixtures\":true,\"actualBrokerWssTested\":false,\"actualSystemServicePipeTested\":false,\"installedHostChanged\":false,\"serviceChanges\":0,\"driverChanges\":0,\"runtimeFilesWritten\":false}\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
