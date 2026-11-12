#pragma once
#include "settings_ui.hpp"
#include "pending_connection.hpp"
#include "windows_account_record.hpp"
#include "broker_status.hpp"
#include "display_maintenance.hpp"
#include "operational_log.hpp"
#include "console_transfer.hpp"
#include "windows_autologon_native.hpp"
#include "windows_account_password.hpp"
#include "encoder_settings.hpp"
#include "signin_stream_preference.hpp"
#include "device_status.hpp"
#include "setup_winfsp.hpp"
#include "display_adapter_inventory.hpp"
#include "host_service_restart.hpp"
#include "host_prerequisites_native.hpp"
#include <mmdeviceapi.h>
#include <iostream>
#include "desktop_name.hpp"
namespace host {
// The Print Spooler service as Host settings shows it (device_status.hpp `spooler`).
inline const char* printSpoolerServiceState() {
  const SC_HANDLE manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);if(!manager)return "unknown";
  const char* state="unknown";
  if(const SC_HANDLE service=OpenServiceW(manager,L"Spooler",SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS)){
    SERVICE_STATUS_PROCESS status{};std::vector<BYTE> config(8192);DWORD needed=0;
    if(QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&needed)){
      if(status.dwCurrentState==SERVICE_RUNNING)state="running";
      else if(QueryServiceConfigW(service,reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config.data()),DWORD(config.size()),&needed))state=reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config.data())->dwStartType==SERVICE_DISABLED?"disabled":status.dwCurrentState==SERVICE_STOPPED?"stopped":"starting";
    }
    CloseServiceHandle(service);
  }else if(GetLastError()==ERROR_SERVICE_DOES_NOT_EXIST)state="missing";
  CloseServiceHandle(manager);return state;
}
inline std::filesystem::path winfspPackagePath() { return std::filesystem::path(executablePath()).parent_path()/L"winfsp"/multitap::winfsp::MsiName; }
// The same endpoint streaming opens for system sound (default render, console role).
inline const char* observeAudioOutput() {
  if(GetSystemMetrics(SM_REMOTESESSION))return "remote";
  const HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);const char* result="unknown";
  {Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;Microsoft::WRL::ComPtr<IMMDevice> device;
   if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)))){const HRESULT found=enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device);result=SUCCEEDED(found)?"present":found==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)?"absent":"unknown";}}
  if(SUCCEEDED(init))CoUninitialize();return result;
}
// devices-v1 (device_status.hpp): each read is independent; one that fails stays unknown.
inline std::string observeDevices() {
  DeviceObservation o;
  try{o.winfsp=drive::fs::winfspInstalled();const auto web=drive::webClientState();o.webClient=web==drive::WebClientState::missing?"missing":web==drive::WebClientState::disabled?"disabled":"usable";}catch(...){}
  try{std::error_code ignored;o.winfspPackage=std::filesystem::is_regular_file(winfspPackagePath(),ignored);}catch(...){}
  try{o.usbPlatform=usb::platformSupported();o.usbPackage=usb::nativeDriverAccess().packagePresent();o.usbController=usb::controllerOwnerName(usb::controllerOwner());}catch(...){}
  try{const bool spooler=printSpoolerProcessId()!=0;o.print=printUnavailableReason(spooler,spooler&&printDriverInstalled(),spooler&&printMonitorInstalled());o.spooler=printSpoolerServiceState();}catch(...){}
  try{o.audio=observeAudioOutput();}catch(...){}
  return deviceStatusJson(o);
}
inline void scrubSettingsJson(Json& json) { if(auto* text=std::get_if<std::string>(&json.value))SecureZeroMemory(text->data(),text->size());else if(auto* object=std::get_if<Json::Object>(&json.value))for(auto& entry:*object)scrubSettingsJson(entry.second);else if(auto* array=std::get_if<Json::Array>(&json.value))for(auto& item:*array)scrubSettingsJson(item); }
struct SettingsScrubber { Json& value; ~SettingsScrubber(){scrubSettingsJson(value);} };
struct SettingsTextScrubber { std::string& value; ~SettingsTextScrubber(){SecureZeroMemory(value.data(),value.size());} };
inline const char* booleanJson(bool value) { return value ? "true" : "false"; }
template<class Apply, class Refresh>
std::string settingsApplyWithCapabilitiesRefresh(Apply&& apply, Refresh&& refresh) {
  const bool requested=applyThenRequestCapabilities(std::forward<Apply>(apply),std::forward<Refresh>(refresh));
  return "{\"ok\":true,\"capabilitiesRefreshRequested\":"+std::string(booleanJson(requested))+'}';
}
// installWinfsp (Host settings > 장치): the Host setup's own WinFsp step
// (setup_winfsp.hpp: pinned MSI by size, SHA-256 and signature, msiexec with
// /norestart, post-install check), offered only while WinFsp is missing and
// after the window's one confirmation. Windows is never restarted: `restart`
// only reports that msiexec asked for one.
inline std::string winfspInstallResult(multitap::winfsp::Backend& backend,const std::wstring& msi,bool installed,bool confirmed) {
  if(!confirmed)return "{\"action\":\"skipped\",\"restart\":false,\"error\":\"confirmation_required\"}";
  if(installed)return "{\"action\":\"skipped\",\"restart\":false,\"error\":\"winfsp_already_installed\"}";
  const auto outcome=multitap::winfsp::ensureInstalled(backend,msi);
  return std::string("{\"action\":\"")+outcome.action+"\",\"restart\":"+booleanJson(outcome.restart)+",\"error\":"+jsonQuote(outcome.error)+'}';
}
// hostAction 276 (설치 마무리): what Host settings' finish of an install did on
// this VM, one record per step, so the server's Host diagnostics show it.
//   stage 1: virtual display driver (installNativeDisplay). ok; error =
//     prerequisites::errorNumber; installed: a node is selected (created,
//     reused or kept); started: the device is started; reboot: Windows asked
//     for a restart; changed: the certificate was added in this run;
//     restartAttempted: the Host service had to be restarted; running: the
//     service uses the device (by itself or after that restart); recovery: a
//     failed run was undone completely.
//   stage 2: WinFsp (installWinfsp). ok; error = winfspErrorNumber; installed:
//     this run installed or upgraded it (false with error 2: it was there
//     already); reboot: msiexec asked for a restart.
// The settings RPC is a short elevated process of its own; only these two
// actions write the Host journal from it (the journal has a cross-process gate).
struct FinishJournal {
  bool previous=operationalLogEnabled;
  FinishJournal(){operationalLogEnabled=true;}
  ~FinishJournal(){operationalLogEnabled=previous;}
  FinishJournal(const FinishJournal&)=delete;FinishJournal& operator=(const FinishJournal&)=delete;
};
// Append only (hostAction 276 stage 2 `error`). 99: a code not listed here.
inline unsigned winfspErrorNumber(const std::string& value){
  static const char* const codes[]={"","confirmation_required","winfsp_already_installed","winfsp_package_mismatch","winfsp_busy","winfsp_blocked","winfsp_install_failed","winfsp_unavailable",
    "winfsp_dll_missing","winfsp_load_failed","winfsp_driver_missing","winfsp_path_invalid"};
  for(unsigned index=0;index<sizeof(codes)/sizeof(codes[0]);++index)if(value==codes[index])return index;
  return 99;
}
// startPrintSpooler: starts a stopped Print Spooler once. Its start type is
// never changed; a Disabled spooler (often deliberate hardening) is refused.
struct SpoolerAccess { std::function<std::string()> state; std::function<DWORD()> start; std::function<void()> wait; };
inline std::string startPrintSpoolerResult(const SpoolerAccess& access) {
  std::string now=access.state(),error;
  if(now=="stopped"){
    const DWORD started=access.start();
    if(started&&started!=ERROR_SERVICE_ALREADY_RUNNING)error=started==ERROR_SERVICE_DISABLED?"print_spooler_disabled":"print_spooler_start_failed";
    else for(int poll=0;poll<40;++poll){now=access.state();if(now!="starting"&&now!="stopped")break;access.wait();}
    if(error.empty()&&now!="running")error="print_spooler_start_failed";
    if(!error.empty())now=access.state();
  }else if(now!="running")error=now=="disabled"?"print_spooler_disabled":"print_spooler_unavailable";
  return "{\"state\":"+jsonQuote(now)+",\"error\":"+jsonQuote(error)+'}';
}
inline DWORD startPrintSpoolerService() {
  const SC_HANDLE manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);if(!manager)return GetLastError();
  DWORD error=ERROR_SUCCESS;
  if(const SC_HANDLE service=OpenServiceW(manager,L"Spooler",SERVICE_START)){if(!StartServiceW(service,0,nullptr))error=GetLastError();CloseServiceHandle(service);}else error=GetLastError();
  CloseServiceHandle(manager);return error;
}
// Both report the follow-up publication separately: a refresh that fails does
// not undo or relabel the completed change (applyThenRequestCapabilities).
template<class Refresh> std::string deviceChangeRpc(const char* field,const std::string& result,Refresh&& refresh) {
  bool requested=false;
  if(JsonParser::parse(result).get("error").string().empty()){try{requested=refresh();}catch(...){}}
  return std::string("{\"ok\":true,\"")+field+"\":"+result+",\"capabilitiesRefreshRequested\":"+booleanJson(requested)+'}';
}
// display-adapters-v1 for Host settings: the counts, plus the exact extra nodes
// (instance IDs stay on this machine) that 정리 may remove once confirmed.
inline DisplayAdapterOwnership settingsAdapterOwnership() {
  std::wstring selection;bool confirmed=false;
  try{confirmed=nativeDisplayConfirmed();if(confirmed)selection=wide(nativeDisplayConfig().get("deviceInstanceId").string());}catch(...){confirmed=false;}
  return readDisplayAdapterOwnership(confirmed,selection);
}
inline std::string observeDisplayAdapters() {
  try{
    const auto nodes=readDisplayAdapterNodes();if(!nodes)return "{\"read\":false}";
    const auto inventory=summarizeDisplayAdapters(*nodes,settingsAdapterOwnership());
    auto json=displayAdapterInventoryJson(inventory,"",DisplayAdapterCleanup::none);json.pop_back();
    return json+",\"extras\":"+displayAdapterExtrasJson(inventory)+'}';
  }catch(...){return "{\"read\":false}";}
}
// 정리: exactly the extras the administrator saw and confirmed (any change in
// between refuses the whole request), each re-proved from the live node right
// before its removal. Current and foreign adapters are never candidates.
struct AdapterCleanupAccess {
  std::function<std::optional<DisplayAdapterInventory>()> inventory;
  std::function<DisplayAdapterCleanup(const DisplayAdapterExtra&)> remove;
};
inline std::string cleanDisplayAdaptersResult(const AdapterCleanupAccess& access,const std::vector<std::wstring>& confirmed) {
  const auto inventory=access.inventory();
  if(!inventory||!inventory->read)return "{\"error\":\"display_adapters_unavailable\"}";
  const auto listed=[&](const std::wstring& id){return std::any_of(confirmed.begin(),confirmed.end(),[&](const auto& c){return legacyDisplayTextEqual(c,id);});};
  if(inventory->extras.empty()||confirmed.size()!=inventory->extras.size()||!std::all_of(inventory->extras.begin(),inventory->extras.end(),[&](const auto& extra){return listed(extra.instance);}))
    return "{\"error\":\"display_adapters_changed\"}";
  uint32_t removed=0,pending=0,failed=0,refused=0;
  for(const auto& extra:inventory->extras){
    switch(access.remove(extra)){
      case DisplayAdapterCleanup::removed:++removed;break;
      case DisplayAdapterCleanup::pending:++pending;break;
      case DisplayAdapterCleanup::refused:++refused;break;
      default:++failed;break;
    }
  }
  return "{\"error\":\"\",\"removed\":"+std::to_string(removed)+",\"pending\":"+std::to_string(pending)+",\"failed\":"+std::to_string(failed)+",\"refused\":"+std::to_string(refused)+'}';
}
inline AdapterCleanupAccess nativeAdapterCleanupAccess() {
  AdapterCleanupAccess access;
  access.inventory=[]()->std::optional<DisplayAdapterInventory>{const auto nodes=readDisplayAdapterNodes();if(!nodes)return std::nullopt;return summarizeDisplayAdapters(*nodes,settingsAdapterOwnership());};
  access.remove=[](const DisplayAdapterExtra& extra){
    const auto result=removeDisplayAdapter(extra.instance,settingsAdapterOwnership(),{DisplayAdapterKind::legacy,DisplayAdapterKind::duplicate,DisplayAdapterKind::unproven});
    if(result==DisplayAdapterCleanup::removed){if(extra.kind==DisplayAdapterKind::legacy)clearLegacyDisplayOwnerRecord();else forgetNativeAdapterNode(extra.instance);}
    return result;
  };
  return access;
}
inline std::string replaceUsbControllerResult(const usb::ReplaceAccess& access,bool confirmed) {
  if(!confirmed)return "{\"error\":\"confirmation_required\",\"restart\":false,\"removed\":0}";
  const auto result=usb::replaceController(access);
  return std::string("{\"error\":")+jsonQuote(result.code)+",\"restart\":"+booleanJson(result.restart)+",\"removed\":"+std::to_string(std::max(0,result.removed))+'}';
}
inline std::string displayRepairRpcResponse(const DisplayHelperOutput& output,bool plan) {
  return "{\"ok\":true,"+std::string(plan?"\"plan\":":"\"result\":")+jsonObject(output.value)+",\"helperExitCode\":"+std::to_string(output.exitCode)+",\"timedOut\":"+booleanJson(output.timedOut)+'}';
}
inline std::string serviceRestartRpc(const Json& request,const std::function<service_restart::Result()>& perform){
  const auto* object=std::get_if<Json::Object>(&request.value);
  const auto* confirmed=std::get_if<bool>(&request.get("confirmed").value);
  if(!object||object->size()!=3||request.get("action").string()!="restartServiceV1"||
      request.get("protocol").string()!=service_restart::protocol||!confirmed||!*confirmed)
    return "{\"ok\":false,\"error\":\"confirmation_required\"}";
  service_restart::Result result;
  try{if(perform)result=perform();}catch(...){ }
  const bool complete=result.outcome==service_restart::Outcome::completed;
  return std::string("{\"ok\":")+booleanJson(complete)+",\"restart\":"+service_restart::json(result)+
    (complete?"":",\"error\":\"host_service_restart_unverified\"")+'}';
}
inline std::string settingsStatus() {
  const auto config = readPublicConfig(); const auto display = virtualDisplayStatus();
  const bool native=nativeDisplaySelected();
  const auto service = readHostServiceSnapshot();
  std::string monitors = "["; for (const auto& item : display.monitors) { if (monitors.size() > 1) monitors += ','; monitors += "{\"id\":" + jsonQuote(item.id) + ",\"width\":" + std::to_string(item.width) + ",\"height\":" + std::to_string(item.height) + '}'; } monitors += ']';

  // The desktop's Windows account (windows_account_record.hpp).
  const auto account=windowsAccountStatus();
  return "{\"version\":\"" MULTITAP_HOST_VERSION_UTF8 "\",\"administrator\":" + std::string(booleanJson(elevated())) + ",\"serverOrigin\":" + jsonQuote(config.get("origin").string()) + ",\"httpConsentOrigin\":" + jsonQuote(config.get("httpConsentOrigin").string()) + ",\"registered\":" + booleanJson(!config.get("desktopId").string().empty()) + ",\"desktopName\":" + jsonQuote(readDesktopName()) + ",\"desktopId\":" + jsonQuote(config.get("desktopId").string()) +
    // host-machine-v1 (credential_binding.hpp): this Windows is a copy of a registered desktop; its registration was kept aside.
    ",\"copiedRegistration\":" + booleanJson(config.get("desktopId").string().empty() && !config.get("copiedRegistration").string().empty()) +
    ",\"service\":{\"installed\":" + (service.presenceKnown?booleanJson(service.installed):"null") + ",\"running\":" + (service.runningKnown?booleanJson(service.running):"null") + ",\"state\":" + jsonQuote(service.key()) + ",\"queryError\":" + std::to_string(service.error) + ",\"restartProtocol\":"+jsonQuote(service_restart::protocol)+"},\"broker\":"+publicBrokerStatus()+",\"interactiveSession\":" + booleanJson(sessionUnlocked(interactiveSession())) +
    ",\"windowsAutologon\":"+autologon::windowsAutologonStatus()+",\"display\":{\"installed\":" + booleanJson(display.installed) + ",\"deviceReady\":" + booleanJson(display.deviceReady) + ",\"activeDisplay\":" + booleanJson(display.active) + ",\"owned\":" + booleanJson(display.owned) + ",\"ready\":" + booleanJson(display.ready()) + ",\"foreignDeviceCount\":" + std::to_string(display.foreign) + ",\"remoteSession\":" + booleanJson(display.remoteSession) + ",\"configurationPending\":" + booleanJson(display.configurationPending) + ",\"recoveryRequired\":" + booleanJson(display.recoveryRequired) + ",\"rebootRequired\":" + booleanJson(display.rebootRequired) + ",\"error\":" + jsonQuote(display.error) + ",\"monitors\":" + monitors +
    ",\"provider\":"+jsonQuote(native?"native":"legacy")+",\"monitorControlSupported\":"+booleanJson(display.monitorControlSupported)+",\"viewportProvisioning\":"+booleanJson(display.viewportProvisioning)+
    "},\"consoleTransfer\":"+publicConsoleTransfer(observeConsoleTransfer(display,native))+",\"connectionRequest\":" + connectionRequestStatus() + ",\"windowsAccount\":{\"configured\":"+booleanJson(account.configured)+",\"username\":"+jsonQuote(account.username)+",\"error\":"+jsonQuote(account.error)+",\"clearProtocol\":"+jsonQuote(account.clearProtocol)+",\"clearRevision\":"+jsonQuote(account.clearRevision)+"},\"devices\":"+observeDevices()+",\"displayAdapters\":"+observeDisplayAdapters()+'}';
}
#ifdef MULTITAP_NATIVE_DISPLAY
inline std::string nativeRecoveryResetRpc(const Json& request){
  std::vector<NativeResetStep> prefix;
  const auto* confirmed=std::get_if<bool>(&request.get("confirmed").value);
  if(!confirmed||!*confirmed){prefix.push_back({"confirm","confirmation_required",false});return nativeResetResultJson(false,prefix,0);}
  prefix.push_back({"confirm","confirmed",true});
  DWORD own=UINT32_MAX;const bool known=ProcessIdToSessionId(GetCurrentProcessId(),&own)!=FALSE;
  const std::string session=!known||GetSystemMetrics(SM_REMOTESESSION)?"remote_session":own!=WTSGetActiveConsoleSessionId()?"not_console":!sessionUnlocked(own)?"locked":"console";
  prefix.push_back({"session",session,session=="console"});if(session!="console")return nativeResetResultJson(false,prefix,0);
  const auto console=[own]{
    NativeResetConsoleAccess access;
    access.eligible=[own]{return sameInteractiveConsole(own)&&sessionUnlocked(own)&&!GetSystemMetrics(SM_REMOTESESSION)&&elevated();};
    access.query=[]{return queryNativeCcd();};
    access.configure=windowsNativeCcdConfigure();
    access.adapter=nativeConsoleAdapterIdentity;
    access.profile=[]{return nativeStreamProfileFileAccess(machineDirectory()/L"private"/L"native-stream-profile.json").load();};
    try{access.ownedInstance=nativeDisplayConfig().get("deviceInstanceId").string();access.machine=machineUuid();}catch(...){ }
    return nativeResetConsoleStep(access);
  };
  return requestNativeRecoveryReset(console,prefix);
}
#endif
inline int settingsRpc() {
  // Host settings may pass its own language choice (MULTITAP_LANG=ko|en, its
  // "언어 / Language" setting); without it the person's Windows language.
  {wchar_t chosen[4]{};const DWORD length=GetEnvironmentVariableW(L"MULTITAP_LANG",chosen,DWORD(std::size(chosen)));
   if(length==2&&chosen[0]==L'k'&&chosen[1]==L'o')multitap::ui::forceLanguageForTest(multitap::ui::Language::korean);
   else if(length==2&&chosen[0]==L'e'&&chosen[1]==L'n')multitap::ui::forceLanguageForTest(multitap::ui::Language::english);}
  std::string bytes; Json request; SettingsScrubber scrubber{request};
  try {
    for (char byte; std::cin.get(byte);) { if (bytes.size() >= 8192) throw std::runtime_error(ui_text::utf8(ui_text::Id::RpcTooLarge)); bytes += byte; }
    request = JsonParser::parse(bytes); SecureZeroMemory(bytes.data(), bytes.size());
    const auto action = request.get("action").string();
    if(action=="logs"){std::cout<<"{\"ok\":true,\"events\":"<<hostLogSnapshot()<<"}\n";return 0;}
    if (action == "status") { const auto status=settingsStatus();std::cout << "{\"ok\":true,\"status\":" << status << "}\n"; return 0; }
    // host-prerequisites-v1 (host_prerequisites.hpp): what a first install still
    // needs. Read only: nothing is installed, trusted or written.
    if(action=="prerequisiteStatus"){
      const auto* fields=std::get_if<Json::Object>(&request.value);if(!fields||fields->size()!=1)throw std::runtime_error("invalid_request");
      bool winfsp=false,package=false;
      try{winfsp=drive::fs::winfspInstalled();}catch(...){}
      try{std::error_code ignored;package=std::filesystem::is_regular_file(winfspPackagePath(),ignored);}catch(...){}
      std::cout<<"{\"ok\":true,\"prerequisites\":"<<prerequisites::statusJson(winfsp,package)<<"}\n";return 0;
    }
    if (action == "windowsAutologonStatus") {
      const auto id=request.get("operationId").string();
      if(!id.empty()&&!autologon::operationUuid(id))throw std::runtime_error("invalid_request");
      std::cout << "{\"ok\":true,\"autologon\":" << autologon::windowsAutologonStatus(id) << "}\n"; return 0;
    }
    if (action == "windowsAutologonApply" || action == "windowsAutologonDisable" || action == "windowsAutologonRepair") {
      std::cout << autologon::windowsAutologonChange(action, request) << '\n'; return 0;
    }
    if(action=="encoderSettings"){std::cout<<"{\"ok\":true,\"encoderSettings\":"<<encoderSettingsStatus()<<"}\n";return 0;}
    if(action=="applyEncoderSettings"){std::cout<<"{\"ok\":true,\"encoderSettings\":"<<applyEncoderSettings(request)<<"}\n";return 0;}
    if (action == "testBroker") { const auto version = testBroker(wide(request.get("origin").string()),wide(request.get("httpConsentOrigin").string())); std::cout << "{\"ok\":true,\"version\":" << jsonQuote(version) << "}\n"; return 0; }
    if (action == "elevate") {
      const auto path = std::filesystem::path(executablePath()).parent_path() / L"MultitapHostSettings.exe";
      if (!trustedMachineFile(path)) throw std::runtime_error(ui_text::utf8(ui_text::Id::RpcOpenInstalled));
      SHELLEXECUTEINFOW info{sizeof(info)}; info.fMask = SEE_MASK_NOCLOSEPROCESS; info.lpVerb = L"runas"; info.lpFile = path.c_str(); info.lpParameters = L"--host-settings"; info.nShow = SW_SHOWNORMAL;
      if (!ShellExecuteExW(&info)) throw std::runtime_error(ui_text::utf8(ui_text::Id::RpcElevationCancelled)); if (info.hProcess) CloseHandle(info.hProcess);
    } else {
      if(!elevated())throw std::runtime_error(ui_text::utf8(ui_text::Id::RpcNeedsAdmin));
      if (action == "requestConnection") {const bool applied=requestHostConnection(wide(request.get("origin").string()),wide(request.get("httpConsentOrigin").string()));if(applied){try{reloadHostService();}catch(...){}}}
      else if (action == "cancelConnection") {cancelHostConnection();}
      else if (action == "cancelConnectionV1") {
        // A new action makes old helpers refuse safely instead of silently
        // ignoring a target field and cancelling whichever request is current.
        const auto* object=std::get_if<Json::Object>(&request.value);
        if(!object||object->size()!=3||request.get("protocol").string()!="cancel-request-v1")throw std::runtime_error("connection_request_changed");
        cancelHostConnectionV1(request.get("revision").string());
      }
      else if(action=="restartServiceV1") {
        std::cout<<serviceRestartRpc(request,service_restart::native)<<'\n';return 0;
      }
      else if (action == "saveRdpCredentials") {std::string password=request.get("password").string(),confirmation=request.get("passwordConfirmation").string();SettingsTextScrubber passwordScrubber{password},confirmationScrubber{confirmation};std::cout<<settingsApplyWithCapabilitiesRefresh([&]{saveWindowsAccount(request.get("username").string(),password,confirmation);},requestHostCapabilitiesRefresh)<<'\n';return 0;}
      else if (action == "clearRdpCredentials") {std::cout<<"{\"ok\":false,\"error\":\"windows_login_clear_update_required\"}\n";return 0;}
      else if (action == "clearWindowsLoginV1") {std::cout<<windows_login_record::rpc(request,windows_login_record::nativeAccess(),requestHostCapabilitiesRefresh)<<'\n';return 0;}
      // A first password for a local account that has none (windows_account_password.hpp).
      else if(action=="windowsAccountPasswordState"){std::cout<<"{\"ok\":true,\"account\":"<<account_password::passwordStateResult(account_password::nativeAccess(),request)<<"}\n";return 0;}
      else if(action=="setWindowsAccountPassword"){std::cout<<"{\"ok\":true,\"password\":"<<account_password::setPasswordResult(account_password::nativeAccess(),request)<<"}\n";return 0;}
      else if(action=="consoleTransfer") {
        const auto source=request.get("sourceSessionId").number(-1),console=request.get("consoleSessionId").number(-1);
        if(!nativeDisplayInteger(source,1,4294967294.0)||!nativeDisplayInteger(console,1,4294967294.0))throw std::runtime_error("console_transfer_session_changed");
        const auto result=requestOwnConsoleTransfer(DWORD(source),DWORD(console));
        std::cout<<"{\"ok\":true,\"transfer\":{\"requested\":"<<booleanJson(result.requested)<<",\"completed\":"<<booleanJson(result.completed)<<"}}\n";return 0;
      }
      else if(action=="resetNativeRecovery"){
        // Administrator escape hatch for stale native recovery records. The
        // boot service decides; this elevated console-session process only
        // answers the CCD console step. Result codes are a fixed vocabulary.
#ifdef MULTITAP_NATIVE_DISPLAY
        std::cout<<"{\"ok\":true,\"reset\":"<<nativeRecoveryResetRpc(request)<<"}\n";return 0;
#else
        throw std::runtime_error("native_display_provider_unavailable");
#endif
      }
      else if(action=="installNativeDisplay"){
        // The finish of a first install, after Host settings' one confirmation
        // (host_prerequisites.hpp). `trustConfirmed` is the consent shown for the
        // pinned Sunshine certificate: without it an untrusted publisher ends the
        // request with nothing changed. Windows is never restarted.
        const auto* confirmed=std::get_if<bool>(&request.get("confirmed").value);const auto* trust=std::get_if<bool>(&request.get("trustConfirmed").value);
        const auto result=prerequisites::installNativeDisplay(confirmed&&*confirmed,trust&&*trust);
        {FinishJournal journal;const std::string done=result.action,service=result.service;
         // hostAction 276 stage 1 (finishStepEvent). stage 1: the virtual display driver.
         recordHostEvent("driverInstall",{{"ok",result.error.empty()},{"reboot",result.restart}});
         recordHostEvent("hostAction",{{"action",uint32_t(276)},{"stage",uint32_t(1)},{"ok",result.error.empty()},{"error",prerequisites::errorNumber(result.error)},
           {"installed",done=="installed"||done=="reused"||done=="kept"},{"started",result.deviceReady},{"reboot",result.restart},{"changed",std::string(result.certificate)=="added"},
           {"restartAttempted",result.restartAttempted},{"running",service=="adopted"||service=="restarted"},{"recovery",result.rolledBack}});}
        std::cout<<deviceChangeRpc("nativeDisplay",prerequisites::resultJson(result),requestHostCapabilitiesRefresh)<<'\n';return 0;
      }
      else if(action=="installWinfsp"){
        const auto* confirmed=std::get_if<bool>(&request.get("confirmed").value);const auto msi=winfspPackagePath();
        multitap::winfsp::Machine backend;
        // Only this Host's own administrator-owned copy; packageIntact() still checks every byte.
        const auto result=winfspInstallResult(backend,trustedMachineFile(msi)?msi.wstring():std::wstring(),drive::fs::winfspInstalled(),confirmed&&*confirmed);
        {FinishJournal journal;const auto parsed=JsonParser::parse(result);const auto code=parsed.get("error").string(),done=parsed.get("action").string();
         // hostAction 276 stage 2: WinFsp, whether Host settings' finish or the 장치 page asked for it.
         recordHostEvent("hostAction",{{"action",uint32_t(276)},{"stage",uint32_t(2)},{"ok",code.empty()||code=="winfsp_already_installed"},{"error",winfspErrorNumber(code)},
           {"installed",done=="installed"||done=="upgraded"},{"reboot",parsed.get("restart").boolean()}});}
        std::cout<<deviceChangeRpc("winfsp",result,requestHostCapabilitiesRefresh)<<'\n';return 0;
      }
      else if(action=="cleanDisplayAdapters"){
        const auto* confirmed=std::get_if<bool>(&request.get("confirmed").value);const auto* ids=std::get_if<Json::Array>(&request.get("ids").value);
        if(!confirmed||!*confirmed||!ids||ids->empty()||ids->size()>64)throw std::runtime_error("display_adapters_confirmation_required");
        std::vector<std::wstring> listed;for(const auto& id:*ids)listed.push_back(wide(id.string()));
        const auto result=cleanDisplayAdaptersResult(nativeAdapterCleanupAccess(),listed);const auto parsed=JsonParser::parse(result);
        recordHostEvent("hostAction",{{"action",uint32_t(273)},{"stage",uint32_t(5)},{"ok",parsed.get("error").string().empty()&&parsed.get("failed").number()==0&&parsed.get("refused").number()==0},
          {"items",uint32_t(parsed.get("removed").number())},{"missing",uint32_t(parsed.get("pending").number())},{"error",uint32_t(parsed.get("failed").number())}});
        std::cout<<"{\"ok\":true,\"adapters\":"<<result<<"}\n";return 0;
      }
      else if(action=="replaceUsbController"){
        const auto* confirmed=std::get_if<bool>(&request.get("confirmed").value);
        const auto result=replaceUsbControllerResult(usb::nativeReplaceAccess(),confirmed&&*confirmed);const auto parsed=JsonParser::parse(result);
        recordUsbEvent(UsbStage::controllerReplaced,parsed.get("error").string().empty(),0,uint32_t(parsed.get("removed").number()));
        std::cout<<deviceChangeRpc("usb",result,requestHostCapabilitiesRefresh)<<'\n';return 0;
      }
      else if(action=="startPrintSpooler"){
        std::cout<<deviceChangeRpc("spooler",startPrintSpoolerResult({[]{return std::string(printSpoolerServiceState());},startPrintSpoolerService,[]{Sleep(500);}}),requestHostCapabilitiesRefresh)<<'\n';return 0;
      }
      else if (action == "displayRepairPlan") {const auto plan=virtualDisplayRepairPlan();std::cout<<displayRepairRpcResponse(plan,true)<<'\n';return 0;}
      else if (action == "repairDisplay") {const auto result=repairVirtualDisplay(request.get("planId").string());std::cout<<displayRepairRpcResponse(result,false)<<'\n';return 0;}
      else if (action == "service") { const auto command = request.get("command").string(); if (command != "start" && command != "stop") throw std::runtime_error(ui_text::utf8(ui_text::Id::RpcServiceUnsupported)); serviceControl(L"MultitapHost", command == "stop" ? SERVICE_CONTROL_STOP : 0); }
      else if (action == "display") { configureIdleDisplay({displayMaintenanceLock,[]{return displayHelper(L"mode-catalog-status",10000);},[]{configureVirtualDisplays();},reloadHostService}); }
      else if (action == "recoverDisplay") { const auto recovered=displayHelper(L"viewport-recover",75000);if(recovered.timedOut||recovered.exitCode||!recovered.value.get("recovered").boolean()||!recovered.value.get("rollbackSucceeded").boolean()||recovered.value.get("configurationPending").boolean(true))throw std::runtime_error(ui_text::utf8(ui_text::Id::RpcRecoverFailed));reloadHostService(); }
      else throw std::runtime_error(ui_text::utf8(ui_text::Id::RpcUnsupported));
    }
    std::cout << "{\"ok\":true}\n"; return 0;
  } catch (const std::exception& error) {
    SecureZeroMemory(bytes.data(), bytes.size()); std::cout << "{\"ok\":false,\"error\":" << jsonQuote(error.what()) << "}\n"; return 1;
  }
}
}
