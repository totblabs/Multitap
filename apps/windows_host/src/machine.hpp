#pragma once
#include "service_worker_drain.hpp"
#include "support.hpp"
#include "host_text.hpp"
#include "owned_output_size.hpp"
#include <winsvc.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <bcrypt.h>
#include <thread>

namespace host {
struct WinHandle {
  HANDLE value = nullptr;
  WinHandle() = default; explicit WinHandle(HANDLE v) : value(v) {}
  ~WinHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  WinHandle(const WinHandle&) = delete; WinHandle& operator=(const WinHandle&) = delete;
  WinHandle(WinHandle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
  WinHandle& operator=(WinHandle&& other) noexcept { if (this != &other) { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = std::exchange(other.value, nullptr); } return *this; }
};
struct ServiceHandle { SC_HANDLE value = nullptr; explicit ServiceHandle(SC_HANDLE v) : value(v) { if (!v) winCheck(FALSE, "Windows service access"); } ~ServiceHandle() { if (value) CloseServiceHandle(value); } };
inline bool elevated() { BOOL admin = FALSE; SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY; PSID sid = nullptr; if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &sid)) { CheckTokenMembership(nullptr, sid, &admin); FreeSid(sid); } return admin != FALSE; }
inline void requireAdmin() { if (!elevated()) throw std::runtime_error("Administrator approval is required to change machine settings"); }
inline std::wstring executablePath() { std::wstring path(32768, 0); DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size())); if (!length || length >= path.size()) winCheck(FALSE, "Host executable path"); path.resize(length); return path; }
inline std::wstring randomName() { std::array<BYTE, 16> bytes{}; check(BCryptGenRandom(nullptr, bytes.data(), DWORD(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG), "Secure local pipe name"); const wchar_t hex[] = L"0123456789abcdef"; std::wstring out; for (BYTE value : bytes) { out += hex[value >> 4]; out += hex[value & 15]; } return out; }
inline void writePublicConfig(const std::string& json) {
  requireAdmin(); prepareMachineDirectory(); const auto path = machineDirectory() / L"settings.json"; const auto temporary = path.wstring() + temporarySuffix();
  const DWORD attributes = GetFileAttributesW(temporary.c_str()); if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Unsafe host settings file");
  { std::ofstream file(temporary, std::ios::binary | std::ios::trunc); if (!file.write(json.data(), std::streamsize(json.size()))) throw std::runtime_error("Cannot write machine settings"); }
  winCheck(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "Save machine settings");
}
inline Json readPublicConfig() { const auto path = machineDirectory() / L"settings.json"; std::ifstream file(path, std::ios::binary); if (!file) return {}; if(!trustedMachineFile(path))throw std::runtime_error("Host machine settings permissions need administrator repair");std::string bytes((std::istreambuf_iterator<char>(file)), {}); return JsonParser::parse(bytes); }
inline void savePublicEnrollment(const Credentials& value) {
  const auto old = readPublicConfig();
  // Keep the administrator's display manager choice ("v1"/"v2") across re-enrollment.
  const auto manager=old.get("displayManager").string();
  // And the administrator's video pixel budget (stream_encode_size.hpp).
  const auto budget=old.get("videoPixelBudget").number(-1);const bool keepBudget=std::isfinite(budget)&&budget==std::floor(budget)&&budget>=921600&&budget<=double(uint64_t(UINT32_MAX)*2/3);
  writePublicConfig("{\"origin\":" + jsonQuote(utf8(value.origin)) + (value.httpConsentOrigin.empty()?"":",\"httpConsentOrigin\":"+jsonQuote(utf8(Origin(value.origin,value.httpConsentOrigin).text))) + ",\"desktopId\":" + jsonQuote(value.desktopId) + (manager=="v1"||manager=="v2"?",\"displayManager\":"+jsonQuote(manager):"") + (keepBudget?",\"videoPixelBudget\":"+std::to_string(uint64_t(budget)):"") + ",\"signinStreamingEnabled\":" + (old.get("signinStreamingEnabled").boolean()?"true":"false") + '}');
}
inline std::string testBroker(const std::wstring& address,const std::wstring& consent={}) {
  const Origin origin(address,consent);if(!origin.policy.secure())requireAdmin();
  return brokerHealth(origin).get("version").string();
}
inline void serviceControl(const wchar_t* name, DWORD control) { requireAdmin(); ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)); ServiceHandle service(OpenServiceW(manager.value, name, SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS)); SERVICE_STATUS status{}; if (control == SERVICE_CONTROL_STOP) { if (!ControlService(service.value, control, &status) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) winCheck(FALSE, "Stop host service"); } else if (!StartServiceW(service.value, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) winCheck(FALSE, "Start host service"); }
inline DWORD interactiveSession() {
  const DWORD console = WTSGetActiveConsoleSessionId();
  WTS_SESSION_INFOW* sessions = nullptr; DWORD count = 0; DWORD selected = 0xffffffff;
  if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return selected;
  for (DWORD i = 0; i < count; i++) if (sessions[i].SessionId && sessions[i].State == WTSActive) { selected = sessions[i].SessionId; if (selected == console) break; }
  WTSFreeMemory(sessions); return selected;
}
struct VirtualMonitor { std::string id; std::wstring name; LONG left = 0, top = 0; DWORD width = 0, height = 0; bool primary = false; bool operator==(const VirtualMonitor&) const = default; };
struct DisplayState { bool installed = false, deviceReady = false, active = false, owned = false, remoteSession = false, configurationPending = false, rebootRequired = false, monitorControlSupported = false, clipboardSupported = false, viewportProvisioning = false, recoveryRequired = false; DWORD foreign = 1; LONG left = 0, top = 0; DWORD width = 0, height = 0; std::wstring name; std::string error; std::vector<VirtualMonitor> monitors; bool ready() const { return installed && deviceReady && active && owned && !recoveryRequired && !configurationPending && !rebootRequired && foreign == 0 && width >= 320 && height >= 180 && checkedFrameLayout(width,height,false) && name.rfind(L"\\\\.\\DISPLAY", 0) == 0 && !monitors.empty(); } bool operator==(const DisplayState&) const = default; };
inline DWORD displayDimension(const Json& value,const char* key){const auto n=value.get(key).number(-1);return std::isfinite(n)&&n>=0&&n<=double(UINT32_MAX)&&n==std::floor(n)?DWORD(n):0;}
inline DisplayState parseDisplayState(const Json& value) {
  DisplayState out; out.installed = value.get("installed").boolean(); out.deviceReady = value.get("deviceReady").boolean(); out.active = value.get("activeDisplay").boolean(); out.owned = value.get("owned").boolean(); out.foreign = DWORD(value.get("foreignDeviceCount").number(1)); out.width = displayDimension(value,"width"); out.height = displayDimension(value,"height"); out.left = LONG(value.get("left").number()); out.top = LONG(value.get("top").number()); out.name = wide(value.get("displayDeviceName").string());
  out.recoveryRequired = value.get("recoveryRequired").boolean(); out.viewportProvisioning = value.get("viewportProvisioning").boolean(); out.clipboardSupported = value.get("clipboardSupported").boolean(); out.monitorControlSupported = value.get("monitorControlSupported").boolean(); out.remoteSession = value.get("remoteSession").boolean(); out.configurationPending = value.get("configurationPending").boolean(); out.rebootRequired = value.get("rebootRequired").boolean(); out.error = value.get("error").string();
  if (const auto* monitors = std::get_if<Json::Array>(&value.get("displays").value)) for (const auto& item : *monitors) { VirtualMonitor monitor; monitor.id = item.get("id").string(); monitor.name = wide(item.get("displayDeviceName").string()); monitor.left = LONG(item.get("left").number()); monitor.top = LONG(item.get("top").number()); monitor.width = displayDimension(item,"width"); monitor.height = displayDimension(item,"height"); monitor.primary = item.get("primary").boolean(); if (item.get("virtual").boolean() && !monitor.id.empty() && monitor.id.size() <= 2 && monitor.id.find_first_not_of("0123456789") == std::string::npos && checkedFrameLayout(monitor.width,monitor.height,false) && monitor.name.rfind(L"\\\\.\\DISPLAY", 0) == 0 && out.monitors.size() < 16) out.monitors.push_back(std::move(monitor)); }
  return out;
}
// Read-only probes may include all advertised display modes. Drain the child
// concurrently so a response larger than the OS pipe buffer cannot deadlock.
struct DisplayHelperOutput { DWORD exitCode = 1; bool timedOut = false; Json value; };
inline bool nativeDisplaySelected();
inline DisplayHelperOutput executeDisplayHelper(const std::filesystem::path& path, const std::wstring& action, DWORD timeout) {
  // A selected provider must never fall through to another driver's helper,
  // including direct repair/install callers that bypass displayHelper().
  if(nativeDisplaySelected())throw std::runtime_error("native_display_legacy_operation_blocked");
  if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("Display helper unavailable");
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE}; HANDLE input = nullptr, output = nullptr;
  winCheck(CreatePipe(&input, &output, &security, 16384), "Display helper pipe");
  WinHandle reader(input), writer(output); winCheck(SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0), "Display helper pipe inheritance");
  WinHandle empty(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (empty.value == INVALID_HANDLE_VALUE) winCheck(FALSE, "Display helper input");
  auto physicalCreation=service_workers::reserveCurrent(service_workers::Role::displayHelper);
  WinHandle physicalJob(physicalCreation.createTrackingJob());
  const DWORD attributeCount=physicalJob.value?2:1;
  SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, attributeCount, 0, &size); std::vector<BYTE> attributes(size);
  auto* list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  winCheck(InitializeProcThreadAttributeList(list, attributeCount, 0, &size), "Display helper attributes");
  struct Cleanup { PPROC_THREAD_ATTRIBUTE_LIST value; ~Cleanup() { DeleteProcThreadAttributeList(value); } } cleanup{list};
  HANDLE handles[]{writer.value, empty.value};
  winCheck(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr), "Display helper inherited handles");
  if(physicalJob.value)winCheck(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,&physicalJob.value,sizeof(physicalJob.value),nullptr,nullptr),"Display helper tracking job");
  STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = list;
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES; startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = writer.value; startup.StartupInfo.hStdInput = empty.value;
  PROCESS_INFORMATION process{}; std::wstring command = L"\"" + path.wstring() + L"\" " + action;
  physicalCreation.requireLaunchAllowed();
  winCheck(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, path.parent_path().c_str(), &startup.StartupInfo, &process), "Start display helper");
  WinHandle child(process.hProcess), thread(process.hThread);physicalCreation.retain(child.value); writer = WinHandle();
  std::string bytes; bool exceeded = false;
  std::thread drain([&] { std::array<char, 4096> buffer{}; DWORD length = 0; while (ReadFile(reader.value, buffer.data(), DWORD(buffer.size()), &length, nullptr) && length) { if (bytes.size() + length <= 65536) bytes.append(buffer.data(), length); else exceeded = true; } });
  DisplayHelperOutput result;
  result.timedOut = WaitForSingleObject(child.value, timeout) != WAIT_OBJECT_0;
  if (result.timedOut) { TerminateProcess(child.value, ERROR_TIMEOUT); WaitForSingleObject(child.value, 5000); }
  GetExitCodeProcess(child.value, &result.exitCode); drain.join();
  if (exceeded) throw std::runtime_error("Display helper response exceeds bounds");
  // A terminated helper may never have emitted its final result, or may have
  // stopped in the middle of it. Preserve timeout/exit metadata without
  // inventing a completed repair result; callers still receive an object.
  if (!bytes.empty()) {
    try { result.value = JsonParser::parse(bytes); }
    catch (const std::exception&) { if (!result.timedOut) throw; result.value.value = Json::Object{}; }
  } else if (result.timedOut) result.value.value = Json::Object{};
  return result;
}
inline DisplayHelperOutput displayHelper(const std::wstring& action, DWORD timeout = 5000) {
  if (action != L"status" && action != L"topology-status" && action != L"viewport-status" && action != L"viewport-recover" && action != L"mode-catalog" && action != L"mode-catalog-status" && action != L"topology 1" && action != L"topology 2") throw std::runtime_error("Invalid display helper operation");
  return executeDisplayHelper(std::filesystem::path(executablePath()).parent_path() / L"MultitapDisplay.exe", action, timeout);
}
inline DisplayHelperOutput applyViewport(const std::wstring& device,int width,int height,DWORD timeout = 75000) {
  const auto prefix=std::wstring(L"\\\\.\\DISPLAY");
  if(!timeout||timeout>75000||device.rfind(prefix,0)!=0||device.size()<=prefix.size()||device.size()>prefix.size()+8||device.find_first_not_of(L"0123456789",prefix.size())!=std::wstring::npos||!ownedOutputSize(uint64_t(std::max(width,0)),uint64_t(std::max(height,0))))throw std::runtime_error("Invalid owned viewport target");
  return executeDisplayHelper(std::filesystem::path(executablePath()).parent_path()/L"MultitapDisplay.exe",L"viewport \""+device+L"\" "+std::to_wstring(width)+L" "+std::to_wstring(height)+L" 60",timeout);
}
}
#include "native_display_status.hpp"
namespace host {
inline DisplayState virtualDisplayStatus() {
  if(nativeDisplaySelected())return nativeDisplayStatus();
  try { const auto result = displayHelper(L"status"); return result.timedOut || result.exitCode ? DisplayState{} : parseDisplayState(result.value); } catch (...) { return {}; }
}
inline DisplayHelperOutput virtualDisplayRepairPlan() {
  requireAdmin();
  return executeDisplayHelper(std::filesystem::path(executablePath()).parent_path()/L"MultitapDisplay.exe",L"repair-plan",15000);
}
inline DisplayHelperOutput repairVirtualDisplay(const std::string& planId) {
  requireAdmin();
  if(planId.size()!=64||planId.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("Invalid display repair plan");
  return executeDisplayHelper(std::filesystem::path(executablePath()).parent_path()/L"MultitapDisplay.exe",L"repair-install "+wide(planId),150000);
}
inline bool verifiedOwnedDisplayIdentity(const Json& value) {
  const auto& diagnostic=value.get("diagnostics");
  return value.get("installed").boolean()&&value.get("owned").boolean()&&
    value.get("deviceCount").number()==1&&value.get("foreignDeviceCount").number(1)==0&&
    value.get("enumerationError").number(1)==0&&!value.get("recoveryRequired").boolean()&&
    diagnostic.get("ownerMarkerPresent").boolean()&&!diagnostic.get("ownerMarkerMissingDevice").boolean()&&
    !diagnostic.get("vendorPathConflict").boolean()&&diagnostic.get("deviceEnumerationError").number(1)==0;
}
inline bool reusableOwnedDisplayDriver(const Json& value) {
  return verifiedOwnedDisplayIdentity(value)&&value.get("deviceReady").boolean();
}
inline DisplayState configureVirtualDisplays(){
  requireAdmin();const auto helper=std::filesystem::path(executablePath()).parent_path()/L"MultitapDisplay.exe";if(!std::filesystem::is_regular_file(helper))throw std::runtime_error(ui_text::utf8(ui_text::Id::DisplayHelperMissing));
  const auto preserved=[](const DisplayState& state){return state.ready()&&(state.monitors.size()==1||state.monitors.size()==2);};
  const auto probe=executeDisplayHelper(helper,L"status",10000);
  if(probe.timedOut||probe.exitCode)throw std::runtime_error("virtual_display_status_failed");
  auto state=parseDisplayState(probe.value);if(state.recoveryRequired)throw std::runtime_error("viewport_recovery_required");if(preserved(state))return state;
  // A verified, already installed device pending restart/console activation
  // must not be installed again merely because an RDP session sees no output.
  const bool verified=verifiedOwnedDisplayIdentity(probe.value);
  const bool reusable=verified&&state.deviceReady;
  if((verified&&state.rebootRequired)||(reusable&&state.remoteSession))return state;
  DisplayHelperOutput installed;
  if(!reusable){
    installed=executeDisplayHelper(helper,L"install",90000);
    if(installed.timedOut)throw std::runtime_error("display_setup_timeout");
    if(installed.exitCode){
      const auto code=installed.value.get("error").string();
      if(code=="existing_virtual_display_configuration"||code=="stale_virtual_display_ownership_requires_review"||code=="existing_virtual_display_requires_review")throw std::runtime_error(code);
      throw std::runtime_error("display_setup_failed");
    }
  }
  state=virtualDisplayStatus();if(preserved(state)||state.rebootRequired||installed.value.get("rebootRequired").boolean())return state;
  if(state.owned&&!state.foreign&&state.monitors.empty()){
    const auto configured=executeDisplayHelper(helper,L"configure",90000);if(configured.timedOut||configured.exitCode)throw std::runtime_error(ui_text::utf8(ui_text::Id::DisplayConfigureFailed));state=virtualDisplayStatus();
  }
  return state;
}
}
