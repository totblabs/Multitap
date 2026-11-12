#pragma once
#include "service_worker_drain.hpp"
// drive-v1 drive letters in the VM (docs/drive-redirection.md).
//  * Service side (LocalSystem): the in-box WebClient service (present, not
//    disabled, started on demand; its size caps raised, nothing else) and a
//    one-shot helper started with the session user's own token.
//  * Helper side (`MultitapHost.exe --drive-mount`, as that user): maps,
//    labels and announces the drives in the user's logon session, removes
//    them again, and sweeps leftovers of an earlier crashed session. Only
//    connections named exactly \\localhost@<port>\Multitap-<token> are touched.
#include "drive_protocol.hpp"
#include "drive_open.hpp"
#include <winnetwk.h>
#include <shlobj.h>
#include <iphlpapi.h>
#pragma comment(lib, "mpr.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace host::drive {
// ---- names (pure) ----
inline std::wstring remoteName(uint16_t port, const std::string& token) { return L"\\\\localhost@" + std::to_wstring(port) + L"\\" + wide(token); }
// A connection this feature made: \\localhost@<port>\Multitap-<43 base64url>.
inline std::optional<std::string> ownToken(const std::wstring& remote) {
  static const std::wstring head = L"\\\\localhost@";
  if (remote.size() < head.size() + 2 || _wcsnicmp(remote.c_str(), head.c_str(), head.size())) return std::nullopt;
  const auto slash = remote.find(L'\\', head.size());
  if (slash == std::wstring::npos || slash == head.size() || slash - head.size() > 5) return std::nullopt;
  for (size_t i = head.size(); i < slash; ++i) if (remote[i] < L'0' || remote[i] > L'9') return std::nullopt;
  std::string token; try { token = utf8(remote.substr(slash + 1)); } catch (...) { return std::nullopt; }
  if (!shareToken(token)) return std::nullopt;
  return token;
}
// Explorer's label key of a network drive: '\' becomes '#'.
inline std::wstring labelKey(const std::wstring& remote) { std::wstring key = remote; std::replace(key.begin(), key.end(), L'\\', L'#'); return key; }
inline constexpr const wchar_t* mountPointsKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MountPoints2";
// The highest free letter from Z down to D.
inline std::optional<wchar_t> freeLetter(DWORD logicalDrives, const std::set<wchar_t>& taken) {
  for (wchar_t letter = L'Z'; letter >= L'D'; --letter) if (!(logicalDrives & (1u << (letter - L'A'))) && !taken.count(letter)) return letter;
  return std::nullopt;
}
inline std::wstring driveLabel(const std::string& name) { return wide(name) + L" (PC)"; } // "<name> (PC)"

// ---- helper side (runs as the session user) ----
inline std::wstring webClientProviderName() {
  wchar_t name[256]{}; DWORD size = sizeof(name);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\WebClient\\NetworkProvider", L"Name", RRF_RT_REG_SZ, nullptr, name, &size) != ERROR_SUCCESS) return {};
  return name;
}
struct Connection { std::wstring local, remote; };
inline std::vector<Connection> ownConnections() {
  std::vector<Connection> out; HANDLE enumeration = nullptr;
  if (WNetOpenEnumW(RESOURCE_CONNECTED, RESOURCETYPE_DISK, 0, nullptr, &enumeration) != NO_ERROR) return out;
  std::vector<BYTE> buffer(65536);
  for (int rounds = 0; rounds < 64; ++rounds) {
    DWORD count = DWORD(-1), size = DWORD(buffer.size());
    const DWORD result = WNetEnumResourceW(enumeration, &count, buffer.data(), &size);
    if (result != NO_ERROR) break;
    const auto* items = reinterpret_cast<const NETRESOURCEW*>(buffer.data());
    for (DWORD i = 0; i < count; ++i) if (items[i].lpRemoteName && ownToken(items[i].lpRemoteName)) out.push_back({items[i].lpLocalName ? items[i].lpLocalName : L"", items[i].lpRemoteName});
  }
  WNetCloseEnum(enumeration);
  return out;
}
// WebDAV connections of this logon session that are not ours: {foreign, unknown}.
// `unknown` is true when the list could not be read (treated as "in use").
struct DavUse { size_t foreign = 0; bool unknown = false; };
inline DavUse foreignDavConnections() {
  DavUse use; const auto provider = webClientProviderName();
  HANDLE enumeration = nullptr;
  const DWORD opened = WNetOpenEnumW(RESOURCE_CONNECTED, RESOURCETYPE_DISK, 0, nullptr, &enumeration);
  if (opened == ERROR_NO_NETWORK || opened == ERROR_NOT_CONNECTED) return use; // no connections at all
  if (opened != NO_ERROR) { use.unknown = true; return use; }
  std::vector<BYTE> buffer(65536);
  for (int rounds = 0; rounds < 64; ++rounds) {
    DWORD count = DWORD(-1), size = DWORD(buffer.size());
    const DWORD result = WNetEnumResourceW(enumeration, &count, buffer.data(), &size);
    if (result == ERROR_NO_MORE_ITEMS) break;
    if (result != NO_ERROR) { use.unknown = true; break; }
    const auto* items = reinterpret_cast<const NETRESOURCEW*>(buffer.data());
    for (DWORD i = 0; i < count; ++i) {
      const bool dav = !provider.empty() && items[i].lpProvider && _wcsicmp(items[i].lpProvider, provider.c_str()) == 0;
      if (dav && !(items[i].lpRemoteName && ownToken(items[i].lpRemoteName))) ++use.foreign;
    }
  }
  WNetCloseEnum(enumeration);
  return use;
}
inline bool removeConnection(const Connection& connection) {
  const auto& name = connection.local.empty() ? connection.remote : connection.local;
  const DWORD result = WNetCancelConnection2W(name.c_str(), 0, TRUE);
  if (result == NO_ERROR && !connection.local.empty()) { const auto root = connection.local + L"\\"; SHChangeNotify(SHCNE_DRIVEREMOVED, SHCNF_PATHW, root.c_str(), nullptr); }
  RegDeleteKeyW(HKEY_CURRENT_USER, (std::wstring(mountPointsKey) + L"\\" + labelKey(connection.remote)).c_str());
  return result == NO_ERROR || result == ERROR_NOT_CONNECTED;
}
// Label keys of ours (all, or those of `tokens`), also without a live
// connection (a crash after labelling, a drive the user disconnected).
inline void sweepLabels(const std::set<std::string>* tokens) {
  HKEY key = nullptr; if (RegOpenKeyExW(HKEY_CURRENT_USER, mountPointsKey, 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS) return;
  std::vector<std::wstring> stale;
  for (DWORD index = 0; index < 4096; ++index) {
    wchar_t name[512]{}; DWORD size = 512;
    if (RegEnumKeyExW(key, index, name, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    std::wstring remote = name; std::replace(remote.begin(), remote.end(), L'#', L'\\');
    if (const auto token = ownToken(remote); token && (!tokens || tokens->count(*token))) stale.push_back(name);
  }
  for (const auto& name : stale) RegDeleteKeyW(key, name.c_str());
  RegCloseKey(key);
}
// One helper command: {"op":"map","port":P,"shares":[{"token":T,"label":L}]},
// {"op":"unmap","tokens":[T...]} or {"op":"sweep"}. Answers one JSON object.
inline std::string driveMountCommand(const Json& command) {
  const auto op = command.get("op").string();
  if (op == "open") {
    const auto* fields = std::get_if<Json::Object>(&command.value);
    const auto token = command.get("token").string();
    const double port = command.get("port").number(0);
    if (!fields || fields->size() != 3 || !shareToken(token) || port < 1024 ||
        port > 65535 || std::floor(port) != port) throw std::runtime_error("Invalid drive helper command");
    const auto remote = remoteName(uint16_t(port), token);
    // A fresh lookup in THIS logon, not a caller-provided/reused drive letter.
    const auto connections = ownConnections();
    const auto current = std::find_if(connections.begin(), connections.end(), [&](const auto& c) { return c.remote == remote; });
    const std::string error = current == connections.end() ? "drive_share_unknown" : openCurrentUserRoot(remote + L"\\");
    return "{\"error\":" + jsonQuote(error) + "}";
  }
  if (op == "sweep" || op == "unmap") {
    std::set<std::string> tokens; const bool all = op == "sweep";
    if (!all) { const auto* list = std::get_if<Json::Array>(&command.get("tokens").value); if (!list || list->size() > maxShares) throw std::runtime_error("Invalid drive helper command"); for (const auto& item : *list) { if (!shareToken(item.string())) throw std::runtime_error("Invalid drive helper command"); tokens.insert(item.string()); } }
    size_t removed = 0;
    for (const auto& connection : ownConnections()) { const auto token = *ownToken(connection.remote); if ((all || tokens.count(token)) && removeConnection(connection)) ++removed; }
    sweepLabels(all ? nullptr : &tokens);
    return "{\"removed\":" + std::to_string(removed) + '}';
  }
  if (op == "connections") { const auto use = foreignDavConnections(); return "{\"foreign\":" + std::to_string(use.foreign) + ",\"unknown\":" + (use.unknown ? "true" : "false") + '}'; }
  if (op != "map") throw std::runtime_error("Invalid drive helper command");
  const double port = command.get("port").number(-1);
  const auto* shares = std::get_if<Json::Array>(&command.get("shares").value);
  if (!(port >= 1024 && port <= 65535 && std::floor(port) == port) || !shares || shares->empty() || shares->size() > maxShares) throw std::runtime_error("Invalid drive helper command");
  const auto provider = webClientProviderName();
  std::set<wchar_t> taken; std::string results;
  for (const auto& share : *shares) {
    const auto token = share.get("token").string(), name = share.get("label").string();
    if (!shareToken(token) || !safeName(name, maxLabelUnits)) throw std::runtime_error("Invalid drive helper command");
    const auto remote = remoteName(uint16_t(port), token);
    // The label first, so Explorer's first view of the drive already has it.
    const auto label = driveLabel(name);
    RegSetKeyValueW(HKEY_CURRENT_USER, (std::wstring(mountPointsKey) + L"\\" + labelKey(remote)).c_str(), L"_LabelFromReg", REG_SZ, label.c_str(), DWORD((label.size() + 1) * sizeof(wchar_t)));
    std::string result = "{\"token\":" + jsonQuote(token);
    DWORD error = ERROR_ALREADY_ASSIGNED; std::optional<wchar_t> letter;
    for (int attempt = 0; attempt < 4 && (error == ERROR_ALREADY_ASSIGNED || error == ERROR_DEVICE_ALREADY_REMEMBERED); ++attempt) {
      letter = freeLetter(GetLogicalDrives(), taken);
      if (!letter) break;
      taken.insert(*letter);
      std::wstring local{*letter, L':'};
      NETRESOURCEW resource{}; resource.dwType = RESOURCETYPE_DISK; resource.lpLocalName = local.data(); resource.lpRemoteName = const_cast<wchar_t*>(remote.c_str()); resource.lpProvider = provider.empty() ? nullptr : const_cast<wchar_t*>(provider.c_str());
      // No CONNECT_UPDATE_PROFILE: never a persistent (logon-restored) mapping.
      error = WNetAddConnection2W(&resource, nullptr, nullptr, 0);
    }
    if (error == NO_ERROR && letter) {
      const std::wstring root{*letter, L':', L'\\'}; SHChangeNotify(SHCNE_DRIVEADD, SHCNF_PATHW, root.c_str(), nullptr);
      result += ",\"letter\":\"" + std::string(1, char(*letter)) + "\"}";
    } else {
      RegDeleteKeyW(HKEY_CURRENT_USER, (std::wstring(mountPointsKey) + L"\\" + labelKey(remote)).c_str());
      result += std::string(",\"error\":\"") + (!letter ? "drive_letter_unavailable" : "drive_map_failed") + "\",\"win32\":" + std::to_string(letter ? error : 0) + '}';
    }
    if (!results.empty()) results += ',';
    results += result;
  }
  return "{\"results\":[" + results + "]}";
}
// `MultitapHost.exe --drive-mount`: one command on stdin, one answer on stdout.
inline int driveMountHelper() {
  try {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!input || input == INVALID_HANDLE_VALUE || !output || output == INVALID_HANDLE_VALUE) return 2;
    std::string bytes; char buffer[4096]; DWORD count = 0;
    while (ReadFile(input, buffer, sizeof(buffer), &count, nullptr) && count) { bytes.append(buffer, count); if (bytes.size() > 16384) return 2; }
    const auto answer = driveMountCommand(JsonParser::parse(bytes)) + "\n";
    DWORD written = 0; return WriteFile(output, answer.data(), DWORD(answer.size()), &written, nullptr) && written == answer.size() ? 0 : 1;
  } catch (...) { return 1; }
}

// ---- service side (LocalSystem) ----
enum class WebClientState { missing, disabled, stopped, running, pending };
inline WebClientState webClientState() {
  const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT); if (!manager) return WebClientState::missing;
  const SC_HANDLE service = OpenServiceW(manager, L"WebClient", SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
  WebClientState state = WebClientState::missing;
  if (service) {
    std::vector<BYTE> config(8192); DWORD needed = 0;
    if (QueryServiceConfigW(service, reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config.data()), DWORD(config.size()), &needed) && reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config.data())->dwStartType == SERVICE_DISABLED) state = WebClientState::disabled;
    else { SERVICE_STATUS_PROCESS status{}; if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) state = status.dwCurrentState == SERVICE_RUNNING ? WebClientState::running : status.dwCurrentState == SERVICE_STOPPED ? WebClientState::stopped : WebClientState::pending; }
    CloseServiceHandle(service);
  }
  CloseServiceHandle(manager);
  return state;
}
inline bool webClientUsable() { const auto state = webClientState(); return state != WebClientState::missing && state != WebClientState::disabled; }
// The redirector's own size caps (bytes per file, bytes of one PROPFIND
// answer) are raised when lower. They only bound sizes; no authentication or
// trust value is read or written. They apply when WebClient (re)starts.
inline constexpr DWORD fileSizeLimit = 0xffffffffu, attributesLimit = 10000000u;
inline bool raiseWebClientLimits() {
  bool changed = false;
  for (const auto& [name, wanted] : {std::pair{L"FileSizeLimitInBytes", fileSizeLimit}, std::pair{L"FileAttributesLimitInBytes", attributesLimit}}) {
    DWORD value = 0, size = sizeof(value);
    const bool present = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\WebClient\\Parameters", name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
    if (present && value >= wanted) continue;
    if (RegSetKeyValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\WebClient\\Parameters", name, REG_DWORD, &wanted, sizeof(wanted)) == ERROR_SUCCESS) changed = true;
  }
  return changed;
}
// ---- preparing WebClient ----
// The redirector reads its size caps only when the service starts. Raising the
// registry values is therefore not enough when WebClient was already running
// (started by Office, Explorer, a SharePoint mapping ...): it would keep the
// 50,000,000-byte cap until a reboot and a 60 MB copy would fail in the VM.
// The service is restarted for that, but only when nothing else uses WebDAV;
// otherwise the cap stays and the client is told (`drive_webclient_old_limit`).
class WebClientSystem {
 public:
  virtual ~WebClientSystem() = default;
  virtual WebClientState state() = 0;
  virtual bool raiseLimits() = 0;            // true when a value was written
  virtual bool limitsNewerThanService() = 0; // the running instance started before the values were written
  virtual bool otherUse() = 0;               // WebDAV in use besides Multitap's own, or not provably idle
  virtual bool start() = 0;                  // start a stopped service and wait until it runs
  virtual bool restart() = 0;                // stop, start, wait until it runs
  virtual bool waitRunning() = 0;            // a service that is starting
};
struct WebClientOutcome {
  std::string error;       // "" when WebClient runs, else a fixed code
  bool started = false;    // it was stopped and is running now
  bool restarted = false;  // it was running with old caps and was restarted
  bool oldLimit = false;   // it runs with the old 50 MB cap
  uint32_t stage = 8;      // Host diagnostic (action 247): 6 started, 7 unavailable, 8 already running, 9 restarted for the caps, 10 old caps (other WebDAV use), 11 old caps (restart failed)
};
inline WebClientOutcome prepareWebClient(WebClientSystem& system) {
  WebClientOutcome outcome; auto state = system.state();
  if (state == WebClientState::missing || state == WebClientState::disabled) { outcome.error = "drive_webclient_unavailable"; outcome.stage = 7; return outcome; }
  const bool raised = system.raiseLimits();
  if (state == WebClientState::stopped) {
    if (!system.start()) { outcome.error = "drive_webclient_unavailable"; outcome.stage = 7; return outcome; }
    outcome.started = true; outcome.stage = 6; return outcome;
  }
  if (state == WebClientState::pending && !system.waitRunning()) { outcome.error = "drive_webclient_unavailable"; outcome.stage = 7; return outcome; }
  if (!raised && !system.limitsNewerThanService()) return outcome; // running with current caps
  if (system.otherUse()) { outcome.oldLimit = true; outcome.stage = 10; return outcome; }
  if (system.restart()) { outcome.restarted = true; outcome.stage = 9; return outcome; }
  if (system.state() != WebClientState::running && !system.waitRunning()) { outcome.error = "drive_webclient_unavailable"; outcome.stage = 7; return outcome; }
  outcome.oldLimit = true; outcome.stage = 11; return outcome;
}// Runs one helper command as the user of `session` (their own token, the one
// Explorer runs with: drive mappings belong to that logon session).
namespace detail {
inline Json runDriveMountHelperWithCreation(DWORD session,const std::string& command,DWORD timeoutMs,const wchar_t* argument,service_workers::Creation physicalCreation) {
  WinHandle physicalJob(physicalCreation.createTrackingJob());
  if (!session || session == 0xffffffff) throw std::runtime_error("no_interactive_session");
  HANDLE raw = nullptr; if (!WTSQueryUserToken(session, &raw)) throw std::runtime_error("no_interactive_session");
  WinHandle user(raw); HANDLE primaryRaw = nullptr;
  winCheck(DuplicateTokenEx(user.value, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID, nullptr, SecurityImpersonation, TokenPrimary, &primaryRaw), "Drive helper token");
  WinHandle primary(primaryRaw);
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE}; HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
  winCheck(CreatePipe(&inRead, &inWrite, &inherit, 16384), "Drive helper input"); WinHandle childIn(inRead), parentIn(inWrite);
  winCheck(CreatePipe(&outRead, &outWrite, &inherit, 16384), "Drive helper output"); WinHandle parentOut(outRead), childOut(outWrite);
  winCheck(SetHandleInformation(parentIn.value, HANDLE_FLAG_INHERIT, 0) && SetHandleInformation(parentOut.value, HANDLE_FLAG_INHERIT, 0), "Drive helper pipe inheritance");
  const DWORD attributeCount=physicalJob.value?2:1;SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, attributeCount, 0, &size); std::vector<BYTE> attributes(size);
  auto* list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  winCheck(InitializeProcThreadAttributeList(list, attributeCount, 0, &size), "Drive helper attributes");
  struct Cleanup { PPROC_THREAD_ATTRIBUTE_LIST value; ~Cleanup() { DeleteProcThreadAttributeList(value); } } cleanup{list};
  HANDLE handles[]{childIn.value, childOut.value};
  winCheck(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr), "Drive helper handles");
  struct Environment { void* value = nullptr; ~Environment() { if (value) DestroyEnvironmentBlock(value); } } environment;
  winCheck(CreateEnvironmentBlock(&environment.value, primary.value, FALSE), "Drive helper environment");
  WinHandle job(CreateJobObjectW(nullptr, nullptr)); winCheck(job.value != nullptr, "Drive helper job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  winCheck(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "Drive helper job limits");
  const auto executable = executablePath(); std::wstring line = L"\"" + executable + L"\" " + argument;
  HANDLE ownedJobs[]{physicalJob.value,job.value};if(physicalJob.value)winCheck(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,ownedJobs,sizeof(ownedJobs),nullptr,nullptr),"Drive helper tracking job");
  STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = list; wchar_t desktop[] = L"winsta0\\default"; startup.StartupInfo.lpDesktop = desktop;
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES; startup.StartupInfo.hStdInput = childIn.value; startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = childOut.value;
  physicalCreation.requireLaunchAllowed();
  PROCESS_INFORMATION process{};
  winCheck(CreateProcessAsUserW(primary.value, executable.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, environment.value, std::filesystem::path(executable).parent_path().c_str(), &startup.StartupInfo, &process), "Start drive helper");
  WinHandle child(process.hProcess), thread(process.hThread);
  physicalCreation.retain(child.value);
  if (!physicalJob.value&&!AssignProcessToJobObject(job.value, child.value)) { TerminateProcess(child.value, 1); winCheck(FALSE, "Isolate drive helper"); }
  childIn = WinHandle(); childOut = WinHandle();
  ResumeThread(thread.value);
  DWORD written = 0; WriteFile(parentIn.value, command.data(), DWORD(command.size()), &written, nullptr); parentIn = WinHandle();
  std::string bytes; bool exceeded = false;
  std::thread drain([&] { char buffer[4096]; DWORD count = 0; while (ReadFile(parentOut.value, buffer, sizeof(buffer), &count, nullptr) && count) { if (bytes.size() + count <= 65536) bytes.append(buffer, count); else exceeded = true; } });
  const bool timedOut = WaitForSingleObject(child.value, timeoutMs) != WAIT_OBJECT_0;
  if (timedOut) { TerminateProcess(child.value, ERROR_TIMEOUT); WaitForSingleObject(child.value, 5000); }
  drain.join();
  DWORD exitCode = 1; GetExitCodeProcess(child.value, &exitCode);
  if (timedOut || exceeded || exitCode) throw std::runtime_error("drive_helper_failed");
  return JsonParser::parse(bytes);
}

} // namespace detail
inline Json runDriveMountHelper(DWORD session,const std::string& command,DWORD timeoutMs=30000,const wchar_t* argument=L"--drive-mount"){
  return detail::runDriveMountHelperWithCreation(session,command,timeoutMs,argument,service_workers::reserveCurrent(service_workers::Role::driveMount));
}
// The typed cleanup entry takes ONLY the existing token list. No operation,
// executable argument, port, map/open/sweep command or timeout can be supplied.
inline Json runDriveUnmountHelper(DWORD session,const std::vector<std::string>& tokens,const service_workers::DriveCleanupPermit& owner){
  if(tokens.size()>maxShares)throw std::runtime_error("Invalid drive helper command");
  std::string list;for(const auto& token:tokens){if(!shareToken(token))throw std::runtime_error("Invalid drive helper command");if(!list.empty())list+=',';list+=jsonQuote(token);}
  return detail::runDriveMountHelperWithCreation(session,"{\"op\":\"unmap\",\"tokens\":["+list+"]}",10000,L"--drive-mount",owner.reserveUnmount());
}
// The real service, registry and connection layer.
class WindowsWebClient final : public WebClientSystem {
 public:
  WebClientState state() override { return webClientState(); }
  bool raiseLimits() override { return raiseWebClientLimits(); }
  bool limitsNewerThanService() override {
    // Parameters' last write against the running instance's start: the process
    // start time, or our own last (re)start when the service shares an older
    // process. A later write of any other value there only costs a restart.
    FILETIME written{}; HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, parameters, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    const bool ok = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &written) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!ok) return false;
    const auto pid = processId(); if (!pid) return false;
    uint64_t started = 0;
    if (const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) { FILETIME created{}, ignored{}; if (GetProcessTimes(process, &created, &ignored, &ignored, &ignored)) started = ticks(created); CloseHandle(process); }
    if (!started) return false; // cannot tell: do not disturb a running service
    started = std::max(started, appliedAt());
    return ticks(written) > started;
  }
  bool otherUse() override {
    if (establishedElsewhere()) return true;
    PWTS_SESSION_INFOW sessions = nullptr; DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return true;
    bool busy = false;
    for (DWORD i = 0; i < count && !busy; ++i) {
      const auto& info = sessions[i];
      if (info.SessionId == 0 || (info.State != WTSActive && info.State != WTSDisconnected)) continue;
      try {
        const auto answer = runDriveMountHelper(info.SessionId, "{\"op\":\"connections\"}", 10000);
        const auto* unknown = std::get_if<bool>(&answer.get("unknown").value);
        busy = answer.get("foreign").number(1) > 0 || !unknown || *unknown;
      } catch (const std::exception& failure) { busy = failure.what() != std::string("no_interactive_session"); }
    }
    WTSFreeMemory(sessions);
    return busy;
  }
  bool start() override { if (!control(SERVICE_START)) return false; const bool up = wait(WebClientState::running); if (up) stamp(); return up; }
  bool restart() override {
    if (!control(SERVICE_STOP) || !wait(WebClientState::stopped)) return false;
    const bool up = control(SERVICE_START) && wait(WebClientState::running);
    if (up) stamp();
    return up;
  }
  bool waitRunning() override { return wait(WebClientState::running); }
 private:
  static constexpr const wchar_t* parameters = L"SYSTEM\\CurrentControlSet\\Services\\WebClient\\Parameters";
  static constexpr const wchar_t* appliedKey = L"SOFTWARE\\Multitap\\Host\\Drive";
  static uint64_t ticks(const FILETIME& time) { return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime; }
  static uint64_t appliedAt() { uint64_t value = 0; DWORD size = sizeof(value); return RegGetValueW(HKEY_LOCAL_MACHINE, appliedKey, L"WebClientStartedAt", RRF_RT_REG_QWORD, nullptr, &value, &size) == ERROR_SUCCESS ? value : 0; }
  static void stamp() { FILETIME now{}; GetSystemTimeAsFileTime(&now); const uint64_t value = ticks(now); RegSetKeyValueW(HKEY_LOCAL_MACHINE, appliedKey, L"WebClientStartedAt", REG_QWORD, &value, sizeof(value)); }
  static bool wait(WebClientState wanted) { for (int i = 0; i < 150; ++i) { if (webClientState() == wanted) return true; Sleep(100); } return webClientState() == wanted; }
  static bool control(DWORD access) {
    const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT); if (!manager) return false;
    const SC_HANDLE service = OpenServiceW(manager, L"WebClient", access);
    bool ok = false;
    if (service) {
      if (access == SERVICE_START) ok = StartServiceW(service, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
      else { SERVICE_STATUS status{}; ok = ControlService(service, SERVICE_CONTROL_STOP, &status) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE; }
      CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return ok;
  }
  static bool loopback6(const UCHAR* address) { for (int i = 0; i < 15; ++i) if (address[i]) return false; return address[15] == 1; }
  static DWORD processId() {
    const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT); if (!manager) return 0;
    DWORD pid = 0;
    if (const SC_HANDLE service = OpenServiceW(manager, L"WebClient", SERVICE_QUERY_STATUS)) { SERVICE_STATUS_PROCESS status{}; DWORD needed = 0; if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) pid = status.dwProcessId; CloseServiceHandle(service); }
    CloseServiceHandle(manager);
    return pid;
  }
  // An established connection of the WebClient process to a non-loopback
  // address: a SharePoint / WebDAV server is in use (any session).
  static bool establishedElsewhere() {
    const auto pid = processId(); if (!pid) return false;
    for (const ULONG family : {ULONG(AF_INET), ULONG(AF_INET6)}) {
      std::vector<BYTE> table(1 << 14); ULONG size = ULONG(table.size()); DWORD result = ERROR_INSUFFICIENT_BUFFER;
      for (int attempt = 0; attempt < 4 && result == ERROR_INSUFFICIENT_BUFFER; ++attempt) { table.resize(size); result = GetExtendedTcpTable(table.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0); }
      if (result != NO_ERROR) return true; // not provably idle
      if (family == AF_INET) {
        const auto* rows = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(table.data());
        for (DWORD i = 0; i < rows->dwNumEntries; ++i) { const auto& row = rows->table[i]; if (row.dwOwningPid == pid && row.dwState == MIB_TCP_STATE_ESTAB && (row.dwRemoteAddr & 0xff) != 127) return true; }
      } else {
        const auto* rows = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(table.data());
        for (DWORD i = 0; i < rows->dwNumEntries; ++i) { const auto& row = rows->table[i]; if (row.dwOwningPid == pid && row.dwState == MIB_TCP_STATE_ESTAB && !loopback6(row.ucRemoteAddr)) return true; }
      }
    }
    return false;
  }
};
}