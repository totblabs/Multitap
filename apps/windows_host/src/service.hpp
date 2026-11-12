#pragma once
#include "json_object.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include "agent.hpp"
#include "pipe_packet.hpp"
#include "service_control_security.hpp"
#include "machine.hpp"
#include "pending_connection.hpp"
#include "credential_binding.hpp"
#include "windows_account_record.hpp"
#include "windows_autologon_hooks.hpp"
#include "stream_console_worker.hpp"
#include "worker_token.hpp"
#include "secure_desktop_launch.hpp"
#include "secure_desktop_tracks.hpp"
#include "system_input.hpp"
#include "worker_exit_fence.hpp"
#include "service_worker_drain.hpp"
#include "process_exit_record.hpp"
#include "worker_relay_diagnostic.hpp"
#include "worker_service_binding.hpp"
#include "connect_stages.hpp"
#include "stream_worker_watchdog.hpp"
#include "track_tunnel.hpp"
#include "udp_tracks_session.hpp"
#include "display_cache.hpp"
#include "display_change_signals.hpp"
#include "capability_inputs.hpp"
#include "thumbnail_privacy.hpp"
#include "thumbnail_capture.hpp"
#include "display_operation.hpp"
#include "stream_resize.hpp"
#include "catalog_maintenance.hpp"
#include "recovery_readiness.hpp"
#include "display_maintenance.hpp"
#include "operational_log.hpp"
#include "service_loop_guard.hpp"
#include "service_configuration_change.hpp"
#include "host_diagnostics.hpp"
#include "encoder_diagnostics.hpp"
#include "native_display_service.hpp"
#include "signin_diagnostic_runtime.hpp"
#include "signin_bridge.hpp"
#include "signin_stream_preference.hpp"
#include "native_display_activation.hpp"
#include "native_activation_ccd.hpp"
#include "native_display_change_rpc.hpp"
#include "native_display_worker_rpc.hpp"
#include "native_display_worker_viewport.hpp"
#include "native_display_worker_count.hpp"
#include "native_display_worker_tracks.hpp"
#include "native_stream_profile_worker.hpp"
#include "initial_viewport.hpp"
#include "initial_profile_tracks.hpp"
#include "profile_track_resize.hpp"
#include "native_stream_expired_rpc.hpp"
#include "native_stream_profile_capability.hpp"
#include "profile_track_capability.hpp"
#include "native_stream_reclaim.hpp"
#include "display_v2.hpp"
#include "display_v2_capture.hpp"
#include "display_v2_rpc.hpp"
#include "display_v2_state_rpc.hpp"
#include "display_v2_tracks.hpp"
#include "display_v2_trace.hpp"
#include "legacy_display_identity.hpp"
#include "display_adapter_inventory.hpp"
#include "native_display_trust.hpp"
#include "render_observation.hpp"
#include "gpu_h264.hpp"
#include <dxgi1_4.h>
#include "qemu_console.hpp"
#include "qemu_console_protocol.hpp"
#include "qemu_console_route.hpp"
#include "broker_status.hpp"
#include "clipboard_worker.hpp"
#include "clipboard_channel.hpp"
#include "drive_redirection.hpp"
#include "display_scale.hpp"
#include "print_redirection.hpp"
#include "usb_redirection.hpp"
#include "desktop_name.hpp"
#include "ime_state_native.hpp"

namespace host {
// sessionUnlocked: secure_desktop_notice.hpp (the worker's capture loop asks it too).
inline NativeWorkerViewportReply requestNativeWorkerViewport(HANDLE pipe,const StreamResize& request,const std::string& phase,const std::string& key,bool windowsVerified,const StreamResizeBudget& budget,const std::atomic<bool>& stopped,std::mutex& writing,std::function<bool(const Json&)> interleave={},const std::string& baseline={}){
  const std::atomic<int64_t> deadline{budget.deadline};
  NativeWorkerRpcAccess access{
    [&](const std::string& message){return pipePacket(pipe,true,{message.begin(),message.end()},stopped,writing,&deadline);},
    [&](DWORD timeout)->std::optional<Json>{bool text=false;std::vector<uint8_t> bytes;if(!readPipePacket(pipe,text,bytes,stopped,std::chrono::milliseconds(timeout),&deadline)||!text||bytes.size()>65536)return std::nullopt;return JsonParser::parse(std::string(bytes.begin(),bytes.end()));},
    std::move(interleave)};
  return nativeWorkerViewportRpc(request,phase,key,windowsVerified,budget,std::move(access),baseline);
}
class UserWorker {
 public:
  WinHandle pipe, process, isolatedJob;
  // `unexited` (optional, caller closes): a wait handle to this worker when its
  // exit was not observed within the lifetime bounds (OwnedWorkerLifetime).
  // `exit` (optional): how an observed exit ended, for the caller to judge.
  // Without it a worker that crashed by itself is recorded here (hostAction 275,
  // stage 3); a worker the Host ended or that returned normally is not.
  UserWorker(DWORD session, const std::atomic<bool>& stopped,bool* exitObserved=nullptr,bool* createdObserved=nullptr,HANDLE* unexited=nullptr,OwnedWorkerExit* exit=nullptr):lifetime_(pipe.value,process.value,isolatedJob.value,exitObserved,8000,5000,unexited,
      [exit](const OwnedWorkerExit& value){if(exit)*exit=value;else if(const auto record=workerCrashExit(value))recordProcessExit(*record);}) {
    auto physicalCreation=service_workers::reserveCurrent(service_workers::Role::userWorker,true);
    WinHandle physicalJob(physicalCreation.createTrackingJob());
    if(createdObserved)*createdObserved=false;
    WinHandle token = interactiveWorkerToken(session); DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);std::vector<BYTE> data(size);winCheck(GetTokenInformation(token.value,TokenUser,data.data(),size,&size),"Worker user identity");LPWSTR sid=nullptr;winCheck(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&sid),"Worker SID");std::wstring dacl=workerPipeSecurityText(sid);LocalFree(sid);PSECURITY_DESCRIPTOR descriptor=nullptr;winCheck(ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(),SDDL_REVISION_1,&descriptor,nullptr),"Worker pipe ACL");SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    const std::wstring name=L"\\\\.\\pipe\\MultitapHost.Worker."+std::to_wstring(GetCurrentProcessId())+L"."+randomName();pipe=WinHandle(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,workerPipeOutboundBytes,workerPipeInboundBytes,0,&security));const DWORD pipeError=GetLastError();LocalFree(descriptor);if(pipe.value==INVALID_HANDLE_VALUE)throw WindowsOperationError("Worker pipe",pipeError);
    isolatedJob=WinHandle(CreateJobObjectW(nullptr,nullptr));winCheck(isolatedJob.value!=nullptr,"Owned worker job");JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;winCheck(SetInformationJobObject(isolatedJob.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"Owned worker job limits");
    struct Environment{void* value=nullptr;~Environment(){if(value)DestroyEnvironmentBlock(value);}}environment;
    winCheck(CreateEnvironmentBlock(&environment.value,token.value,FALSE),"Worker environment");const auto executable=executablePath();std::wstring command=L"\""+executable+L"\" --worker --pipe "+name+L" --service-pid "+std::to_wstring(GetCurrentProcessId());service_workers::JobListStartup ownedStartup(physicalJob.value,isolatedJob.value);auto& startup=ownedStartup.startup.StartupInfo;wchar_t desktop[]=L"winsta0\\default";startup.lpDesktop=desktop;PROCESS_INFORMATION child{};physicalCreation.requireLaunchAllowed();const BOOL created=CreateProcessAsUserW(token.value,executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,environment.value,std::filesystem::path(executable).parent_path().c_str(),&startup,&child);winCheck(created,"Launch interactive worker");process=WinHandle(child.hProcess);WinHandle thread(child.hThread);if(createdObserved)*createdObserved=true;
    physicalCreation.retain(process.value);
    const auto serviceBinding=createWorkerServiceBinding(process.value);
    if(ResumeThread(thread.value)==DWORD(-1))winCheck(FALSE,"Resume owned worker");
    WinHandle connected(CreateEventW(nullptr,TRUE,FALSE,nullptr));OVERLAPPED operation{};operation.hEvent=connected.value;BOOL success=ConnectNamedPipe(pipe.value,&operation);const DWORD error=GetLastError();if(!success&&error==ERROR_IO_PENDING){for(int i=0;i<80&&!stopped&&WaitForSingleObject(connected.value,100)==WAIT_TIMEOUT;i++){}DWORD bytes=0;success=GetOverlappedResult(pipe.value,&operation,&bytes,FALSE);if(!success){CancelIoEx(pipe.value,&operation);GetOverlappedResult(pipe.value,&operation,&bytes,TRUE);}}else if(!success&&error==ERROR_PIPE_CONNECTED)success=TRUE;if(!success)throw std::runtime_error("Worker connection timeout");ULONG client=0;winCheck(GetNamedPipeClientProcessId(pipe.value,&client),"Worker peer PID");if(client!=child.dwProcessId)throw std::runtime_error("Worker process mismatch");
    std::mutex bindingWrite;const auto bindingPacket=workerServiceBindingPacket(serviceBinding);
    if(!pipePacket(pipe.value,true,{bindingPacket.begin(),bindingPacket.end()},stopped,bindingWrite))throw std::runtime_error("Worker service binding unavailable");
    bool bindingText=false;std::vector<uint8_t> bindingReply;
    if(!readPipePacket(pipe.value,bindingText,bindingReply,stopped,std::chrono::seconds(5))||!bindingText||bindingReply.size()>4096||
        !workerServiceBindingAckMatches(JsonParser::parse(std::string(bindingReply.begin(),bindingReply.end())),serviceBinding))
      throw std::runtime_error("Worker service binding not acknowledged");
    lifetime_.connected();
  }
  ~UserWorker()=default;
 private:OwnedWorkerLifetime lifetime_;
};

inline std::string safeViewportFailure(const std::string& reason) {
  if(reason=="native_display_activation_pending")return "native_display_change_pending";
  if(reason=="native_display_not_ready"||reason=="native_display_provider_unavailable"||reason=="native_display_snapshot_invalid"||reason=="native_display_configuration_untrusted"||reason=="native_display_topology_mismatch"||reason=="native_display_change_pending"||reason=="native_topology_not_ready"||reason=="native_topology_recovery_required")return reason;
  static const std::set<std::string> allowed{"", "no_interactive_session", "interactive_console_required", "administrator_required", "viewport_probe_pending", "viewport_probe_failed", "viewport_probe_timeout", "viewport_support_unavailable", "viewport_recovery_required", "viewport_settings_unavailable", "viewport_settings_untrusted", "viewport_settings_read_failed", "viewport_original_settings_invalid", "viewport_original_settings_modified", "viewport_settings_modified_externally", "viewport_package_path_invalid", "viewport_package_path_untrusted", "viewport_package_not_administrator_controlled", "viewport_installed_driver_unavailable", "viewport_installed_driver_untrusted", "viewport_installed_driver_mismatch", "viewport_sources_unavailable", "viewport_connector_identity_unavailable", "viewport_connector_identity_ambiguous", "viewport_ccd_query_failed", "viewport_ccd_source_invalid", "viewport_ccd_snapshot_limit", "viewport_topology_changed", "viewport_ccd_validate_failed", "monitor_not_available", "driver_hash_mismatch", "driver_signature_untrusted", "driver_catalog_membership_invalid"};
  if(allowed.count(reason))return reason;const auto catalog=catalogMaintenanceReason(reason);return catalog!="mode_catalog_failed"||reason=="mode_catalog_failed"?catalog:"viewport_probe_failed";
}
inline void applyViewportProbe(DisplayState& display,const DisplayHelperOutput& probe,bool desktop,bool console,bool privileged) {
  display.viewportProvisioning=false;
  if(probe.timedOut){display.error="viewport_probe_timeout";return;}
  if(probe.exitCode){display.error=safeViewportFailure(probe.value.get("error").string());if(display.error.empty())display.error="viewport_probe_failed";return;}
  display.viewportProvisioning=viewportProvisioningReady(probe.value,display,desktop,console,privileged);
  display.error=display.viewportProvisioning?"":safeViewportFailure(probe.value.get("error").string());
  if(!display.viewportProvisioning&&display.error.empty())display.error="viewport_support_unavailable";
}
struct WorkerViewportProbeAccess {
  std::function<DisplayHelperOutput(const std::wstring&, DWORD)> helper;
  std::function<int64_t()> now = streamMonotonicMillis;
  bool desktop = false, console = false, privileged = false, clipboard = false;
  std::function<bool()> cancelled = [] { return false; };
  std::function<bool()> nativeProvider = nativeDisplaySelected;
  std::function<DisplayState()> nativeProbe = nativeDisplayStatus;
};
// Resize-only read-only probe: unlike the ordinary diagnostic fallback, both
// helpers consume one explicit budget and no third status helper is launched.
inline DisplayState boundedWorkerViewportProbe(DWORD timeoutMs, const WorkerViewportProbeAccess& access) {
  if(!timeoutMs||timeoutMs>streamResizeProbeTimeoutMs)throw std::runtime_error("Invalid viewport probe budget");
  DisplayState display;display.clipboardSupported=access.clipboard;
  if(access.cancelled()){display.error="viewport_probe_failed";return display;}
  if(access.nativeProvider()){display=access.nativeProbe();display.clipboardSupported=access.clipboard;return display;}
  if(!access.desktop||!access.console||!access.privileged){display.remoteSession=!access.console;display.error=!access.desktop?"no_interactive_session":!access.console?"interactive_console_required":"administrator_required";return display;}
  const auto deadline=access.now()+timeoutMs;
  const auto remaining=[&]{if(access.cancelled())throw std::runtime_error("viewport_probe_failed");const auto value=deadline-access.now();if(value<=0)throw std::runtime_error("viewport_probe_timeout");return DWORD(std::min<int64_t>(5000,value));};
  try {
    const auto topology=access.helper(L"topology-status",remaining());
    remaining();
    if(topology.timedOut||topology.exitCode){
      const auto partial=parseDisplayState(topology.value.get("state"));
      display.configurationPending=partial.configurationPending;
      display.recoveryRequired=partial.recoveryRequired;
      display.rebootRequired=partial.rebootRequired;
      display.error=topology.timedOut?"viewport_probe_timeout":"viewport_probe_failed";return display;
    }
    display=parseDisplayState(topology.value.get("state"));
    display.clipboardSupported=access.clipboard;
    display.monitorControlSupported=monitorControlReady(topology.value,display,access.desktop,access.console);
    const auto viewport=access.helper(L"viewport-status",remaining());
    remaining();
    applyViewportProbe(display,viewport,access.desktop,access.console,access.privileged);
  }catch(const std::exception& error){display.active=false;display.viewportProvisioning=false;display.error=safeViewportFailure(error.what());}
  catch(...){display.active=false;display.viewportProvisioning=false;display.error="viewport_probe_failed";}
  return display;
}
inline std::string viewportFailureReason(const DisplayState& display,bool interactive,bool capture,bool console) {
  // The actual devnode requirement remains actionable even in an RDP or
  // locked session. A console check must not hide a required driver restart.
  if(display.rebootRequired)return "reboot_required";
  if(!interactive)return "no_interactive_session";
  if(!console)return "interactive_console_required";
  if(!display.error.empty())return safeViewportFailure(display.error);
  if(display.recoveryRequired)return "viewport_recovery_required";
  if(!display.ready())return "owned_virtual_display_not_ready";
  if(!capture)return "viewport_probe_pending";
  if(display.ready()&&display.viewportProvisioning)return "";
  return display.error.empty()?"viewport_support_unavailable":safeViewportFailure(display.error);
}
inline std::string workerDisplayJson(const DisplayState& display) {
  std::string monitors = "[";
  for (const auto& monitor : display.monitors) { if (monitors.size() > 1) monitors += ','; monitors += "{\"id\":" + jsonQuote(monitor.id) + ",\"displayDeviceName\":" + jsonQuote(utf8(monitor.name)) + ",\"left\":" + std::to_string(monitor.left) + ",\"top\":" + std::to_string(monitor.top) + ",\"width\":" + std::to_string(monitor.width) + ",\"height\":" + std::to_string(monitor.height) + ",\"primary\":" + (monitor.primary ? "true" : "false") + ",\"virtual\":true}"; }
  return "{\"installed\":" + std::string(display.installed ? "true" : "false") + ",\"deviceReady\":" + (display.deviceReady ? "true" : "false") + ",\"activeDisplay\":" + (display.active ? "true" : "false") + ",\"owned\":" + (display.owned ? "true" : "false") + ",\"foreignDeviceCount\":" + std::to_string(display.foreign) + ",\"width\":" + std::to_string(display.width) + ",\"height\":" + std::to_string(display.height) + ",\"left\":" + std::to_string(display.left) + ",\"top\":" + std::to_string(display.top) + ",\"displayDeviceName\":" + jsonQuote(utf8(display.name)) + ",\"remoteSession\":" + (display.remoteSession ? "true" : "false") + ",\"configurationPending\":" + (display.configurationPending ? "true" : "false") + ",\"rebootRequired\":" + (display.rebootRequired ? "true" : "false") + ",\"clipboardSupported\":" + (display.clipboardSupported ? "true" : "false") + ",\"recoveryRequired\":" + (display.recoveryRequired ? "true" : "false") + ",\"viewportProvisioning\":" + (display.viewportProvisioning ? "true" : "false") + ",\"error\":"+jsonQuote(safeViewportFailure(display.error))+",\"monitorControlSupported\":" + (display.monitorControlSupported ? "true" : "false") + ",\"displays\":" + monitors + "]}";
}
// A read-only worker can run topology and viewport helpers sequentially.
// Keep their explicit budgets and the whole-message IPC deadline together.
inline constexpr DWORD workerDisplayHelperTimeoutMs = 5000;
inline constexpr DWORD workerDisplayProbeIpcMarginMs = 5000;
inline constexpr DWORD workerDisplayProbeReplyTimeoutMs =
    workerDisplayHelperTimeoutMs * 2 + workerDisplayProbeIpcMarginMs;
struct WorkerDisplayProbeResult {
  DisplayState display;
  bool desktopAvailable = false, privileged = false, cancelled = false;
  bool nativeActivationAcknowledged=false,nativeActivationAttempted=false;
  std::optional<NativeActivationDiagnostic> nativeActivationDiagnostic;
};
inline WorkerDisplayProbeResult readWorkerDisplayProbe(
    HANDLE pipe, const std::atomic<bool>& stopped,
    DWORD timeoutMs = workerDisplayProbeReplyTimeoutMs) {
  if (!timeoutMs || timeoutMs > workerDisplayProbeReplyTimeoutMs)
    throw std::runtime_error("Invalid worker probe timeout");
  WorkerDisplayProbeResult result;
  const int64_t deadline = streamMonotonicMillis() + timeoutMs;
  const std::atomic<int64_t> absoluteDeadline{deadline};
  bool text = false; std::vector<uint8_t> bytes;
  const bool received = readPipePacket(pipe, text, bytes, stopped,
      std::chrono::milliseconds(timeoutMs), &absoluteDeadline);
  if (stopped) { result.cancelled = true; return result; }
  if (streamMonotonicMillis() >= deadline) {
    result.display.error = "viewport_probe_timeout"; return result;
  }
  result.display.error = "viewport_probe_failed";
  if (!received || !text || bytes.size() > 65536) return result;
  try {
    const auto value = JsonParser::parse(std::string(bytes.begin(), bytes.end()));
    if (value.get("type").string() != "worker.display" ||
        !std::holds_alternative<Json::Object>(value.get("display").value) ||
        !std::holds_alternative<bool>(value.get("desktopAvailable").value) ||
        !std::holds_alternative<bool>(value.get("privileged").value)) return result;
    const auto& display = value.get("display");
    for (const auto key : {"installed", "deviceReady", "activeDisplay", "owned",
         "remoteSession", "configurationPending", "rebootRequired",
         "recoveryRequired", "viewportProvisioning", "monitorControlSupported",
         "clipboardSupported"})
      if (!std::holds_alternative<bool>(display.get(key).value)) return result;
    for (const auto key : {"foreignDeviceCount", "width", "height", "left", "top"})
      if (!std::holds_alternative<double>(display.get(key).value) ||
          !std::isfinite(display.get(key).number())) return result;
    if (!std::holds_alternative<std::string>(display.get("displayDeviceName").value) ||
        !std::holds_alternative<std::string>(display.get("error").value) ||
        !std::holds_alternative<Json::Array>(display.get("displays").value)) return result;
    result.display = parseDisplayState(display);
    result.desktopAvailable = value.get("desktopAvailable").boolean();
    result.privileged = value.get("privileged").boolean();
    result.nativeActivationAcknowledged=std::holds_alternative<bool>(value.get("nativeActivationAttempted").value);
    result.nativeActivationAttempted=value.get("nativeActivationAttempted").boolean();
    if(!std::holds_alternative<std::nullptr_t>(value.get("nativeActivation").value))result.nativeActivationDiagnostic=parseNativeActivationDiagnostic(value.get("nativeActivation"));
    result.display.error = safeViewportFailure(result.display.error);
  } catch (...) {}
  return result;
}
// Whether this interactive worker may change Windows' display configuration
// for the Host. An administrator's worker always could. With the native
// provider under display manager v2 any signed-in console user's worker can:
// what it does there is QueryDisplayConfig/SetDisplayConfig for its own
// session (Windows asks for access to the console session's desktop, not for
// an administrator; nf-winuser-setdisplayconfig, ERROR_ACCESS_DENIED), the
// owned outputs' sizes and count are the service's SDK calls, and the
// protected state file is the service's too (display_v2_state_rpc.hpp). The
// v1 transaction model and the older display helper still open administrator-
// only files and device settings from the worker, so they keep the old rule
// and its `administrator_required` answer. One evaluation per worker process:
// the service fixes the display manager when it starts.
inline bool workerDisplayAuthority(bool administrator,bool nativeProvider,bool v2){return administrator||(nativeProvider&&v2);}
inline bool workerDisplayAuthority(){
  if(elevated())return true;
  static const bool standard=[]{try{return workerDisplayAuthority(false,nativeDisplaySelected(),displayManagerV2Selected());}catch(...){return false;}}();
  return standard;
}
inline DisplayState workerDisplayProbe() {
  DWORD session = 0xffffffff; ProcessIdToSessionId(GetCurrentProcessId(), &session);
  const bool desktop = interactiveDesktopAvailable(), console = sameInteractiveConsole(session);
  if(nativeDisplaySelected()){
    const bool authority=workerDisplayAuthority();
    auto display=nativeDisplayStatus();display.clipboardSupported=clipboardRuntimeReady();
    display.viewportProvisioning=nativeViewportTrialReady(display,nativeDisplayConfig(),desktop&&console&&authority);
    if(!desktop||!console||!authority){display.viewportProvisioning=false;display.monitorControlSupported=false;display.error=!desktop?"no_interactive_session":!console?"interactive_console_required":"administrator_required";}
    return display;
  }
  if (desktop && console && elevated()) {
    try {
      const auto result = displayHelper(L"topology-status", workerDisplayHelperTimeoutMs);
      if (!result.timedOut && !result.exitCode) {
        auto display = parseDisplayState(result.value.get("state"));
        display.monitorControlSupported = monitorControlReady(result.value, display, desktop, console);
        display.clipboardSupported=clipboardRuntimeReady();
        try {const auto viewport=displayHelper(L"viewport-status", workerDisplayHelperTimeoutMs);applyViewportProbe(display,viewport,desktop,console,elevated());}catch(const std::exception& error){display.error=safeViewportFailure(error.what());}catch(...){display.error="viewport_probe_failed";}
        return display;
      }
    } catch (...) {}
  }
  auto display=virtualDisplayStatus();display.clipboardSupported=clipboardRuntimeReady();if(display.error.empty())display.error=!desktop?"no_interactive_session":!console?"interactive_console_required":!elevated()?"administrator_required":"viewport_probe_failed";return display;
}
inline DisplayState workerViewportProbe(DWORD timeoutMs, const std::function<bool()>& cancelled) {
  DWORD session=0xffffffff;ProcessIdToSessionId(GetCurrentProcessId(),&session);
  return boundedWorkerViewportProbe(timeoutMs,
      {displayHelper,streamMonotonicMillis,interactiveDesktopAvailable(),
       sameInteractiveConsole(session),elevated(),clipboardRuntimeReady(),cancelled});
}
inline NativeBootstrapAccess nativeWorkerBootstrapAccess(){
  DWORD session=0xffffffff;ProcessIdToSessionId(GetCurrentProcessId(),&session);
  // Activating the owned outputs is a SetDisplayConfig in this session and
  // nothing else (no file, no device call): workerDisplayAuthority.
  const auto eligible=[session]{return sameInteractiveConsole(session)&&sessionUnlocked(session)&&interactiveDesktopAvailable()&&workerDisplayAuthority();};
  return {[]{
#ifdef MULTITAP_NATIVE_DISPLAY
    return nativeDisplaySelected();
#else
    return false;
#endif
  },eligible,workerDisplayProbe,nativeDisplayLeaseSnapshot,
    [eligible](std::span<const NativeDisplayTarget> targets,const std::string& key){
      auto access=windowsNativeTopologyAccess([eligible,key]{try{const auto state=nativeDisplayLeaseSnapshot();return eligible()&&!state.get("configurationPending").boolean()&&nativeDisplayLeaseKey(state)==key;}catch(...){return false;}});
      std::optional<NativeCcdDiagnostic> diagnostic;
      access.query=[&]{try{return queryNativeActivationCcd();}catch(const NativeActivationCcdError& error){diagnostic=error.diagnostic;throw;}};
      auto result=activateNativeTopology(targets,std::move(access));result.ccdDiagnostic=diagnostic;return result;
    }};
}
inline NativeWorkerTrackAccess nativeWorkerTrackAccess(HANDLE pipe,const NativeTrackResize& request,std::atomic<bool>& stopped,std::mutex& writing,const StreamResizeBudget& budget,std::function<bool(const Json&)> interleave={}){
  DWORD session=0xffffffff;ProcessIdToSessionId(GetCurrentProcessId(),&session);
  const auto eligible=[session,&stopped]{return !stopped&&sameInteractiveConsole(session)&&sessionUnlocked(session)&&interactiveDesktopAvailable()&&elevated();};
  NativeWorkerTrackAccess access;access.eligible=eligible;access.probe=[budget]{budget.remaining(1);return workerDisplayProbe();};access.observed=currentCaptureMonitors;access.snapshot=nativeDisplayLeaseSnapshot;access.topology=windowsNativeTopologyAccess(eligible);access.recoveryScope=[](const NativeTopologyBaseline& baseline){return captureNativeRecoveryScope(baseline);};
  access.exchange=[pipe,request,&stopped,&writing,interleave=std::move(interleave)](const std::string& phase,const std::string& key,bool verified,const std::string& baseline,const StreamResizeBudget& phaseBudget){
    NativeWorkerRpcAccess io;io.write=[&](const std::string& message){return pipePacket(pipe,true,{message.begin(),message.end()},stopped,writing);};io.read=[&](DWORD timeout)->std::optional<Json>{bool text=false;std::vector<uint8_t> bytes;if(!readPipePacket(pipe,text,bytes,stopped,std::chrono::milliseconds(timeout))||!text||bytes.size()>65536)return {};return JsonParser::parse(std::string(bytes.begin(),bytes.end()));};io.interleave=interleave;
    return nativeWorkerTrackViewportRpc(request,phase,key,verified,phaseBudget,std::move(io),baseline);
  };return access;
}
inline std::string workerProbeReply(const DisplayState& display,bool desktop,bool privileged,std::optional<bool> activation={},const std::optional<NativeActivationDiagnostic>& diagnostic={}){
  return "{\"type\":\"worker.display\",\"desktopAvailable\":"+std::string(desktop?"true":"false")+",\"privileged\":"+(privileged?"true":"false")+(activation?",\"nativeActivationAttempted\":"+std::string(*activation?"true":"false"):"")+(activation&&diagnostic?",\"nativeActivation\":"+nativeActivationDiagnosticJson(*diagnostic):"")+",\"display\":"+workerDisplayJson(display)+'}';
}
inline bool catalogReadyResult(const DisplayHelperOutput& result) {
  const auto& value=result.value;const auto display=parseDisplayState(value.get("state"));
  return !result.timedOut&&!result.exitCode&&value.get("modeCatalogRevision").number()==1&&value.get("requiredModeCount").number()==18&&value.get("modeCatalogReady").boolean()&&value.get("verifiedModeCount").number()==18&&display.monitors.size()>=1&&display.monitors.size()<=2&&value.get("verifiedSourceCount").number()==double(display.monitors.size())&&!value.get("configurationPending").boolean(true)&&!value.get("recoveryRequired").boolean()&&!value.get("rebootRequired").boolean()&&display.ready();
}
inline bool catalogPendingResult(const DisplayHelperOutput& result) {
  const auto& value=result.value;return !result.timedOut&&!result.exitCode&&value.get("readOnly").boolean()&&value.get("modeCatalogRevision").number()==1&&value.get("requiredModeCount").number()==18&&!value.get("modeCatalogReady").boolean()&&value.get("reason").string()=="mode_catalog_update_required"&&!value.get("recoveryRequired").boolean()&&!value.get("rebootRequired").boolean()&&parseDisplayState(value.get("state")).ready();
}
inline CatalogOutcome catalogOutcome(const DisplayHelperOutput& result) {
  const auto& value=result.value;
  if(result.timedOut)return {CatalogPhase::Unknown,"mode_catalog_timeout"};
  if(catalogReadyResult(result))return {CatalogPhase::Ready,""};
  CatalogOutcome outcome;outcome.recoveryRequired=value.get("recoveryRequired").boolean();outcome.rebootRequired=value.get("rebootRequired").boolean();outcome.rollbackAttempted=value.get("rollbackAttempted").boolean();outcome.rollbackSucceeded=outcome.rollbackAttempted&&value.get("rollbackSucceeded").boolean();
  outcome.reason=catalogMaintenanceReason(value.get("reason").string());if(outcome.reason.empty())outcome.reason="mode_catalog_failed";
  outcome.deferredWithoutMutation=value.get("deferred").boolean()&&!result.exitCode&&!value.get("deviceRestartAttempted").boolean()&&!value.get("catalogChanged").boolean()&&!outcome.rollbackAttempted&&!outcome.recoveryRequired;
  outcome.phase=outcome.deferredWithoutMutation?CatalogPhase::Deferred:CatalogPhase::Failed;
  if(outcome.recoveryRequired||outcome.reason=="viewport_rollback_incomplete"){outcome.phase=CatalogPhase::Unknown;outcome.reason="viewport_recovery_required";}
  return outcome;
}
struct CatalogPipeFailure:std::runtime_error{explicit CatalogPipeFailure(const char* reason):std::runtime_error(reason){}};
inline CatalogOutcome runServiceCatalog(const std::shared_ptr<CatalogOperation>& operation,const CatalogMaintenance::Commit& commit) {
  if(operation->cancelled||!sameInteractiveConsole(operation->session)||!sessionUnlocked(operation->session))return {CatalogPhase::Deferred,"interactive_console_required"};
  const auto gate=displayMaintenanceLock();
  if(!gate)return {CatalogPhase::Deferred,"viewport_busy"};
  UserWorker child(operation->session,operation->cancelled);std::mutex writing;
  HANDLE duplicate=nullptr;winCheck(DuplicateHandle(GetCurrentProcess(),child.isolatedJob.value,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS),"Catalog job cancellation handle");auto job=std::make_shared<WinHandle>(duplicate);
  operation->setAbort([job]{TerminateJobObject(job->value,ERROR_CANCELLED);});
  const auto write=[&](const char* type){const auto message="{\"type\":"+jsonQuote(type)+','+displayOperationFields(operation->key)+'}';return pipePacket(child.pipe.value,true,{message.begin(),message.end()},operation->cancelled,writing);};
  const auto read=[&](const char* type,std::chrono::seconds timeout){bool text=false;std::vector<uint8_t> bytes;const std::atomic<int64_t> deadline{streamMonotonicMillis()+std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()};if(!readPipePacket(child.pipe.value,text,bytes,operation->cancelled,timeout,&deadline)||!text||bytes.size()>65536)throw CatalogPipeFailure(streamMonotonicMillis()>=deadline.load()?"mode_catalog_timeout":"mode_catalog_failed");const auto response=JsonParser::parse(std::string(bytes.begin(),bytes.end()));if(response.get("type").string()!=type||!operation->key.matches(response))throw CatalogPipeFailure("mode_catalog_failed");return response;};
  try {
    if(!write("worker.catalog.prepare"))return {CatalogPhase::Failed,"mode_catalog_failed"};
    const auto prepared=read("worker.catalog.prepared",std::chrono::seconds(15));
    DisplayHelperOutput result;result.value=prepared.get("helper");result.exitCode=DWORD(prepared.get("exitCode").number(1));result.timedOut=prepared.get("timedOut").boolean();
    if(catalogReadyResult(result))return {CatalogPhase::Ready,""};
    const bool pending=prepared.get("interactive").boolean()&&prepared.get("privileged").boolean()&&catalogPendingResult(result);
    if(!pending)return catalogOutcome(result);
    if(operation->cancelled||!sameInteractiveConsole(operation->session)||!sessionUnlocked(operation->session)||!commit()){write("worker.catalog.cancel");return {CatalogPhase::Deferred,"mode_catalog_cancelled"};}
    if(!write("worker.catalog.apply"))return {CatalogPhase::Unknown,"mode_catalog_failed"};
    const auto completed=read("worker.catalog.result",std::chrono::seconds(100));result.value=completed.get("helper");result.exitCode=DWORD(completed.get("exitCode").number(1));result.timedOut=completed.get("timedOut").boolean();
    return catalogOutcome(result);
  }catch(const CatalogPipeFailure& error){TerminateJobObject(job->value,ERROR_CANCELLED);return {operation->committed?CatalogPhase::Unknown:CatalogPhase::Failed,operation->cancelled?"mode_catalog_cancelled":error.what()};}catch(...){TerminateJobObject(job->value,ERROR_CANCELLED);return {operation->committed?CatalogPhase::Unknown:CatalogPhase::Failed,operation->cancelled?"mode_catalog_cancelled":"mode_catalog_failed"};}
}
struct PendingDisplayOperation {
  DisplayOperationKey key; int count = 0; DWORD session = 0xffffffff; std::atomic<bool> cancelled{false};
};
// hostAction 211: the adapters doing 3D work. `stage` and `expected` are the vendor
// and device ID of the console compositor's adapter, `error` and `missing` those of
// the busiest over all processes (0 none, 1 not found). `ok` means a hardware GPU
// renders (the compositor's adapter or the busiest one). Looked up by LUID: the encoder inventory skips software
// adapters, and those are exactly what must be told apart here.
inline std::pair<uint32_t,uint32_t> renderAdapterIdentity(const std::optional<uint64_t>& luid){
  if(!luid)return {0,0};
  Com<IDXGIFactory4> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return {1,0};
  Com<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 description{};
  if(FAILED(factory->EnumAdapterByLuid(LUID{DWORD(*luid),LONG(uint32_t(*luid>>32))},IID_PPV_ARGS(&adapter)))||FAILED(adapter->GetDesc1(&description)))return {1,0};
  return {description.VendorId,description.DeviceId};
}
inline void recordRenderObservation(const RenderReading& reading){
  const auto compositor=renderAdapterIdentity(reading.compositor),busiest=renderAdapterIdentity(reading.busiest);
  // ok: a hardware GPU does the stream's render work, as the compositor's busiest adapter or the busiest overall.
  // On a Proxmox VM the compositor also draws the console display on the basic adapter, which can be its busiest
  // (measured: compositor 5140/140, busiest the P40, while the stream captured on the P40: 243/1).
  const bool hardware=gpu::vendorFromId(compositor.first)!=gpu::Vendor::unknown||gpu::vendorFromId(busiest.first)!=gpu::Vendor::unknown;
  recordHostEvent("hostAction",{{"action",uint32_t(211)},{"ok",hardware},{"stage",compositor.first},{"expected",compositor.second},{"error",busiest.first},{"missing",busiest.second}});
}
class ServiceBridge final : public AgentBridge {
#ifdef MULTITAP_SESSION_STOP_TEST
  friend struct SessionStopTestAccess;
#endif
 public:
  explicit ServiceBridge(Credentials credentials,CatalogMaintenance* maintenance=nullptr,NativeDisplayService* nativeDisplay=nullptr,std::shared_ptr<signin::broker::SigninBridge> signinRuntime={}) : credentials_(std::move(credentials)),maintenance_(maintenance),nativeDisplay_(nativeDisplay),signinRuntime_(std::move(signinRuntime)) {}
  // Set once, before Agent/preview callbacks start. Recreated bridges borrow
  // the SAME service-owned diagnostic activity flag, never a peer boolean.
  void diagnosticActivity(std::shared_ptr<std::atomic<bool>> activity){diagnosticActivity_=std::move(activity);}
  bool privateDiagnosticBusy()const{return (diagnosticActivity_&&diagnosticActivity_->load())||(signinRuntime_&&signinRuntime_->busy());}
  void observeSigninWorker(){signinWorkerObservation_=true;}
  signin::native::Handle signinWorkerQuery(){std::lock_guard lock(signinWorkerMutex_);HANDLE copy=nullptr;if(!signinWorkerQuery_.value||!DuplicateHandle(GetCurrentProcess(),signinWorkerQuery_.value,GetCurrentProcess(),&copy,PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,0))return {};return signin::native::Handle(copy);}
  bool signinOriginalRestored(){try{std::lock_guard lifecycle(lifecycleMutex_);return workerQuiescentNow()&&v2Session_&&nativeDisplay_&&nativeDisplay_->streamStopClean(nativeStopInstance_)&&!displayV2FileStore(displayV2StatePath()).load();}catch(...){return false;}}
  void diagnosticActivityChanged(){invalidateDisplay();readinessChanged_=true;}
  void closeSchedulingAndJoin() { stopMainAdapters();profileRecoveryClosing_=true;drives_.closeSchedulingAndJoin();stop();{std::lock_guard launch(profileRecoveryLaunchMutex_);if(profileRecoveryWorker_.joinable())profileRecoveryWorker_.join();}probeStopped_ = true; if (probe_.joinable()) probe_.join(); ime_.stop(); }
  ~ServiceBridge() override { closeSchedulingAndJoin(); }
  // Display manager v2: one protected state file; if it exists and no stream is
  // active, an interactive worker restores it once per (backed-off) attempt.
  // A failed attempt never blocks a stream: a new start keeps the older
  // snapshot as its original. Unlock/logon rearm the attempt budget.
  //
  // The Console's end goes through here too (rearmConsoleActivation): the
  // console's restore point is the topology from before it, foreign displays
  // included (qemu_console.hpp), and this restores it at once. Until that first
  // look the display probe waits (consoleRestorePending_), so the owned-output
  // activation it may run does not race the restore.
  void recoverDisplayV2(){
    pollQemuConsoleCompletion();
    if(!v2Mode()){consoleRestorePending_=false;return;}
    std::lock_guard lifecycle(lifecycleMutex_);std::lock_guard launch(profileRecoveryLaunchMutex_);
    if(privateDiagnosticBusy())return;
    // v2Rpc_ stays set until the stream thread has finished with its worker, which
    // restores the same state on stop; never run a second restore beside it.
    if(profileRecoveryClosing_||profileRecoveryRunning_||(!stopped_&&streaming_)||v2Rpc_||consoleRunning_||displayOperationRunning_||!qemuConsoleQuiescent())return;
    if(v2RecoveryRearm_.exchange(false))v2RecoveryNext_=0;
    // Every return below without a recovery attempt ends the probe's wait: it
    // never waits longer than this one look.
    struct Looked{std::atomic<bool>& pending;bool launched=false;~Looked(){if(!launched)pending=false;}}looked{consoleRestorePending_};
    const auto now=streamMonotonicMillis();if(now<v2RecoveryNext_)return;
    if(!v1Migrated_){
      v1Migrated_=true;
      try{
        const auto moved=migrateDisplayV1Leftovers(machineDirectory()/L"private",displayV2FileStore(displayV2StatePath()),displayV1ProfileState,trustedMachineFile,displayV2ArchiveStamp());
        if(moved.found)recordHostEvent("hostAction",{{"action",uint32_t(216)},{"ok",moved.archived},{"changed",moved.stateCreated}});
      }catch(...){ }
    }
    if(!displayV2StatePresent())return;
    const auto session=interactiveSession();if(!sameInteractiveConsole(session)||!sessionUnlocked(session))return;
    // Any signed-in console user's worker restores it: an administrator's opens
    // the protected file itself, another user's asks this service for it
    // (display_v2_state_rpc.hpp). A standard user's worker that could not
    // get the file would answer "nothing to restore" and be started again
    // every 5 s for as long as that user stayed signed in.
    const unsigned attempts=v2RecoveryAttempts_.load();
    v2RecoveryNext_=now+std::min<int64_t>(60000,int64_t(5000)<<std::min(attempts,4u));v2RecoveryAttempts_=attempts+1;
    if(profileRecoveryWorker_.joinable())profileRecoveryWorker_.join();
    profileRecoveryRunning_=true;
    try{profileRecoveryWorker_=std::thread([this,session]{performDisplayV2Recovery(session);consoleRestorePending_=false;profileRecoveryRunning_=false;});looked.launched=true;}catch(...){profileRecoveryRunning_=false;}
  }
  // display-adapters-v1 (display_adapter_inventory.hpp). Every minute: observe the
  // Display-class nodes (diagnostics, settings) and record the proven current node
  // in the ledger. Proven extras (the legacy adapter, recorded duplicates) are
  // removed off the loop thread when nothing streams and the runtime's own adapter
  // is started, at most three rounds per service run. Unproven nodes are only
  // counted: Host settings removes them after an administrator confirms the list.
  void maintainDisplayAdapters(){
    std::lock_guard lifecycle(lifecycleMutex_);std::lock_guard launch(profileRecoveryLaunchMutex_);
    if(privateDiagnosticBusy())return;
    const auto now=streamMonotonicMillis();if(now<adapterDueNext_)return;adapterDueNext_=now+60000;
    const bool idle=!(profileRecoveryClosing_||profileRecoveryRunning_||(!stopped_&&streaming_)||v2Rpc_||consoleRunning_||displayOperationRunning_||!qemuConsoleQuiescent());
    bool confirmed=false;std::wstring selection;
    try{confirmed=nativeDisplayConfirmed();if(confirmed)selection=wide(nativeDisplayConfig().get("deviceInstanceId").string());}catch(...){confirmed=false;}
    const auto owner=readDisplayAdapterOwnership(confirmed,selection);
    std::optional<std::vector<NativeConsoleAdapterIdentity>> nodes;try{nodes=readDisplayAdapterNodes();}catch(...){ }
    const auto inventory=nodes?summarizeDisplayAdapters(*nodes,owner):DisplayAdapterInventory{};
    if(nodes){
      // The proven current node joins the ledger, so a later reinstall's leftover is provable.
      if(inventory.current==1&&!std::any_of(owner.created.begin(),owner.created.end(),[&](const auto& id){return legacyDisplayTextEqual(id,owner.current);}))(void)recordNativeAdapterNode(owner.current);
      for(const auto& id:owner.created)if(!std::any_of(nodes->begin(),nodes->end(),[&](const auto& node){return legacyDisplayTextEqual(node.instanceId,id);}))forgetNativeAdapterNode(id);
    }
    // Once per run: an earlier Host noted an added driver certificate only in the journal (native_display_trust.hpp).
    backfillNativeDisplayTrustOnce("37E100B2B382866FA7CE04EE33FE53DF53AEDC6D");
    const char* blocker=displayAdapterCleanupBlocker(inventory,confirmed,idle,displayV2StatePresent());
    publishAdapterInventory(inventory,blocker);
    if(*blocker||(!inventory.legacy&&!inventory.duplicate)||adapterCleanupRounds_>=3)return;
    std::vector<DisplayAdapterExtra> proven;for(const auto& extra:inventory.extras)if(extra.kind!=DisplayAdapterKind::unproven)proven.push_back(extra);
    if(profileRecoveryWorker_.joinable())profileRecoveryWorker_.join();
    ++adapterCleanupRounds_;profileRecoveryRunning_=true;
    try{profileRecoveryWorker_=std::thread([this,owner,proven]{performAdapterCleanup(owner,proven);profileRecoveryRunning_=false;});}catch(...){profileRecoveryRunning_=false;}
  }
  // Diagnostics read the newest observation; a changed one is also one event
  // (hostAction 273 stage 0: expected=current, stale=legacy, missingOwned=duplicate,
  // fallback=unproven, items=foreign, ok=nothing of ours left over).
  void publishAdapterInventory(const DisplayAdapterInventory& inventory,const char* blocker){
    // Counts only: a blocker that comes and goes with every stream is not an event.
    const std::string fingerprint=std::to_string(inventory.read)+":"+std::to_string(inventory.current)+":"+std::to_string(inventory.legacy)+":"+std::to_string(inventory.duplicate)+":"+std::to_string(inventory.unproven)+":"+std::to_string(inventory.foreign);
    {std::lock_guard lock(adapterMutex_);adapterInventory_=displayAdapterInventoryJson(inventory,blocker,adapterLastCleanup_);}
    if(fingerprint==adapterFingerprint_)return;adapterFingerprint_=fingerprint;
    if(!inventory.read)return;
    recordHostEvent("hostAction",{{"action",uint32_t(273)},{"stage",uint32_t(0)},{"ok",!inventory.legacy&&!inventory.duplicate&&!inventory.unproven},
      {"expected",inventory.current},{"stale",inventory.legacy},{"missingOwned",inventory.duplicate},{"fallback",inventory.unproven},{"items",inventory.foreign}});
  }
  void recoverPendingStreamProfile(){
#ifdef MULTITAP_NATIVE_DISPLAY
    if(v2Mode())return;
    std::lock_guard lifecycle(lifecycleMutex_);std::lock_guard launch(profileRecoveryLaunchMutex_);
    if(privateDiagnosticBusy())return;
    // Every early exit records one fixed gate code for automatic diagnostics.
    const auto block=[](const char* code){nativeRecoveryNote(NativeRecoveryField::blocker,code);};
    if(profileRecoveryClosing_||profileRecoveryRunning_){block("recovery_running");return;}
    if(!nativeDisplay_||!nativeDisplaySelected()){block("not_selected");return;}
    if(nativeDisplay_->hasStreamProfileReservation()){block("reservation_held");return;}
    if((!stopped_&&streaming_)||consoleRunning_||displayOperationRunning_||!qemuConsoleQuiescent()){block("session_active");return;}
    const auto now=streamMonotonicMillis();if(now<restartProfileNext_)return;restartProfileNext_=now+5000;
    try{
      const auto directory=machineDirectory()/L"private";auto bytes=nativeStreamProfileFileAccess(directory/L"native-stream-profile.json").load(),launchBytes=nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json").load();
      std::string expiredRetirement;auto retirementIo=nativeStreamRetirementFileAccess(directory);const auto retirementBytes=retirementIo.marker.load();if(retirementBytes){const auto held=parseNativeStreamRetirement(*retirementBytes);if(held.phase=="prepared"){const auto archivedProfile=retirementIo.archive(held.operation,0),archivedLaunch=retirementIo.archive(held.operation,1);if(!archivedProfile||!archivedLaunch||nativeBaselineDigest(*archivedProfile)!=held.pins[0]||nativeBaselineDigest(*archivedLaunch)!=held.pins[1]||(bytes&&bytes!=archivedProfile)||(launchBytes&&launchBytes!=archivedLaunch)){block("retirement_archive_mismatch");return;}bytes=archivedProfile;launchBytes=archivedLaunch;expiredRetirement=held.expiredConsoleSha256;}}
      if(!bytes||!launchBytes){block(!bytes&&!launchBytes?"none":!bytes?"profile_missing":"launch_missing");return;}
      const auto profile=parseNativeStreamProfileRecord(*bytes);const auto source=parseNativeStreamLaunch(*launchBytes);
      if(profile.phase==NativeStreamProfilePhase::restored){block("profile_restored");return;}
      if(!nativeStreamLaunchMatches(source,profile)){block("launch_mismatch");return;}
      if(source.service==nativeStreamProcessIdentity(GetCurrentProcess())){block("launch_current_service");return;}
      const auto currentSession=interactiveSession();if(!sameInteractiveConsole(currentSession)||!sessionUnlocked(currentSession)){block("console_session_required");return;}const auto currentLogon=nativeSessionRecoveryScope(currentSession);
      const bool retirement=currentLogon.logon!=profile.scope.logon;
      if(retirement)expiredRetirement.clear(); // A fresh login uses ordinary retirement authority.
      if(!retirement&&!nativeRecoveryLogonMatches(profile.scope,currentLogon)){block("logon_unverified");return;}
      if(!retirement&&expiredRetirement.empty()&&!nativeDisplay_->expiredStreamProfileQuiescent(profile)){block("sdk_not_quiescent");return;}
      if(observeNativeStreamProcess(source.service).state!=NativeStreamProcessState::gone){block("original_service_alive");return;}
      if(observeNativeStreamProcess(source.worker).state!=NativeStreamProcessState::gone){block("original_worker_alive");return;}
      const auto retryKey=nativeBaselineDigest(profile.key.operation+'|'+currentLogon.logon+'|'+(retirement?"retirement":!expiredRetirement.empty()?"expired-retirement":"expired-restore"));
      if(restartProfileOperation_!=retryKey){restartProfileOperation_=retryKey;restartProfileAttempts_=0;}
      if(restartProfileAttempts_>=3){nativeRecoveryTrace().budget(restartProfileAttempts_,true);block("retry_budget_exhausted");return;}
      // This candidate never adopts a live lease after a service crash. Wait
      // for exact SDK absence, then restore only the verified recovery console.
      if(profileRecoveryWorker_.joinable())profileRecoveryWorker_.join();++restartProfileAttempts_;profileRecoveryRunning_=true;
      nativeRecoveryTrace().attempt(retirement?"retirement":!expiredRetirement.empty()?"expired_retirement":"expired_restore",restartProfileAttempts_,GetTickCount64());nativeRecoveryTrace().budget(restartProfileAttempts_,false);block("started");
      profileRecoveryWorker_=std::thread([this,profile,retirement,currentSession,expiredRetirement]{if(retirement||!expiredRetirement.empty())performRestartProfileRecovery(profile,true,currentSession,expiredRetirement);else performExpiredProfileRecovery(profile,currentSession);profileRecoveryRunning_=false;});
    }catch(...){block("records_invalid");profileRecoveryRunning_=false;}
#endif
  }
#ifdef MULTITAP_NATIVE_DISPLAY
  // Administrator record reset: hold the lifecycle and recovery-launch locks
  // for the WHOLE orchestration so no session, display change or recovery
  // worker can start between the idle check and the record move.
  std::string nativeResetOtherBusy(){
    if(!stopped_&&streaming_)return "stream_active";if(consoleRunning_)return "console_operation";if(displayOperationRunning_)return "display_operation";
    if(profileRecoveryRunning_||profileRecoveryClosing_)return "recovery_running";if(!workerQuiescentNow())return "worker_running";if(nativeResetRdpSessionActive())return "rdp_session_active";return "";
  }
  std::string nativeResetBusy(){return privateDiagnosticBusy()?std::string("recovery_running"):nativeResetOtherBusy();}
  std::string nativeDiagnosticOtherBusy(){
    const auto busy=nativeResetOtherBusy();
    return !busy.empty()?busy:(probing_||thumbnailActive_.load()?std::string("worker_running"):std::string());
  }
  template<class F> bool underNativeResetLocks(F&& body){std::lock_guard lifecycle(lifecycleMutex_);std::lock_guard launch(profileRecoveryLaunchMutex_);return body();}
  void nativeResetCompleted(){
    std::lock_guard lifecycle(lifecycleMutex_);restartProfileOperation_.clear();restartProfileAttempts_=0;restartProfileNext_=0;initialFallback_.clear();
    {std::lock_guard lock(profileRecoveryMutex_);profileRetry_=NativeStreamProfileRetryGate{};}
    nativeRecoveryTrace().reset();invalidateDisplay();
  }
#endif
  // New sessions wait while the previous stream worker may still run. Its exit
  // used to be judged once, at the end of its stream: a worker that outlived
  // the 8 s + 5 s bounds (stuck in a display/driver call, a starved VM) kept
  // every later stream start at host_maintenance until the service
  // restarted. The retained wait handle of that exact process is re-checked
  // here, so its actual exit reopens sessions and completes the stop evidence
  // (hostAction 220 stage 2). Nothing is assumed while it still runs.
  bool workerQuiescentNow(){
    if(!qemuConsoleQuiescent())return false;
    if(workerQuiescent_)return true;
    std::lock_guard lifecycle(lifecycleMutex_);std::lock_guard lock(unexitedMutex_);
    if(workerQuiescent_)return true;
    if(!unexitedWorker_.value||WaitForSingleObject(unexitedWorker_.value,0)!=WAIT_OBJECT_0)return false;
    unexitedWorker_=WinHandle();workerQuiescent_=true;
    if(lastSessionStop_&&lastSessionStop_->session==unexitedSession_)lastSessionStop_->quiescent=true;
    unexitedSession_.clear();
    recordHostEvent("hostAction",{{"action",uint32_t(220)},{"stage",uint32_t(2)},{"ok",true}});
    return true;
  }
  // A remote Host program update defers while any user-visible operation runs.
  bool hostUpdateSessionActive() const {
    return privateDiagnosticBusy()||(!stopped_&&streaming_)||consoleRunning_||displayOperationRunning_||profileRecoveryRunning_;
  }
  // Render adapter re-arm (NativeDisplayService::setRenderRearmIdle): the virtual
  // display may be stopped and recreated only while nothing holds or restores
  // it, the bar recoverDisplayV2 and a Host update use, plus the last session
  // worker process has exited and no v2 state file waits to be restored.
  // Service loop thread only, outside the display service's mutex.
  bool renderRearmIdle()noexcept{
    try{
      if(privateDiagnosticBusy()||profileRecoveryClosing_||profileRecoveryRunning_||(!stopped_&&streaming_)||v2Rpc_||consoleRunning_||displayOperationRunning_||!qemuConsoleQuiescent())return false;
      if(!workerQuiescentNow()||displayV2StatePresent())return false;
      return true;
    }catch(...){return false;}
  }
  // A worker's GPU path record whose capture is not on hardware (stage 43
  // reason 2, WARP): ask the SDK owner for its one re-arm. It decides and
  // waits for an idle display; this stream is not touched.
  void observeGpuPath(uint32_t stage,uint32_t error)noexcept{try{if(nativeDisplay_&&streamObservationNoHardwareCapture(stage,error))(void)nativeDisplay_->requestRenderRearm();}catch(...){ }}
  std::string diagnostics() override {
    const DWORD session=interactiveSession();const bool interactive=sessionUnlocked(session),console=sameInteractiveConsole(session);
    bool capture=false,observed=false;int64_t observationAge=-1;const auto display=displayCache_.snapshot(session,interactive,&capture,&observed,&observationAge);Json native,events,consoleRecovery,recovery;
    try{if(nativeDisplaySelected()&&nativeDisplay_){
#ifdef MULTITAP_NATIVE_DISPLAY
      native=nativeDisplay_->diagnosticSnapshot();
      // Read-only protected-record explanation + last recovery decisions.
      NativeRecoveryInventoryAccess inventory;inventory.directory=machineDirectory()/L"private";inventory.service=nativeStreamProcessIdentity(GetCurrentProcess());inventory.observe=[](const auto& process){return observeNativeStreamProcess(process).state;};
      try{if(sameInteractiveConsole(session))inventory.logon=nativeSessionRecoveryScope(session);}catch(...){ }
      try{recovery=JsonParser::parse(nativeRecoveryInventoryJson(inventory,nativeRecoveryTrace(),GetTickCount64()));}catch(...){ }
#endif
    }}catch(...){ }
    try{events=JsonParser::parse(hostLogSnapshot());}catch(...){ }
    try{consoleRecovery=JsonParser::parse(streamConsoleDiagnosticSummary(observeStreamConsoleRecovery()));}catch(...){ }
    const auto reason=observed?viewportFailureReason(display,interactive,capture,console):!interactive?"no_interactive_session":!console?"interactive_console_required":"viewport_probe_pending";
    return appendEncoderDiagnostic(buildHostDiagnosticReport(display,interactive,console,capture,reason,native,events,observed,observationAge,consoleRecovery,recovery,deviceInventoryDiagnostic(),&diagnosticDelivery_),encoderDiagnostics_.json(GetTickCount64()));
  }
  HostDiagnosticDelivery diagnosticDelivery_;
  bool diagnosticsAcknowledged(bool accepted) override {return diagnosticDelivery_.acknowledged(accepted);}
  std::string signinEligibility() override {return signinRuntime_?signinRuntime_->eligibilityMessage():std::string();}
  void mainConnected(HINTERNET actual,std::function<bool(const std::string&)>,std::function<bool(HINTERNET)>) override {
    if(signinRuntime_)signinRuntime_->connectionOpened(actual);
  }
  void mainDisconnected(HINTERNET actual) override {if(signinRuntime_)signinRuntime_->connectionClosed(actual);}
  void stopMainAdapters() override {if(ownsSignin_&&signinRuntime_)signinRuntime_->stop(signinOwnedSession_);}
  bool mainControl(HINTERNET actual,const Json& value,uint64_t) override {
    // The VM's name for Host settings' title (desktop_name.hpp).
    if(value.get("type").string()=="host.capabilities.ack"){const auto name=value.get("desktopName").string();if(!name.empty())try{saveDesktopName(name);}catch(...){}}
    if(signinRuntime_&&value.get("type").string()=="host.capabilities.ack")signinRuntime_->protocolNegotiated(actual,value.get("accepted").boolean()&&value.get("signinServiceProtocol").string()=="signin-service-v1");
    return false;
  }
  std::string capabilities() override {
    const auto privacyObservation = thumbnailPrivacy_.begin();
    // One session view for every field (capability_inputs.hpp): kept until a
    // WTS session notification, a display change or 30 s. Read after the
    // privacy ticket: the control handler counts a session change before the
    // loop rotates the privacy epoch, so a ticket never labels an older view.
    const auto view = capabilityInputs_.session();
    const DWORD session = view.session; const bool interactive = view.unlocked;scheduleProfileRecovery(session,interactive);probeDisplay(session, interactive);bool captureAvailable=false,sampled=false; DisplayState display = displayCache_.snapshot(session, interactive,&captureAvailable,&sampled);
    // A secure desktop (a UAC prompt, Ctrl+Alt+Del) of this unlocked session: the last probe's worker found no
    // interactive desktop. Reported as secureDesktop (below) so the broker ends a stream's cleanup barrier under
    // it: Windows refuses every display change until the prompt is answered, and the Console answers it.
    const bool probeNoDesktop=interactive&&sampled&&!captureAvailable&&display.error=="no_interactive_session";
    secureDesktopReturned(probeNoDesktop,interactive&&sampled&&captureAvailable);
    // Unused value: the read refuses (throws) an untrusted settings file. About
    // 0.2 wakeups (idle_wake_test); a cache would keep an unopened file's {} unchecked.
    const auto settings = readPublicConfig();
    bool viewportDegraded=false;
    if(nativeDisplaySelected()){
      bool verified=false;std::string leaseKey;try{const auto snapshot=nativeDisplayLeaseSnapshot();leaseKey=nativeDisplayLeaseKey(snapshot);verified=nativeActivationPublished(snapshot)&&nativeActivationGate_.readinessAllowed(std::to_string(session)+":"+leaseKey);}catch(...){ }
      if(!verified){display.active=false;display.viewportProvisioning=false;display.monitorControlSupported=false;display.configurationPending=true;if(display.error.empty())display.error="native_display_activation_pending";}
      // An unreadable lease keeps an armed fallback; a different lease clears it.
      viewportDegraded=initialFallback_.armed()&&(leaseKey.empty()||initialFallback_.active(session,leaseKey));
    }
    if(nativeDisplay_&&nativeDisplay_->streamProfilePending()){
      display.monitorControlSupported=false;
      if(!profileSession_||stopped_||!streaming_){display.viewportProvisioning=false;display.active=false;display.recoveryRequired=true;display.configurationPending=true;display.error="native_topology_recovery_required";}
    }
    const bool recovering=profileRecoveryRunning_;
    const bool maintenance=privateDiagnosticBusy()||recovering||(maintenance_&&maintenance_->busy());
    // An invalidated cache (every stream start/stop) is "not yet observed", not
    // a device teardown; recording its defaults showed false installed:false.
    if(sampled)recordHostEvent("displayObserved",{{"installed",display.installed},{"started",display.deviceReady},{"active",display.active},{"owned",display.owned},{"pending",display.configurationPending},{"recovery",display.recoveryRequired},{"reboot",display.rebootRequired},{"remote",display.remoteSession},{"monitors",int64_t(display.monitors.size())},{"ok",display.ready()},{"running",1}},true);
    const bool console=capabilityConsole(view);const bool viewport=!viewportDegraded&&!maintenance&&interactive&&captureAvailable&&console&&display.ready()&&display.viewportProvisioning;
    bool profileViewport=false,profileTracks=false;
#ifdef MULTITAP_NATIVE_DISPLAY
    try{profileViewport=nativeStreamProfileViewportCapability(display,nativeDisplayConfig(),nativeDisplaySelected(),viewport,profileSession_&&!stopped_&&streaming_);}catch(...){ }
    // v2 streams hold no profile reservation; the same wire fields are kept.
    try{profileTracks=nativeStreamProfileTracksCapability(display,nativeDisplayConfig(),nativeDisplaySelected(),viewport,
      nativeStreamProfileTracksActive(profileSession_,v2Session_,stopped_,streaming_),nativeDisplayLeaseSnapshot(),GetCurrentProcessId());}catch(...){ }
#endif
    const auto maintenanceReason=privateDiagnosticBusy()||recovering?std::string("host_maintenance"):(maintenance_?maintenance_->reason():"");const auto viewportReason=maintenanceReason.empty()?viewportFailureReason(display,interactive,captureAvailable,console):safeViewportFailure(maintenanceReason);
    StreamConsolePlan consolePlan;bool consoleRecovery=false;
#ifdef MULTITAP_NATIVE_DISPLAY
    // The device facts are read now; the Windows identities come from the cache.
    try{const auto observation=observeStreamConsoleRecovery([this](DWORD id){return capabilityInputs_.identity(id);});consoleRecovery=observation.native;consolePlan=planStreamConsoleRecovery(observation);}catch(...){ }
#endif
    const auto signinFields=signinRuntime_?signinRuntime_->capabilityFields():std::string();
    const auto consoleFields=signinFields+",\"consoleRecoverySupported\":"+std::string(consoleRecovery?"true":"false")+",\"consoleRecoveryAvailable\":"+(!maintenance&&!consoleRunning_&&consolePlan.eligible?"true":"false")+",\"consoleRecoveryReason\":"+jsonQuote(consolePlan.ready?"console_ready":safeStreamConsoleReason(consolePlan.reason))+nativeStreamProfileCapabilityFields(profileViewport)+nativeStreamProfileTracksCapabilityFields(profileTracks,profileViewport)+(v2Mode()&&nativeDisplaySelected()?",\"qemuConsoleProtocol\":"+jsonQuote(qemuConsoleProtocol):std::string())+(probeNoDesktop&&console?",\"secureDesktop\":true":"");
    std::string monitors="[";for(const auto& monitor:display.monitors){if(monitors.size()>1)monitors+=',';monitors+="{\"id\":"+jsonQuote(monitor.id)+",\"width\":"+std::to_string(monitor.width)+",\"height\":"+std::to_string(monitor.height)+",\"primary\":"+(monitor.primary?"true":"false");if(viewport)monitors+=advertisedOwnedModes(monitor.width,monitor.height);monitors+='}';}monitors+=']';
    // The thumbnail permission uses the same view, taken after the privacy
    // ticket: a WTS change counted before the ticket made the view read afresh,
    // one after it is caught by the ticket. The capture itself reads WTS afresh.
    const bool thumbnailAllowed = !privateDiagnosticBusy() && view.session != 0xffffffff && view.unlocked && console;
    const auto thumbnailFields = thumbnailPrivacyCapabilityFields(thumbnailPrivacy_.snapshot(privacyObservation, thumbnailAllowed)) + thumbnailCaptureCapabilityFields();
    // rdpSupported and rdpEnabled are always false: the Host has no RDP, and the
    // broker requires both booleans in every capability report.
    // keyframe-v1: stream.keyframe reaches the worker as an IDR request (keyFrame()).
    const auto original="{\"observabilityProtocol\":\"stream-observability-v1\",\"streamSizeProtocol\":\"stream-size-v1\",\"streamSizeProtocols\":[\"stream-size-v1\",\"stream-size-v2\"],\"sessionStopProtocol\":\"stop-v1\",\"keyFrameProtocol\":\"" + std::string(keyFrameProtocol) + "\",\"hostResumeProtocol\":\"" + std::string(hostResumeProtocol) + "\",\"hostChannelResumeProtocol\":\"" + std::string(channelResumeProtocol) + "\",\"udpMediaProtocol\":\"" + std::string(udpMediaProtocol) + "\",\"udpDirectHostProtocol\":\"udp-direct-v1\",\"udpTracksHostProtocol\":\"udp-tracks-v1\",\"audioCodecProtocol\":\"" + std::string(audioOpusProtocol) + "\",\"audioCodecTracksProtocol\":\"" + std::string(audioOpusTracksProtocol) + "\",\"trackResumeProtocol\":\"" + std::string(trackResumeProtocol) + "\",\"bitrateProtocol\":\"" + std::string(bitrateAdaptiveProtocol) + "\",\"maxBitrate\":" + std::to_string(adaptiveMaximumBitrate) + ",\"inputLatencyProtocol\":\"" + std::string(inputLatencyProtocol) + "\",\"cursorProtocol\":\"" + std::string(cursor::protocol) + "\",\"cursorTracksProtocol\":\"" + std::string(cursor::tracksProtocol) + "\",\"cursorEchoProtocol\":\"" + std::string(cursor::echoProtocol) + "\",\"cursorComposeProtocol\":\"" + std::string(cursor::composeProtocol) + "\",\"textInputProtocol\":\"" + std::string(textInputProtocol) + "\",\"displayScaleProtocol\":\"" + std::string(displayScaleProtocolName) + "\",\"displayScaleOutputsProtocol\":\"" + std::string(displayScaleOutputsProtocolName) + "\",\"touchInputProtocol\":\"" + std::string(touchInputProtocol) + "\",\"relativeMouseProtocol\":\"" + std::string(relativeMouseProtocol) + "\",\"relativeMouseTracksProtocol\":\"" + std::string(relativeMouseTracksProtocol) + "\",\"caretProtocol\":\"" + std::string(caretProtocol) + "\"" + printCapabilityFields() + usbCapabilityFields() + (interactive && driveCapable() ? ",\"driveProtocol\":\"" + std::string(drive::protocol) + "\"" + (driveFsCapable() ? ",\"driveFs\":true" : "") : std::string()) + (interactive && !driveCapable() ? ",\"driveUnavailableReason\":\"drive_webclient_unavailable\"" : std::string()) + ",\"diagnosticsSupported\":true,\"imeStateProtocol\":\"" + std::string(ime::protocol) + "\",\"service\":true,\"interactiveSession\":" + std::string(interactive ? "true" : "false") + ",\"streaming\":" + (!maintenance&&interactive && display.ready() && captureAvailable ? "true" : "false") + ",\"rdpSupported\":false,\"rdpEnabled\":false,\"virtualDisplayReady\":" + (!maintenance&&display.ready() ? "true" : "false") + ",\"clipboard\":" + (!maintenance&&interactive && captureAvailable && display.ready() && display.clipboardSupported ? "true" : "false") + ",\"clipboardSync\":" + (!maintenance&&interactive && captureAvailable && display.ready() && display.clipboardSupported ? "true" : "false") + ",\"virtualMonitorControl\":" + (!maintenance&&interactive && captureAvailable && console && display.ready() && display.monitorControlSupported ? "true" : "false") + ",\"dynamicResolution\":"+(viewport?"true":"false")+",\"viewportProvisioning\":"+(viewport?"true":"false")+",\"viewportFailureReason\":"+jsonQuote(viewportReason)+(viewport?",\"resizeProtocol\":\"epoch-v1\"":"")+consoleFields+thumbnailFields+",\"monitors\":"+monitors+'}';
    return original;
  }
  // thumbnail-capture-v1 (thumbnail_capture.hpp). The privacy ticket is taken
  // before the first session read: a WTS change during the capture discards it.
  ThumbnailShot thumbnail() override {
    ++thumbnailActive_;struct Release{std::atomic<uint32_t>& count;~Release(){--count;}}release{thumbnailActive_};
    if(privateDiagnosticBusy()){ThumbnailShot shot;shot.state=ThumbnailState::busy;return shot;}
    const auto ticket = thumbnailPrivacy_.begin();
    return thumbnailShooter_.take([this, ticket](bool allowed) { return thumbnailPrivacy_.snapshot(ticket, allowed&&!privateDiagnosticBusy()); });
  }
  void displayControl(const Json& request, Send send) override {
    const auto key = displayOperationKey(request);
    if (request.get("type").string() == "host.display.cancel") {
      std::lock_guard lock(displayOperationMutex_);
      if (displayOperation_ && displayOperation_->key.matches(request)) displayOperation_->cancelled = true;
      return;
    }
    const int count = displayOperationCount(request);
    std::lock_guard lifecycle(lifecycleMutex_);
    if (privateDiagnosticBusy() || (streaming_ && !stopped_) || consoleRunning_ || profileRecoveryRunning_ || !qemuConsoleQuiescent()) {
      const auto response = "{\"type\":\"host.display.result\"," + displayOperationFields(key) + ",\"ok\":false,\"count\":" + std::to_string(count) + ",\"reason\":\"display_control_unavailable\",\"capabilities\":" + capabilities() + '}';
      send(true, {response.begin(), response.end()}); return;
    }
    { std::lock_guard lock(displayOperationMutex_); if (displayOperationRunning_) return; }
    auto activity=maintenance_?maintenance_->reserveTopology():std::shared_ptr<CatalogActivity>{};
    if(maintenance_&&!activity){const auto response="{\"type\":\"host.display.result\","+displayOperationFields(key)+",\"ok\":false,\"count\":"+std::to_string(count)+",\"reason\":\"display_control_unavailable\",\"capabilities\":"+capabilities()+'}';send(true,{response.begin(),response.end()});return;}
    if (displayOperationWorker_.joinable()) displayOperationWorker_.join();
    auto operation = std::make_shared<PendingDisplayOperation>(); operation->key = key; operation->count = count; operation->session = interactiveSession();
    { std::lock_guard lock(displayOperationMutex_); displayOperation_ = operation; displayOperationRunning_ = true; }
    invalidateDisplay();
    displayOperationWorker_ = std::thread([this, operation, send = std::move(send),activity=std::move(activity)] {
      (void)activity;
      bool ok = false; std::string reason = "worker_failed"; DisplayState display; bool desktop = false;uint32_t nativeStage=0,nativeError=0;
      const bool nativeCount=nativeDisplay_&&nativeDisplaySelected();
      // Display manager v2 (docs/display-manager-v2.md, "Output count"): the
      // worker snapshots, asks for the SDK count over the private v2 count RPC,
      // re-lays Windows and verifies; no v1 count transaction/checkpoint. The
      // broker/client messages and the displayConfigure event stay the same.
      const bool v2=nativeCount&&v2Mode();
      std::optional<DisplayV2CountDiagnostic> v2Diagnostic;
      const int64_t countDeadline=streamMonotonicMillis()+nativeCountTimeoutMs;
      const auto ticket = displayCache_.begin(operation->session);
      try {
        if (!sameInteractiveConsole(operation->session) || !sessionUnlocked(operation->session)) reason = "no_interactive_session";
        else if (operation->cancelled) reason = "operation_cancelled";
        else {
          std::atomic<bool> stopped{false}; UserWorker child(operation->session, stopped); std::mutex writing;
          const auto write = [&](const char* type) { const auto message = "{\"type\":" + jsonQuote(type) + ',' + displayOperationFields(operation->key) + ",\"count\":" + std::to_string(operation->count) + ",\"workerDeadlineMs\":"+std::to_string(countDeadline)+(v2?",\"displayManager\":\"v2\"":"")+'}'; return pipePacket(child.pipe.value, true, {message.begin(), message.end()}, stopped, writing); };
          bool applied=false;
          const auto read=[&](std::chrono::milliseconds timeout){
            const auto until=Clock::now()+timeout;
            for(;;){
              const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(until-Clock::now());bool text=false;std::vector<uint8_t> bytes;
              if(remaining.count()<=0||!readPipePacket(child.pipe.value,text,bytes,stopped,remaining)||!text||bytes.size()>65536)throw std::runtime_error("Display worker response unavailable");
              auto response=JsonParser::parse(std::string(bytes.begin(),bytes.end()));
              if(v2){
                // SDK size (restoring a saved size) and SDK count requests of
                // this operation's worker only; never the v1 count RPC. Its
                // state file requests, when it is not an administrator's.
                if(const auto state=displayV2StatePacket(response,applied,operation->session);state.handled){
                  if(state.response.empty()||!pipePacket(child.pipe.value,true,{state.response.begin(),state.response.end()},stopped,writing))throw std::runtime_error("Display worker v2 response failed");
                  continue;
                }
                const auto dispatched=dispatchDisplayV2(response,applied,
                  [&](const std::string& monitor,DWORD width,DWORD height){return nativeDisplay_->setOwnedSizeV2(monitor,width,height);},
                  [&](int requested)->std::optional<std::string>{
                    if(!displayV2CountAuthorized(requested,operation->count,applied,operation->cancelled,streamMonotonicMillis()<countDeadline,sameInteractiveConsole(operation->session),sessionUnlocked(operation->session)))return {};
                    return nativeDisplay_->setOwnedCountV2(requested);
                  });
                if(!dispatched.handled)return response;
                if(dispatched.response.empty()||!pipePacket(child.pipe.value,true,{dispatched.response.begin(),dispatched.response.end()},stopped,writing))throw std::runtime_error("Display worker v2 response failed");
                continue;
              }
              if(response.get("type").string().rfind("worker.native.count.",0)!=0)return response;
              NativeCountContext context;context.request=NativeCountRequest{operation->key,operation->count};context.deadline=countDeadline;context.applied=nativeCount&&applied;
              context.authorized=!operation->cancelled;context.console=sameInteractiveConsole(operation->session);context.unlocked=sessionUnlocked(operation->session);
              if(context.applied&&context.console&&context.unlocked&&response.get("type").string()!="worker.native.count.rollback")try{context.scope=nativeSessionRecoveryScope(operation->session);}catch(...){ }
              const auto dispatched=dispatchNativeCount(response,context,nativeDisplay_);
              if(!dispatched.handled||dispatched.response.empty()||!pipePacket(child.pipe.value,true,{dispatched.response.begin(),dispatched.response.end()},stopped,writing))throw std::runtime_error("Display worker native response failed");
            }
          };
          if (!write("worker.display.prepare")) throw std::runtime_error("Display worker preparation failed");
          auto response=read(std::chrono::seconds(12));
          if (response.get("type").string() != "worker.display.prepared" || !operation->key.matches(response)||response.get("count").number()!=operation->count) throw std::runtime_error("Display worker correlation mismatch");
          if (!response.get("ok").boolean()) {
            reason = safeDisplayReason(response.get("reason").string()); display = parseDisplayState(response.get("display")); desktop = response.get("desktopAvailable").boolean();
          } else {
            const bool commit = !operation->cancelled && sameInteractiveConsole(operation->session) && sessionUnlocked(operation->session);
            applied=commit;
            if (!write(commit ? "worker.display.apply" : "worker.display.cancel")) throw std::runtime_error("Display worker commit failed");
            response=read(std::chrono::seconds(30));
            if (response.get("type").string() != "worker.display.result" || !operation->key.matches(response)||response.get("count").number()!=operation->count) throw std::runtime_error("Display worker result correlation mismatch");
            if(nativeCount){
              const auto stage=response.get("nativeStage").number(-1),error=response.get("nativeError").number(-1);
              if(!std::isfinite(stage)||std::floor(stage)!=stage||stage<0||stage>(v2?9:7)||!std::isfinite(error)||std::floor(error)!=error||error<0||error>UINT32_MAX||(response.get("ok").boolean()&&(stage||error)))throw std::runtime_error("Invalid display worker diagnostic");
              nativeStage=uint32_t(stage);nativeError=uint32_t(error);
              // v2 worker stages are reported as 100+stage (108 = foreign
              // display changed, error = displayV2ForeignCode) so they never
              // collide with the v1 phases 1-7 or the service codes 8/9.
              if(v2){
                if(!std::holds_alternative<std::nullptr_t>(response.get("v2").value)){v2Diagnostic=parseDisplayV2CountDiagnostic(response);if(!v2Diagnostic)throw std::runtime_error("Invalid display worker diagnostic");}
                if(nativeStage)nativeStage+=100;
              }
            }
            display = parseDisplayState(response.get("display")); desktop = response.get("desktopAvailable").boolean();
            ok = response.get("ok").boolean() && display.ready() && display.monitors.size() == size_t(operation->count);
            reason = ok ? "" : safeDisplayReason(response.get("reason").string());
          }
        }
      } catch (...) { reason = "worker_failed";if(nativeCount&&!nativeStage)nativeStage=9; }
      if (operation->cancelled) { ok = false; reason = "operation_cancelled"; }
      if (!sameInteractiveConsole(operation->session) || !sessionUnlocked(operation->session)) { ok = false; reason = "no_interactive_session"; display = {}; desktop = false; }
      if(nativeCount){
        bool safe=false;
        if(desktop&&display.ready()&&(!operation->cancelled||!ok))try{safe=nativeDisplay_->acknowledgeCountOperation({operation->key,operation->count},ok);}catch(...){ }
        if(!safe){
          if(!nativeStage)nativeStage=8;
          nativeDisplay_->cancelCountOperation(operation->key);ok=false;
          display.active=false;display.viewportProvisioning=false;display.monitorControlSupported=false;
          try{const auto state=JsonParser::parse(nativeDisplay_->viewportSnapshot());display.recoveryRequired=state.get("recoveryRequired").boolean();display.configurationPending=display.recoveryRequired||state.get("configurationPending").boolean();}catch(...){display.recoveryRequired=true;display.configurationPending=true;}
          display.error=display.recoveryRequired?"native_topology_recovery_required":"native_display_not_ready";
        }else try{display.monitorControlSupported=JsonParser::parse(nativeDisplay_->viewportSnapshot()).get("countControlAvailable").boolean();}catch(...){display.monitorControlSupported=false;}
      }
      const auto diagnosticMonitors=uint32_t(display.monitors.size());const auto diagnosticRecovery=display.recoveryRequired;
      if(!displayCache_.complete(ticket, std::move(display), Clock::now(), desktop)){ok=false;reason="operation_cancelled";} readinessChanged_ = true;
      std::string caps;
      try { caps = capabilities(); } catch (...) { ok = false; reason = "worker_failed"; caps = "{\"service\":true,\"interactiveSession\":false,\"streaming\":false,\"rdpSupported\":false,\"rdpEnabled\":false,\"virtualDisplayReady\":false,\"virtualMonitorControl\":false,\"monitors\":[]}"; }
      // v2 count detail (fixed codes, like action 210 for a stream start):
      // stage = raw v2 stage, error = displayV2ForeignCode at stage 8,
      // monitors = active other displays before the change (console included),
      // active = console positively identified, changed = Windows activated a
      // display that was inactive before, recovery = the snapshot was restored
      // after a failure, pending = a state file remains for automatic recovery.
      if(v2Diagnostic)recordHostEvent("hostAction",{{"action",uint32_t(212)},{"ok",v2Diagnostic->ok},{"stage",v2Diagnostic->stage},{"error",v2Diagnostic->error},{"monitors",v2Diagnostic->foreignCount},
        {"active",v2Diagnostic->consoleIdentified},{"changed",v2Diagnostic->displaysReturned},{"recovery",v2Diagnostic->restored},{"pending",v2Diagnostic->stateKept}});
      recordHostEvent("displayConfigure",{{"ok",ok},{"action",uint32_t(operation->count)},{"monitors",diagnosticMonitors},{"stage",nativeStage},{"error",nativeError},{"recovery",diagnosticRecovery}});
      const auto response = "{\"type\":\"host.display.result\"," + displayOperationFields(operation->key) + ",\"ok\":" + (ok ? "true" : "false") + ",\"count\":" + std::to_string(operation->count) + (ok ? "" : ",\"reason\":" + jsonQuote(reason)) + ",\"capabilities\":" + caps + '}';
      try { send(true, {response.begin(), response.end()}); } catch (...) {}
      { std::lock_guard lock(displayOperationMutex_); displayOperationRunning_ = false; }
    });
  }
  // ime-state-v1 (ime_state.hpp): answered on the IME thread, never here.
  void imeQuery(const Json& request,Send send) override {
    const auto id=ime::queryId(request);if(!id)return;
    (void)ime_.post(*id,[send=std::move(send)](const std::string& packet){send(true,{packet.begin(),packet.end()});});
  }
  void qemuConsoleControl(const Json& request,Send send) override {
    const auto key=displayOperationKey(request);
    const auto type=request.get("type").string();
    if(request.get("protocol").string()!=qemuConsoleProtocol)return;
    const auto* requestFields=std::get_if<Json::Object>(&request.value);
    if((type=="host.qemu-console.cancel"||type=="host.qemu-console.status")&&(!requestFields||requestFields->size()!=4))return;
    if(type=="host.qemu-console.cancel"){
      std::string bytes;
      {std::lock_guard lock(consoleMutex_);const auto& op=consoleOperation_;
        if(!op||!op->qemu||!op->key.matches(request))return;
        op->cancelled=true;op->hold.release();op->result.ok=false;op->result.reason="qemu_console_cancelled";
        const bool quiet=op->finished&&op->quiet;consoleRunning_=!quiet||(op->hadHold&&!op->rearmChecked);
        bytes=qemuConsoleResultPacket(key,false,op->result.attempted,quiet,op->result.reason);
      }
      (void)qemuConsoleQuiescent();invalidateDisplay();send(true,{bytes.begin(),bytes.end()});
      return;
    }
    if(type=="host.qemu-console.renew"){
      uint32_t sequence=0;try{sequence=qemuConsoleRenewSequence(request);}catch(...){return;}
      (void)qemuConsoleQuiescent();std::shared_ptr<ConsoleOperation> operation;
      {std::lock_guard lock(consoleMutex_);operation=consoleOperation_;if(!operation||!operation->qemu||!operation->key.matches(request)||!operation->finished)return;}
      bool identity=false;
      // A parked hold renews the SDK owner's park with it; one that already ran out cannot be renewed.
      if(operation->parked){try{identity=qemuConsoleParkIdentityCurrent(nativeConsoleUuid(qemuConsoleMachine())==nativeConsoleUuid(operation->qemuProof.machineUuid),
          nativeDisplay_&&nativeDisplay_->renewConsolePark(qemuConsoleHoldMillis+qemuConsoleParkMarginMillis));}catch(...){ }}
      else try{const auto snapshot=nativeDisplayLeaseSnapshot();identity=qemuConsoleHoldIdentityCurrent(sameInteractiveConsole(operation->qemuSession),
          nativeConsoleUuid(machineUuid())==nativeConsoleUuid(operation->qemuProof.machineUuid),
          snapshot.get("servicePid").number()==GetCurrentProcessId()&&nativeDisplayLeaseKey(snapshot)==operation->qemuLease,snapshot.get("configurationPending").boolean());}catch(...){ }
      std::string bytes;bool changed=false;
      {std::lock_guard lock(consoleMutex_);if(consoleOperation_!=operation)return;
        const auto now=uint64_t(streamMonotonicMillis());
        if(operation->hold.expire(now)){operation->cancelled=true;operation->result.ok=false;operation->result.reason="qemu_console_timeout";consoleRunning_=!operation->quiet||(operation->hadHold&&!operation->rearmChecked);changed=true;}
        if(operation->hold.held()&&!identity){operation->hold.release();operation->cancelled=true;operation->result.ok=false;operation->result.reason="qemu_console_identity_changed";consoleRunning_=!operation->quiet||(operation->hadHold&&!operation->rearmChecked);changed=true;}
        if(!operation->hold.held())bytes=qemuConsoleResultPacket(key,false,operation->result.attempted,operation->quiet,operation->result.reason);
        else if(!operation->cancelled&&operation->hold.renew(sequence,now))bytes=qemuConsoleHeldPacket(key,sequence);
      }
      if(changed){(void)qemuConsoleQuiescent();invalidateDisplay();}if(!bytes.empty())send(true,{bytes.begin(),bytes.end()});return;
    }
    if(type=="host.qemu-console.status"){
      (void)qemuConsoleQuiescent();
      std::string bytes;
      {std::lock_guard lock(consoleMutex_);const auto& op=consoleOperation_;
        if(!op||!op->qemu||!op->key.matches(request)||!op->finished)return;
        const bool ok=op->result.ok&&op->quiet&&!op->cancelled&&op->hold.held();
        bytes=qemuConsoleResultPacket(key,ok,op->result.attempted,op->quiet,op->result.reason);
      }
      send(true,{bytes.begin(),bytes.end()});return;
    }
    const auto proof=qemuConsoleProof(request);
    const auto reply=[&](const char* reason){const auto bytes=qemuConsoleResultPacket(key,false,false,true,reason);send(true,{bytes.begin(),bytes.end()});};
    std::lock_guard lifecycle(lifecycleMutex_);
    {std::lock_guard lock(consoleMutex_);const auto& prior=consoleOperation_;
      if(prior&&prior->qemu&&prior->key.matches(request)){
        if(prior->finished){const auto bytes=qemuConsoleResultPacket(key,prior->result.ok&&prior->quiet&&!prior->cancelled&&prior->hold.held(),prior->result.attempted,prior->quiet,prior->result.reason);send(true,{bytes.begin(),bytes.end()});}
        return;
      }
    }
    if(privateDiagnosticBusy()||consoleRunning_||displayOperationRunning_||profileRecoveryRunning_||(!stopped_&&streaming_)||!workerQuiescentNow()){reply("qemu_console_busy");return;}
    const auto session=interactiveSession();
    // Signed out, locked or a standard user's session: no worker can act there, the SDK owner can (qemu_console_route.hpp).
    const auto facts=qemuConsoleFacts(session);
    if(qemuConsoleRoute(facts)==QemuConsoleRoute::park){parkQemuConsole(key,proof,facts,std::move(send));return;}
    if(!v2Mode()||!nativeDisplay_||!nativeDisplaySelected()||!sameInteractiveConsole(session)||!sessionUnlocked(session)){reply("qemu_console_unavailable");return;}
    std::string lease;
    try{const auto snapshot=nativeDisplayLeaseSnapshot();lease=nativeDisplayLeaseKey(snapshot);if(snapshot.get("servicePid").number()!=GetCurrentProcessId()||snapshot.get("configurationPending").boolean()||nativeConsoleUuid(machineUuid())!=nativeConsoleUuid(proof.machineUuid)){reply("qemu_console_identity_changed");return;}}
    catch(...){reply("qemu_console_unavailable");return;}
    auto activity=maintenance_?maintenance_->reserveTopology():std::shared_ptr<CatalogActivity>{};
    if(maintenance_&&!activity){reply("qemu_console_busy");return;}
    if(consoleWorker_.joinable())consoleWorker_.join();
    {std::lock_guard gate(probeMutex_);probeStopped_=true;if(probe_.joinable())probe_.join();}
    auto operation=std::make_shared<ConsoleOperation>();operation->key=key;operation->qemu=true;operation->deadline=streamMonotonicMillis()+18000;
    operation->qemuSession=session;operation->qemuProof=proof;operation->qemuLease=lease;
    {std::lock_guard lock(consoleMutex_);consoleOperation_=operation;consoleRunning_=true;}
    try{consoleWorker_=std::thread([this,operation,session,proof,lease,send=std::move(send),activity=std::move(activity)]()mutable{
      QemuConsoleResult result;bool exited=false,created=false,dispatched=false;HANDLE unexited=nullptr;
      try{
        UserWorker child(session,operation->cancelled,&exited,&created,&unexited);std::mutex writing;
        const auto command="{\"type\":\"worker.qemu-console.prepare\",\"protocol\":"+jsonQuote(qemuConsoleProtocol)+','+displayOperationFields(operation->key)+",\"machineUuid\":"+jsonQuote(proof.machineUuid)+",\"model\":\"std\",\"leaseKey\":"+jsonQuote(lease)+",\"workerDeadlineMs\":"+std::to_string(operation->deadline)+'}';
        if(operation->cancelled||!sameInteractiveConsole(session)||!sessionUnlocked(session))throw std::runtime_error("qemu_console_cancelled");
        dispatched=true;
        if(!pipePacket(child.pipe.value,true,{command.begin(),command.end()},operation->cancelled,writing,&operation->deadline))throw std::runtime_error("qemu_console_cancelled");
        Json value;
        for(;;){
          bool text=false;std::vector<uint8_t> bytes;
          if(!readPipePacket(child.pipe.value,text,bytes,operation->cancelled,std::chrono::seconds(18),&operation->deadline)||!text||bytes.size()>4096)throw std::runtime_error("qemu_console_timeout");
          value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));
          if(value.get("type").string()!="worker.qemu-console.authority")break;
          const auto* fields=std::get_if<Json::Object>(&value.value);
          bool allowed=fields&&fields->size()==5&&value.get("protocol").string()==qemuConsoleProtocol&&operation->key.matches(value)&&value.get("leaseKey").string()==lease&&!operation->cancelled&&streamMonotonicMillis()<operation->deadline.load()&&sameInteractiveConsole(session)&&sessionUnlocked(session);
          try{const auto snapshot=nativeDisplayLeaseSnapshot();allowed=allowed&&!snapshot.get("configurationPending").boolean()&&snapshot.get("servicePid").number()==GetCurrentProcessId()&&nativeDisplayLeaseKey(snapshot)==lease&&nativeConsoleUuid(machineUuid())==nativeConsoleUuid(proof.machineUuid);}catch(...){allowed=false;}
          const auto response="{\"type\":\"worker.qemu-console.authority.result\",\"protocol\":"+jsonQuote(qemuConsoleProtocol)+','+displayOperationFields(operation->key)+",\"ok\":"+(allowed?"true":"false")+'}';
          if(!pipePacket(child.pipe.value,true,{response.begin(),response.end()},operation->cancelled,writing,&operation->deadline)||!allowed)throw std::runtime_error("qemu_console_cancelled");
        }
        const auto* fields=std::get_if<Json::Object>(&value.value);
        if(!fields||fields->size()!=8||value.get("type").string()!="worker.qemu-console.result"||value.get("protocol").string()!=qemuConsoleProtocol||!operation->key.matches(value)||!std::holds_alternative<bool>(value.get("ok").value)||!std::holds_alternative<bool>(value.get("attempted").value))throw std::runtime_error("qemu_console_unavailable");
        result={value.get("ok").boolean(),value.get("attempted").boolean(),safeQemuConsoleReason(value.get("reason").string())};
        if(result.ok&&(result.reason!="qemu_console_active"&&result.reason!="qemu_console_restored"))throw std::runtime_error("qemu_console_unavailable");
      }catch(const std::exception& error){result.ok=false;result.attempted=result.attempted||dispatched;result.reason=safeQemuConsoleReason(error.what());}
      catch(...){result.ok=false;result.attempted=result.attempted||dispatched;result.reason="qemu_console_unavailable";}
      const bool quiet=!created||exited;
      if(!quiet){result.ok=false;result.reason="qemu_console_unavailable";}
      if(operation->cancelled){result.ok=false;result.reason="qemu_console_cancelled";}
      if(result.ok){
        try{const auto snapshot=nativeDisplayLeaseSnapshot();
          if(streamMonotonicMillis()>=operation->deadline.load()||!sameInteractiveConsole(session)||!sessionUnlocked(session)||snapshot.get("configurationPending").boolean()||snapshot.get("servicePid").number()!=GetCurrentProcessId()||nativeDisplayLeaseKey(snapshot)!=lease||nativeConsoleUuid(machineUuid())!=nativeConsoleUuid(proof.machineUuid)){result.ok=false;result.reason="qemu_console_identity_changed";}
        }catch(...){result.ok=false;result.reason="qemu_console_unavailable";}
      }
      std::string bytes;
      {std::lock_guard lock(consoleMutex_);
        if(operation->cancelled||streamMonotonicMillis()>=operation->deadline.load()){result.ok=false;result.reason=operation->cancelled?"qemu_console_cancelled":"qemu_console_timeout";}
        operation->result=result;operation->quiet=quiet;operation->unexited=WinHandle(unexited);operation->finished=true;
        if(result.ok&&quiet){operation->hold.begin(uint64_t(streamMonotonicMillis()));operation->hadHold=true;}
        bytes=qemuConsoleResultPacket(operation->key,result.ok,result.attempted,quiet,result.reason);
        if(consoleOperation_==operation)consoleRunning_=operation->hold.held()||!quiet;
      }
      invalidateDisplay();activity.reset();readinessChanged_=true;
      try{send(true,{bytes.begin(),bytes.end()});}catch(...){ }
    });}catch(...){std::lock_guard lock(consoleMutex_);consoleOperation_.reset();consoleRunning_=false;reply("qemu_console_unavailable");}
  }
  std::string qemuConsoleMachine(){
#ifdef MULTITAP_SESSION_STOP_TEST
    if(qemuMachineFixture_)return qemuMachineFixture_();
#endif
    return machineUuid();
  }
  QemuConsoleSessionFacts qemuConsoleFacts(DWORD session){
#ifdef MULTITAP_SESSION_STOP_TEST
    if(qemuFactsFixture_)return qemuFactsFixture_();
#endif
    QemuConsoleSessionFacts facts;
    facts.provider=v2Mode()&&nativeDisplay_&&nativeDisplaySelected();
    facts.console=sameInteractiveConsole(session);facts.unlocked=facts.console&&sessionUnlocked(session);
    facts.elevated=facts.unlocked&&interactiveWorkerAdministrator(session);
    // A secure desktop of that session, as far as the last display probe knows (qemu_console_route.hpp).
    if(facts.elevated){bool capture=false,sampled=false;const auto display=displayCache_.snapshot(session,true,&capture,&sampled);facts.desktop=!(sampled&&!capture&&display.error=="no_interactive_session");}
    return facts;
  }
  // Console park (qemu_console_route.hpp), called under lifecycleMutex_ after the
  // busy checks every console request passes. The answer uses the protocol's
  // existing words so every released broker holds and renews it as it does the
  // worker's: `qemu_console_restored` with attempted=true is "the Host changed
  // the display for this console and holds it". Nothing was changed when the
  // SDK owner refuses: qemu_console_unavailable, not attempted, and the console
  // opens with the status `unknown` as it always did.
  void parkQemuConsole(const DisplayOperationKey& key,const NativeConsoleModelProof& proof,const QemuConsoleSessionFacts& facts,Send send){
    const auto reply=[&](const char* reason){const auto bytes=qemuConsoleResultPacket(key,false,false,true,reason);send(true,{bytes.begin(),bytes.end()});};
    try{if(nativeConsoleUuid(qemuConsoleMachine())!=nativeConsoleUuid(proof.machineUuid)){reply("qemu_console_identity_changed");return;}}
    catch(...){reply("qemu_console_unavailable");return;}
    auto activity=maintenance_?maintenance_->reserveTopology():std::shared_ptr<CatalogActivity>{};
    if(maintenance_&&!activity){reply("qemu_console_busy");return;}
    if(consoleWorker_.joinable())consoleWorker_.join();
    {std::lock_guard gate(probeMutex_);probeStopped_=true;if(probe_.joinable())probe_.join();}
    auto operation=std::make_shared<ConsoleOperation>();operation->key=key;operation->qemu=true;operation->parked=true;operation->deadline=streamMonotonicMillis()+18000;
    operation->qemuProof=proof;
    {std::lock_guard lock(consoleMutex_);consoleOperation_=operation;consoleRunning_=true;}
    const uint32_t cause=qemuConsoleParkCause(facts);
    // The SDK stop is a driver call: off the connection's thread, like the worker's.
    try{consoleWorker_=std::thread([this,operation,cause,send,activity]()mutable{
      auto parked=NativeConsolePark::refused;
      if(!operation->cancelled)parked=nativeDisplay_->parkForConsole(qemuConsoleHoldMillis+qemuConsoleParkMarginMillis);
      QemuConsoleResult result;result.ok=parked==NativeConsolePark::parked;result.attempted=parked!=NativeConsolePark::refused;
      result.reason=result.ok?"qemu_console_restored":"qemu_console_unavailable";
      auto refusal=result.ok?QemuConsoleParkRefusal::none:QemuConsoleParkRefusal::displayBusy;
      std::string bytes;
      {std::lock_guard lock(consoleMutex_);
        if(operation->cancelled||streamMonotonicMillis()>=operation->deadline.load()){result.ok=false;result.reason=operation->cancelled?"qemu_console_cancelled":"qemu_console_timeout";refusal=QemuConsoleParkRefusal::cancelled;}
        operation->result=result;operation->quiet=true;operation->finished=true;
        if(result.ok){operation->hold.begin(uint64_t(streamMonotonicMillis()));operation->hadHold=true;}
        bytes=qemuConsoleResultPacket(operation->key,result.ok,result.attempted,true,result.reason);
        if(consoleOperation_==operation)consoleRunning_=operation->hold.held();
      }
      // Parked for a console that is no longer coming: the outputs return at once.
      if(!result.ok&&parked==NativeConsolePark::parked)nativeDisplay_->endConsolePark();
      recordHostEvent("hostAction",{{"action",qemuConsoleParkAction},{"stage",uint32_t(1)},{"ok",result.ok},{"cause",cause},{"error",uint32_t(refusal)},{"changed",result.attempted}});
      invalidateDisplay();activity.reset();readinessChanged_=true;
      try{send(true,{bytes.begin(),bytes.end()});}catch(...){ }
    });}catch(...){std::lock_guard lock(consoleMutex_);consoleOperation_.reset();consoleRunning_=false;reply("qemu_console_unavailable");}
  }
  void consoleControl(const Json& request,Send send) override {
    const auto key=displayOperationKey(request);
    if(request.get("type").string()=="host.console.cancel"){
      std::lock_guard lock(consoleMutex_);if(consoleOperation_&&!consoleOperation_->qemu&&consoleOperation_->key.matches(request))consoleOperation_->cancelled=true;return;
    }
    const auto reply=[&](bool ok,const std::string& reason,bool attempted=false){const auto text="{\"type\":\"host.console.result\","+displayOperationFields(key)+",\"ok\":"+(ok?"true":"false")+",\"attempted\":"+(attempted?"true":"false")+",\"reason\":"+jsonQuote(reason.empty()?"":safeStreamConsoleReason(reason))+'}';send(true,{text.begin(),text.end()});};
    std::lock_guard lifecycle(lifecycleMutex_);
    if(request.get("userConfirmed").boolean()!=true){reply(false,"operation_cancelled");return;}
    if(privateDiagnosticBusy()||consoleRunning_||displayOperationRunning_||profileRecoveryRunning_||(!stopped_&&streaming_)||!qemuConsoleQuiescent()){reply(false,"console_busy");return;}
    auto activity=maintenance_?maintenance_->reserveTopology():std::shared_ptr<CatalogActivity>{};
    if(maintenance_&&!activity){reply(false,"console_busy");return;}
    StreamConsolePlan plan;try{plan=planStreamConsoleRecovery(observeStreamConsoleRecovery());}catch(...){reply(false,"windows_session_unknown");return;}
    if(plan.ready){reply(true,"");return;}
    if(!plan.eligible){reply(false,plan.reason);return;}
    if(consoleWorker_.joinable())consoleWorker_.join();
    {std::lock_guard gate(probeMutex_);probeStopped_=true;if(probe_.joinable())probe_.join();}
    auto operation=std::make_shared<ConsoleOperation>();operation->key=key;operation->plan=std::move(plan);operation->deadline=streamMonotonicMillis()+15000;
    {std::lock_guard lock(consoleMutex_);consoleOperation_=operation;consoleRunning_=true;}
    consoleWorker_=std::thread([this,operation,send=std::move(send),activity=std::move(activity)]()mutable{
      bool ok=false,attempted=false,committed=false;DWORD systemError=0;std::string reason="worker_failed";
      const auto current=[&]{return !operation->cancelled&&streamMonotonicMillis()<operation->deadline;};
      try{
        UserWorker child(operation->plan.source.session,operation->cancelled);std::mutex writing;
        const auto write=[&](const std::string& bytes){return pipePacket(child.pipe.value,true,{bytes.begin(),bytes.end()},operation->cancelled,writing);};
        const auto read=[&](const char* type){
          bool text=false;std::vector<uint8_t> bytes;const auto remaining=operation->deadline-streamMonotonicMillis();
          if(remaining<=0||!readPipePacket(child.pipe.value,text,bytes,operation->cancelled,std::chrono::milliseconds(remaining))||!text||bytes.size()>4096)throw std::runtime_error("console_timeout");
          const auto value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));
          const auto code=value.get("systemError").number(-1);
          if(value.get("type").string()!=type||!operation->key.matches(value)||!std::holds_alternative<bool>(value.get("ok").value)||!std::holds_alternative<bool>(value.get("attempted").value)||!std::isfinite(code)||code<0||code>UINT32_MAX||std::floor(code)!=code||(value.get("ok").boolean()&&code))throw std::runtime_error("worker_failed");return value;
        };
        const auto prepare="{\"type\":\"worker.console.prepare\","+displayOperationFields(operation->key)+",\"sourceBinding\":"+streamConsoleBindingJson(operation->plan.source)+",\"consoleSessionId\":"+std::to_string(operation->plan.console)+",\"workerDeadlineMs\":"+std::to_string(operation->deadline)+'}';
        if(!current()||!write(prepare))throw std::runtime_error("operation_cancelled");
        const auto prepared=read("worker.console.prepared");
        systemError=DWORD(prepared.get("systemError").number());if(!prepared.get("ok").boolean())throw std::runtime_error(safeStreamConsoleReason(prepared.get("reason").string()));
        if(!current())throw std::runtime_error("operation_cancelled");
        const auto fresh=planStreamConsoleRecovery(observeStreamConsoleRecovery());
        if(!sameStreamConsolePlan(operation->plan,fresh))throw std::runtime_error("console_binding_changed");
        const auto commit="{\"type\":\"worker.console.apply\","+displayOperationFields(operation->key)+'}';
        committed=true;if(!write(commit))throw std::runtime_error("worker_failed");
        const auto result=read("worker.console.result");attempted=result.get("attempted").boolean();systemError=DWORD(result.get("systemError").number());reason=safeStreamConsoleReason(result.get("reason").string());
        // Verify the same logon on the console. A successful API return is not
        // proof that its desktop is unlocked or available for capture.
        while(current()){
          const auto after=planStreamConsoleRecovery(observeStreamConsoleRecovery());
          if(after.ready&&sameStreamConsoleBinding(after.source,operation->plan.source)){ok=true;reason.clear();break;}
          if(after.reason=="windows_session_locked"||!result.get("ok").boolean()){reason=after.reason=="windows_session_locked"?after.reason:reason;break;}
          Sleep(100);
        }
        if(!ok&&reason.empty())reason="console_transfer_pending";
      }catch(const WindowsOperationError& error){attempted=attempted||committed;systemError=error.systemError();reason="worker_failed";}
      catch(const std::exception& error){attempted=attempted||committed;reason=safeStreamConsoleReason(error.what());}
      catch(...){attempted=attempted||committed;reason="worker_failed";}
      if(operation->cancelled){ok=false;reason="operation_cancelled";}
      invalidateDisplay();activity.reset();consoleRunning_=false;readinessChanged_=true;
      const auto bytes="{\"type\":\"host.console.result\","+displayOperationFields(operation->key)+",\"ok\":"+(ok?"true":"false")+",\"attempted\":"+(attempted?"true":"false")+",\"reason\":"+jsonQuote(ok?"":safeStreamConsoleReason(reason))+",\"systemError\":"+std::to_string(ok?0:systemError)+'}';
      try{send(true,{bytes.begin(),bytes.end()});}catch(...){ }
    });
  }
  void start(const Json& request, Send send) override {
    const auto received=ConnectStages::Clock::now();
    std::lock_guard lifecycle(lifecycleMutex_);
    if(signin::broker::SigninBridge::requested(request)){
      // hostAction 293: the service refused a sign-in start before admission.
      // Bits: 1 no sign-in runtime, 2 previous worker not quiescent, 4 a stream
      // is running, 8 private diagnostic busy.
      if(!signinRuntime_||!workerQuiescentNow()||(!stopped_&&streaming_)||privateDiagnosticBusy()){recordHostEvent("hostAction",{{"action",293},{"ok",false},{"error",int64_t((!signinRuntime_?1:0)|(!workerQuiescentNow()?2:0)|((!stopped_&&streaming_)?4:0)|(privateDiagnosticBusy()?8:0))}});const auto denied="{\"type\":\"stream.unavailable\",\"sessionId\":"+jsonQuote(request.get("sessionId").string())+",\"reason\":\"signin_stream_unavailable\"}";send(true,{denied.begin(),denied.end()});return;}
      const auto credentials=credentials_;auto* maintenance=maintenance_;auto* display=nativeDisplay_;
      const bool accepted=signinRuntime_->start(request,send,[credentials,maintenance,display]{auto inner=std::make_unique<ServiceBridge>(credentials,maintenance,display);inner->observeSigninWorker();auto* exact=inner.get();signin::broker::SigninBridge::DesktopLane lane;lane.workerQuery=[exact]{return exact->signinWorkerQuery();};lane.fullOriginalRestored=[exact]{return exact->signinOriginalRestored();};lane.bridge=std::move(inner);return lane;});
      if(accepted){ownsSignin_=true;signinOwnedSession_=request.get("sessionId").string();return;}const auto denied="{\"type\":\"stream.unavailable\",\"sessionId\":"+jsonQuote(request.get("sessionId").string())+",\"reason\":\"signin_stream_unavailable\"}";send(true,{denied.begin(),denied.end()});return;
    }
    // The Host has no RDP. A broker or client that still asks is refused before
    // anything stops or starts, with the reason released clients already
    // explain; Windows' own Remote Desktop settings are left as they are.
    if(request.get("mode").string()=="rdp"){const auto bytes="{\"type\":\"session.unavailable\",\"sessionId\":"+jsonQuote(request.get("sessionId").string())+",\"reason\":\"rdp_not_supported\"}";send(true,{bytes.begin(),bytes.end()});return;}
    if(privateDiagnosticBusy()){const auto bytes="{\"type\":\"stream.unavailable\",\"sessionId\":"+jsonQuote(request.get("sessionId").string())+",\"reason\":\"host_maintenance\"}";send(true,{bytes.begin(),bytes.end()});return;}
    if(!qemuConsoleQuiescent()||consoleRunning_){const auto bytes=std::string("{\"type\":\"stream.unavailable\"")+",\"sessionId\":"+jsonQuote(request.get("sessionId").string())+",\"reason\":\"host_maintenance\"}";send(true,{bytes.begin(),bytes.end()});return;}
#ifdef MULTITAP_SESSION_STOP_TEST
    if(qemuAdmissionModel_){++qemuStreamAdmissions_;return;}
#endif
    rejectedStream_=request.get("sessionId").string();
    if(profileRecoveryRunning_){recordHostEvent("hostAction",{{"action",uint32_t(220)},{"stage",uint32_t(4)},{"ok",false}});const auto message=std::string("{\"type\":\"stream.unavailable\"")+",\"sessionId\":"+jsonQuote(request.get("sessionId").string())+",\"reason\":\"host_maintenance\"}";send(true,{message.begin(),message.end()});return;}
    auto activity=maintenance_?maintenance_->reserveSession():std::shared_ptr<CatalogActivity>{};
    if(maintenance_&&!activity){recordHostEvent("hostAction",{{"action",uint32_t(220)},{"stage",uint32_t(5)},{"ok",false}});const auto message=std::string("{\"type\":\"stream.unavailable\"")+",\"sessionId\":"+jsonQuote(request.get("sessionId").string())+",\"reason\":\"host_maintenance\"}";send(true,{message.begin(),message.end()});return;}
    stop();
    if(!workerQuiescentNow()){recordHostEvent("hostAction",{{"action",uint32_t(220)},{"stage",uint32_t(3)},{"ok",false}});const auto message=std::string("{\"type\":\"stream.unavailable\"")+",\"sessionId\":"+jsonQuote(request.get("sessionId").string())+",\"reason\":\"host_maintenance\"}";send(true,{message.begin(),message.end()});return;}
    std::unique_lock<std::mutex> nativeProbeLock(probeMutex_,std::defer_lock);bool nativeReady=true;std::string profileLease;Json profileSnapshot;
    if(nativeDisplaySelected()){
      nativeProbeLock.lock();probeStopped_=true;if(probe_.joinable())probe_.join();
      try{const auto snapshot=nativeDisplayLeaseSnapshot();profileSnapshot=snapshot;profileLease=nativeDisplayLeaseKey(snapshot);nativeReady=nativeActivationPublished(snapshot)&&nativeActivationGate_.readinessAllowed(std::to_string(interactiveSession())+":"+profileLease);}catch(...){nativeReady=false;}
    }
    pendingBitrate_=0;pendingScale_=0;pendingCaret_=0;pendingCursorEcho_=0;pendingCursorCompose_=0;pendingKeyFrame_=false;pendingCursorResend_=false;pendingRecover_=0;send_ = std::move(send); id_ = request.get("sessionId").string(); streaming_ = true;sessionModeIsStream_=streaming_;rejectedStream_.reset();workerCreatedForSession_=false;sessionHadProfile_=false;v2Session_=false;nativeStopGuard_=sessionModeIsStream_&&nativeDisplaySelected();nativeStopInstance_=profileSnapshot.get("deviceInstanceId").string();if(sessionModeIsStream_)lastSessionStop_.reset();epochMedia_=request.get("resizeProtocol").string()=="epoch-v1";streamSizesV2_=streamSizeV2(request);streamMonitor_=request.get("monitor").string();currentEpoch_=0;
    sessionActivity_=std::move(activity);
    try{pendingEncoderLimits_.reset();encoderLimits_=viewportEncoderLimits(request);if(encoderLimits_&&!hasInitialViewport(request))throw std::runtime_error("initial_viewport_invalid");}catch(...){unavailable("virtual_display_required");return;}
    if(!nativeReady){unavailable("virtual_display_required");return;}
    session_ = interactiveSession(); if (!sessionUnlocked(session_)) { unavailable("no_interactive_session"); return; }
    const bool wantsProfile=!std::holds_alternative<std::nullptr_t>(request.get("consoleProfile").value),initial=hasInitialViewport(request),trackSession=request.get("mediaProtocol").string()=="tracks-v1",initialGroup=hasInitialProfileTracks(request);
    // Display manager v2 handles every native stream: single output (initial
    // viewport or current geometry) and independent outputs (tracks-v1, the
    // client's "모든 모니터"), with or without a console proof. The host config
    // displayManager "v1" keeps the old transaction path for both.
    const bool v2=v2Mode()&&nativeDisplaySelected();
    v2Session_=v2;sessionHadProfile_=v2&&wantsProfile;
    profileSession_=false;
    try{
      if(wantsProfile){(void)nativeStreamProfileRequest(request,initialGroup);if(!nativeDisplay_||!nativeDisplaySelected())throw std::runtime_error("native_stream_profile_pending");}
      if(initialGroup){std::lock_guard lock(resizeMutex_);if(!wantsProfile||!nativeDisplay_||!sameInteractiveConsole(session_)||!initialGroupCompletion_.arm(request,initialProfileTracksEnabled(nativeDisplayConfig(),nativeDisplaySelected()),nativeDisplay_->hasStreamProfileReservation()))throw std::runtime_error("initial_profile_tracks_invalid");profileInstanceForInitial_=profileSnapshot.get("deviceInstanceId").string();}
      if(initial){std::lock_guard lock(resizeMutex_);if(!nativeDisplay_||!nativeDisplaySelected()||!epochMedia_||trackSession||nativeDisplay_->hasStreamProfileReservation()||!sameInteractiveConsole(session_)||!initialCompletion_.arm(request,true,false,wantsProfile))throw std::runtime_error("initial_viewport_invalid");profileInstanceForInitial_=profileSnapshot.get("deviceInstanceId").string();pendingResize_=*initialCompletion_.request();resizeTicket_=displayCache_.begin(session_);resizeDeadline_=streamMonotonicMillis()+streamResizeTimeoutMs;}
    }catch(...){unavailable("virtual_display_required");return;}
    stopping_=false;stopFence_.reset(id_,sessionHadProfile_);
    stopped_ = false;
    if(trackSession){
      try{std::lock_guard lock(resizeMutex_);currentTrack_=nativeInitialTrackViewport(request);pendingTrack_=currentTrack_;trackMedia_=initialTrack_=true;trackMetadataReady_=trackResizeVerified_=false;resizeTicket_=displayCache_.begin(session_);resizeDeadline_=streamMonotonicMillis()+streamResizeTimeoutMs;if(v2)profileInstanceForInitial_=profileSnapshot.get("deviceInstanceId").string();}catch(...){stopped_=true;streaming_=false;unavailable("worker_failed");return;}
      try{tracks_.start(credentials_,id_,request.get("tunnelUrl").string(),
        [this,expected=id_](const std::string& message){
          trackControl(message,expected);
        },[this]{if(auto udp=udpTracks_.load())udp->suspend();if(!stopped_.exchange(true)){if(const auto pipe=pipe_.load())CancelIoEx(pipe,nullptr);unavailable("worker_failed");}},&request,
        // tracks-resume-v1 (track_tunnel.hpp): the worker keeps streaming through a
        // lost tunnel; nothing the client held stays pressed meanwhile.
        request.get("trackResumeProtocol").string()==trackResumeProtocol,
        [this,expected=id_]{if(auto udp=udpTracks_.load())udp->suspend();trackControl("{\"type\":\"input.reset\",\"sessionId\":"+jsonQuote(expected)+'}',expected);});
      }catch(...){stopped_=true;streaming_=false;unavailable("worker_failed");return;}
    }
    if(trackSession&&request.get("udpTracksProtocol").string()==udpTracksProtocol){
      try{udpTracks_.store(std::make_shared<UdpTracksSession>(id_,UdpTracksSession::Callbacks{
        [this](const std::string& value){return tracks_.send(true,{value.begin(),value.end()});},
        [this,expected=id_](const Json& value){trackControl(jsonObject(value),expected,true);},
        [this]{pendingKeyFrame_=true;SetEvent(relayWake_.value);}},relativeMouseTrackSessionRequested(request)));}catch(...){ }
    }
    const auto encoderTicket=encoderDiagnostics_.begin();
    workerQuiescent_=false;worker_ = std::thread([this, request,trackSession,wantsProfile,initial,initialGroup,v2,encoderTicket,startLease=profileLease,startSnapshot=profileSnapshot,received] {
      struct UdpWorkerExit{std::shared_ptr<UdpTracksSession> session;~UdpWorkerExit(){if(session)session->close();}}udpExit{udpTracks_.load()};
      // connect_stages.hpp: where this start's time goes (diagnostics only).
      ConnectStages stages;stages.received=received;stages.admitted=ConnectStages::Clock::now();bool firstVideo=false;
      // This thread relays the stream (worker pipe -> send queue): keep the
      // relay ahead of normal-priority applications while it runs.
      StreamPriority streamPriority;
      bool workerExited=false,startDispatched=false,workerCreated=false,initialFallback=false,initialSdkUnsupported=false;uint32_t prepareStage=1;HANDLE unexitedWorker=nullptr;
      const bool trackCursor=trackSession&&cursor::trackSessionRequested(request),trackOpus=trackSession&&trackAudioOpusGranted(request);
      WorkerRelayFacts relayFacts;OwnedWorkerExit workerExit;DisplayV2TraceCollector v2Trace;
      const auto consumeV2Trace=[&](Json& value){consumeDisplayV2Trace(value,v2,id_,v2Trace,[](auto values){recordHostEvent("hostAction",values);});};
      const auto preparationLog=[](bool ok,uint32_t stage,uint32_t error=0){recordHostEvent("hostAction",{{"action",uint32_t(200)},{"ok",ok},{"stage",stage},{"error",error}});};
      if(trackSession){
        // Independent outputs: output 0's size and the requested output count.
        uint32_t width=0,height=0,outputs=0;try{const auto tracks=nativeInitialTrackViewport(request);width=tracks.outputs.front().width;height=tracks.outputs.front().height;outputs=uint32_t(tracks.outputs.size());}catch(...){ }
        recordHostEvent("hostAction",{{"action",uint32_t(200)},{"ok",true},{"stage",uint32_t(1)},{"width",width},{"height",height},{"monitors",outputs}});
      }else recordHostEvent("hostAction",{{"action",uint32_t(200)},{"ok",true},{"stage",uint32_t(1)},{"width",uint32_t(std::clamp((initial?request.get("initialViewport"):request).get("width").number(),0.0,16384.0))},{"height",uint32_t(std::clamp((initial?request.get("initialViewport"):request).get("height").number(),0.0,16384.0))}});
      // hostAction 275 (process_exit_record.hpp) after the relay record: a session
      // worker that ended by itself without a stop, or crashed, with its code and run time.
      struct Finished{ServiceBridge* bridge;EncoderDiagnosticCache::Ticket encoderTicket;bool& exited;bool& dispatched;bool& created;HANDLE& unexited;WorkerRelayFacts& relayFacts;OwnedWorkerExit& workerExit;DisplayV2TraceCollector& trace;~Finished(){try{bridge->encoderDiagnostics_.retire(encoderTicket);}catch(...){ }bridge->completeProfileWorkerExit(exited,dispatched,created,std::exchange(unexited,nullptr));emitWorkerRelayDiagnostic(relayFacts,[](auto values){recordHostEvent("hostAction",values);});if(const auto record=sessionWorkerExit(workerExit,relayFacts.requested))recordProcessExit(*record);trace.finished([](auto values){recordHostEvent("hostAction",values);});}}finished{this,encoderTicket,workerExited,startDispatched,workerCreated,unexitedWorker,relayFacts,workerExit,v2Trace};
      try {
        if(stopped_){relayFacts.ended(WorkerRelayCause::stopRequested);relayFacts.requested=true;if(trackSession)tracks_.stop();return;}
        if(trackSession&&!tracks_.waitReady(15000)){relayFacts.ended(WorkerRelayCause::scopeExit);relayFacts.requested=stopped_||stopping_;tracks_.stop();stopped_=true;streaming_=false;unavailable("worker_failed");return;}
        UserWorker child(session_,stopped_,&workerExited,&workerCreated,&unexitedWorker,&workerExit);const HANDLE pipe=child.pipe.value;pipe_=pipe;prepareStage=2;
        if(signinWorkerObservation_){HANDLE query=nullptr;if(!DuplicateHandle(GetCurrentProcess(),child.process.value,GetCurrentProcess(),&query,PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,0))throw std::runtime_error("signin_worker_identity_unavailable");std::lock_guard held(signinWorkerMutex_);signinWorkerQuery_=WinHandle(query);}stages.worker=ConnectStages::Clock::now();
        struct SourceGuard{TrackSourceTunnel* source;~SourceGuard(){if(source)source->stop();}}sourceGuard{trackSession?&tracks_:nullptr};
        WorkerRelayProbe relayProbe(relayFacts,child.process.value,stopped_,stopping_);
        // A v2 tracks start sends the broker's command unchanged (phase two must
        // equal what the worker prepared); the v1 path carries its deadline.
        auto command=request;if(trackSession&&!initialGroup&&!v2)streamField(command,"workerDeadlineMs",double(resizeDeadline_.load()));
        if(v2){
          // v2 start in the interactive worker: snapshot -> owned size(s) ->
          // owned primary ("1" at the client's place for tracks) -> identified
          // console off -> verify. Its only service calls are the SDK size RPCs
          // answered below. No v1 journals, reservation or launch receipt.
          prepareStage=3;v2Rpc_=true;
          if(!initial&&!trackSession)resizeDeadline_=streamMonotonicMillis()+streamResizeTimeoutMs;
          const auto prepare="{\"type\":"+jsonQuote(trackSession?"worker.v2.tracks.start":"worker.v2.start")+",\"sourceCommand\":"+jsonObject(request)+",\"workerDeadlineMs\":"+std::to_string(resizeDeadline_.load())+'}';
          requireWorker(pipePacket(pipe,true,{prepare.begin(),prepare.end()},stopped_,writeMutex_,&resizeDeadline_),"Cannot prepare display v2");
          bool prepared=false,text=false;std::vector<uint8_t> bytes;
          while(!stopped_&&!stopping_&&readPipePacket(pipe,text,bytes,stopped_,std::chrono::seconds(15),&resizeDeadline_)){
            if(!text||bytes.size()>65536)break;auto value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));
            consumeV2Trace(value);
            const auto sized=displayV2Packet(value,v2Rpc_.load(),session_);if(sized.handled){if(sized.response.empty()||!pipePacket(pipe,true,{sized.response.begin(),sized.response.end()},stopped_,writeMutex_,&resizeDeadline_))break;continue;}
            if(value.get("type").string()=="worker.stream.failure"){const auto diagnostic=streamPreparationObservation(value,id_);if(diagnostic){preparationLog(streamObservationOk(diagnostic->first,diagnostic->second),200+diagnostic->first,diagnostic->second);observeGpuPath(diagnostic->first,diagnostic->second);}continue;}
            if(trackSession){
              // Fixed codes only: hostAction 210 (as a single-output start),
              // 200 stage 100+v2 stage, and 214 for the restore of a failure.
              const auto started=parseDisplayV2TracksStarted(value,id_);if(!started)break;
              recordHostEvent("hostAction",{{"action",uint32_t(210)},{"ok",started->ok},{"stage",started->stage},{"error",started->error},{"changed",started->consoleRemoved},
                {"recovery",started->consoleProof},{"active",started->consoleIdentified},{"monitors",started->foreignCount}});
              preparationLog(started->ok,100+started->stage,started->error);
              if(started->restoreAttempted)recordHostEvent("hostAction",{{"action",uint32_t(214)},{"ok",started->restored},{"stage",started->restoreStage},{"changed",false}});
              if(!started->ok){std::lock_guard lock(resizeMutex_);const bool current=!stopped_&&!stopping_&&request.get("sessionId").string()==id_&&pendingTrack_&&pendingTrack_->sessionId==id_&&pendingTrack_->topologyEpoch==request.get("topologyEpoch").number(-1)&&trackMedia_&&sameInteractiveConsole(session_)&&sessionUnlocked(session_)&&interactiveSession()==session_;initialSdkUnsupported=streamInitialSdkUnsupported(streamSizeV2(request),current,true,started->stage,started->error);break;}
              std::lock_guard lock(resizeMutex_);
              const bool eligible=!stopped_&&!stopping_&&pendingTrack_&&trackMedia_&&started->desktopAvailable&&sameInteractiveConsole(session_)&&sessionUnlocked(session_)&&interactiveSession()==session_;
              if(!eligible||!initialProfileTrackV2Matches(started->display,*pendingTrack_,nativeDisplayLeaseSnapshot(),GetCurrentProcessId(),profileInstanceForInitial_))break;
              const auto publish=[&](const DisplayState& display){return displayCache_.complete(resizeTicket_,display,Clock::now(),true);};
              if(initialGroup?!initialGroupCompletion_.acceptV2Worker(started->display,eligible,publish):!publish(started->display))break;
              readinessChanged_=true;trackResizeVerified_=true;prepared=true;break;
            }
            const auto stage=value.get("nativeStage").number(-1),error=value.get("nativeError").number(-1);
            if(!nativeDisplayInteger(stage,0,9)||!nativeDisplayInteger(error,0,UINT32_MAX))break;
            // recovery = console proof supplied, active = console positively
            // identified, monitors = active other displays before the change.
            recordHostEvent("hostAction",{{"action",uint32_t(210)},{"ok",value.get("ok").boolean()},{"stage",uint32_t(stage)},{"error",uint32_t(error)},{"changed",value.get("consoleRemoved").boolean()},
              {"recovery",value.get("consoleProof").boolean()},{"active",value.get("consoleIdentified").boolean()},{"monitors",uint32_t(std::clamp(value.get("foreignCount").number(),0.0,64.0))}});
            if(initial){
              std::lock_guard lock(resizeMutex_);const bool eligible=!stopped_&&!stopping_&&pendingResize_&&sameInteractiveConsole(session_)&&sessionUnlocked(session_)&&interactiveSession()==session_&&epochMedia_;
              if(value.get("type").string()!="worker.initial.result"||!pendingResize_||!pendingResize_->matches(value))break;
              preparationLog(value.get("ok").boolean(),100+uint32_t(stage),uint32_t(error));
              if(!value.get("ok").boolean()){const bool failed=std::holds_alternative<bool>(value.get("ok").value);initialSdkUnsupported=streamInitialSdkUnsupported(streamSizeV2(request),eligible&&request.get("sessionId").string()==id_&&pendingResize_->sessionId==id_,failed,uint32_t(stage),uint32_t(error));break;}
              if(!initialCompletion_.acceptWorker(value,eligible,[&](const DisplayState& display){
                // The service's own lease must already carry the requested size.
                try{const auto targets=nativeDisplayTargets(JsonParser::parse(nativeDisplay_->viewportSnapshot()));if(targets.front().id!="0"||targets.front().width!=DWORD(pendingResize_->width)||targets.front().height!=DWORD(pendingResize_->height))return false;}catch(...){return false;}
                return displayCache_.complete(resizeTicket_,display,Clock::now(),true);
              }))break;
              command=*initialCompletion_.preparedCommand();prepared=true;break;
            }
            if(value.get("type").string()!="worker.v2.started"||value.get("sessionId").string()!=id_||!std::holds_alternative<bool>(value.get("ok").value))break;
            preparationLog(value.get("ok").boolean(),100+uint32_t(stage),uint32_t(error));
            if(!value.get("ok").boolean()){const bool current=!stopped_&&!stopping_&&request.get("sessionId").string()==id_&&sameInteractiveConsole(session_)&&sessionUnlocked(session_)&&interactiveSession()==session_;initialSdkUnsupported=streamInitialSdkUnsupported(streamSizeV2(request),current,true,uint32_t(stage),uint32_t(error));break;}
            const auto ticket=displayCache_.begin(session_);displayCache_.complete(ticket,parseDisplayState(value.get("display")),Clock::now(),value.get("desktopAvailable").boolean());readinessChanged_=true;
            prepared=true;break;
          }
          // A tracks start keeps its deadline until the first metadata arrives.
          if(!initial&&!trackSession)resizeDeadline_=0;
          if(!prepared||stopped_||stopping_)throw std::runtime_error("display_v2_start_failed");prepareStage=4;
        }else if(initial){
          prepareStage=3;
          const auto prepare="{\"type\":\"worker.initial.prepare\",\"sourceCommand\":"+jsonObject(request)+",\"workerDeadlineMs\":"+std::to_string(resizeDeadline_.load())+'}';
          requireWorker(pipePacket(pipe,true,{prepare.begin(),prepare.end()},stopped_,writeMutex_,&resizeDeadline_),"Cannot prepare initial viewport");
          bool prepared=false,text=false;std::vector<uint8_t> bytes;
          while(!stopped_&&!stopping_&&readPipePacket(pipe,text,bytes,stopped_,std::chrono::seconds(15),&resizeDeadline_)){
            if(!text||bytes.size()>65536)break;const auto value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));
            const auto native=nativeViewportPacket(value);if(native.handled){if(native.response.empty()||!pipePacket(pipe,true,{native.response.begin(),native.response.end()},stopped_,writeMutex_,&resizeDeadline_))break;continue;}
            std::lock_guard lock(resizeMutex_);const bool eligible=!stopped_&&!stopping_&&pendingResize_&&sameInteractiveConsole(session_)&&sessionUnlocked(session_)&&interactiveSession()==session_&&epochMedia_;
            if(value.get("type").string()!="worker.initial.result"||!pendingResize_||!pendingResize_->matches(value))break;
            const auto nativeStage=value.get("nativeStage").number(-1),nativeError=value.get("nativeError").number(-1);
            if(!std::isfinite(nativeStage)||std::floor(nativeStage)!=nativeStage||nativeStage<0||nativeStage>9||!std::isfinite(nativeError)||std::floor(nativeError)!=nativeError||nativeError<0||nativeError>UINT32_MAX||(value.get("ok").boolean()&&(nativeStage||nativeError)))break;
            preparationLog(value.get("ok").boolean(),100+uint32_t(nativeStage),uint32_t(nativeError));
            if(!value.get("ok").boolean()){
              // Fixed pre-change site code (stage 300+nativeReason) for diagnostics.
              const auto nativeReason=value.get("nativeReason").number(0);
              if(nativeDisplayInteger(nativeReason,1,255))recordHostEvent("hostAction",{{"action",uint32_t(200)},{"stage",300+uint32_t(nativeReason)},{"error",uint32_t(nativeError)},{"ok",false}});
              // Nothing changed: degrade the advertised capability for this exact
              // lease so the next connection streams the current geometry. This
              // attempt still fails; the released broker requires its size.
              try{if(eligible&&nativeDisplay_&&!nativeDisplay_->hasStreamProfileReservation())if(const auto fallback=initialViewportFallbackAdmitted(value,session_,startLease,nativeDisplayLeaseSnapshot(),startSnapshot)){initialFallback_.arm(*fallback);initialFallback=true;recordHostEvent("hostAction",{{"action",uint32_t(203)},{"stage",fallback->stage},{"error",fallback->reason},{"ok",true}});}}catch(...){ }
              break;
            }
            if(!initialCompletion_.acceptWorker(value,eligible,[&](const DisplayState& display){
              const auto snapshot=nativeDisplayLeaseSnapshot();const auto lease=nativeStreamProfileLease(snapshot,GetCurrentProcessId());
              if(!nativeDisplay_||nativeDisplay_->hasStreamProfileReservation()||snapshot.get("deviceInstanceId").string()!=profileInstanceForInitial_||lease.targets.size()!=1||lease.targets[0].id!="0"||lease.targets[0].width!=DWORD(pendingResize_->width)||lease.targets[0].height!=DWORD(pendingResize_->height))return false;
              return displayCache_.complete(resizeTicket_,display,Clock::now(),true);
            }))break;
            command=*initialCompletion_.preparedCommand();prepared=true;break;
          }
          if(!prepared||stopped_||stopping_)throw std::runtime_error("initial_viewport_failed");prepareStage=4;
        }
        if(initialGroup&&!v2){
          prepareStage=15;
          const auto prepare="{\"type\":\"worker.initial.tracks.prepare\",\"sourceCommand\":"+jsonObject(request)+",\"workerDeadlineMs\":"+std::to_string(resizeDeadline_.load())+'}';
          requireWorker(pipePacket(pipe,true,{prepare.begin(),prepare.end()},stopped_,writeMutex_,&resizeDeadline_),"Cannot prepare initial group viewport");
          bool prepared=false,text=false;std::vector<uint8_t> bytes;
          while(!stopped_&&!stopping_&&readPipePacket(pipe,text,bytes,stopped_,std::chrono::seconds(15),&resizeDeadline_)){
            if(!text||bytes.size()>65536)break;const auto value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));
            const auto native=nativeTrackViewportPacket(value);if(native.handled){if(native.response.empty()||!pipePacket(pipe,true,{native.response.begin(),native.response.end()},stopped_,writeMutex_,&resizeDeadline_))break;continue;}
            // The worker's fixed failure code (stage 18 -> 218) precedes its result.
            if(value.get("type").string()=="worker.stream.failure"){const auto diagnostic=streamPreparationObservation(value,id_);if(diagnostic){preparationLog(streamObservationOk(diagnostic->first,diagnostic->second),200+diagnostic->first,diagnostic->second);observeGpuPath(diagnostic->first,diagnostic->second);}continue;}
            std::lock_guard lock(resizeMutex_);const bool eligible=!stopped_&&!stopping_&&pendingTrack_&&sameInteractiveConsole(session_)&&sessionUnlocked(session_)&&interactiveSession()==session_&&trackMedia_;
            if(!initialGroupCompletion_.acceptWorker(value,eligible,[&](const DisplayState& display){
              if(!nativeDisplay_||nativeDisplay_->hasStreamProfileReservation()||!initialProfileTrackLeaseMatches(nativeDisplayLeaseSnapshot(),display,*pendingTrack_,GetCurrentProcessId(),profileInstanceForInitial_))return false;
              return displayCache_.complete(resizeTicket_,display,Clock::now(),true);
            }))break;
            command=*initialGroupCompletion_.preparedCommand();trackResizeVerified_=true;prepared=true;preparationLog(true,prepareStage);break;
          }
          if(!prepared||stopped_||stopping_)throw std::runtime_error("initial_profile_tracks_failed");
        }
        if(wantsProfile&&!v2){
          prepareStage=5;
          const auto snapshot=nativeDisplayLeaseSnapshot();
          if(initialGroup){std::lock_guard lock(resizeMutex_);if(!pendingTrack_||!initialGroupCompletion_.workerVerified()||!initialProfileTrackLeaseMatches(snapshot,displayCache_.snapshot(session_,sessionUnlocked(session_)),*pendingTrack_,GetCurrentProcessId(),profileInstanceForInitial_))throw std::runtime_error("initial_profile_tracks_failed");}
          profileInstance_=snapshot.get("deviceInstanceId").string();profileGeneration_=nativeStreamProfileLease(snapshot,GetCurrentProcessId()).generation;profileOperation_=nativeBaselineDigest(utf8(randomName())+id_);
          if(!nativeDisplay_||stopped_||stopping_||!nativeDisplay_->reserveStreamProfile(id_,nativeDisplayLeaseKey(snapshot)))throw std::runtime_error("native_stream_profile_pending");
          profileSession_=true;sessionHadProfile_=true;stopFence_.reset(id_,true);
        }
        if(profileSession_){
          prepareStage=6;
          // The child is authenticated and waiting on its private pipe. Persist
          // its exact lifetime and SDK output mapping before allowing mutation.
          ServiceHandle manager(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT));ServiceHandle service(OpenServiceW(manager.value,L"MultitapHost",SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG));
          SERVICE_STATUS_PROCESS state{};DWORD needed=0;const auto executable=brokerServiceExecutable(service.value);
          if(!QueryServiceStatusEx(service.value,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&state),sizeof(state),&needed)||state.dwProcessId!=GetCurrentProcessId()||state.dwCurrentState!=SERVICE_RUNNING||!brokerSystemProcess(GetCurrentProcess())||!brokerProcessExecutable(GetCurrentProcess(),executable)||!brokerProcessExecutable(child.process.value,executable))throw std::runtime_error("native_stream_launch_identity_unavailable");
          NativeStreamLaunchReceipt launch;launch.key={id_,profileOperation_,profileInstance_,session_};launch.entry.generation=profileGeneration_;launch.service=nativeStreamProcessIdentity(GetCurrentProcess());launch.worker=nativeStreamProcessIdentity(child.process.value);
          const auto login=nativeSessionRecoveryScope(session_);HANDLE raw=nullptr;if(!OpenProcessToken(child.process.value,TOKEN_QUERY,&raw))throw std::runtime_error("native_stream_launch_identity_unavailable");WinHandle token(raw);launch.scope=nativeTokenRecoveryScope(token.value,session_);
          if(!nativeRecoveryLogonMatches(login,launch.scope)||WaitForSingleObject(child.process.value,0)!=WAIT_TIMEOUT)throw std::runtime_error("native_stream_launch_identity_unavailable");
          const auto digest=nativeDisplay_->saveStreamProfileLaunch(std::move(launch));if(!digest)throw std::runtime_error("native_stream_launch_save_failed");
          streamField(command,"profileOperation",profileOperation_);streamField(command,"profileLaunchSha256",*digest);
        }
        const auto commandText = jsonObject(command);prepareStage=7;startDispatched=true;stages.dispatched=ConnectStages::Clock::now();requireWorker(pipePacket(pipe, true, {commandText.begin(), commandText.end()}, stopped_, writeMutex_), "Cannot start worker session");
        if(stopping_){const auto stop="{\"type\":\"session.stop\",\"sessionId\":"+jsonQuote(id_)+'}';requireWorker(pipePacket(pipe,true,{stop.begin(),stop.end()},stopped_,writeMutex_),"Cannot stop starting worker");}
        // channel-resume-v1 (channel_redial.hpp): before the channels start, so no end of theirs is missed. Only a
        // session whose Broker named the protocol dials print, drives and USB again; the clipboard channel does by itself.
        channelRedials(!stopped_&&request.get("channelResumeProtocol").string()==channelResumeProtocol);
        if(!stopped_&&!request.get("clipboardUrl").string().empty())try{clipboard_.start(credentials_,id_,request.get("clipboardUrl").string(),[this,expected=id_](const std::string& payload){const auto pipe=pipe_.load();if(!pipe||stopped_)return;const auto packet="{\"type\":\"clip.packet\",\"sessionId\":"+jsonQuote(expected)+",\"generation\":"+std::to_string(clipboard_.generation())+",\"payload\":"+payload+'}';pipePacket(pipe,true,{packet.begin(),packet.end()},stopped_,writeMutex_);});}catch(...){/* The clipboard channel did not start: record it (hostAction 253, stage watchFailed) instead of losing it. */recordHostEvent("hostAction",{{"action",clipboardDiagnosticAction},{"stage",ClipboardDiagnostic::watchFailed},{"ok",0}});}
        // print-v1: the broker granted this session's printers permission.
        if(!stopped_){const auto printUrl=printSessionUrl(request,id_);if(!printUrl.empty())try{print_.start(credentials_,id_,printUrl,session_);}catch(...){recordPrintEvent(PrintStage::channelClosed,false);}}
        // drive-v1: only a session the broker granted it (its own channel; file data never uses this pipe).
        if(!stopped_&&request.get("driveProtocol").string()==drive::protocol&&!request.get("driveUrl").string().empty())try{drives_.start(credentials_,id_,request.get("driveUrl").string(),session_,request.get("driveFs").boolean());}catch(...){}
        // usb-v1: the broker granted this session's usbDevices permission (the devices attach to the VM, not to this Windows user).
        if(!stopped_){const auto usbUrl=usbSessionUrl(request,id_);if(!usbUrl.empty())try{usb_.start(credentials_,id_,usbUrl,usbSessionGrant(request));}catch(...){recordUsbEvent(UsbStage::channelClosed,false);}}
        bool text = false, terminal = false,relayRead=false; auto lastVideo=Clock::now();std::vector<uint8_t> bytes;
        // Role::secure (secure_desktop_service.hpp): while a UAC prompt of this unlocked session shows, the user
        // worker asks this service to launch the Winlogon-desktop helper. Every worker.secure.* message is
        // consumed below and never reaches the broker; the client's input reaches the helper from control(), never
        // through the worker. The helper never outlives this relay: the guard ends it on every way out.
        secure::ServiceSecure::Env secureEnv;
        secureEnv.eligible=[&]{std::lock_guard lock(resizeMutex_);return !stopped_&&!stopping_&&streaming_&&!pendingResize_&&!pendingTrack_&&!initialCompletion_.pending()&&sameInteractiveConsole(session_)&&sessionUnlocked(session_)&&WaitForSingleObject(child.process.value,0)==WAIT_TIMEOUT;};
        secureEnv.launch=[&](const secure::HelperArgs& args)->std::optional<secure::HelperGrant>{const auto sid=secure::processUserSid(child.process.value);if(!sid)return {};return secure::launchSecureDesktopHelper(executablePath(),session_,child.process.value,*sid,args);};
        secureEnv.toWorker=[&](const std::string& line){return pipePacket(pipe,true,{line.begin(),line.end()},stopped_,writeMutex_);};
        secureEnv.nowMs=[]{return uint64_t(GetTickCount64());};
        // hostAction 200 stage 246 (secure_desktop_scope.hpp, system_input.hpp): counts and levels only.
        const auto inputRecord=[](uint32_t kind,uint32_t value){recordHostEvent("hostAction",{{"action",uint32_t(200)},{"ok",true},{"stage",uint32_t(200)+secure::inputRecordStage},{"error",secure::input246(kind,value)}});};
        secureEnv.counts=[inputRecord](uint32_t routed,uint32_t heldBack,uint32_t writeFailed){inputRecord(secure::input246Routed,routed);if(heldBack)inputRecord(secure::input246HeldBack,heldBack);if(writeFailed)inputRecord(secure::input246WriteFailed,writeFailed);};
        // Independent outputs (secure_desktop_tracks.hpp): one helper per output, the same launch and
        // rules; their input comes from trackControl() through the system input's placement (below), so the
        // slot control() reads stays empty for such a stream.
        const auto secureTracks=trackSession?std::make_shared<secure::ServiceSecureTracks>(id_,secureEnv):std::shared_ptr<secure::ServiceSecureTracks>{};
        const auto secureService=std::make_shared<secure::ServiceSecure>(id_,std::move(secureEnv));
        struct SecureServiceEnd{std::atomic<std::shared_ptr<secure::ServiceSecure>>& slot;std::shared_ptr<secure::ServiceSecure> service;std::shared_ptr<secure::ServiceSecureTracks> tracks;~SecureServiceEnd(){slot.store({});service->close();if(tracks)tracks->close();}} secureServiceEnd{secure_,secureService,secureTracks};
        if(!trackSession)secure_.store(secureService);
        // System input (system_input.hpp). A worker below High integrity (a
        // standard user's) cannot operate elevated windows or a UAC prompt on the user's desktop, and an
        // administrator's elevated worker cannot operate that prompt either (consent.exe is at System integrity):
        // Windows drops such input without an error. For such a stream the client's input is written from
        // control() to a SYSTEM injector this service starts here, in the stream's own console session, for the
        // whole stream. The worker only learns that an input was injected elsewhere. Independent outputs keep
        // the worker's own injection (their coordinates are the worker's track plan). The injector ends with
        // this relay on every way out, and by itself on a lock, a sign-out or a session change.
        const auto systemInput=std::make_shared<sysinput::ServiceSystemInput>();
        std::function<secure::ipc::InputCounts()> systemInputCounts;
        struct SystemInputEnd{std::atomic<std::shared_ptr<sysinput::ServiceSystemInput>>& slot;std::shared_ptr<sysinput::ServiceSystemInput> input;std::function<secure::ipc::InputCounts()>& counts;std::function<void(uint32_t,uint32_t)> record;
          ~SystemInputEnd(){slot.store({});const bool had=input->held(),counted=input->counted();const auto service=input->close();
            // A stream without an injector whose inputs went to secure-desktop helpers: the service's own counts.
            if(!had&&counted)try{if(service.trackAsked)record(sysinput::record246TrackAsked,service.trackAsked);if(service.trackDropped)record(sysinput::record246TrackDropped,service.trackDropped);if(service.secureRouted)record(sysinput::record246SecureRouted,service.secureRouted);}catch(...){ }
            if(!had)return;
            try{const auto seen=counts?counts():secure::ipc::InputCounts{};
              record(sysinput::record246Received,seen.received);record(sysinput::record246Routed,service.routed);
              if(seen.injected)record(sysinput::record246Injected,seen.injected);if(seen.dropped)record(sysinput::record246Dropped,seen.dropped);if(seen.refused)record(sysinput::record246Refused,seen.refused);
              if(seen.sendFailed){record(sysinput::record246SendFailed,seen.sendFailed);record(sysinput::record246LastError,seen.lastError);}
              if(seen.offDesktop)record(sysinput::record246OffDesktop,seen.offDesktop);if(service.writeFailed)record(sysinput::record246WriteFailed,service.writeFailed);if(service.fellBack)record(sysinput::record246FellBack,service.fellBack);
              if(service.trackAsked)record(sysinput::record246TrackAsked,service.trackAsked);if(service.trackDropped)record(sysinput::record246TrackDropped,service.trackDropped);
              if(service.secureRouted)record(sysinput::record246SecureRouted,service.secureRouted);
            }catch(...){ }}} systemInputEnd{systemInput_,systemInput,systemInputCounts,inputRecord};
        systemInput->tracks(trackSession);
        if(secureTracks)systemInput->secureSink({[secureTracks]{return secureTracks->routing();},
          [secureTracks](const std::string& message,int32_t x,int32_t y,bool positional){return secureTracks->send(message,x,y,positional);},
          [secureTracks](const std::string& message){return secureTracks->reset(message);}});
        systemInput_.store(systemInput);
        if(!stopped_&&!stopping_&&sameInteractiveConsole(session_)&&sessionUnlocked(session_))try{
          uint32_t workerLevel=secure::integrityUnknown;{HANDLE raw=nullptr;if(OpenProcessToken(child.process.value,TOKEN_QUERY,&raw)){WinHandle token(raw);workerLevel=secure::worker::tokenLevel(token.value);}}
          const auto policy=[](const wchar_t* name,DWORD fallback){DWORD value=fallback,size=sizeof(value);return RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",name,RRF_RT_REG_DWORD,nullptr,&value,&size)==ERROR_SUCCESS?value:fallback;};
          const auto need=sysinput::systemInputNeed(workerLevel,policy(L"PromptOnSecureDesktop",1)!=0,policy(L"ConsentPromptBehaviorAdmin",5));
          if(need!=sysinput::Need::none){
            // Independent outputs take keys, positions, buttons and wheel, and 게임 마우스 by their own grant; text
            // and touch are not part of their input (track_input.hpp), so the track plan refuses them.
            const uint32_t grants=trackSession?(relativeMouseTrackSessionRequested(command)?1u:0u):(relativeMouseSessionRequested(command)?1u:0u)|(textInputSessionRequested(command)?2u:0u)|(touchInputSessionRequested(command)?4u:0u);
            const auto sid=secure::processUserSid(child.process.value);
            auto launch=sid?secure::launchSystemInput(executablePath(),session_,child.process.value,*sid,grants):std::nullopt;
            const uint32_t detail=(workerLevel&15u)|(uint32_t(need==sysinput::Need::workerNotElevated?1:2)<<4);
            if(launch){systemInputCounts=launch->counts;systemInput->attach(launch->helper);inputRecord(sysinput::record246Started,detail);}
            else inputRecord(sysinput::record246StartFailed,detail);
          }
        }catch(...){ }
        const auto sendCommand=[&](const std::string& line){if(pipePacket(pipe,true,{line.begin(),line.end()},stopped_,writeMutex_))return true;relayFacts.ended(WorkerRelayCause::commandWriteFailed);return false;};
        // Commands for the worker leave after a packet and, on a still screen
        // where none comes, as soon as one waits. Only while no resize/initial
        // RPC reads this pipe (any other command aborts those).
        const auto deliverPending=[&]{
          // Independent outputs take only a display-scale-outputs-v1 request,
          // once no track transaction reads the pipe.
          if(trackSession&&!terminal&&pendingScale_.load()){
            std::lock_guard lock(resizeMutex_);
            if(stopping_||pendingTrack_||initialCompletion_.pending())return true;
            if(const int scale=pendingScale_.exchange(0))return sendCommand("{\"type\":\"display.scale\",\"sessionId\":"+jsonQuote(id_)+",\"scale\":"+std::to_string(scale)+'}');
            return true;
          }
          if(trackSession||prepareStage<10||terminal)return true;
          if(!pendingBitrate_.load()&&!pendingKeyFrame_.load()&&!pendingRecover_.load()&&!pendingDecoder_.load()&&!pendingScale_.load()&&!pendingCaret_.load()&&!pendingCursorEcho_.load()&&!pendingCursorCompose_.load())return true;
          std::lock_guard lock(resizeMutex_);
          if(stopping_||pendingResize_||pendingTrack_||initialCompletion_.pending())return true;
          if(const int target=pendingBitrate_.exchange(0))if(!sendCommand("{\"type\":\"stream.bitrate\",\"sessionId\":"+jsonQuote(id_)+",\"bitrate\":"+std::to_string(target)+'}'))return false;
          // The broker channel's send queue dropped this stream's video: the
          // worker codes its next frame as an IDR.
          if(pendingKeyFrame_.exchange(false))if(!sendCommand("{\"type\":\"stream.keyframe\",\"sessionId\":"+jsonQuote(id_)+(pendingCursorResend_.exchange(false)?",\"cursor\":true":"")+'}'))return false;
          // udp-media-v1 LOSS: the worker's next frame references only the client's last good one.
          if(const uint64_t lost=pendingRecover_.exchange(0))if(!sendCommand("{\"type\":\"stream.recover\",\"sessionId\":"+jsonQuote(id_)+",\"pts\":"+std::to_string(lost-1)+'}'))return false;
          if(const auto report=pendingDecoder_.exchange(0))if(!sendCommand(streamDecoderMessage(id_,report)))return false;
          if(const int scale=pendingScale_.exchange(0))if(!sendCommand("{\"type\":\"display.scale\",\"sessionId\":"+jsonQuote(id_)+",\"scale\":"+std::to_string(scale)+'}'))return false;
          if(const int watch=pendingCaret_.exchange(0))if(!sendCommand("{\"type\":\"caret.watch\",\"sessionId\":"+jsonQuote(id_)+",\"on\":"+(watch==1?"true":"false")+'}'))return false;
          if(const int echo=pendingCursorEcho_.exchange(0))if(!sendCommand("{\"type\":\"cursor.echo\",\"sessionId\":"+jsonQuote(id_)+",\"on\":"+(echo==1?"true":"false")+'}'))return false;
          if(const int compose=pendingCursorCompose_.exchange(0))if(!sendCommand("{\"type\":\"cursor.compose\",\"sessionId\":"+jsonQuote(id_)+",\"on\":"+(compose==1?"true":"false")+'}'))return false;
          return true;
        };
        const PipeWake pendingWake{relayWake_.value,deliverPending};
        while (!stopped_ && (relayRead=readPipePacket(pipe, text, bytes, stopped_,std::chrono::seconds(15),&resizeDeadline_,relayWake_.value?&pendingWake:nullptr))) {
          const auto deadline=resizeDeadline_.load();if(deadline>0&&deadline<=streamMonotonicMillis()){relayFacts.ended(WorkerRelayCause::resizeExpired);break;}
          if (text && bytes.size() > 65536){relayFacts.ended(WorkerRelayCause::controlTooLarge);break;}
          bool handledControl=false;
          if(text){auto value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));
            enum class ControlAction{forward,handled,ended,invalid};
            const auto dispatch=[&]()->ControlAction{
              // Same-package worker diagnostic only. Valid, stale and malformed
              // observations are all consumed here and never reach the broker.
              consumeV2Trace(value);
              if(value.get("type").string()=="worker.encoder.usage"){observeEncoderUsage(value,encoderTicket);return ControlAction::handled;}
              // Role::secure: request / active / stop, and anything else under worker.secure.*, end here.
              if(secureTracks?secureTracks->consume(value):secureService->consume(value))return ControlAction::handled;
              // System input: the worker's rectangle for the injector (worker.input.*), consumed whether or not one runs.
              if(systemInput->consume(value,id_))return ControlAction::handled;
              if(value.get("type").string()=="worker.session.stopped"){if(!stopFence_.acknowledge(value))return ControlAction::invalid;terminal=true;return ControlAction::ended;}if(value.get("type").string()=="clip.diag"){recordClipboardWorkerDiagnostic(value,id_);return ControlAction::handled;}if(value.get("type").string()=="clip.packet"){if(value.get("sessionId").string()==id_)clipboard_.send(jsonObject(value.get("payload")),clipboardGenerationNumber(value.get("generation").number()));return ControlAction::handled;}
            if(value.get("type").string()=="worker.stream.failure"){const auto diagnostic=streamPreparationObservation(value,id_);if(!diagnostic)return ControlAction::invalid;preparationLog(streamObservationOk(diagnostic->first,diagnostic->second),200+diagnostic->first,diagnostic->second);observeGpuPath(diagnostic->first,diagnostic->second);return ControlAction::handled;}
            if(value.get("type").string()=="stream.unavailable"&&value.get("sessionId").string()==id_)preparationLog(false,90,streamPreparationReasonCode(value.get("reason").string()));
            if(value.get("type").string()=="worker.profile.active"){
              if(!profileSession_||value.get("sessionId").string()!=id_)return ControlAction::invalid;
              const auto display=parseDisplayState(value.get("display"));if(!display.ready()||display.monitors.empty()||display.monitors.front().id!="0"||!display.monitors.front().primary)return ControlAction::invalid;
#ifdef MULTITAP_NATIVE_DISPLAY
              const auto directory=machineDirectory()/L"private";const auto profileBytes=nativeStreamProfileFileAccess(directory/L"native-stream-profile.json").load(),launchBytes=nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json").load();if(!profileBytes||!launchBytes)return ControlAction::invalid;const auto profile=parseNativeStreamProfileRecord(*profileBytes);const auto launch=parseNativeStreamLaunch(*launchBytes);
              const auto lease=nativeStreamProfileLease(nativeDisplayLeaseSnapshot(),GetCurrentProcessId());
              if(profile.key!=NativeStreamProfileKey{id_,profileOperation_,profileInstance_,session_}||profile.phase!=NativeStreamProfilePhase::active||profile.resize||profile.stopRequested||!sameNativeStreamLease(profile.current,lease)||!nativeStreamLaunchMatches(launch,profile)||launch.worker!=nativeStreamProcessIdentity(child.process.value)||WaitForSingleObject(child.process.value,0)!=WAIT_TIMEOUT)return ControlAction::invalid;
#else
              return ControlAction::invalid;
#endif
              const auto ticket=displayCache_.begin(session_);displayCache_.complete(ticket,display,Clock::now(),value.get("desktopAvailable").boolean());readinessChanged_=true;prepareStage=8;preparationLog(true,prepareStage);return ControlAction::handled;
            }
            if(v2){const auto sized=displayV2Packet(value,v2Rpc_.load(),session_);if(sized.handled){if(sized.response.empty()||!pipePacket(pipe,true,{sized.response.begin(),sized.response.end()},stopped_,writeMutex_))return ControlAction::invalid;return ControlAction::handled;}}
            // A v2 tracks stream never runs the v1 track transaction RPCs.
            if(!(v2&&trackSession)){const auto native=trackSession?nativeTrackViewportPacket(value):nativeViewportPacket(value);if(native.handled){if(native.response.empty()||!pipePacket(pipe,true,{native.response.begin(),native.response.end()},stopped_,writeMutex_))return ControlAction::invalid;return ControlAction::handled;}}
            const auto action=trackSession?trackResizePacket(value,lastVideo):resizePacket(value,lastVideo);if(action==1)return ControlAction::handled;if(action<0){unavailable("worker_failed");return ControlAction::invalid;}terminal=action==2||(value.get("type").string()=="stream.unavailable"&&value.get("sessionId").string()==id_);
            observeEncoderStats(value,encoderTicket);
            // Extra encoder details are private on the released exact-shape
            // dual wire; current-worker diagnostics retain them above.
            if(value.get("type").string()=="stream.tracks.stats"){auto legacy=value;auto* object=std::get_if<Json::Object>(&legacy.value);if(object)if(auto found=object->find("tracks");found!=object->end())if(auto* rows=std::get_if<Json::Array>(&found->second.value))for(auto& row:*rows)if(auto* fields=std::get_if<Json::Object>(&row.value))fields->erase("encoderInfo");const auto packet=jsonObject(legacy);bytes.assign(packet.begin(),packet.end());}
            // The start's own stream.info carries its stages (connect_stages.hpp);
            // independent outputs keep their exact-shape stream.tracks.info.
            if(!trackSession&&!stages.info&&value.get("type").string()=="stream.info"){stages.info=ConnectStages::Clock::now();bytes=withStartStages(std::move(bytes),stages);}
              return ControlAction::forward;
            };
            const auto action=dispatch();if(action==ControlAction::invalid||action==ControlAction::ended){relayFacts.ended(action==ControlAction::ended?WorkerRelayCause::workerStopAck:WorkerRelayCause::controlDispatchFailed);break;}handledControl=action==ControlAction::handled;
          }else if(trackSession){const auto packet=trackWorkerPacket(bytes,trackCursor,trackOpus);if(packet==TrackWorkerPacket::invalid){relayFacts.ended(WorkerRelayCause::trackPacketInvalid);break;}if(packet==TrackWorkerPacket::video)lastVideo=Clock::now();}
          else if(initialPending()){relayFacts.ended(WorkerRelayCause::initialMediaPending);break;}
          else if(bytes.size()>=12&&bytes[0]==1){lastVideo=Clock::now();if(prepareStage<10){prepareStage=10;firstVideo=true;stages.frame=ConnectStages::Clock::now();}}
          if(!streamWorkerVideoAlive(lastVideo,Clock::now(),resizeDeadline_.load(),streamMonotonicMillis())){relayFacts.ended(WorkerRelayCause::videoWatchdog);break;}
          if(handledControl)continue;
          if(stopping_)continue;
          if(trackSession){const auto udp=udpTracks_.load();if(!(udp&&udp->outgoing(text,bytes))&&!tracks_.send(text,std::move(bytes))){relayFacts.ended(WorkerRelayCause::trackSendFailed);break;}}else send_(text, std::move(bytes));
          // Its flushed log record follows the first video instead of delaying it.
          if(firstVideo){firstVideo=false;recordHostEvent("hostAction",{{"action",uint32_t(200)},{"ok",true},{"stage",uint32_t(10)},{"error",uint32_t(0)},
            {"admitMs",ConnectStages::logged(stages.admitMs())},{"workerMs",ConnectStages::logged(stages.workerMs())},{"displayMs",ConnectStages::logged(stages.displayMs())},
            {"mediaMs",ConnectStages::logged(stages.mediaMs())},{"frameMs",ConnectStages::logged(stages.frameMs())}});}
          if(!deliverPending())break;
          if(terminal){stopping_=true;resizeDeadline_=0;const auto stop="{\"type\":\"session.stop\",\"sessionId\":"+jsonQuote(id_)+'}';if(!pipePacket(pipe,true,{stop.begin(),stop.end()},stopped_,writeMutex_)){relayFacts.ended(WorkerRelayCause::commandWriteFailed);break;}terminal=false;}
        }
        relayFacts.readEnded(stopped_||stopping_,relayRead);
        relayProbe.capture();
        endChannelRedials();
        clipboard_.stop();print_.stop();drives_.stop();usb_.stop();
        const bool wasStopped=stopped_.exchange(true);const bool unexpected=!wasStopped&&!stopping_;streaming_=false;if(unexpected){invalidateDisplay();if(!terminal)unavailable("worker_failed");}
        pipe_=nullptr;
      } catch (...) { relayFacts.ended(WorkerRelayCause::exception);if(!relayFacts.captured)relayFacts.requested=stopped_||stopping_;preparationLog(false,prepareStage);if(trackSession)tracks_.stop();endChannelRedials();clipboard_.stop();print_.stop();drives_.stop();usb_.stop();pipe_=nullptr;const bool wasStopped=stopped_.exchange(true);const bool unexpected=!wasStopped&&!stopping_;streaming_=false;if(unexpected){invalidateDisplay();unavailable(initialFallback?"host_maintenance":initialSdkUnsupported?"unsupported_display_mode":"worker_failed");} }
      v2Rpc_=false;
      if(nativeDisplay_)nativeDisplay_->cancelViewportSession(id_);
    });
  }
  // Adaptive bitrate (AgentBridge): only stored here; the session's pipe
  // thread forwards it to the worker when that is safe.
  void bitrate(const std::string&, int value) override { pendingBitrate_.store(value); SetEvent(relayWake_.value); }
  void keyFrame() override { pendingKeyFrame_.store(true); SetEvent(relayWake_.value); }
  // host-resume-v1: carried by the key frame request that follows it.
  void cursorResend() override { pendingCursorResend_.store(true); }
  // host-resume-v1: the clipboard channel waits out the hold and starts again.
  // channel-resume-v1: the same for the client's absence (session.channels), and for all four channels. They
  // wait while either side is away and start again when both are there (ChannelPresence).
  void clipboardAway() override { if(presence_.host(true))channelsAway(); }
  void clipboardResumed() override { if(presence_.host(false))channelsBack(); }
  void peerAway() override { if(presence_.peer(true))channelsAway(); }
  void peerBack() override { if(presence_.peer(false))channelsBack(); }
  void channelsAway(){clipboard_.away();printRedial_.away();driveRedial_.away();usbRedial_.away();}
  void channelsBack(){clipboard_.resumed();printRedial_.resumed();driveRedial_.resumed();usbRedial_.resumed();}
  // A new session: nobody is away, and its print, drive and USB channels are dialled again after a transient
  // end only where the Broker named channel-resume-v1 (`enabled`); otherwise an end stays an end, as released.
  void channelRedials(bool enabled){
    endChannelRedials();presence_.reset();
    if(!enabled){print_.onEnded({});drives_.onEnded({});usb_.onEnded({});return;}
    print_.onEnded([this](bool transient){printRedial_.ended(transient);});
    drives_.onEnded([this](bool transient){driveRedial_.ended(transient);});
    usb_.onEnded([this](bool transient){usbRedial_.ended(transient);});
    printRedial_.begin([this]{print_.redial();});
    driveRedial_.begin([this]{drives_.redial();});
    usbRedial_.begin([this]{usb_.redial();});
  }
  // Before the channels stop: no dial starts or still runs after this returns.
  void endChannelRedials(){printRedial_.end();driveRedial_.end();usbRedial_.end();}
  // udp-media-v1 LOSS: the oldest last good PTS waiting, plus one (it covers every newer one).
  void recover(uint64_t lastGood) override {
    const uint64_t want = lastGood + 1; uint64_t seen = pendingRecover_.load();
    while ((!seen || want < seen) && !pendingRecover_.compare_exchange_weak(seen, want)) {}
    SetEvent(relayWake_.value);
  }
  void control(const Json& value) override {
    std::lock_guard lifecycle(lifecycleMutex_);if(signinRuntime_&&signinRuntime_->control(value))return;const auto pipe=pipe_.load();if(!pipe||stopped_||stopping_)return;
    auto command=value;
    const auto type=value.get("type").string();
    // display-scale-v1: only the latest request is kept and handed to the
    // worker by the pipe thread when no resize/initial RPC reads the pipe.
    if(type=="display.scale"){if(const auto percent=displayScaleMessage(value);percent&&streaming_&&value.get("sessionId").string()==id_){pendingScale_.store(*percent);SetEvent(relayWake_.value);}return;}
    // stream-size-v1: likewise only the latest decoder report is kept.
    if(type=="stream.decoder"){if(const auto report=streamDecoderReport(value);report&&streaming_&&value.get("sessionId").string()==id_){pendingDecoder_.store(*report);SetEvent(relayWake_.value);}return;}
    // caret-v1: only the latest watch request (1 on, 2 off) is kept for the pipe thread.
    if(type=="caret.watch"){if(const auto on=caretWatchRequest(value);on&&streaming_&&value.get("sessionId").string()==id_){pendingCaret_.store(*on?1:2);SetEvent(relayWake_.value);}return;}
    // cursor-echo-v1: likewise only the latest request.
    if(type=="cursor.echo"){if(const auto on=cursor::echoRequest(value);on&&streaming_&&value.get("sessionId").string()==id_){pendingCursorEcho_.store(*on?1:2);SetEvent(relayWake_.value);}return;}
    // cursor-compose-v1: likewise only the latest request.
    if(type=="cursor.compose"){if(const auto on=cursor::composeRequest(value);on&&streaming_&&value.get("sessionId").string()==id_){pendingCursorCompose_.store(*on?1:2);SetEvent(relayWake_.value);}return;}
    // stream.lossless: the client's tiles-v1 opt-in (see Agent ping), for the worker.
    if(type!="stream.resize"&&type!="input"&&type!="input.reset"&&type!="stream.lossless")return;
    if(type=="stream.resize") {
      const auto request=streamResizeRequest(value);(void)resizeDecoderLimits(value,streamSizesV2_);const bool sizeAllowed=streamSizesV2_||legacyStreamOutputSize(uint64_t(request.width),uint64_t(request.height));bool allowed=false;
      {std::lock_guard lock(resizeMutex_);bool capture=false;const auto display=displayCache_.snapshot(session_,sessionUnlocked(session_),&capture);allowed=sizeAllowed&&epochMedia_&&request.sessionId==id_&&request.monitor==streamMonitor_&&request.epoch>currentEpoch_&&!pendingResize_&&capture&&display.viewportProvisioning&&sameInteractiveConsole(session_)&&sessionUnlocked(session_);
        if(allowed){pendingEncoderLimits_=resizeViewportEncoderLimits(encoderLimits_,value,streamSizesV2_);encoderDiagnostics_.invalidate();pendingResize_=request;resizeVerified_=false;resizeTicket_=displayCache_.begin(session_);resizeDeadline_=streamMonotonicMillis()+streamResizeTimeoutMs;streamField(command,"workerDeadlineMs",double(resizeDeadline_.load()));}}
      if(!allowed){const auto response="{\"type\":\"stream.resize.failed\","+streamResizeFields(request)+",\"reason\":\"unsupported_display_mode\",\"unchanged\":true}";send_(true,{response.begin(),response.end()});return;}
    }else if(type=="input") {std::lock_guard lock(resizeMutex_);if(pendingResize_)return;}
    // Role::secure: while the SYSTEM helper this service launched streams a UAC prompt of this live stream, the
    // client's key/mouse/text input goes to the helper's private pipe from here (the worker cannot reach the
    // Winlogon desktop, and never carries input to the helper). input.reset goes to both.
    if(type=="input"||type=="input.reset")if(const auto secure=secure_.load())if(secure->route(command))return;
    // System input: an input this service writes to its SYSTEM injector goes on to the worker marked, so the
    // worker does not inject it again; a marker of the client's own never passes.
    if(type=="input"||type=="input.reset"){if(auto* fields=std::get_if<Json::Object>(&command.value)){fields->erase(sysinput::workerMarker);fields->erase(sysinput::workerSeq);}if(const auto system=systemInput_.load())(void)system->route(command);}
    const auto text=jsonObject(command);pipePacket(pipe,true,{text.begin(),text.end()},stopped_,writeMutex_);
  }
  void stop() override {
    std::lock_guard lifecycle(lifecycleMutex_);if(ownsSignin_&&signinRuntime_)signinRuntime_->stop(signinOwnedSession_);stopping_=true;encoderDiagnostics_.retire();
    if(auto udp=udpTracks_.exchange({}))udp->close();
    {std::lock_guard lock(consoleMutex_);if(consoleOperation_){consoleOperation_->cancelled=true;consoleOperation_->hold.release();consoleOperation_->result.ok=false;consoleOperation_->result.reason="qemu_console_cancelled";}}if(consoleWorker_.joinable())consoleWorker_.join();consoleRunning_=!qemuConsoleQuiescent();
    {std::lock_guard lock(resizeMutex_);encoderDiagnosticEpoch_=0;pendingResize_.reset();pendingEncoderLimits_.reset();pendingTrack_.reset();currentTrack_.reset();initialCompletion_={};initialGroupCompletion_={};resizeVerified_=false;trackMedia_=trackMetadataReady_=trackResizeVerified_=initialTrack_=false;resizeDeadline_=0;}
    {std::lock_guard lock(displayOperationMutex_);if(displayOperation_)displayOperation_->cancelled=true;}if(displayOperationWorker_.joinable())displayOperationWorker_.join();endChannelRedials();clipboard_.stop();print_.stop();drives_.stop();usb_.stop();
    if(const auto pipe=pipe_.load();pipe&&!stopped_){const auto command="{\"type\":\"session.stop\",\"sessionId\":"+jsonQuote(id_)+'}';(void)pipePacket(pipe,true,{command.begin(),command.end()},stopped_,writeMutex_);}
    // No pipe-reader mutex is held here. Keep reads alive until the capture has
    // stopped and the worker has durably restored its exact profile, or timed out.
    if(worker_.joinable()&&!stopped_)stopFence_.wait(std::chrono::seconds(15));
    stopped_=true;tracks_.cancel();if(const auto pipe=pipe_.load())CancelIoEx(pipe,nullptr);if(worker_.joinable())worker_.join();tracks_.stop();
    pipe_=nullptr;streaming_=false;profileSession_=false;session_=UINT32_MAX;sessionActivity_.reset();
  }
  SessionStopEvidence stopSession(const std::string& session) override {
    std::lock_guard lifecycle(lifecycleMutex_);
    if(signinRuntime_&&signinRuntime_->owns(session)){signinRuntime_->stop(session);return signinRuntime_->stoppedSession(session);}
    if(rejectedStream_&&*rejectedStream_==session){const bool quiet=workerQuiescentNow();return {quiet,false,quiet};}
    if(session!=id_)return {};
    const auto instance=nativeStopInstance_;
    stop();
    const bool profile=sessionHadProfile_;
    const bool native=sessionModeIsStream_&&nativeStopGuard_&&(workerCreatedForSession_||profile);
    lastSessionStop_=StoppedSession{session,instance,workerQuiescentNow(),profile,native,sessionModeIsStream_&&v2Session_};
    return stoppedSession(session);
  }
  SessionStopEvidence stoppedSession(const std::string& session) override {
    std::lock_guard lifecycle(lifecycleMutex_);
    if(signinRuntime_&&signinRuntime_->owns(session))return signinRuntime_->stoppedSession(session);
    if(rejectedStream_&&*rejectedStream_==session){const bool quiet=workerQuiescentNow();return {quiet,false,quiet};}
    // A late exit of this session's worker completes its stop evidence.
    (void)workerQuiescentNow();
    if(!lastSessionStop_||lastSessionStop_->session!=session)return {};
    const auto& value=*lastSessionStop_;
    // v2: the stop already ran its restore; a saved state left by a failed one
    // belongs to the service's automatic recovery and never blocks what comes
    // next (docs/display-manager-v2.md). Holding the broker's cleanup barrier on
    // it would block every stream and even the VM reboot that would clear it
    // while a restore keeps failing.
    const bool restored=value.quiescent&&(value.v2||(!value.native&&!value.profile)||
      (value.native&&nativeDisplay_&&!profileRecoveryRunning_&&nativeDisplay_->streamStopClean(value.instance)));
    return {value.quiescent,value.profile,restored};
  }
  void sessionChanged(const std::vector<SessionEvent>& events) {
    // Invalidate before acquiring lifecycleMutex_: concurrent cap observations
    // cannot attach old allowed=true to the new source, even for idle Hosts.
    // The capability inputs first: a report holding the new privacy ticket
    // must not find a session view read before this change.
    if (std::any_of(events.begin(), events.end(), [](const auto& event) { return event.type != 0; })) {
      capabilityInputs_.invalidate(); thumbnailPrivacy_.rotate(); readinessChanged_ = true;
    }
    std::lock_guard lifecycle(lifecycleMutex_);
    const DWORD current = interactiveSession();
    const bool unlocked = sessionUnlocked(current), activeStream = streaming_ && !stopped_;
    // Fresh unlock/logon/console connect rearms the bounded profile-restore
    // budgets (in-service gate and restart-recovery attempts) for that session.
    // A new Windows logon retries the full initial viewport path.
    for(const auto& event:events)if(event.type==WTS_SESSION_LOGON||event.type==WTS_SESSION_LOGOFF)initialFallback_.clear();
    for(const auto& event:events)if(event.type==WTS_SESSION_UNLOCK||event.type==WTS_SESSION_LOGON||event.type==WTS_CONSOLE_CONNECT){
      if(event.session==current){std::lock_guard lock(profileRecoveryMutex_);(void)profileRetry_.rearm(event.session);}
      restartProfileAttempts_=0;v2RecoveryAttempts_=0;v2RecoveryIneligible_=0;v2RecoveryNext_=0;
    }
    if(activeStream)for(const auto& event:events)if(event.type&&(event.session==session_||event.session==0xffffffff))recordHostEvent("hostAction",{{"action",uint32_t(201)},{"stage",uint32_t(event.type)},{"ok",!workerSessionMustRestart(session_,current,unlocked,{event})},{"active",true},{"changed",workerSessionMustRestart(session_,current,unlocked,{event})}});
    {std::lock_guard lock(displayOperationMutex_);if(displayOperationRunning_&&displayOperation_&&workerSessionMustRestart(displayOperation_->session,current,unlocked,events))displayOperation_->cancelled=true;}
    if (activeStream && !workerSessionMustRestart(session_, current, unlocked, events)) return;
    invalidateDisplay();
    if (activeStream) { unavailable(workerSessionStopReason(session_, current, unlocked, sameInteractiveConsole(current))); stop(); }
    scheduleProfileRecovery(current,unlocked);
    probeDisplay(current, unlocked);
  }
  bool streaming()const{return (streaming_&&!stopped_)||(signinRuntime_&&signinRuntime_->busy());}
  bool readinessChanged(){const bool changed=readinessChanged_.exchange(false);return profileRecoveryRunning_.takeChanged()||changed;}
  // boot-ready-v1: a probe result that changes readiness wakes the service loop.
  void readinessWake(HANDLE event){readinessChanged_.wake(event);}
  // boot-ready-v1: while booting, the loop re-probes a not-ready display when the
  // cache says it is due (5 s). Until now the next probe waited for the next
  // capabilities read, a heartbeat up to 10 s later. Same gates and interval.
  void bootProbe(){const DWORD session=interactiveSession();probeDisplay(session,sessionUnlocked(session));}
  void maintenanceChanged(){invalidateDisplay();}
  // A capabilities request from Host settings (WinFsp installed, spooler started,
  // Windows account saved) publishes fresh facts, not the up-to-60-second cached ones.
  void refreshDeviceObservations(){driveCheckedAt_=0;printRedirectionRecheck()=true;capabilityInputs_.invalidate();}
 private:
  std::shared_ptr<std::atomic<bool>> diagnosticActivity_;
  std::atomic<uint32_t> thumbnailActive_{0};
  // The capability report's session view and console identities (capability_inputs.hpp).
  CapabilityInputs capabilityInputs_{{[] { CapabilitySessionView view; view.session = interactiveSession(); view.unlocked = sessionUnlocked(view.session); return view; },
    observeStreamConsoleIdentity, [] { return machineFileStamp(windowsAccountRecordPath()); }}};
  // Fresh per service/bridge owner (including configuration reload). Failure
  // to obtain entropy only disables thumbnail permission, not Host startup.
  ThumbnailPrivacyEpoch thumbnailPrivacy_{[] { return utf8(randomName()); }};
  // The same unlocked-console rule as thumbnailPrivacy's `allowed`; the helper
  // runs with the signed-in user's own token (drive/print helper path).
  ThumbnailShooter thumbnailShooter_{
    [] { ThumbnailSession value; value.id = interactiveSession(); value.unlocked = sessionUnlocked(value.id); value.console = sameInteractiveConsole(value.id); return value; },
    [](DWORD session) { return drive::runDriveMountHelper(session, "{}", thumbnailHelperTimeoutMs, L"--thumbnail-capture"); }};
  EncoderDiagnosticCache encoderDiagnostics_;EncoderUsageJournalPolicy encoderUsage_;uint32_t encoderDiagnosticEpoch_=0;
  void observeEncoderStats(const Json& value,EncoderDiagnosticCache::Ticket ticket){
    const auto type=value.get("type").string();
    if(type=="stream.info"||type=="stream.tracks.info"){std::lock_guard lock(resizeMutex_);if(!encoderDiagnostics_.current(ticket)||stopped_||stopping_||value.get("sessionId").string()!=id_)return;const auto epoch=value.get("epoch").number(-1);if(type=="stream.info"&&std::isfinite(epoch)&&epoch>=1&&epoch<=65535&&std::floor(epoch)==epoch)encoderDiagnosticEpoch_=uint32_t(epoch);encoderDiagnostics_.clear(ticket);return;}
    if(type!="stream.stats"&&type!="stream.tracks.stats")return;
    std::vector<EncoderDiagnosticTrack> observations;
    {std::lock_guard lock(resizeMutex_);
      if(stopped_||stopping_||pendingResize_||pendingTrack_||value.get("sessionId").string()!=id_){encoderDiagnostics_.clear(ticket);return;}
      if(type=="stream.stats"){
        if(trackMedia_||(epochMedia_&&(!encoderDiagnosticEpoch_||value.get("epoch").number(-1)!=encoderDiagnosticEpoch_)))return;
        const auto info=parseEncoderDiagnostic(value.get("encoderInfo"));if(info)observations.push_back({"0",encoderDiagnosticEpoch_,*info});
      }else{
        if(!trackMedia_||!trackMetadataReady_||!currentTrack_||value.get("topologyEpoch").number(-1)!=currentTrack_->topologyEpoch)return;
        const auto* tracks=std::get_if<Json::Array>(&value.get("tracks").value);if(!tracks||tracks->size()!=currentTrack_->outputs.size())return;
        for(size_t i=0;i<tracks->size();++i){const auto& track=(*tracks)[i];const auto info=parseEncoderDiagnostic(track.get("encoderInfo"));
          if(!info||track.get("id").string()!=currentTrack_->outputs[i].id||track.get("epoch").number(-1)!=currentTrack_->epochs[i]){observations.clear();break;}
          observations.push_back({currentTrack_->outputs[i].id,currentTrack_->epochs[i],*info});}
      }
      if(observations.empty())encoderDiagnostics_.clear(ticket);else (void)encoderDiagnostics_.observe(ticket,std::move(observations),GetTickCount64());
    }
  }
  void observeEncoderUsage(const Json& value,EncoderDiagnosticCache::Ticket ticket)noexcept{
    try{
      const auto observation=parseEncoderUsagePacket(value);if(!observation)return;bool record=false;
      {std::lock_guard lock(resizeMutex_);
        if(!encoderDiagnostics_.current(ticket)||stopped_||stopping_||!streaming_||pendingResize_||pendingTrack_)return;
        std::vector<EncoderDiagnosticTrack> expected;uint32_t topology=0;
        if(trackMedia_){if(!trackMetadataReady_||!currentTrack_)return;topology=currentTrack_->topologyEpoch;for(size_t i=0;i<currentTrack_->outputs.size();++i)expected.push_back({currentTrack_->outputs[i].id,currentTrack_->epochs[i],{}});}
        else expected.push_back({"0",epochMedia_?encoderDiagnosticEpoch_:0,{}});
        if(!encoderUsageMatches(*observation,id_,topology,expected))return;
        record=encoderUsage_.observe(ticket,ticket,observation->track,true);
      }
      // No statistics inference. This snapshot came from the actual output
      // callback, so CPU initialization without a new CPU frame cannot log use.
      const auto& track=observation->track;
      if(record&&encoderDiagnostics_.current(ticket))recordHostEvent("hostAction",{{"action",254},{"stage",encoderUsageBackend(track.info.backend)},{"format",track.info.vendorId},{"fallback",track.info.gpuFallbackCode},{"error",track.info.softwareFallbackCode},{"items",track.id=="0"?0:1},{"expected",track.epoch},{"ok",1},{"owned",track.info.hardwareVerified},{"active",1}});
      if(record&&encoderDiagnostics_.current(ticket))if(const auto failure=encoderGpuFailureJournalRecord(*observation))recordHostEvent("hostAction",{{"action",255},{"stage",failure->site},{"win32",failure->nativeStatus},{"format",failure->vendorId},{"cause",failure->deviceId},{"error",failure->gpuFallbackCode},{"fallback",failure->cpuBackend},{"items",failure->track},{"expected",failure->epoch},{"ok",0},{"active",1}});
    }catch(...){/* Optional diagnostic must not fail streaming. */}
  }
  std::mutex resizeMutex_;std::optional<StreamResize> pendingResize_;DisplayReadinessCache::Ticket resizeTicket_{};std::atomic<int64_t> resizeDeadline_{0};bool epochMedia_=false,streamSizesV2_=false;std::string streamMonitor_;uint16_t currentEpoch_=0;
  InitialViewportCompletion initialCompletion_;std::string profileInstanceForInitial_;bool resizeVerified_=false;
  InitialViewportFallbackLatch initialFallback_;
  InitialProfileTrackCompletion initialGroupCompletion_;
  bool initialPending(){std::lock_guard lock(resizeMutex_);return initialCompletion_.pending();}
  std::optional<NativeTrackResize> currentTrack_,pendingTrack_;bool trackMedia_=false,trackMetadataReady_=false,trackResizeVerified_=false,initialTrack_=false;
  void trackControl(const std::string& message,const std::string& expected,bool ordered=false){
    const auto value=JsonParser::parse(message);const auto pipe=pipe_.load();if(value.get("sessionId").string()!=expected||!pipe||stopped_||stopping_)return;auto command=value;const auto type=value.get("type").string();
    // Role::secure: worker.secure.* exists only between this service and its worker. A client's tunnel never
    // carries one to the worker (its worker.secure.ready names handles the worker would adopt).
    if(secure::secureControlConsumed(type)||type.rfind(sysinput::controlPrefix,0)==0)return;
    if(!ordered){
      if(const auto udp=udpTracks_.load();udp&&udp->control(value))return;
      if(type.starts_with("udp.tracks."))return;
      if(type=="input.direct"){
        const auto inner=value.get("input").get("type").string();if(value.get("protocol").string()!="udp-direct-v1"||!udpTrackNumber(value.get("sequence"),1)||(inner!="input"&&inner!="input.reset"&&inner!="input.fence"))return;
        if(inner!="input.fence"){auto input=value.get("input");if(auto* object=std::get_if<Json::Object>(&input.value))(*object)["sessionId"]=Json{expected};trackControl(jsonObject(input),expected,true);}return;
      }
    }
    if(type=="stream.tracks.resize"){
      const auto request=nativeTrackResizeRequest(value);(void)resizeDecoderLimits(value,streamSizesV2_);const bool sizeAllowed=streamSizesV2_||legacyTrackRequestSize(request);bool allowed=false;
      {std::lock_guard lock(resizeMutex_);bool capture=false;const auto display=displayCache_.snapshot(session_,sessionUnlocked(session_),&capture);allowed=sizeAllowed&&trackMedia_&&trackMetadataReady_&&currentTrack_&&!pendingTrack_&&!pendingResize_&&nativeDisplay_&&nativeDisplaySelected()&&request.sessionId==id_&&nativeTrackResizeFollows(*currentTrack_,request)&&capture&&display.viewportProvisioning&&display.monitors.size()==request.outputs.size()&&sameInteractiveConsole(session_)&&sessionUnlocked(session_);
        if(allowed){encoderDiagnostics_.invalidate();pendingTrack_=request;initialTrack_=trackResizeVerified_=false;resizeTicket_=displayCache_.begin(session_);resizeDeadline_=streamMonotonicMillis()+streamResizeTimeoutMs;streamField(command,"workerDeadlineMs",double(resizeDeadline_.load()));}}
      if(!allowed){const auto response=nativeTrackResizeFailure(request,"unsupported_display_mode");tracks_.send(true,{response.begin(),response.end()});return;}
    }else if(type=="input"){std::lock_guard lock(resizeMutex_);if(pendingTrack_)return;}
    // System input on independent outputs (system_input.hpp): the service keeps the client's event and asks the
    // worker only to place it; the worker's answer (worker.input.track) comes back through the relay and the
    // event then goes to the injector. A marker of the client's own never passes.
    if(type=="input"||type=="input.reset"){if(auto* fields=std::get_if<Json::Object>(&command.value)){fields->erase(sysinput::workerMarker);fields->erase(sysinput::workerSeq);}if(const auto system=systemInput_.load())(void)system->routeTrack(command);}
    const auto bytes=jsonObject(command);if(!pipePacket(pipe,true,{bytes.begin(),bytes.end()},stopped_,writeMutex_)){stopped_=true;tracks_.cancel();CancelIoEx(pipe,nullptr);}
  }
  NativeViewportDispatch nativeTrackViewportPacket(const Json& value){
    NativeTrackViewportContext context;
    {std::lock_guard lock(resizeMutex_);context.pending=pendingTrack_;context.deadline=resizeDeadline_.load();context.authorized=nativeDisplay_&&trackMedia_&&nativeDisplaySelected()&&pendingTrack_&&pendingTrack_->sessionId==id_;}
    context.console=sameInteractiveConsole(session_);context.unlocked=sessionUnlocked(session_);context.stopped=stopped_.load()||!streaming_.load();
    if(context.authorized&&!context.stopped&&context.console&&context.unlocked&&value.get("type").string()!="worker.native.tracks.resize.rollback")try{context.scope=nativeSessionRecoveryScope(session_);}catch(...){ }
    return dispatchNativeTrackViewport(value,context,nativeDisplay_);
  }
  int trackResizePacket(const Json& value,Clock::time_point& lastVideo){
    const auto type=value.get("type").string();if(type!="worker.tracks.resize.result"&&type!="stream.tracks.info"&&type!="stream.tracks.resize.failed"&&type!="stream.tracks.resize.restored")return 0;
    std::lock_guard lock(resizeMutex_);if(stopped_||!trackMedia_)return -1;
    const auto correlated=[&]{return pendingTrack_&&value.get("sessionId").string()==pendingTrack_->sessionId&&value.get("requestId").string()==pendingTrack_->requestId&&value.get("resizeId").string()==pendingTrack_->resizeId&&value.get("topologyEpoch").number()==pendingTrack_->topologyEpoch;};
    if(type=="stream.tracks.resize.restored"){
      if(v2Session_){
        // v2 last resort: the worker could not apply the requested group and
        // put the stream's previous group back in place (sizes, places, media
        // epochs under the request's new topology). Profile track sessions
        // only, the same restored-group path the broker and clients accept.
        try{
          if(!initialGroupCompletion_.complete()||!correlated()||!currentTrack_||!profileTrackRestoredMatches(*currentTrack_,*pendingTrack_,value)||!sameInteractiveConsole(session_)||!sessionUnlocked(session_))return -1;
          const auto restored=nativeTrackResizeRequest(value);
          if(!initialProfileTrackDisplayMatches(displayCache_.snapshot(session_,true),restored,true))return -1;
          pendingTrack_=restored;trackResizeVerified_=true;trackMetadataReady_=false;
          recordHostEvent("hostAction",{{"action",uint32_t(202)},{"stage",uint32_t(420)},{"ok",true},{"monitors",uint32_t(restored.outputs.size())}});
          return 0;
        }catch(...){return -1;}
      }
#ifdef MULTITAP_NATIVE_DISPLAY
      try{
        if(!profileSession_||!initialGroupCompletion_.complete()||!nativeDisplay_||!nativeDisplay_->hasStreamProfileReservation()||!correlated()||!currentTrack_||!profileTrackRestoredMatches(*currentTrack_,*pendingTrack_,value)||!sameInteractiveConsole(session_)||!sessionUnlocked(session_))return -1;
        const auto restored=nativeTrackResizeRequest(value);const auto display=displayCache_.snapshot(session_,true);
        const auto directory=machineDirectory()/L"private";const auto profileBytes=nativeStreamProfileFileAccess(directory/L"native-stream-profile.json").load(),receiptBytes=nativeStreamViewportReceiptFileAccess(directory/L"native-stream-viewport-commit.json").load(),runtimeBytes=nativeRuntimeFileAccess(directory/L"native-display-runtime.json").load();if(!profileBytes||!receiptBytes||!runtimeBytes)return -1;
        const auto profile=parseNativeStreamProfileRecord(*profileBytes);const auto snapshot=nativeDisplayLeaseSnapshot();const auto lease=nativeStreamProfileLease(snapshot,GetCurrentProcessId());const auto baseline=nativeBaselineFileAccess(directory/L"native-display-baseline.json").load();
        if(profile.key!=NativeStreamProfileKey{id_,profileOperation_,profileInstance_,session_}||!initialProfileTrackDisplayMatches(display,restored,true)||!initialProfileTrackLeaseMatches(snapshot,display,restored,GetCurrentProcessId(),profileInstance_)||!profileTrackRestoreReceiptMatches(profile,*pendingTrack_,parseNativeStreamViewportReceipt(*receiptBytes),*runtimeBytes,baseline.value_or(""),lease))return -1;
        pendingTrack_=restored;trackResizeVerified_=true;trackMetadataReady_=false;return 0;
      }catch(...){return -1;}
#else
      return -1;
#endif
    }
    if(type=="worker.tracks.resize.result"){
      if(!correlated())return 1;
      if(!nativeSameTrackResize(nativeTrackResizeRequest(value),*pendingTrack_)||!sameInteractiveConsole(session_)||!sessionUnlocked(session_))return -1;
      if(v2Session_){
        // Same fixed codes as a single-output v2 resize (hostAction 202).
        const auto diagnostic=streamViewportDiagnostic(value);if(!diagnostic)return -1;const auto& root=pendingTrack_->outputs.front();
        recordHostEvent("hostAction",{{"action",uint32_t(202)},{"stage",100+diagnostic->nativeStage},{"error",diagnostic->nativeError},{"ok",value.get("ok").boolean()},{"width",root.width},{"height",root.height},{"monitors",uint32_t(pendingTrack_->outputs.size())}});
        if(diagnostic->consoleReturned)recordHostEvent("hostAction",{{"action",uint32_t(202)},{"stage",uint32_t(400)},{"ok",value.get("ok").boolean()},{"active",true}});
        recordDisplayV2Retries(value);
      }
      // v2 resolves a failed resize by restoring its own state file; it never
      // latches the SDK service into viewport recovery.
      const auto display=parseDisplayState(value.get("display"));if(!v2Session_&&display.recoveryRequired&&nativeDisplay_)nativeDisplay_->cancelViewportSession(id_,true);
      const bool completed=displayCache_.complete(resizeTicket_,display,Clock::now(),value.get("desktopAvailable").boolean());readinessChanged_=true;trackResizeVerified_=completed&&std::holds_alternative<bool>(value.get("ok").value)&&value.get("ok").boolean()&&display.ready();return 1;
    }
    if(type=="stream.tracks.resize.failed"){if(!correlated())return 1;pendingTrack_.reset();resizeDeadline_=0;lastVideo=Clock::now();return 2;}
    if(pendingTrack_){const auto display=displayCache_.snapshot(session_,sessionUnlocked(session_));if(!nativeTrackMetadataMayComplete(value,*pendingTrack_,display,trackResizeVerified_,initialTrack_,streamSizesV2_))return -1;if(initialGroupCompletion_.pending()&&!initialGroupCompletion_.acceptMetadata(value,display,!stopped_&&!stopping_&&sameInteractiveConsole(session_)&&sessionUnlocked(session_),streamSizesV2_))return -1;currentTrack_=pendingTrack_;pendingTrack_.reset();trackResizeVerified_=false;resizeDeadline_=0;lastVideo=Clock::now();}
    else if(!currentTrack_||!nativeTrackMetadataMayComplete(value,*currentTrack_,displayCache_.snapshot(session_,sessionUnlocked(session_)),true,initialTrack_,streamSizesV2_))return -1;
    trackMetadataReady_=true;return 0;
  }
  NativeViewportDispatch nativeViewportPacket(const Json& value){
    NativeViewportContext context;
    {std::lock_guard lock(resizeMutex_);context.pending=pendingResize_;context.deadline=resizeDeadline_.load();context.authorized=nativeDisplay_&&epochMedia_&&nativeDisplaySelected()&&pendingResize_&&pendingResize_->sessionId==id_&&pendingResize_->monitor==streamMonitor_;}
    context.console=sameInteractiveConsole(session_);context.unlocked=sessionUnlocked(session_);context.stopped=stopped_.load()||!streaming_.load();
    if(context.authorized&&!context.stopped&&context.console&&context.unlocked&&value.get("type").string()!="worker.native.resize.rollback")try{context.scope=nativeSessionRecoveryScope(session_);}catch(...){ }
    return dispatchNativeViewport(value,context,nativeDisplay_);
  }
  int resizePacket(const Json& value,Clock::time_point& lastVideo) {
    const auto type=value.get("type").string();if(type!="worker.resize.result"&&type!="stream.info"&&type!="stream.resize.failed")return 0;
    std::lock_guard lock(resizeMutex_);
    if(initialCompletion_.pending()){
      if(type!="stream.info")return -1;
      if(!initialCompletion_.acceptMetadata(value,displayCache_.snapshot(session_,sessionUnlocked(session_)),!stopped_&&!stopping_&&sameInteractiveConsole(session_)&&sessionUnlocked(session_)))return -1;
      currentEpoch_=pendingResize_->epoch;pendingResize_.reset();resizeDeadline_=0;lastVideo=Clock::now();return 0;
    }
    if(type=="worker.resize.result") {
      if(pendingResize_&&pendingResize_->matches(value)&&!stopped_&&interactiveSession()==session_&&sessionUnlocked(session_)) {
        const auto diagnostic=streamViewportDiagnostic(value);if(!diagnostic)return -1;
        recordHostEvent("hostAction",{{"action",uint32_t(202)},{"stage",100+diagnostic->nativeStage},{"error",diagnostic->nativeError},{"ok",value.get("ok").boolean()},{"width",uint32_t(pendingResize_->width)},{"height",uint32_t(pendingResize_->height)}});
        if(diagnostic->nativeReason)recordHostEvent("hostAction",{{"action",uint32_t(202)},{"stage",300+diagnostic->nativeReason},{"error",diagnostic->nativeError},{"ok",false}});
        if(diagnostic->consoleReturned)recordHostEvent("hostAction",{{"action",uint32_t(202)},{"stage",uint32_t(400)},{"ok",value.get("ok").boolean()},{"active",true}});
        if(diagnostic->profileStage)recordHostEvent("hostAction",{{"action",uint32_t(202)},{"stage",200+diagnostic->profileStage},{"ok",false},{"recovery",true}});
        if(v2Session_)recordDisplayV2Retries(value);
        // v2 resolves a failed resize by restoring its own state file; it never
        // latches the SDK service into viewport recovery.
        const auto display=parseDisplayState(value.get("display"));if(!v2Session_&&nativeDisplay_&&nativeDisplaySelected()&&display.recoveryRequired)nativeDisplay_->cancelViewportSession(id_,true);resizeVerified_=displayCache_.complete(resizeTicket_,display,Clock::now(),value.get("desktopAvailable").boolean())&&std::holds_alternative<bool>(value.get("ok").value)&&value.get("ok").boolean()&&display.ready();readinessChanged_=true;
      }return 1;
    }
    if(!pendingResize_)return type=="stream.resize.failed"?1:0;
    if(!pendingResize_->matches(value))return 1;
    if(type=="stream.info"&&(!resizeVerified_||value.get("sourceWidth").number()!=pendingResize_->width||value.get("sourceHeight").number()!=pendingResize_->height||value.get("fpsTarget").number()!=pendingResize_->fps||(pendingEncoderLimits_&&!pendingEncoderLimits_->matches(value,*pendingResize_))))return -1;
    if(type=="stream.info"){currentEpoch_=pendingResize_->epoch;encoderLimits_=pendingEncoderLimits_;}
    pendingResize_.reset();pendingEncoderLimits_.reset();resizeVerified_=false;resizeDeadline_=0;lastVideo=Clock::now();return type=="stream.resize.failed"?2:0;
  }
  Credentials credentials_; Send send_; std::string id_; std::atomic<DWORD> session_{0xffffffff}; std::atomic<bool> stopped_{true}; std::atomic<HANDLE> pipe_{nullptr}; std::thread worker_; std::mutex writeMutex_; ClipboardChannel clipboard_;PrintRedirection print_;UsbRedirection usb_; std::atomic<bool> streaming_{false};std::recursive_mutex lifecycleMutex_;
  // drive-v1 (docs/drive-redirection.md): its own relay channel, loopback
  // WebDAV endpoint and drive letters, beside (never inside) the worker pipe.
  drive::Redirection drives_{std::make_unique<drive::ChannelLink>(), std::make_unique<drive::WindowsMounter>(), std::chrono::seconds(65), std::make_unique<drive::fs::WinFspMounter>()};
  // channel-resume-v1 (channel_redial.hpp): declared after the channels they dial, so they end before those.
  ChannelPresence presence_; ChannelRedial printRedial_, driveRedial_, usbRedial_;
  // Drives work when WinFsp is installed (ranged file system, driveFs) or the in-box WebClient (WebDAV redirector) exists and is not disabled.
  bool driveCapable(){const auto now=GetTickCount64();if(!driveCheckedAt_||now-driveCheckedAt_>30000){driveFsCapable_=drive::fs::winfspInstalled();driveCapable_=driveFsCapable_||drive::webClientUsable();driveCheckedAt_=now;}return driveCapable_;}
  bool driveFsCapable(){driveCapable();return driveFsCapable_;}
  std::atomic<bool> driveCapable_{false},driveFsCapable_{false};std::atomic<uint64_t> driveCheckedAt_{0};
  std::shared_ptr<signin::broker::SigninBridge> signinRuntime_;std::string signinOwnedSession_;bool ownsSignin_=false,signinWorkerObservation_=false;std::mutex signinWorkerMutex_;WinHandle signinWorkerQuery_;
  CatalogMaintenance* maintenance_=nullptr;std::shared_ptr<CatalogActivity> sessionActivity_;
  NativeDisplayService* nativeDisplay_=nullptr;
  std::optional<ViewportEncoderLimits> encoderLimits_,pendingEncoderLimits_;
  std::atomic<bool> stopping_{false},profileSession_{false};NativeStreamStopFence stopFence_;
  std::atomic<int> pendingBitrate_{0};  // adaptive target for the worker (0 = none)
  std::atomic<int> pendingCaret_{0};  // caret-v1 watch request for the worker (1 on, 2 off, 0 none)
  std::atomic<int> pendingCursorEcho_{0};  // cursor-echo-v1 request for the worker (1 on, 2 off, 0 none)
  std::atomic<int> pendingCursorCompose_{0};  // cursor-compose-v1 request for the worker (1 on, 2 off, 0 none)
  std::atomic<int> pendingScale_{0};  // display-scale-v1 request for the worker (0 = none)
  std::atomic<bool> pendingKeyFrame_{false};  // an IDR for the worker (dropped video)
  std::atomic<bool> pendingCursorResend_{false};  // the worker sends its cursor shapes and state again with that IDR (host-resume-v1)
  std::atomic<std::shared_ptr<secure::ServiceSecure>> secure_;  // Role::secure of the running relay (none between streams)
  // ime-state-v1: the console session's 한/영 mode, read by the session user's helper only when the Broker asks.
  ime::Service ime_{[]{auto access=ime::nativeWatchAccess([]()->std::optional<DWORD>{const DWORD session=interactiveSession();if(!sameInteractiveConsole(session)||!sessionUnlocked(session))return std::nullopt;return session;});access.record=[](uint32_t stage,uint32_t error,uint32_t format){recordHostEvent("hostAction",{{"action",uint32_t(296)},{"stage",stage},{"error",error},{"format",format},{"ok",stage==4}});};return access;}()};
  std::atomic<std::shared_ptr<sysinput::ServiceSystemInput>> systemInput_;  // the running relay's system input (system_input.hpp)
  std::atomic<uint64_t> pendingRecover_{0};  // udp-media-v1 LOSS for the worker: last good PTS + 1 (0 = none)
  std::atomic<uint64_t> pendingDecoder_{0};  // stream-size-v1 client decoder report for the worker (0 = none)
  WinHandle relayWake_{CreateEventW(nullptr,FALSE,FALSE,nullptr)};  // one of the above waits while the relay thread reads the pipe
  std::atomic<bool> workerQuiescent_{true};
  // The stream worker whose exit was not observed within its bounds (wait-only
  // handle) and its session; see workerQuiescentNow().
  std::mutex unexitedMutex_;WinHandle unexitedWorker_;std::string unexitedSession_;
  std::atomic<bool> workerCreatedForSession_{false},sessionHadProfile_{false};
  std::optional<std::string> rejectedStream_;
  bool sessionModeIsStream_=false,nativeStopGuard_=false;std::string nativeStopInstance_;
  struct StoppedSession{std::string session,instance;bool quiescent=false,profile=false,native=false,v2=false;};
  // Display manager v2 state (docs/display-manager-v2.md).
  std::atomic<bool> v2Session_{false},v2Rpc_{false};
  int64_t v2RecoveryNext_=0;std::atomic<unsigned> v2RecoveryAttempts_{0},v2RecoveryIneligible_{0};bool v1Migrated_=false;
  // capabilities(): a probe saw the secure desktop, and a later one saw the desktop again. The saved display
  // state a stop could not restore under the prompt is then restored at once, not at the end of a backoff that
  // grew to a minute while the prompt showed.
  std::atomic<bool> secureDesktopSeen_{false},v2RecoveryRearm_{false};
  // A held Console ended (rearmConsoleActivation): the display probe waits until
  // recoverDisplayV2 has looked at the console's restore point once.
  std::atomic<bool> consoleRestorePending_{false};
  void secureDesktopReturned(bool showing,bool available){
    if(showing)secureDesktopSeen_=true;
    else if(available&&secureDesktopSeen_.exchange(false))v2RecoveryRearm_=true;
  }
  bool v2Mode(){return nativeDisplay_&&nativeDisplay_->displayManagerV2();}
  // What a v2 resize needed (fixed counts, display_v2_tracks.hpp): hostAction
  // 202 stage 410 = session packets kept while an RPC waited (each one used to
  // fail the resize), 411 = SDK size calls asked again, 412 = capture
  // confirmation rounds that re-planned. `ok` is the resize's own result.
  static void recordDisplayV2Retries(const Json& value){
    const auto retries=displayV2RetryDiagnostic(value);const bool ok=value.get("ok").boolean();
    if(retries.deferredPackets)recordHostEvent("hostAction",{{"action",uint32_t(202)},{"stage",uint32_t(410)},{"error",retries.deferredPackets},{"ok",ok}});
    if(retries.sdkRetries)recordHostEvent("hostAction",{{"action",uint32_t(202)},{"stage",uint32_t(411)},{"error",retries.sdkRetries},{"ok",ok}});
    if(retries.confirmRetries)recordHostEvent("hostAction",{{"action",uint32_t(202)},{"stage",uint32_t(412)},{"error",retries.confirmRetries},{"ok",ok}});
  }
  // The state file for a worker that is not an administrator
  // (display_v2_state_rpc.hpp). `authorized` and `session` are the caller's:
  // the one v2 operation this service is running, and the Windows session it
  // started that worker in. Reading needs that session to be the console's
  // active one; writing also needs it unlocked. The owned outputs a saved state
  // may name are the ones this service holds at that moment.
  std::mutex displayV2StateMutex_;
  DisplayV2Dispatch displayV2StatePacket(const Json& value,bool authorized,DWORD session){
    if(value.get("type").string()!="worker.v2.state")return {};
    DisplayV2StateService service;
    service.store=displayV2StateStore();
    service.reader=[&]{return authorized&&v2Mode()&&displayV2StateSession(session,false);};
    service.writer=[&]{return displayV2StateSession(session,true);};
    service.owned=[this]{return nativeDisplayTargets(JsonParser::parse(nativeDisplay_->viewportSnapshot()));};
    std::lock_guard lock(displayV2StateMutex_);
    return dispatchDisplayV2State(value,service);
  }
  DisplayV2Store displayV2StateStore(){
#ifdef MULTITAP_SESSION_STOP_TEST
    if(displayV2StateStoreFixture_)return displayV2StateStoreFixture_();
#endif
    return displayV2FileStore(displayV2StatePath());
  }
  bool displayV2StateSession(DWORD session,bool unlocked){
#ifdef MULTITAP_SESSION_STOP_TEST
    if(displayV2StateSessionFixture_)return displayV2StateSessionFixture_(session,unlocked);
#endif
    return sameInteractiveConsole(session)&&(!unlocked||sessionUnlocked(session));
  }
#ifdef MULTITAP_SESSION_STOP_TEST
  std::function<DisplayV2Store()> displayV2StateStoreFixture_;std::function<bool(DWORD,bool)> displayV2StateSessionFixture_;
#endif
  DisplayV2Dispatch displayV2Packet(const Json& value,bool authorized,DWORD session){
    if(auto state=displayV2StatePacket(value,authorized,session);state.handled)return state;
    return dispatchDisplayV2(value,authorized&&v2Mode(),[this](const std::string& monitor,DWORD width,DWORD height){return nativeDisplay_->setOwnedSizeV2(monitor,width,height);},{},
      [this](std::span<const DisplayV2Output> outputs){std::vector<NativeRuntimeOutput> sizes;for(const auto& o:outputs)sizes.push_back({o.id,o.width,o.height});return nativeDisplay_->setOwnedSizesV2(sizes);});
  }
  static std::optional<DisplayV2State> displayV1ProfileState(const std::string& bytes){
    const auto record=parseNativeStreamProfileRecord(bytes);
    if(record.phase==NativeStreamProfilePhase::restored||!record.profile.suppressedConsole)return {};
    const auto& console=*record.profile.suppressedConsole;
    return DisplayV2State{"v1-migrated",displayV2ActiveCcd(record.profile.before),record.profile.targets,DisplayV2Console{console.sourceAdapter,console.targetAdapter,console.source,console.target}};
  }
  static std::wstring displayV2ArchiveStamp(){SYSTEMTIME now{};GetSystemTime(&now);wchar_t text[40]{};swprintf_s(text,L"%04u%02u%02u-%02u%02u%02u-",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);return std::wstring(text)+randomName().substr(0,8);}
  void performDisplayV2Recovery(DWORD session)noexcept{
    struct Changed{ServiceBridge* bridge;~Changed(){bridge->invalidateDisplay();}}changed{this};
    double outcome=-1,stage=0,error=0;Json detail;
    try{
      std::atomic<bool> cancelled{false};std::mutex writing;bool exited=false;
      UserWorker child(session,cancelled,&exited);
      const std::atomic<int64_t> deadline{streamMonotonicMillis()+30000};
      // About two minutes of attempts: a saved adapter that stayed gone is not coming back.
      const std::string command=v2RecoveryAttempts_.load()>=6+v2RecoveryIneligible_.load()?"{\"type\":\"worker.v2.restore\",\"discardStale\":true}":"{\"type\":\"worker.v2.restore\"}";
      if(pipePacket(child.pipe.value,true,{command.begin(),command.end()},cancelled,writing,&deadline)){
        bool text=false;std::vector<uint8_t> bytes;
        while(!profileRecoveryClosing_&&readPipePacket(child.pipe.value,text,bytes,cancelled,std::chrono::seconds(20),&deadline)){
          if(!text||bytes.size()>65536)break;const auto value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));
          const auto sized=displayV2Packet(value,true,session);if(sized.handled){if(sized.response.empty()||!pipePacket(child.pipe.value,true,{sized.response.begin(),sized.response.end()},cancelled,writing,&deadline))break;continue;}
          if(value.get("type").string()=="worker.v2.restored"){outcome=value.get("outcome").number(-1);stage=value.get("stage").number();error=value.get("error").number();detail=value;}
          break;
        }
      }
    }catch(...){ }
    const bool ok=outcome==0||outcome==1||outcome==2;
    const auto count=[&](const char* key){const auto v=detail.get(key).number();return nativeDisplayInteger(v,0,64)?uint32_t(v):uint32_t(0);};
    recordHostEvent("hostAction",{{"action",uint32_t(214)},{"ok",ok},{"stage",nativeDisplayInteger(stage,0,255)?uint32_t(stage):uint32_t(0)},{"error",nativeDisplayInteger(error,0,UINT32_MAX)?uint32_t(error):uint32_t(0)},{"changed",outcome==2},
      {"expected",count("expected")},{"missing",count("missing")},{"missingOwned",count("missingOwned")},{"fallback",count("fallback")},
      {"consoleExpected",detail.get("consoleExpected").boolean()},{"consoleActive",detail.get("consoleActive").boolean()},
      {"stale",count("stale")},{"remapped",detail.get("remapped").boolean()}});
    // An attempt refused before it looked (stage 1: a UAC prompt on the secure desktop, a lock in between) says
    // nothing about the saved adapters, so it does not count toward giving them up (discardStale). Otherwise
    // about two minutes under a prompt would be enough for that.
    if(ok){v2RecoveryAttempts_=0;v2RecoveryIneligible_=0;}
    else if(stage==double(displayV2Stage::ineligible))++v2RecoveryIneligible_;
    readinessChanged_=true;
  }
  // Each proven extra is proven again from its live node right before it goes.
  // The legacy adapter keeps its own event (hostAction 217, stage = LegacyDisplayRetire);
  // the round is hostAction 273 stage 1: items removed, missing pending a restart,
  // error failed. Any answer but a failure ends the rounds for this service run.
  void performAdapterCleanup(const DisplayAdapterOwnership& owner,const std::vector<DisplayAdapterExtra>& proven)noexcept{
    struct Changed{ServiceBridge* bridge;~Changed(){bridge->invalidateDisplay();}}changed{this};
    auto overall=DisplayAdapterCleanup::none;uint32_t removed=0,pending=0,failed=0;
    for(const auto& extra:proven){
      auto result=DisplayAdapterCleanup::failed;
      try{
        result=removeDisplayAdapter(extra.instance,owner,{extra.kind});
        if(result==DisplayAdapterCleanup::removed){if(extra.kind==DisplayAdapterKind::legacy)clearLegacyDisplayOwnerRecord();else forgetNativeAdapterNode(extra.instance);}
      }catch(...){ }
      if(extra.kind==DisplayAdapterKind::legacy){
        const auto legacy=result==DisplayAdapterCleanup::removed?LegacyDisplayRetire::removed:result==DisplayAdapterCleanup::pending?LegacyDisplayRetire::pending:result==DisplayAdapterCleanup::refused?LegacyDisplayRetire::refused:LegacyDisplayRetire::failed;
        recordHostEvent("hostAction",{{"action",uint32_t(217)},{"ok",legacy==LegacyDisplayRetire::removed},{"stage",uint32_t(legacy)}});
      }
      removed+=result==DisplayAdapterCleanup::removed;pending+=result==DisplayAdapterCleanup::pending;failed+=result==DisplayAdapterCleanup::failed||result==DisplayAdapterCleanup::refused;
      overall=combineDisplayAdapterCleanup(overall,result);
    }
    if(overall!=DisplayAdapterCleanup::failed)adapterCleanupRounds_=3;
    {std::lock_guard lock(adapterMutex_);adapterLastCleanup_=overall;}
    adapterDueNext_=0;  // the next loop observes (and reports) the result
    recordHostEvent("hostAction",{{"action",uint32_t(273)},{"stage",uint32_t(1)},{"ok",!failed&&!pending},{"items",removed},{"missing",pending},{"error",failed}});
    readinessChanged_=true;
  }
  // display-adapters-v1 and the USB controller owner for the Host diagnostics.
  std::string deviceInventoryDiagnostic(){
    std::string adapters;{std::lock_guard lock(adapterMutex_);adapters=adapterInventory_;}
    const auto now=GetTickCount64();usb::ControllerOwner owner;
    {std::lock_guard lock(adapterMutex_);if(!usbOwnerAt_||now-usbOwnerAt_>60000){usbOwner_=usb::controllerOwner();usbOwnerAt_=now;}owner=usbOwner_;}
    return "{\"schema\":1,\"displayAdapters\":"+(adapters.empty()?std::string("{\"read\":false}"):adapters)+",\"usbController\":\""+usb::controllerOwnerName(owner)+"\"}";
  }
  std::optional<StoppedSession> lastSessionStop_;
  std::string profileInstance_,profileOperation_;uint32_t profileGeneration_=0;
  std::mutex profileRecoveryMutex_,profileRecoveryLaunchMutex_;NativeStreamProfileRetryGate profileRetry_;
  std::atomic<unsigned> adapterCleanupRounds_{0};std::atomic<int64_t> adapterDueNext_{0};std::mutex adapterMutex_;std::string adapterInventory_,adapterFingerprint_;DisplayAdapterCleanup adapterLastCleanup_=DisplayAdapterCleanup::none;usb::ControllerOwner usbOwner_=usb::ControllerOwner::unknown;ULONGLONG usbOwnerAt_=0;
  std::thread profileRecoveryWorker_;RecoveryReadiness profileRecoveryRunning_;std::atomic<bool> profileRecoveryClosing_{false};
  int64_t restartProfileNext_=0;std::string restartProfileOperation_;unsigned restartProfileAttempts_=0;
  std::atomic<bool> probing_{false},probeStopped_{false};LoopWakeFlag readinessChanged_;std::thread probe_;std::mutex probeMutex_;DisplayReadinessCache displayCache_;
  struct ConsoleOperation{DisplayOperationKey key;StreamConsolePlan plan;std::atomic<int64_t> deadline{0};std::atomic<bool> cancelled{false};bool qemu=false,finished=false,quiet=false;QemuConsoleResult result;WinHandle unexited;
    QemuConsoleHold hold;DWORD qemuSession=UINT32_MAX;NativeConsoleModelProof qemuProof;std::string qemuLease;bool hadHold=false,rearmChecked=false;
    // Console park (qemu_console_route.hpp): no worker and no lease; the SDK owner is stopped while the hold lasts.
    bool parked=false;};
  std::mutex consoleMutex_;std::shared_ptr<ConsoleOperation> consoleOperation_;std::thread consoleWorker_;std::atomic<bool> consoleRunning_{false};
#ifdef MULTITAP_SESSION_STOP_TEST
  bool qemuAdmissionModel_=false;unsigned qemuProbeAdmissions_=0,qemuStreamAdmissions_=0;
  // The Windows session a console request finds, and this machine's identity, without a real session.
  std::function<QemuConsoleSessionFacts()> qemuFactsFixture_;std::function<std::string()> qemuMachineFixture_;
  std::function<void()> diagnosticProbeInterleave_;
#endif
  bool qemuConsoleQuiescent(uint64_t now=uint64_t(streamMonotonicMillis())){
    bool changed=false,quiet=true;std::shared_ptr<ConsoleOperation> rearm;
    {std::lock_guard lock(consoleMutex_);const auto& op=consoleOperation_;
      if(!op||!op->qemu)return true;
      if(op->hold.expire(now)){op->cancelled=true;op->result.ok=false;op->result.reason="qemu_console_timeout";changed=true;}
      if(!op->finished)return false;
      if(!op->quiet&&op->unexited.value&&WaitForSingleObject(op->unexited.value,0)==WAIT_OBJECT_0){op->quiet=true;op->unexited=WinHandle();changed=true;}
      if(!op->hold.held()&&op->quiet&&op->hadHold&&!op->rearmChecked)rearm=op;
      consoleRunning_=op->hold.held()||!op->quiet||bool(rearm);quiet=op->quiet;
    }
    if(rearm){rearmConsoleActivation(rearm);changed=true;}
    if(changed)invalidateDisplay();return quiet;
  }
  void rearmConsoleActivation(const std::shared_ptr<ConsoleOperation>& operation){
    if(operation->parked){
      // Console park: the SDK owner starts again at its next tick, and its new
      // lease is activated by the ordinary display probe like after a service start.
      if(nativeDisplay_)nativeDisplay_->endConsolePark();
      recordHostEvent("hostAction",{{"action",qemuConsoleParkAction},{"stage",uint32_t(2)},{"ok",true}});
      std::lock_guard lock(consoleMutex_);
      if(consoleOperation_==operation&&!operation->hold.held()&&operation->quiet){operation->rearmChecked=true;consoleRunning_=false;}
      return;
    }
    std::lock_guard probe(probeMutex_);
    // The worker left the topology from before the console as the protected
    // restore point (foreign displays included, qemu_console.hpp). The ordinary
    // v2 recovery restores it now, not after its backoff, and the display probe
    // (whose activation re-adds the owned outputs) waits until it has looked.
    // Set before consoleRunning_ clears below, so no probe slips in between.
    consoleRestorePending_=true;v2RecoveryRearm_=true;
    bool current=false;
    if(nativeDisplay_&&!probing_)try{
      const auto snapshot=nativeDisplayLeaseSnapshot();current=snapshot.get("servicePid").number()==GetCurrentProcessId()&&
        !snapshot.get("configurationPending").boolean()&&nativeDisplayLeaseKey(snapshot)==operation->qemuLease&&
        nativeConsoleUuid(machineUuid())==nativeConsoleUuid(operation->qemuProof.machineUuid);
      if(qemuConsoleActivationRearmAllowed(operation->hadHold,operation->quiet,current,sameInteractiveConsole(operation->qemuSession),probing_))
        (void)nativeActivationGate_.rearmAfterConsole(std::to_string(operation->qemuSession)+":"+operation->qemuLease);
    }catch(...){ }
    std::lock_guard lock(consoleMutex_);
    if(consoleOperation_==operation&&!operation->hold.held()&&operation->quiet){operation->rearmChecked=true;consoleRunning_=false;}
  }
  bool qemuConsoleHolding(){(void)qemuConsoleQuiescent();std::lock_guard lock(consoleMutex_);return consoleOperation_&&consoleOperation_->qemu&&consoleOperation_->hold.held();}
  void pollQemuConsoleCompletion(){(void)qemuConsoleQuiescent();}
  NativeActivationGate nativeActivationGate_;TrackSourceTunnel tracks_;std::atomic<std::shared_ptr<UdpTracksSession>> udpTracks_;
  std::mutex displayOperationMutex_;std::shared_ptr<PendingDisplayOperation> displayOperation_;std::atomic<bool> displayOperationRunning_{false};std::thread displayOperationWorker_;
  void invalidateDisplay(){displayCache_.invalidate();capabilityInputs_.invalidate();probeStopped_=true;readinessChanged_=true;}
  void performExpiredProfileRecovery(const NativeStreamProfileRecord& profile,DWORD workerSession)noexcept{
#ifdef MULTITAP_NATIVE_DISPLAY
    struct Changed{ServiceBridge* bridge;~Changed(){bridge->invalidateDisplay();}}changed{this};
    bool workerExited=false,reserved=false,retired=false;std::atomic<bool> cancelled{false};std::mutex writing;const std::atomic<int64_t> deadline{streamMonotonicMillis()+28000};
    struct Outcome{std::string code="failed";~Outcome(){nativeRecoveryNote(NativeRecoveryField::worker,code);}}outcome;
    try{
      const auto directory=machineDirectory()/L"private";const auto launchBytes=nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json").load();if(!launchBytes){outcome.code="launch_missing";return;}const auto launch=parseNativeStreamLaunch(*launchBytes);if(!nativeStreamLaunchMatches(launch,profile)){outcome.code="launch_mismatch";return;}
      auto oldService=std::make_shared<NativeStreamProcessObservation>(observeNativeStreamProcess(launch.service)),oldWorker=std::make_shared<NativeStreamProcessObservation>(observeNativeStreamProcess(launch.worker));if(oldService->state!=NativeStreamProcessState::gone||oldWorker->state!=NativeStreamProcessState::gone){outcome.code="original_alive";return;}
      {
        outcome.code="worker_unavailable";UserWorker child(workerSession,cancelled,&workerExited);outcome.code="failed";const auto service=nativeStreamProcessIdentity(GetCurrentProcess()),worker=nativeStreamProcessIdentity(child.process.value);
        const auto current=[&](const NativeRecoveryScope& expected){
          if(profileRecoveryClosing_||cancelled||streamMonotonicMillis()>=deadline||!sameInteractiveConsole(workerSession)||!sessionUnlocked(workerSession)||WaitForSingleObject(child.process.value,0)!=WAIT_TIMEOUT||nativeStreamProcessIdentity(child.process.value)!=worker)return false;
          ServiceHandle manager(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT));ServiceHandle registered(OpenServiceW(manager.value,L"MultitapHost",SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG));SERVICE_STATUS_PROCESS state{};DWORD needed=0;const auto executable=brokerServiceExecutable(registered.value);
          if(!QueryServiceStatusEx(registered.value,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&state),sizeof(state),&needed)||state.dwCurrentState!=SERVICE_RUNNING||state.dwProcessId!=service.pid||!brokerSystemProcess(GetCurrentProcess())||!brokerProcessExecutable(GetCurrentProcess(),executable)||!brokerProcessExecutable(child.process.value,executable))return false;
          HANDLE raw=nullptr;if(!OpenProcessToken(child.process.value,TOKEN_QUERY,&raw))return false;WinHandle token(raw);const auto login=nativeSessionRecoveryScope(workerSession),workerLogin=nativeTokenRecoveryScope(token.value,workerSession);return nativeRecoveryLogonMatches(expected,login)&&nativeRecoveryLogonMatches(login,workerLogin);
        };
        const auto processes=[&](const NativeStreamProcessIdentity& expected){
          if(expected==launch.service&&oldService->process.value)return classifyNativeStreamProcess(expected,nativeStreamProcessIdentity(oldService->process.value),WaitForSingleObject(oldService->process.value,0));
          if(expected==launch.worker&&oldWorker->process.value)return classifyNativeStreamProcess(expected,nativeStreamProcessIdentity(oldWorker->process.value),WaitForSingleObject(oldWorker->process.value,0));
          if(expected==service)return classifyNativeStreamProcess(expected,nativeStreamProcessIdentity(GetCurrentProcess()),WaitForSingleObject(GetCurrentProcess(),0));
          if(expected==worker)return classifyNativeStreamProcess(expected,nativeStreamProcessIdentity(child.process.value),WaitForSingleObject(child.process.value,0));return observeNativeStreamProcess(expected).state;
        };
        if(!nativeDisplay_||!current(profile.scope)){outcome.code="identity_unverified";return;}if(!nativeDisplay_->reserveExpiredStreamProfile(profile,worker,processes,current)){outcome.code="reserve_rejected";return;}reserved=true;outcome.code="restore_rejected";
        auto operation=nativeBaselineDigest(utf8(randomName())+profile.key.operation);const auto previous=nativeStreamExpiredConsoleFileAccess(directory/L"native-stream-expired-console.json").load();if(previous){const auto marker=parseNativeStreamExpiredConsoleRecord(*previous);if(marker.profileSha256!=nativeBaselineDigest(nativeStreamProfileRecordBytes(profile))||marker.launchSha256!=nativeBaselineDigest(*launchBytes))throw std::runtime_error("native_stream_profile_recovery_required");operation=marker.operation;}
        const auto command="{\"type\":\"worker.profile.expired.restore\",\"sessionId\":"+jsonQuote(profile.key.stream)+",\"profileOperation\":"+jsonQuote(profile.key.operation)+",\"deviceInstanceId\":"+jsonQuote(profile.key.instance)+",\"journalSha256\":"+jsonQuote(nativeBaselineDigest(nativeStreamProfileRecordBytes(profile)))+",\"operation\":"+jsonQuote(operation)+'}';
        if(!pipePacket(child.pipe.value,true,{command.begin(),command.end()},cancelled,writing,&deadline))throw std::runtime_error("native_stream_profile_recovery_required");
        std::string completion;bool text=false;std::vector<uint8_t> packet;
        for(unsigned exchanges=0;exchanges<64&&current(profile.scope);++exchanges){
          if(!readPipePacket(child.pipe.value,text,packet,cancelled,std::chrono::seconds(4),&deadline)||!text||packet.size()>nativeStreamExpiredRpcLimit)break;const auto bytes=std::string(packet.begin(),packet.end());const auto value=JsonParser::parse(bytes);
          if(value.get("type").string()=="worker.profile.expired.authority"){
            const auto requested=parseNativeStreamExpiredAuthorityRequest(bytes);if(requested.record.operation!=operation)break;const auto proof=nativeDisplay_->expiredStreamProfileAuthority(requested.record,processes,current);if(!proof){outcome.code="restore_authority_lost";break;}const auto reply=nativeStreamExpiredAuthorityReplyBytes({requested.nonce,nativeBaselineDigest(nativeStreamExpiredConsoleRecordBytes(requested.record)),*proof});if(!pipePacket(child.pipe.value,true,{reply.begin(),reply.end()},cancelled,writing,&deadline))break;continue;
          }
          {const auto* fields=std::get_if<Json::Object>(&value.value);nativeStreamJournalRequire(fields&&(fields->size()==5||(fields->size()==6&&fields->contains("reason"))));}
          if(value.get("type").string()=="worker.profile.expired.result"&&std::holds_alternative<bool>(value.get("ok").value)&&!value.get("ok").boolean()){const auto reason=value.get("reason").string();outcome.code=nativeRecoveryCodes(NativeRecoveryField::worker).count(reason)?reason:"restore_rejected";}
          if(value.get("type").string()!="worker.profile.expired.result"||value.get("sessionId").string()!=profile.key.stream||value.get("operation").string()!=operation||!std::holds_alternative<bool>(value.get("ok").value)||!value.get("ok").boolean()||!nativeRecoveryHex(value.get("markerSha256").string()))break;
          const auto marker=nativeStreamExpiredConsoleFileAccess(directory/L"native-stream-expired-console.json").load();if(!marker||nativeBaselineDigest(*marker)!=value.get("markerSha256").string()||parseNativeStreamExpiredConsoleRecord(*marker).phase!="completed")break;completion=value.get("markerSha256").string();break;
        }
        if(completion.empty()||!current(profile.scope))throw std::runtime_error("native_stream_profile_recovery_required");
        outcome.code="observe_failed";const auto nonce=nativeBaselineDigest(utf8(randomName())+operation);const auto observe="{\"type\":\"worker.profile.retire.observe\",\"sessionId\":"+jsonQuote(profile.key.stream)+",\"operation\":"+jsonQuote(profile.key.operation)+",\"deviceInstanceId\":"+jsonQuote(profile.key.instance)+",\"journalSha256\":"+jsonQuote(nativeBaselineDigest(nativeStreamProfileRecordBytes(profile)))+",\"nonce\":"+jsonQuote(nonce)+",\"expiredConsoleSha256\":"+jsonQuote(completion)+'}';
        if(!pipePacket(child.pipe.value,true,{observe.begin(),observe.end()},cancelled,writing,&deadline)||!readPipePacket(child.pipe.value,text,packet,cancelled,std::chrono::seconds(4),&deadline)||!text||packet.size()>65536)throw std::runtime_error("native_stream_profile_recovery_required");
        const auto observed=parseNativeStreamRetirementObservation(JsonParser::parse(std::string(packet.begin(),packet.end())),profile,nonce,GetTickCount64(),true);outcome.code="retire_rejected";retired=nativeDisplay_->retireStreamProfile(profile,worker,observed,processes,current);
      }
      if(retired){outcome.code="complete_rejected";if(nativeDisplay_->completeRetiredStreamProfile(profile.key,workerExited)){reserved=false;outcome.code="succeeded";}}
    }catch(...){ }
    if(reserved&&nativeDisplay_)nativeDisplay_->releaseExpiredStreamProfile(profile.key,workerExited);
#else
    (void)profile;(void)workerSession;
#endif
  }
  void performRestartProfileRecovery(const NativeStreamProfileRecord& profile,bool retirement,DWORD workerSession,const std::string& expiredRetirement={})noexcept{
#ifdef MULTITAP_NATIVE_DISPLAY
    struct Changed{ServiceBridge* bridge;~Changed(){bridge->invalidateDisplay();}}changed{this};
    bool workerExited=false,acknowledged=false,retired=false;std::optional<NativeStreamReclaimCommit> commit;
    NativeStreamStopFence fence;fence.reset(profile.key.stream,true);std::atomic<bool> cancelled{false};std::mutex writing;
    const std::atomic<int64_t> deadline{streamMonotonicMillis()+25000};
    struct Outcome{std::string code="failed";~Outcome(){nativeRecoveryNote(NativeRecoveryField::worker,code);}}outcome;
    try{
      const auto directory=machineDirectory()/L"private";auto sourceBytes=nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json").load();
      if(!sourceBytes&&retirement){auto io=nativeStreamRetirementFileAccess(directory);const auto pending=io.marker.load();if(pending){const auto marker=parseNativeStreamRetirement(*pending);if(marker.phase=="prepared"&&marker.key==profile.key){sourceBytes=io.archive(marker.operation,1);if(sourceBytes&&nativeBaselineDigest(*sourceBytes)!=marker.pins[1]){outcome.code="launch_mismatch";return;}}}}
      if(!sourceBytes){outcome.code="launch_missing";return;}const auto source=parseNativeStreamLaunch(*sourceBytes);if(!nativeStreamLaunchMatches(source,profile)){outcome.code="launch_mismatch";return;}
      auto oldService=std::make_shared<NativeStreamProcessObservation>(observeNativeStreamProcess(source.service)),oldWorker=std::make_shared<NativeStreamProcessObservation>(observeNativeStreamProcess(source.worker));
      if(oldService->state!=NativeStreamProcessState::gone||oldWorker->state!=NativeStreamProcessState::gone){outcome.code="original_alive";return;}
      {
        outcome.code="worker_unavailable";UserWorker child(workerSession,cancelled,&workerExited);outcome.code="identity_unverified";const auto service=nativeStreamProcessIdentity(GetCurrentProcess()),worker=nativeStreamProcessIdentity(child.process.value);const auto expectedLogin=retirement?nativeSessionRecoveryScope(workerSession):profile.scope;
        const auto current=[&](const NativeRecoveryScope& expected){
          if(profileRecoveryClosing_||cancelled||streamMonotonicMillis()>=deadline||!sameInteractiveConsole(workerSession)||!sessionUnlocked(workerSession))return false;
          ServiceHandle manager(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT));ServiceHandle registered(OpenServiceW(manager.value,L"MultitapHost",SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG));
          SERVICE_STATUS_PROCESS state{};DWORD needed=0;const auto executable=brokerServiceExecutable(registered.value);
          if(!QueryServiceStatusEx(registered.value,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&state),sizeof(state),&needed)||state.dwCurrentState!=SERVICE_RUNNING||state.dwProcessId!=service.pid||!brokerSystemProcess(GetCurrentProcess())||!brokerProcessExecutable(GetCurrentProcess(),executable)||!brokerProcessExecutable(child.process.value,executable)||nativeStreamProcessIdentity(child.process.value)!=worker||WaitForSingleObject(child.process.value,0)!=WAIT_TIMEOUT)return false;
          HANDLE raw=nullptr;if(!OpenProcessToken(child.process.value,TOKEN_QUERY,&raw))return false;WinHandle token(raw);const auto login=nativeSessionRecoveryScope(workerSession),workerLogin=nativeTokenRecoveryScope(token.value,workerSession);
          return nativeRecoveryLogonMatches(expected,login)&&nativeRecoveryLogonMatches(login,workerLogin);
        };
        if(!current(expectedLogin))return;
        outcome.code="observe_failed";const auto nonce=nativeBaselineDigest(utf8(randomName())+profile.key.operation);const auto observe="{\"type\":"+jsonQuote(retirement?"worker.profile.retire.observe":"worker.profile.observe")+",\"sessionId\":"+jsonQuote(profile.key.stream)+",\"operation\":"+jsonQuote(profile.key.operation)+",\"deviceInstanceId\":"+jsonQuote(profile.key.instance)+",\"journalSha256\":"+jsonQuote(nativeBaselineDigest(nativeStreamProfileRecordBytes(profile)))+",\"nonce\":"+jsonQuote(nonce)+(expiredRetirement.empty()?std::string():",\"expiredConsoleSha256\":"+jsonQuote(expiredRetirement))+'}';
        if(!pipePacket(child.pipe.value,true,{observe.begin(),observe.end()},cancelled,writing,&deadline))return;
        bool text=false;std::vector<uint8_t> packet;
        if(!readPipePacket(child.pipe.value,text,packet,cancelled,std::chrono::seconds(8),&deadline)||!text||packet.size()>65536)return;
        const auto processes=[&](const NativeStreamProcessIdentity& expected){
          if(expected==source.service&&oldService->process.value)return classifyNativeStreamProcess(expected,nativeStreamProcessIdentity(oldService->process.value),WaitForSingleObject(oldService->process.value,0));
          if(expected==source.worker&&oldWorker->process.value)return classifyNativeStreamProcess(expected,nativeStreamProcessIdentity(oldWorker->process.value),WaitForSingleObject(oldWorker->process.value,0));
          if(expected==service)return classifyNativeStreamProcess(expected,nativeStreamProcessIdentity(GetCurrentProcess()),WaitForSingleObject(GetCurrentProcess(),0));
          if(expected==worker)return classifyNativeStreamProcess(expected,nativeStreamProcessIdentity(child.process.value),WaitForSingleObject(child.process.value,0));
          return observeNativeStreamProcess(expected).state;
        };
        if(retirement){const auto observed=parseNativeStreamRetirementObservation(JsonParser::parse(std::string(packet.begin(),packet.end())),profile,nonce,GetTickCount64(),!expiredRetirement.empty());outcome.code="retire_rejected";retired=nativeDisplay_->retireStreamProfile(profile,worker,observed,processes,current);}
        else{
        const auto observed=parseNativeStreamRestartObservation(JsonParser::parse(std::string(packet.begin(),packet.end())),profile,nonce);
        outcome.code="reclaim_rejected";commit=nativeDisplay_->reclaimStreamProfile(profile,worker,observed,processes,current);if(!commit)return;outcome.code="recover_unacknowledged";
        const auto recover="{\"type\":\"worker.profile.recover\",\"sessionId\":"+jsonQuote(profile.key.stream)+",\"deviceInstanceId\":"+jsonQuote(profile.key.instance)+",\"generation\":"+std::to_string(commit->generation)+",\"journalSha256\":"+jsonQuote(nativeBaselineDigest(nativeStreamProfileRecordBytes(profile)))+",\"operation\":"+jsonQuote(profile.key.operation)+",\"workerExited\":true,\"reclaim\":"+nativeStreamReclaimCommitBytes(*commit)+'}';
        if(!current(profile.scope)||!pipePacket(child.pipe.value,true,{recover.begin(),recover.end()},cancelled,writing,&deadline))return;
        if(readPipePacket(child.pipe.value,text,packet,cancelled,std::chrono::seconds(12),&deadline)&&text&&packet.size()<=4096)acknowledged=fence.acknowledge(JsonParser::parse(std::string(packet.begin(),packet.end())));
        }
      }
      if(retired){outcome.code=nativeDisplay_->completeRetiredStreamProfile(profile.key,workerExited)?"succeeded":"complete_rejected";return;}
      fence.finished(acknowledged&&workerExited);
      if(commit&&fence.released())outcome.code=nativeDisplay_->completeReclaimedStreamProfile(profile.key,*commit,workerExited)?"succeeded":"complete_rejected";
    }catch(...){ }
#else
    (void)profile;(void)retirement;(void)workerSession;(void)expiredRetirement;
#endif
  }
  void completeProfileWorkerExit(bool originalExited,bool startDispatched,bool workerCreated,HANDLE unexited=nullptr)noexcept{
    WinHandle retained(unexited);
    workerCreatedForSession_=workerCreated;
    const bool quiescent=originalExited||(!workerCreated&&!startDispatched);
    {
      // A display manager v2 or non-profile stream keeps a wait handle to a worker
      // that outlived its bounds; workerQuiescentNow() reopens sessions once that
      // exact process has exited. A v1 profile stream keeps its own recovery path.
      std::lock_guard lock(unexitedMutex_);
      const bool retain=!quiescent&&retained.value&&!(profileSession_&&!v2Session_);
      unexitedWorker_=retain?std::move(retained):WinHandle();unexitedSession_=retain?id_:std::string();
      workerQuiescent_=quiescent;
      if(!quiescent)recordHostEvent("hostAction",{{"action",uint32_t(220)},{"stage",uint32_t(1)},{"ok",false},{"changed",retain}});
    }
    stopFence_.finished(originalExited);
    // A v2 stop restored the pre-stream topology; never keep advertising the
    // streaming geometry from the cache.
    if(v2Session_){invalidateDisplay();return;}
    if(!profileSession_)return;
    try{
      if(stopFence_.released()&&nativeDisplay_){nativeDisplay_->releaseStreamProfile(id_);invalidateDisplay();return;}
      if(nativeDisplay_&&nativeDisplay_->releaseUnpreparedStreamProfile({id_,profileOperation_,profileInstance_,session_},profileGeneration_,startDispatched,originalExited||(!workerCreated&&!startDispatched))){invalidateDisplay();return;}
      const NativeStreamProfileRecoveryBinding binding{id_,profileOperation_,profileInstance_,session_,profileGeneration_};
      std::optional<NativeStreamProfileRecoveryBinding> attempt;
      {std::lock_guard lock(profileRecoveryMutex_);if(profileRetry_.arm(binding,originalExited))attempt=profileRetry_.begin(session_,sameInteractiveConsole(session_)&&sessionUnlocked(session_),streamMonotonicMillis());}
      if(attempt)performProfileRecovery(*attempt);
    }catch(...){ }
    invalidateDisplay();
  }
  void scheduleProfileRecovery(DWORD session,bool unlocked){
    if(v2Mode())return;
    std::lock_guard launch(profileRecoveryLaunchMutex_);
    if(privateDiagnosticBusy())return;
    if(profileRecoveryClosing_||profileRecoveryRunning_||!unlocked||!sameInteractiveConsole(session))return;
    if(profileRecoveryWorker_.joinable())profileRecoveryWorker_.join();
    std::optional<NativeStreamProfileRecoveryBinding> attempt;
    {std::lock_guard lock(profileRecoveryMutex_);attempt=profileRetry_.begin(session,true,streamMonotonicMillis());}
    if(!attempt)return;
    profileRecoveryRunning_=true;
    try{profileRecoveryWorker_=std::thread([this,binding=*attempt]{performProfileRecovery(binding);profileRecoveryRunning_=false;});}
    catch(...){profileRecoveryRunning_=false;std::lock_guard lock(profileRecoveryMutex_);profileRetry_.complete(*attempt,true,false,streamMonotonicMillis());}
  }
  void performProfileRecovery(const NativeStreamProfileRecoveryBinding& binding)noexcept{
    bool quiescent=true,restored=false;
    try{NativeStreamStopFence fence;fence.reset(binding.stream,true);recoverProfileWorker(binding,fence,quiescent);restored=fence.released()&&nativeDisplay_&&nativeDisplay_->releaseStreamProfile(binding.stream);}catch(...){ }
    invalidateDisplay();
    std::lock_guard lock(profileRecoveryMutex_);profileRetry_.complete(binding,quiescent,restored,streamMonotonicMillis());
  }
  void recoverProfileWorker(const NativeStreamProfileRecoveryBinding& binding,NativeStreamStopFence& fence,bool& quiescent){
#ifdef MULTITAP_NATIVE_DISPLAY
    if(!nativeDisplay_||!nativeDisplay_->hasStreamProfileReservation()||!sameInteractiveConsole(binding.session)||!sessionUnlocked(binding.session))return;
    // Keep only the existing lease alive while a separate authenticated worker
    // verifies/restores it. This worker never starts capture or new activation.
    NativeStreamProfileCleanupHeartbeat heartbeat;
    if(!heartbeat.start([this]{return nativeDisplay_->refreshStreamProfileLease();}))return;
    const auto current=JsonParser::parse(nativeDisplay_->viewportSnapshot());const auto generation=nativeStreamProfileLease(current,GetCurrentProcessId()).generation;
    if(current.get("deviceInstanceId").string()!=binding.instance||generation<binding.generation)return;
    const auto snapshot=nativeDisplayCleanupLeaseSnapshot(GetCurrentProcessId(),binding.instance,generation);
    const auto lease=nativeStreamProfileLease(snapshot,GetCurrentProcessId());
    const auto store=nativeStreamProfileFileAccess(machineDirectory()/L"private"/L"native-stream-profile.json");
    const auto bytes=store.load();std::string digest,operation;
    if(bytes){
      const auto record=parseNativeStreamProfileRecord(*bytes);
      if(record.key.stream!=binding.stream||record.key.operation!=binding.operation||record.key.instance!=binding.instance||record.key.session!=binding.session)return;
      if(!sameNativeStreamLease(record.current,lease)){
        if(!record.resize)return;const auto directory=machineDirectory()/L"private";const auto receiptBytes=nativeStreamViewportReceiptFileAccess(directory/L"native-stream-viewport-commit.json").load();const auto runtimeBytes=nativeRuntimeFileAccess(directory/L"native-display-runtime.json").load();if(!receiptBytes||!runtimeBytes)return;
        if(!std::visit([&](const auto& receipt){if constexpr(std::is_same_v<std::decay_t<decltype(receipt)>,NativeStreamTrackCommit>){const auto baseline=nativeBaselineFileAccess(directory/L"native-display-baseline.json").load();if(!baseline||!nativeStreamTrackCommitBaselineMatches(receipt,*baseline))return false;}return nativeStreamViewportCommitSource(record,receipt,true)&&sameNativeStreamLease(nativeStreamReceiptAfter(receipt),lease)&&nativeBaselineDigest(*runtimeBytes)==receipt.runtimeSha256;},parseNativeStreamViewportReceipt(*receiptBytes)))return;
      }
      digest=nativeBaselineDigest(*bytes);operation=record.key.operation;
    }
    const auto command="{\"type\":\"worker.profile.recover\",\"sessionId\":"+jsonQuote(binding.stream)+",\"deviceInstanceId\":"+jsonQuote(binding.instance)+",\"generation\":"+std::to_string(generation)+",\"journalSha256\":"+jsonQuote(digest)+",\"operation\":"+jsonQuote(operation)+",\"workerExited\":true}";
    std::atomic<bool> recoveryCancelled{false};std::mutex writing;bool recoveryExited=false,acknowledged=false;
    const std::atomic<int64_t> deadline{streamMonotonicMillis()+20000};
    quiescent=false;
    try{
      UserWorker child(binding.session,recoveryCancelled,&recoveryExited);
      if(!pipePacket(child.pipe.value,true,{command.begin(),command.end()},recoveryCancelled,writing,&deadline))throw std::runtime_error("native_stream_profile_recovery_pipe_failed");
      bool text=false;std::vector<uint8_t> packet;
      if(readPipePacket(child.pipe.value,text,packet,recoveryCancelled,std::chrono::seconds(12),&deadline)&&text&&packet.size()<=4096)
        acknowledged=fence.acknowledge(JsonParser::parse(std::string(packet.begin(),packet.end())));
    }catch(...){quiescent=recoveryExited;return;}
    quiescent=recoveryExited;
    // A restore ACK from a worker that may still execute is insufficient.
    // The original worker exit was already observed before this method ran.
    fence.finished(acknowledged&&recoveryExited);
#else
    (void)binding;(void)fence;(void)quiescent;
#endif
  }
  void probeDisplay(DWORD session,bool unlocked) {
    if(unlocked)bootTimeline().mark(BootStage::logon);
    std::lock_guard gate(probeMutex_);
    // Read before the worker looks: a signal during the probe makes it due again.
    const auto key=displayProbeKey();
    if(privateDiagnosticBusy()||!unlocked||probing_||displayOperationRunning_||consoleRunning_||consoleRestorePending_||profileRecoveryRunning_||(nativeDisplay_&&nativeDisplay_->streamProfilePending())||(maintenance_&&maintenance_->busy())||(!stopped_&&streaming_)||!displayCache_.due(session,key))return;
    if(probe_.joinable())probe_.join();
#ifdef MULTITAP_SESSION_STOP_TEST
    if(diagnosticProbeInterleave_)diagnosticProbeInterleave_();
#endif
    // Publish the SC observation fence before re-reading the diagnostic flag.
    // Its busy=true -> probing read and this probing=true -> busy read cannot
    // both miss each other, even after the earlier admission check passed.
    probing_=true;
    if(privateDiagnosticBusy()){probing_=false;return;}
#ifdef MULTITAP_SESSION_STOP_TEST
    if(qemuAdmissionModel_){++qemuProbeAdmissions_;probing_=false;return;}
#endif
    probeStopped_=false;const auto ticket=displayCache_.begin(session,key);
    probe_=std::thread([this,session,ticket] {
      WorkerDisplayProbeResult result;
      result.display.error="viewport_probe_failed";
      bool activation=false,commandBegan=false,workerExited=false;std::string activationKey;uint32_t activationGeneration=0;
#ifdef MULTITAP_NATIVE_DISPLAY
      try{if(nativeDisplaySelected()){const auto snapshot=nativeDisplayLeaseSnapshot();activationKey=std::to_string(session)+":"+nativeDisplayLeaseKey(snapshot);activationGeneration=DWORD(snapshot.get("generation").number());activation=nativeActivationGate_.begin(activationKey);}}catch(...){ }
#endif
      try {
        UserWorker child(session,probeStopped_,&workerExited);std::mutex write;
        const std::string request=activation?"{\"type\":\"worker.native.activate\"}":"{\"type\":\"worker.probe\"}";
        commandBegan=true;
        if(pipePacket(child.pipe.value,true,{request.begin(),request.end()},probeStopped_,write)){
          result=readWorkerDisplayProbe(child.pipe.value,probeStopped_);
          if(result.desktopAvailable)bootTimeline().mark(BootStage::worker);
        }
      } catch(const WindowsOperationError& error){
        if(activation)result.nativeActivationDiagnostic=NativeActivationDiagnostic{"native_activation_worker_unavailable",error.systemError(),commandBegan,false};
      } catch (...) {}
#ifdef MULTITAP_NATIVE_DISPLAY
      if(activation&&nativeDisplay_)nativeDisplay_->observeActivation(activationGeneration,result.nativeActivationDiagnostic.value_or(NativeActivationDiagnostic{"native_activation_worker_unavailable",0,commandBegan&&!result.nativeActivationAcknowledged,false}));
      if(activation&&result.nativeActivationDiagnostic&&result.nativeActivationDiagnostic->ccd){const auto& detail=*result.nativeActivationDiagnostic->ccd;
        // 265: stage=sample*10+role, error=issue, expected=index, width/height=mode types.
        recordHostEvent("hostAction",{{"action",uint32_t(265)},{"stage",detail.stage},{"error",detail.issue},{"expected",detail.index},{"width",detail.expected},{"height",detail.observed},{"ok",false}});}
#endif
      if(activation)nativeActivationGate_.observed(activationKey,!commandBegan||result.nativeActivationAcknowledged,commandBegan&&(!result.nativeActivationAcknowledged||result.nativeActivationAttempted),result.display.ready());
      if(!activationKey.empty()&&!nativeActivationGate_.readinessAllowed(activationKey)){
        if(nativeDisplay_)nativeDisplay_->activationRecovery(activationGeneration,activation&&workerExited);
        result.display.active=false;result.display.viewportProvisioning=false;result.display.monitorControlSupported=false;
        result.display.recoveryRequired=true;result.display.configurationPending=true;result.display.error="native_topology_recovery_required";
      }
      const bool eligible=result.display.ready()&&result.desktopAvailable&&result.privileged&&sameInteractiveConsole(session);
      bool activationCommitted=false;
      if(eligible&&activation&&nativeDisplay_&&!result.cancelled&&!probeStopped_&&interactiveSession()==session&&sessionUnlocked(session)){
        activationCommitted=nativeDisplay_->activationReady(activationGeneration);
        if(!activationCommitted){result.display.active=false;result.display.recoveryRequired=true;result.display.configurationPending=true;result.display.error="native_topology_recovery_required";}
      }
      // Announced when the report would change (it was after every probe); an
      // activation also moved the gate the report reads, so it always is.
      bool changed=false;
      if(!result.cancelled&&!probeStopped_&&interactiveSession()==session&&sessionUnlocked(session)&&displayCache_.complete(ticket,std::move(result.display),Clock::now(),result.desktopAvailable,activationCommitted,&changed)) {
        if(changed||activation)readinessChanged_=true;if(maintenance_)maintenance_->consider(session,eligible);
      }
      probing_=false;
    });
  }
  static void requireWorker(bool success, const char* reason) { if (!success) throw std::runtime_error(reason); }
  void unavailable(const char* reason, bool stream = true) { if (!send_) return;if(stream)recordHostEvent("hostAction",{{"action",uint32_t(200)},{"stage",uint32_t(50)},{"error",streamPreparationReasonCode(reason)},{"ok",false}}); const std::string message = "{\"type\":" + jsonQuote(stream ? "stream.unavailable" : "session.unavailable") + ",\"sessionId\":" + jsonQuote(id_) + ",\"reason\":" + jsonQuote(reason) + '}'; send_(true, {message.begin(), message.end()}); }
};

struct WorkerDisplayAccess {
  std::function<DisplayState()> probe;
  std::function<bool()> interactive, privileged;
  std::function<DisplayHelperOutput(int)> apply;
  std::function<bool()> nativeProvider=[] { return false; };
};
inline WorkerDisplayAccess nativeWorkerDisplayAccess() {
  DWORD session = 0xffffffff; ProcessIdToSessionId(GetCurrentProcessId(), &session);
  // `privileged`: a count change under display manager v2 needs no more than a
  // stream does (workerDisplayAuthority); v1 and the older helper keep elevated().
  return {workerDisplayProbe, [session] { return sameInteractiveConsole(session) && sessionUnlocked(session) && interactiveDesktopAvailable(); }, [] { return workerDisplayAuthority(); },
    [](int count) { return displayHelper(count == 1 ? L"topology 1" : L"topology 2", 20000); },nativeDisplaySelected};
}
// Display manager v2 count change in the interactive worker: real CCD, the
// service's published lease, the private v2 size/count RPCs on this pipe and
// the protected state file. No console proof reaches a count request, so the
// console is never identified here; it simply keeps its pre-change state.
inline DisplayV2Access windowsDisplayV2CountAccess(HANDLE pipe,std::atomic<bool>& stopped,std::mutex& writeMutex,std::function<bool()> eligible,DWORD rpcTimeoutMs){
  DisplayV2Access access;
#ifdef MULTITAP_NATIVE_DISPLAY
    if(nativeDisplaySelected())access.modeSupported=[](DWORD width,DWORD height){return multitap::vdisplay::ownedOutputModeSupported(width,height);};
#endif
  access.query=[]{return queryNativeCcd();};
  access.configure=windowsNativeCcdConfigure();
  access.owned=[]{return nativeDisplayTargets(nativeDisplayLeaseSnapshot());};
  const auto write=[pipe,&stopped,&writeMutex](const std::string& message){return pipePacket(pipe,true,{message.begin(),message.end()},stopped,writeMutex);};
  const auto read=[pipe,&stopped](DWORD timeout)->std::optional<Json>{bool isText=false;std::vector<uint8_t> bytes;if(!readPipePacket(pipe,isText,bytes,stopped,std::chrono::milliseconds(timeout))||!isText||bytes.size()>65536)return {};return JsonParser::parse(std::string(bytes.begin(),bytes.end()));};
  access.resize=[write,read,rpcTimeoutMs](const std::string& monitor,DWORD width,DWORD height){return displayV2SizeRpc({utf8(randomName()),monitor,width,height},write,read,{},rpcTimeoutMs);};
  access.count=[write,read,rpcTimeoutMs](size_t count){return displayV2CountRpc({utf8(randomName()),int(count)},write,read,{},rpcTimeoutMs);};
  if(nativeDisplayConfirmed())access.retired=legacyRetiredResolver(nativeConsoleAdapterIdentity,legacyOwnerInstanceFromRegistry);
  access.hidden=[](LUID){return true;};
  // As in a stream's worker: only an administrator's worker opens the state file itself.
  access.store=elevated()?displayV2FileStore(displayV2StatePath()):displayV2PipeStore(write,read,{},[rpcTimeoutMs]{return rpcTimeoutMs;},[]{return utf8(randomName());});
  access.eligible=std::move(eligible);
  return access;
}
// Bounded wait (no display change) until GDI/capture lists exactly the owned
// outputs at their SDK sizes, output 0 primary, each one selectable alone.
inline std::optional<DisplayState> displayV2CountCapture(std::span<const NativeDisplayTarget> targets,const std::function<DisplayState()>& probe){
  const auto until=GetTickCount64()+2500;
  for(;;){
    try{
      const auto display=probe();
      if(display.ready()&&display.monitors.size()==targets.size()){
        const auto observed=currentCaptureMonitors();bool all=true;
        for(const auto& target:targets){
          const auto monitor=std::find_if(display.monitors.begin(),display.monitors.end(),[&](const auto& m){return m.id==target.id;});
          if(monitor==display.monitors.end()||monitor->width!=target.width||monitor->height!=target.height||(target.id=="0")!=monitor->primary||selectOwnedCapture(display,target.id,observed).monitors.size()!=1){all=false;break;}
        }
        if(all)return display;
      }
    }catch(...){ }
    if(GetTickCount64()>=until)return {};Sleep(50);
  }
}
inline void configureWorkerDisplays(HANDLE pipe, const Json& request, std::atomic<bool>& stopped, std::mutex& writeMutex, WorkerDisplayAccess access = nativeWorkerDisplayAccess()) {
  const auto key = displayOperationKey(request); const int count = displayOperationCount(request);
  auto display = access.probe();uint32_t nativeStage=0,nativeError=0;std::string v2Fields;
  const auto reply = [&](const char* type, bool ok, const std::string& reason) {
    const auto response = "{\"type\":" + jsonQuote(type) + ',' + displayOperationFields(key) + ",\"ok\":" + (ok ? "true" : "false") + ",\"count\":" + std::to_string(count) + (ok ? "" : ",\"reason\":" + jsonQuote(reason)) + ",\"nativeStage\":"+std::to_string(nativeStage)+",\"nativeError\":"+std::to_string(nativeError)+v2Fields+",\"desktopAvailable\":" + (access.interactive() ? "true" : "false") + ",\"display\":" + workerDisplayJson(display) + '}';
    return pipePacket(pipe, true, {response.begin(), response.end()}, stopped, writeMutex);
  };
  const bool console = access.interactive();
  if (!console || !access.privileged() || !display.monitorControlSupported) {
    reply("worker.display.prepared", false, console ? "display_control_unavailable" : "no_interactive_session"); return;
  }
  if (!reply("worker.display.prepared", true, "")) return;
  bool text = false; std::vector<uint8_t> bytes;
  if (!readPipePacket(pipe, text, bytes, stopped, std::chrono::seconds(5)) || !text || bytes.size() > 4096) return;
  const auto commit = JsonParser::parse(std::string(bytes.begin(), bytes.end()));
  if (!key.matches(commit) || displayOperationCount(commit) != count) return;
  bool ok = false,nativeApplied=false; std::string reason = "operation_cancelled";
  if (commit.get("type").string() == "worker.display.apply") {
    if (!access.interactive() || !access.privileged()) reason = "no_interactive_session";
    else {
      try {
#ifdef MULTITAP_NATIVE_DISPLAY
        if(access.nativeProvider()&&commit.get("displayManager").string()=="v2"){
          // Display manager v2 (docs/display-manager-v2.md, "Output count").
          nativeApplied=true;
          const int64_t remaining=workerResizeDeadline(request,streamMonotonicMillis())-streamMonotonicMillis();
          const auto eligible=[&]{return !stopped&&access.interactive()&&access.privileged();};
          std::optional<DisplayState> confirmed;
          auto v2=windowsDisplayV2CountAccess(pipe,stopped,writeMutex,eligible,5000);
          v2.usable=[&](std::span<const NativeDisplayTarget> targets){confirmed=displayV2CountCapture(targets,access.probe);return confirmed.has_value();};
          DisplayV2Result changed;
          try{
            // Budget: change, then the previous SDK count (RPC) and a restore.
            DisplaySessionV2 session(std::move(v2));
            changed=session.count("count-"+key.id,size_t(count),false,GetTickCount64()+uint64_t(std::max<int64_t>(remaining-15000,1000)),10000);
          }catch(...){changed.ok=false;if(!changed.stage)changed.stage=displayV2Stage::internal;}
          const auto diagnostic=displayV2CountDiagnostic(changed);
          ok=changed.ok;reason=ok?"":"display_configuration_failed";nativeStage=diagnostic.stage;nativeError=diagnostic.error;v2Fields=displayV2CountDiagnosticFields(diagnostic);
          display=ok&&confirmed?*confirmed:access.probe();
        }else if(access.nativeProvider()){
          nativeApplied=true;
          StreamResizeBudget budget;budget.deadline=workerResizeDeadline(request,streamMonotonicMillis());budget.cancelled=[&stopped]{return stopped.load();};
          const auto eligible=[&]{return !stopped&&access.interactive()&&access.privileged();};
          NativeWorkerCountAccess native;native.eligible=eligible;native.probe=access.probe;native.observed=currentCaptureMonitors;native.snapshot=nativeDisplayLeaseSnapshot;native.topology=windowsNativeTopologyAccess(eligible);native.recoveryScope=[](const NativeTopologyBaseline& baseline){return captureNativeRecoveryScope(baseline);};
          native.exchange=[&](const std::string& phase,const std::string& keyValue,bool verified,const std::string& baseline,const StreamResizeBudget& phaseBudget){
            NativeWorkerRpcAccess io;io.write=[&](const std::string& message){return pipePacket(pipe,true,{message.begin(),message.end()},stopped,writeMutex);};
            io.read=[&](DWORD timeout)->std::optional<Json>{bool text=false;std::vector<uint8_t> bytes;if(!readPipePacket(pipe,text,bytes,stopped,std::chrono::milliseconds(timeout))||!text||bytes.size()>65536)return {};return JsonParser::parse(std::string(bytes.begin(),bytes.end()));};
            return nativeWorkerCountRpc({key,count},phase,keyValue,verified,baseline,phaseBudget,io);
          };
          const auto changed=applyNativeWorkerCount({key,count},std::move(native),budget);display=changed.display;ok=changed.ok;reason=changed.reason;nativeStage=changed.failureStage;nativeError=changed.systemError;
        }else
#endif
        {
        const auto applied = access.apply(count);
        reason = applied.timedOut ? "display_timeout" : "display_configuration_failed";
        ok = !applied.timedOut && !applied.exitCode && applied.value.get("topologyApplied").boolean() && !applied.value.get("configurationPending").boolean(true) && applied.value.get("activeDisplayCountRequested").number() == count;
        }
      } catch (...) { reason = "display_configuration_failed"; }
    }
  } else if (commit.get("type").string() != "worker.display.cancel") return;
  if(!nativeApplied)display = access.probe();
  if (!access.interactive()) { ok = false; reason = "no_interactive_session"; }
  else if (ok && (!display.ready() || display.monitors.size() != size_t(count))) { ok = false; reason = "display_count_mismatch"; }
  reply("worker.display.result", ok, reason);
}
struct WorkerCatalogAccess {
  std::function<DisplayHelperOutput(bool)> helper;
  std::function<bool()> interactive,privileged;
};
inline WorkerCatalogAccess nativeWorkerCatalogAccess() {
  DWORD session=0xffffffff;ProcessIdToSessionId(GetCurrentProcessId(),&session);
  return {[](bool readOnly){return displayHelper(readOnly?L"mode-catalog-status":L"mode-catalog",readOnly?10000:85000);},[session]{return sameInteractiveConsole(session)&&sessionUnlocked(session)&&interactiveDesktopAvailable();},elevated};
}
inline void configureWorkerCatalog(HANDLE pipe,const Json& request,std::atomic<bool>& stopped,std::mutex& writing,WorkerCatalogAccess access=nativeWorkerCatalogAccess()) {
  const auto key=displayOperationKey(request);
  const auto deferred=[](const char* reason){DisplayHelperOutput result;result.exitCode=0;result.value=JsonParser::parse("{\"deferred\":true,\"readOnly\":true,\"modeCatalogReady\":false,\"catalogChanged\":false,\"deviceRestartAttempted\":false,\"reason\":"+jsonQuote(reason)+'}');return result;};
  const auto reply=[&](const char* type,const DisplayHelperOutput& helper){const auto message="{\"type\":"+jsonQuote(type)+','+displayOperationFields(key)+",\"interactive\":"+(access.interactive()?"true":"false")+",\"privileged\":"+(access.privileged()?"true":"false")+",\"exitCode\":"+std::to_string(helper.exitCode)+",\"timedOut\":"+(helper.timedOut?"true":"false")+",\"helper\":"+jsonObject(helper.value)+'}';return pipePacket(pipe,true,{message.begin(),message.end()},stopped,writing);};
  auto prepared=!access.interactive()?deferred("interactive_console_required"):!access.privileged()?deferred("administrator_required"):access.helper(true);
  if(!reply("worker.catalog.prepared",prepared)||stopped||!catalogPendingResult(prepared))return;
  bool text=false;std::vector<uint8_t> bytes;
  if(!readPipePacket(pipe,text,bytes,stopped,std::chrono::seconds(5))||!text||bytes.size()>4096)return;
  const auto commit=JsonParser::parse(std::string(bytes.begin(),bytes.end()));if(!key.matches(commit))return;
  if(commit.get("type").string()!="worker.catalog.apply")return;
  const auto result=!access.interactive()?deferred("interactive_console_required"):!access.privileged()?deferred("administrator_required"):access.helper(false);
  reply("worker.catalog.result",result);
}
inline int interactiveWorker(const std::wstring& pipeName, DWORD servicePid) {
  if (pipeName.rfind(L"\\\\.\\pipe\\MultitapHost.Worker.", 0) != 0 || pipeName.size() > 160 || !servicePid) throw std::runtime_error("Invalid worker pipe");
  if (!WaitNamedPipeW(pipeName.c_str(), 8000)) winCheck(FALSE, "Wait for host service pipe");
  WinHandle pipe(CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr)); if (pipe.value == INVALID_HANDLE_VALUE) winCheck(FALSE, "Open host service pipe");
  ULONG peer = 0; winCheck(GetNamedPipeServerProcessId(pipe.value, &peer), "Worker server identity"); if (peer != servicePid) throw std::runtime_error("Unexpected worker server process");
  ServiceHandle manager(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT));ServiceHandle service(OpenServiceW(manager.value,L"MultitapHost",SERVICE_QUERY_STATUS));SERVICE_STATUS_PROCESS status{};DWORD needed=0;winCheck(QueryServiceStatusEx(service.value,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&needed),"Validate registered service process");if(status.dwProcessId!=peer||(status.dwCurrentState!=SERVICE_RUNNING&&status.dwCurrentState!=SERVICE_STOP_PENDING))throw std::runtime_error("Worker peer is not the registered host service");
  const bool cleanupOnly=status.dwCurrentState==SERVICE_STOP_PENDING;
  std::atomic<bool> stopped{false}; std::mutex writeMutex;
  bool bindingText=false;std::vector<uint8_t> bindingBytes;std::string bindingNonce;
  if(!readPipePacket(pipe.value,bindingText,bindingBytes,stopped,std::chrono::seconds(5))||!bindingText||bindingBytes.size()>4096)
    throw std::runtime_error("Worker service binding missing");
  WinHandle boundService=acceptWorkerServiceBinding(JsonParser::parse(std::string(bindingBytes.begin(),bindingBytes.end())),peer,bindingNonce);
  const auto bindingAck=workerServiceBindingAck(nativeStreamProcessIdentity(boundService.value),bindingNonce);
  if(!pipePacket(pipe.value,true,{bindingAck.begin(),bindingAck.end()},stopped,writeMutex))throw std::runtime_error("Worker service binding reply failed");
  Agent worker([](const auto&) {}, nullptr, [&](bool text, std::vector<uint8_t> data) { if (!pipePacket(pipe.value, text, std::move(data), stopped, writeMutex)) stopped = true; });
  std::unique_ptr<ClipboardWorker> clipboard;std::string clipboardSession,logicalMonitor;Json streamCommand;
  std::unique_ptr<NativeStreamProfileWorker> streamProfile;std::string streamSession,initialSession;std::optional<Json> initialPrepared;bool initialAttempted=false,initialGroupPrepared=false,profileRequired=false,cleanupHandled=false,cleanupResult=false;
  [[maybe_unused]] int64_t profileCancelDeadline=0;
  struct ProfileCancelDeadlineReset{int64_t& value;~ProfileCancelDeadlineReset(){value=0;}};
  const auto diagnosticSession=std::make_shared<std::string>();
  const auto preparationFailure=[&,diagnosticSession](uint32_t stage,uint32_t error){try{const auto packet=streamPreparationPacket(*diagnosticSession,stage,error);(void)pipePacket(pipe.value,true,{packet.begin(),packet.end()},stopped,writeMutex);}catch(...){ }};
  worker.captureDiagnostic(preparationFailure);
  // x264 -> MFT fallback (media.hpp EncoderFallback code) as stage 40 of the same fixed record; cleared before `worker` stops its encoders.
  struct EncoderFallbackDiagnostics{explicit EncoderFallbackDiagnostics(std::function<void(uint32_t)> sink){setEncoderFallbackObserver(std::move(sink));}~EncoderFallbackDiagnostics(){setEncoderFallbackObserver({});}}encoderFallbackDiagnostics{[preparationFailure](uint32_t code){preparationFailure(40,code);}};
  // Display manager v2 (worker side): one session object and its protected
  // state file are all the display bookkeeping. The service answers only SDK
  // size requests; CCD changes and verification happen here. The RPC uses its
  // own stop flag so stop/failure restoration can still reach the service.
  std::unique_ptr<DisplaySessionV2> displayV2;std::optional<Json> v2Prepared;bool displayV2Tracks=false;std::optional<DisplayV2SnapshotEvidence> v2TraceSnapshot;
  std::atomic<bool> v2RpcStopped{false};DWORD v2RpcTimeout=15000;
  const auto withV2Trace=[&](std::string response,uint32_t phase,const std::optional<DisplayV2RestoreResult>& result=std::nullopt)noexcept{
    try{
      if(!displayV2||initialSession.empty())return response;
      if(phase==1)v2TraceSnapshot=displayV2->snapshotEvidence();
      if(!v2TraceSnapshot)return response;
      DisplayV2TracePacket packet;packet.phase=phase;packet.snapshot=*v2TraceSnapshot;
      if(phase==2){packet.restore=result;packet.after=displayV2->currentGeometryEvidence();packet.statePreserved=displayV2->savedStatePresentEvidence();}
      return withDisplayV2Trace(std::move(response),initialSession,packet);
    }catch(...){return response;}
  };
  // Session traffic read while a private RPC waits for its reply (the
  // client's lossless opt-in and pings arrive every second): kept and handled
  // by the loop below after the current command, never a reason to abandon the
  // RPC (display_v2_rpc.hpp WorkerPipeBacklog).
  WorkerPipeBacklog backlog;
  // display-scale-v1 (display_scale.hpp): the granted session's Windows scale
  // on its one owned output (each owned output: trackScale below), restored
  // when this worker's session ends.
  DisplayScaleKeeper displayScale(nativeDisplayScaleAccess());
  const auto publishScale=[&](std::optional<DisplayScaleResult> result){
    if(!result||streamSession.empty())return;
    try{const auto packet=displayScaleAppliedPacket(streamSession,*result);(void)pipePacket(pipe.value,true,{packet.begin(),packet.end()},stopped,writeMutex);}catch(...){ }
  };
  const auto applyScale=[&](const Json& command){
    if(command.get("mediaProtocol").string()=="tracks-v1")return;
    try{publishScale(displayScale.stream(command));}catch(...){ }
  };
  // Independent outputs: with display-scale-outputs-v1 the same scale on each
  // owned output at every start/restart; without it they are never scaled and
  // a leftover record of a monitor they stream (a crashed earlier session's)
  // is put back at their start. [start] grants it (the broker's session.start).
  DisplayScaleOutputs trackScale(nativeDisplayScaleAccess());
  const auto publishTrackScale=[&](const std::optional<DisplayScaleOutputResults>& result){
    if(!result||streamSession.empty())return;
    try{const auto packet=displayScaleOutputsAppliedPacket(streamSession,*result);(void)pipePacket(pipe.value,true,{packet.begin(),packet.end()},stopped,writeMutex);}catch(...){ }
  };
  const auto applyTrackScale=[&](const Json& start,const TrackSourcePlan& plan){
    try{std::vector<DisplayScaleOutput> outputs;for(const auto& output:plan.outputs)outputs.push_back({std::to_string(output.media.id),output.capture.monitors.size()==1?output.capture.monitors.front().name:std::wstring()});publishTrackScale(trackScale.stream(start,outputs));}catch(...){ }
  };
  const auto v2Interleave=[&](const Json& value){
    const auto type=value.get("type").string();
    if(type=="clip.packet"){if(clipboard&&value.get("sessionId").string()==clipboardSession)clipboard->message(value.get("payload"),clipboardGenerationNumber(value.get("generation").number()));return true;}
    if(type=="input.reset"){worker.localControl(jsonObject(value));return true;}
    if(type=="input")return true;
    if(type=="session.stop"||type=="session.ended"){stopped=true;return true;}
    return backlog.defer(value);
  };
  const auto displayV2Access=[&](std::optional<NativeConsoleModelProof> proof){
    DWORD ownSession=UINT32_MAX;ProcessIdToSessionId(GetCurrentProcessId(),&ownSession);
    DisplayV2Access access;
#ifdef MULTITAP_NATIVE_DISPLAY
    if(nativeDisplaySelected())access.modeSupported=[](DWORD width,DWORD height){return multitap::vdisplay::ownedOutputModeSupported(width,height);};
#endif
    access.query=[]{return queryNativeCcd();};
    access.configure=windowsNativeCcdConfigure();
    const auto cleanupBinding=std::make_shared<std::pair<std::string,uint32_t>>();
    access.owned=[cleanupBinding]{
      const auto snapshot=nativeDisplayLeaseSnapshot();
      const auto held=nativeDisplayHeldLease(snapshot);
      cleanupBinding->first=held.instance;cleanupBinding->second=held.generation;
      return held.targets;
    };
    // An administrator's worker keeps every file access it had. Any other
    // worker has none of its own: the state file is asked of the service
    // (display_v2_state_rpc.hpp) and the runtime receipt below is not read.
    const bool administrator=elevated();
    access.restoreOwned=[servicePid,cleanupBinding,observe=access.owned,administrator]{
      // A recovery-only worker has not observed a held lease yet. It must
      // first obtain the normal RUNNING proof; STOP_PENDING cannot create one.
      if(cleanupBinding->first.empty())return observe();
      // The receipt is in the administrators-only folder. Without it the
      // ordinary RUNNING proof still serves every stop of a running service;
      // a restore while the service is stopping fails here, keeps the state
      // file and is made by the next service start's recovery.
      if(!administrator)return observe();
      uint32_t generation=cleanupBinding->second;
      // SDK resizes advance the same owner's generation. Use the protected
      // durable runtime receipt, as v1 profile cleanup uses its own journal,
      // before the exact PID/instance/generation cleanup admission below.
      const auto bytes=readNativeRuntimeFile(machineDirectory()/L"private"/L"native-display-runtime.json",trustedMachineFile);
      if(!bytes)throw std::runtime_error("native_display_snapshot_invalid");
      generation=nativeDisplayCleanupRuntimeGeneration(cleanupBinding->first,generation,bytes);
      return nativeDisplayTargets(nativeDisplayCleanupLeaseSnapshot(servicePid,cleanupBinding->first,generation));
    };
    access.poll=[]{Sleep(displayV2StreamPollMs);};
    const auto write=[&](const std::string& message){return pipePacket(pipe.value,true,{message.begin(),message.end()},v2RpcStopped,writeMutex);};
    const auto read=[&](DWORD timeout)->std::optional<Json>{bool isText=false;std::vector<uint8_t> bytes;if(!readPipePacket(pipe.value,isText,bytes,v2RpcStopped,std::chrono::milliseconds(timeout))||!isText||bytes.size()>65536)return {};return JsonParser::parse(std::string(bytes.begin(),bytes.end()));};
    access.resize=[&,write,read](const std::string& monitor,DWORD width,DWORD height){return displayV2SizeRpc({utf8(randomName()),monitor,width,height},write,read,v2Interleave,v2RpcTimeout);};
    // Both outputs of a dual-output stream in one SDK reconfiguration.
    access.sizes=[&,write,read](std::span<const DisplayV2Output> outputs){return displayV2SizesRpc({utf8(randomName()),{outputs.begin(),outputs.end()}},write,read,v2Interleave,v2RpcTimeout);};
    if(proof)access.identifyConsole=[proof=*proof](const NativeCcdData& ccd,std::span<const NativeDisplayTarget> owned)->std::optional<DisplayV2Console>{
      // The existing positive QEMU std-VGA identification; nothing else is
      // ever deactivated. Failure means "deactivate nothing", not "fail".
      const auto active=displayV2ActiveCcd(ccd);
      const auto identity=identifyNativeStreamConsole(proof,machineUuid(),GetSystemMetrics(SM_REMOTESESSION)!=0,active.paths,owned,nativeConsoleAdapterIdentity);
      if(!identity.binding)return {};
      const auto& path=identity.binding->path;return DisplayV2Console{path.sourceAdapter,path.targetAdapter,path.source,path.target};
    };
    // A positively identified retired Multitap legacy output is never saved or
    // restored, but only once the native provider has actually replaced it.
    if(nativeDisplayConfirmed())access.retired=legacyRetiredResolver(nativeConsoleAdapterIdentity,legacyOwnerInstanceFromRegistry);
    access.hidden=[](LUID){return true;};
    access.store=administrator?displayV2FileStore(displayV2StatePath()):displayV2PipeStore(write,read,v2Interleave,[&]{return v2RpcTimeout;},[]{return utf8(randomName());});
    access.eligible=[ownSession]{return sameInteractiveConsole(ownSession)&&sessionUnlocked(ownSession)&&interactiveDesktopAvailable()&&workerDisplayAuthority();};
    return access;
  };
  // The service publishes its lease before answering; Windows may need a
  // moment before GDI/CCD report the new owned primary source. Bounded wait
  // for fresh evidence only (no further display change), then bind capture.
  // The display this worker reports also says whether its clipboard works:
  // a v2 start/resize answer without it advertises "no clipboard" to the
  // broker for the whole session.
  const auto withClipboard=[](DisplayState display){display.clipboardSupported=clipboardRuntimeReady();return display;};
  const auto displayV2Capture=[withClipboard](Json& command,DWORD width,DWORD height)->std::optional<DisplayState>{
    const auto until=GetTickCount64()+2500;
    for(;;){
      try{
        const auto display=nativeDisplayStatus();
        if(bindDisplayV2SingleCapture(command,display,width,height,currentCaptureMonitors()))return withClipboard(display);
      }catch(...){ }
      if(GetTickCount64()>=until)return {};Sleep(displayV2StreamPollMs);
    }
  };
  // Independent outputs: the same bounded wait until GDI/CCD list exactly the
  // requested owned outputs at their sizes and places ("0" primary at the
  // origin) and each one can be captured alone; then bind the track sources.
  const auto displayV2TrackCapture=[withClipboard](const Json& command,const NativeTrackResize& request,std::optional<TrackSourcePlan>& source)->std::optional<DisplayState>{
    const auto until=GetTickCount64()+2500;
    for(;;){
      try{
        const auto display=nativeDisplayStatus();
        if(initialProfileTrackDisplayMatches(display,request,true)){source=bindOwnedTrackSources(command,display,currentCaptureMonitors());return withClipboard(display);}
      }catch(...){source.reset();}
      if(GetTickCount64()>=until)return {};Sleep(50);
    }
  };
#ifdef MULTITAP_NATIVE_DISPLAY
  const auto makeProfileWorker=[&](std::string instance=std::string(),uint32_t generation=0){
    DWORD ownSession=UINT32_MAX;ProcessIdToSessionId(GetCurrentProcessId(),&ownSession);
    const auto binding=std::make_shared<std::pair<std::string,uint32_t>>(std::move(instance),generation);
    NativeStreamProfileWorkerAccess access;
    access.eligible=[&,ownSession](bool restoring){return (restoring||!stopped)&&nativeDisplaySelected()&&sameInteractiveConsole(ownSession)&&sessionUnlocked(ownSession)&&interactiveDesktopAvailable()&&elevated();};
    access.snapshot=[servicePid,binding](bool restoring){
      auto generation=binding->second;
      if(restoring&&!binding->first.empty())try{const auto bytes=nativeStreamProfileFileAccess(machineDirectory()/L"private"/L"native-stream-profile.json").load();if(bytes){const auto profile=parseNativeStreamProfileRecord(*bytes);if(profile.key.instance==binding->first&&profile.current.servicePid==servicePid&&profile.current.generation>=generation)generation=profile.current.generation;}}catch(...){ }
      const auto value=restoring?nativeDisplayCleanupLeaseSnapshot(servicePid,binding->first,generation):nativeDisplayLeaseSnapshot();
      if(binding->first.empty()){binding->first=value.get("deviceInstanceId").string();binding->second=uint32_t(value.get("generation").number());}return value;
    };
    access.topology=windowsNativeTopologyAccess([]{return true;});access.scope=[](const NativeTopologyBaseline& baseline){return captureNativeRecoveryScope(baseline);};
    access.machineUuid=host::machineUuid;access.resolver=nativeConsoleAdapterIdentity;access.stopCapture=[&]{worker.suspendStreaming();};
    access.loginScope=nativeCurrentRecoveryScope;
    access.selectedInstance=[]{return nativeDisplayConfig().get("deviceInstanceId").string();};
    access.unchangedViewport=[&,ownSession](const NativeStreamProfileRecord& profile,const NativeStreamCancelRequest& request,const NativeStreamProfileEvidence& proof)->std::optional<NativeStreamViewportCancelEvidence>{
      try{
        StreamResizeBudget budget;budget.deadline=profileCancelDeadline;budget.cancelled=[&]{return stopped.load();};budget.remaining(1);
        if(!sameInteractiveConsole(ownSession)||!sessionUnlocked(ownSession)||!interactiveDesktopAvailable()||!elevated())return {};
        const auto snapshot=nativeDisplayLeaseSnapshot();if(snapshot.get("deviceInstanceId").string()!=profile.key.instance||!sameNativeStreamLease(nativeStreamProfileLease(snapshot,servicePid),profile.current))return {};
        const auto display=nativeDisplayStatus();const auto observed=currentCaptureMonitors();if(!display.ready()||display.remoteSession||display.monitors.size()!=profile.current.targets.size())return {};for(const auto& target:profile.current.targets)if(selectOwnedCapture(display,target.id,observed).monitors.size()!=1)return {};
        const auto monitor=std::visit([](const auto& value)->std::string{if constexpr(std::is_same_v<std::decay_t<decltype(value)>,StreamResize>)return value.monitor;else return "0";},request);
        const auto baseline=nativeRecoveryBaselineBytes({profile.key.instance,profile.current.generation,captureNativeTopologyBaseline(nativeActiveCcd(proof.topology),profile.current.targets,monitor),proof.scope});
        const std::atomic<int64_t> deadline{budget.deadline};
        NativeWorkerRpcAccess io{
          [&](const std::string& message){return pipePacket(pipe.value,true,{message.begin(),message.end()},stopped,writeMutex,&deadline);},
          [&](DWORD timeout)->std::optional<Json>{bool text=false;std::vector<uint8_t> bytes;if(!readPipePacket(pipe.value,text,bytes,stopped,std::chrono::milliseconds(timeout),&deadline)||!text||bytes.size()>65536)return {};return JsonParser::parse(std::string(bytes.begin(),bytes.end()));},
          [&](const Json& value){const auto type=value.get("type").string();if(type=="clip.packet"){if(clipboard&&value.get("sessionId").string()==clipboardSession)clipboard->message(value.get("payload"),clipboardGenerationNumber(value.get("generation").number()));return true;}if(type=="input.reset"){worker.localControl(jsonObject(value));return true;}if((type=="session.stop"||type=="session.ended")&&value.get("sessionId").string()==profile.key.stream){stopped=true;return false;}return false;}};
        const auto key=nativeDisplayLeaseKey(snapshot);
        const auto response=std::visit([&](const auto& value){if constexpr(std::is_same_v<std::decay_t<decltype(value)>,StreamResize>)return nativeWorkerViewportRpc(value,"worker.native.resize.unchanged",key,true,budget,std::move(io),baseline);else return nativeWorkerTrackViewportRpc(value,"worker.native.tracks.resize.unchanged",key,true,budget,std::move(io),baseline);},request);
        if(!response.received||!response.accepted||response.pending||response.recoveryRequired||response.driverChangeAttempted)return {};
        const auto directory=machineDirectory()/L"private";const auto receipt=nativeStreamViewportReceiptFileAccess(directory/L"native-stream-viewport-commit.json").load(),runtime=nativeRuntimeFileAccess(directory/L"native-display-runtime.json").load();if(!receipt||!runtime)return {};return NativeStreamViewportCancelEvidence{parseNativeStreamViewportCancel(*receipt),*runtime};
      }catch(...){return {};}
    };
    access.viewportCommit=[]()->std::optional<std::pair<NativeStreamViewportCommit,std::string>>{
      try{const auto directory=machineDirectory()/L"private";const auto receipt=nativeStreamViewportCommitFileAccess(directory/L"native-stream-viewport-commit.json").load(),runtime=nativeRuntimeFileAccess(directory/L"native-display-runtime.json").load();if(!receipt||!runtime)return {};return std::pair{parseNativeStreamViewportCommit(*receipt),*runtime};}catch(...){return {};}
    };
    access.trackViewportCommit=[]()->std::optional<NativeStreamTrackCommitEvidence>{
      try{const auto directory=machineDirectory()/L"private";const auto receipt=nativeStreamViewportReceiptFileAccess(directory/L"native-stream-viewport-commit.json").load(),runtime=nativeRuntimeFileAccess(directory/L"native-display-runtime.json").load(),baseline=nativeBaselineFileAccess(directory/L"native-display-baseline.json").load();if(!receipt||!runtime||!baseline)return {};return NativeStreamTrackCommitEvidence{parseNativeStreamTrackCommit(*receipt),*runtime,*baseline};}catch(...){return {};}
    };
    access.expiredConsole=[](const NativeStreamProfileRecord& profile,const NativeStreamRetirementObservation& observed)->std::optional<NativeStreamRetirementExpiredConsoleProof>{
      try{const auto directory=machineDirectory()/L"private";const auto marker=nativeStreamExpiredConsoleFileAccess(directory/L"native-stream-expired-console.json").load();if(!marker)return {};auto launch=nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json").load();if(!launch){auto io=nativeStreamRetirementFileAccess(directory);const auto pending=io.marker.load();if(!pending)return {};const auto held=parseNativeStreamRetirement(*pending);launch=io.archive(held.operation,1);}if(!launch)return {};return nativeExpiredRetirementCompletion(profile,observed,*launch,*marker);}catch(...){return {};}
    };
    const auto serviceProcess=std::make_shared<WinHandle>(duplicateWorkerServiceQueryHandle(boundService.value));
    access.launchProof=[servicePid,serviceProcess,preparationFailure](const NativeStreamProfileRecord& profile,bool recoveryWorker){
      uint32_t failure=51;
      try{const auto bytes=nativeStreamLaunchFileAccess(machineDirectory()/L"private"/L"native-stream-launch.json").load();if(bytes)failure=nativeStreamLaunchWorkerProofFailure(parseNativeStreamLaunch(*bytes),profile,servicePid,nativeStreamProcessIdentity(GetCurrentProcess()),recoveryWorker,serviceProcess->value);}catch(...){failure=52;}
      if(failure)preparationFailure(15,failure);return failure==0;
    };
    // A different service lifetime must prove its committed private reclaim;
    // it cannot borrow the original launch's old service PID or a boolean ACK.
    // Keep the queried service handle alive for all restore validations.
    const auto serviceIdentity=serviceProcess->value?nativeStreamProcessIdentity(serviceProcess->value):NativeStreamProcessIdentity{};
    access.reclaimProof=[serviceProcess,serviceIdentity](const NativeStreamProfileRecord& profile,const NativeStreamProfileEvidence& observed,const Json& receipt){
      try{
        if(!serviceProcess->value||WaitForSingleObject(serviceProcess->value,0)!=WAIT_TIMEOUT||nativeStreamProcessIdentity(serviceProcess->value)!=serviceIdentity)return false;
        const auto directory=machineDirectory()/L"private";
        return nativeStreamReclaimRestorationProof(profile,observed,parseNativeStreamReclaimCommit(receipt),
          nativeStreamReclaimFileAccess(directory/L"native-stream-reclaim.json"),nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json"),
          nativeStreamProfileFileAccess(directory/L"native-stream-profile.json"),nativeRuntimeFileAccess(directory/L"native-display-runtime.json"),nativeDisplayIdentityFileAccess(directory/L"native-display.dpapi"),
          serviceIdentity,nativeStreamProcessIdentity(GetCurrentProcess()),[](const auto& process){return observeNativeStreamProcess(process).state;},GetTickCount64());
      }catch(...){return false;}
    };
    access.restartObservationProof=[serviceProcess,serviceIdentity](const NativeStreamProfileRecord& profile){
      try{
        const auto directory=machineDirectory()/L"private";auto bytes=nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json").load();
        if(!bytes){auto io=nativeStreamRetirementFileAccess(directory);const auto pending=io.marker.load();if(pending){const auto marker=parseNativeStreamRetirement(*pending);if(marker.phase=="prepared"&&marker.key==profile.key){bytes=io.archive(marker.operation,1);if(bytes&&nativeBaselineDigest(*bytes)!=marker.pins[1])return false;}}}
        if(!bytes)return false;const auto launch=parseNativeStreamLaunch(*bytes);
        const auto current=nativeStreamProcessIdentity(GetCurrentProcess());
        return serviceProcess->value&&WaitForSingleObject(serviceProcess->value,0)==WAIT_TIMEOUT&&nativeStreamProcessIdentity(serviceProcess->value)==serviceIdentity&&serviceIdentity!=launch.service&&current!=launch.worker&&
          nativeStreamLaunchMatches(launch,profile)&&observeNativeStreamProcess(launch.service).state==NativeStreamProcessState::gone&&observeNativeStreamProcess(launch.worker).state==NativeStreamProcessState::gone;
      }catch(...){return false;}
    };
    access.retirementRecord=[]{
      try{auto io=nativeStreamRetirementFileAccess(machineDirectory()/L"private");const auto bytes=io.marker.load();if(!bytes)return std::optional<NativeStreamProfileRecord>{};const auto marker=parseNativeStreamRetirement(*bytes);if(marker.phase!="prepared")return std::optional<NativeStreamProfileRecord>{};const auto profile=io.archive(marker.operation,0);if(!profile||nativeBaselineDigest(*profile)!=marker.pins[0])return std::optional<NativeStreamProfileRecord>{};const auto record=parseNativeStreamProfileRecord(*profile);return record.key==marker.key?std::optional(record):std::nullopt;}catch(...){return std::optional<NativeStreamProfileRecord>{};}
    };
    return std::make_unique<NativeStreamProfileWorker>(servicePid,ownSession,nativeStreamProfileFileAccess(machineDirectory()/L"private"/L"native-stream-profile.json"),std::move(access));
  };
#endif
  bool text = false; std::vector<uint8_t> data;
  try{
  while (!stopped) {
    // Session traffic kept during the previous command's RPC wait comes first,
    // in the order it arrived.
    Json command;
    if(auto kept=backlog.next())command=std::move(*kept);
    else{if(!readPipePacket(pipe.value, text, data, stopped)||!text||data.size()>65536)break;command=JsonParser::parse(std::string(data.begin(),data.end()));}
    if(!nativeStreamProfileWorkerCommandAllowed(cleanupOnly,command.get("type").string()))break;
    if(command.get("type").string()=="worker.profile.expired.restore"){
      if(!streamSession.empty()||streamProfile||initialAttempted||cleanupOnly)break;
#ifdef MULTITAP_NATIVE_DISPLAY
      nativeStreamJournalFields(command,6);const auto directory=machineDirectory()/L"private",history=directory/L"native-stream-expired-history";const auto profileBytes=nativeStreamProfileFileAccess(directory/L"native-stream-profile.json").load();if(!profileBytes||nativeBaselineDigest(*profileBytes)!=command.get("journalSha256").string())break;const auto profile=parseNativeStreamProfileRecord(*profileBytes);
      if(profile.key.stream!=command.get("sessionId").string()||profile.key.operation!=command.get("profileOperation").string()||profile.key.instance!=command.get("deviceInstanceId").string()||nativeDisplayConfig().get("deviceInstanceId").string()!=profile.key.instance||!nativeRecoveryHex(command.get("operation").string())||nativeConsoleUuid(machineUuid())!=nativeConsoleUuid(profile.console.proof.machineUuid))break;
      DWORD ownSession=UINT32_MAX;if(!ProcessIdToSessionId(GetCurrentProcessId(),&ownSession))break;
      const auto eligible=[&]{try{return !stopped&&nativeDisplaySelected()&&ownSession==profile.key.session&&sameInteractiveConsole(ownSession)&&sessionUnlocked(ownSession)&&interactiveDesktopAvailable()&&elevated()&&nativeRecoveryLogonMatches(profile.scope,nativeCurrentRecoveryScope());}catch(...){return false;}};
      if(!eligible()||(!CreateDirectoryW(history.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)||!trustedMachineFile(history))break;
      WinHandle serviceProcess=duplicateWorkerServiceQueryHandle(boundService.value);if(!serviceProcess.value)break;const NativeStreamExpiredConsoleExecutor executor{nativeStreamProcessIdentity(serviceProcess.value),nativeStreamProcessIdentity(GetCurrentProcess())};
      auto topology=windowsNativeTopologyAccess(eligible);const std::atomic<int64_t> deadline{streamMonotonicMillis()+22000};
      NativeStreamExpiredConsoleTransactionAccess access;access.marker=nativeStreamExpiredConsoleFileAccess(directory/L"native-stream-expired-console.json");access.profiles=nativeStreamProfileFileAccess(directory/L"native-stream-profile.json");access.launches=nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json");access.history=[history](const std::string& sha){return nativeStreamExpiredConsoleHistoryAccess(history,sha);};
      access.observe=[&]{if(!eligible())throw std::runtime_error("native_stream_expired_console_unverified");const auto ccd=topology.query();NativeTopologyBaseline baseline;baseline.ccd=nativeActiveCcd(ccd);return NativeStreamExpiredConsoleObservation{ccd,captureNativeRecoveryScope(baseline),machineUuid()};};access.identity={nativeConsoleAdapterIdentity,nativeQueryForeignBinding};access.configure=topology.configure;access.now=[]{return GetTickCount64();};access.privateSources=nativeExpiredPrivateSources(directory);access.resolveLedger=resolveNativeStreamExpiredLedger;
      access.authority=[&](const NativeStreamExpiredConsoleAuthorityRequest& requested)->std::optional<NativeStreamExpiredConsoleAuthorityProof>{
        if(!eligible()||requested.executor!=executor||WaitForSingleObject(serviceProcess.value,0)!=WAIT_TIMEOUT)return {};const NativeStreamExpiredAuthorityRequest message{nativeBaselineDigest(utf8(randomName())+requested.record.operation),requested.record,requested.stage};const auto bytes=nativeStreamExpiredAuthorityRequestBytes(message);if(!pipePacket(pipe.value,true,{bytes.begin(),bytes.end()},stopped,writeMutex,&deadline))return {};
        bool isText=false;std::vector<uint8_t> response;if(!readPipePacket(pipe.value,isText,response,stopped,std::chrono::seconds(3),&deadline)||!isText||response.size()>nativeStreamExpiredRpcLimit)return {};const auto reply=parseNativeStreamExpiredAuthorityReply(std::string(response.begin(),response.end()));if(!nativeStreamExpiredAuthorityReplyMatches(reply,message)||!eligible())return {};return reply.proof;
      };
      const auto result=NativeStreamExpiredConsoleTransaction(std::move(access)).restore(command.get("operation").string(),executor);const auto marker=nativeStreamExpiredConsoleFileAccess(directory/L"native-stream-expired-console.json").load();const auto response="{\"type\":\"worker.profile.expired.result\",\"sessionId\":"+jsonQuote(profile.key.stream)+",\"operation\":"+jsonQuote(command.get("operation").string())+",\"ok\":"+(result.ok?"true":"false")+",\"markerSha256\":"+jsonQuote(result.ok&&marker?nativeBaselineDigest(*marker):"")+",\"reason\":"+jsonQuote(result.ok?"succeeded":nativeRecoveryRestoreCode(result.reason))+'}';
      if(!pipePacket(pipe.value,true,{response.begin(),response.end()},stopped,writeMutex)||!result.ok)break;continue;
#else
      break;
#endif
    }
    if(command.get("type").string()=="worker.profile.observe"||command.get("type").string()=="worker.profile.retire.observe"){
      if(!streamSession.empty())break;
#ifdef MULTITAP_NATIVE_DISPLAY
      auto observer=makeProfileWorker();const auto packet=command.get("type").string()=="worker.profile.observe"?observer->observeRestart(command):observer->observeRetirement(command);if(!packet||!pipePacket(pipe.value,true,{packet->begin(),packet->end()},stopped,writeMutex))break;
#else
      break;
#endif
      continue;
    }
    if(command.get("type").string()=="worker.profile.recover"){
      if(!streamSession.empty())break;streamSession=command.get("sessionId").string();profileRequired=true;cleanupHandled=true;
#ifdef MULTITAP_NATIVE_DISPLAY
      if(!nativeDisplayInteger(command.get("generation").number(),1,UINT32_MAX))break;
      streamProfile=makeProfileWorker(command.get("deviceInstanceId").string(),uint32_t(command.get("generation").number()));
      cleanupResult=streamProfile->recover(command);
#endif
      break;
    }
    if(command.get("type").string()=="worker.console.prepare"){
      StreamConsoleWorkerAccess access;
      access.write=[&](const std::string& message){return pipePacket(pipe.value,true,{message.begin(),message.end()},stopped,writeMutex);};
      access.read=[&](DWORD timeout)->std::optional<Json>{bool text=false;std::vector<uint8_t> bytes;if(!readPipePacket(pipe.value,text,bytes,stopped,std::chrono::milliseconds(timeout))||!text||bytes.size()>4096)return {};return JsonParser::parse(std::string(bytes.begin(),bytes.end()));};
      access.binding=ownStreamConsoleBinding;access.observe=[]{return observeConsoleTransfer(virtualDisplayStatus(),nativeDisplaySelected());};
      access.connect=[](DWORD source,DWORD console){wchar_t password[]=L"";if(!WTSConnectSessionW(source,console,password,FALSE)){const auto code=GetLastError();throw WindowsOperationError("WTSConnectSessionW",code);}return true;};access.cancelled=[&]{return stopped.load();};
      prepareStreamConsoleWorker(command,std::move(access));break;
    }
    if(command.get("type").string()=="clip.packet"){if(clipboard&&command.get("sessionId").string()==clipboardSession)clipboard->message(command.get("payload"),clipboardGenerationNumber(command.get("generation").number()));continue;}
    if(command.get("type").string()=="worker.v2.start"){
      if(!streamSession.empty()||streamProfile||initialAttempted||displayV2||cleanupOnly)break;initialAttempted=true;
      const auto source=command.get("sourceCommand");
      std::optional<StreamResize> request;std::optional<NativeConsoleModelProof> proof;int64_t remaining=0;
      try{
        initialSession=source.get("sessionId").string();*diagnosticSession=initialSession;
        if(source.get("type").string()!="session.start"||source.get("mode").string()!="stream"||source.get("mediaProtocol").string()=="tracks-v1"||hasInitialProfileTracks(source))break;
        if(hasInitialViewport(source)){request=initialViewportRequest(source,true);if(!request)break;}
        if(!std::holds_alternative<std::nullptr_t>(source.get("consoleProfile").value))proof=nativeStreamProfileRequest(source);
        remaining=workerResizeDeadline(command,streamMonotonicMillis())-streamMonotonicMillis();
      }catch(...){preparationFailure(17,0);break;}
      profileRequired=proof.has_value();
      DisplayV2Result result;DisplayState display;Json prepared=source;bool ok=false;
      try{
        DWORD width=0,height=0;
        if(request){width=DWORD(request->width);height=DWORD(request->height);}
        else{const auto targets=nativeDisplayTargets(nativeDisplayLeaseSnapshot());width=targets.front().width;height=targets.front().height;}
        worker.suspendStreaming();v2RpcTimeout=15000;
        // media_warmup.hpp: the capture device, Windows Graphics Capture and the
        // loopback endpoint warm up beside the display change (nothing captured).
        worker.warmMedia(source.get("audio").boolean(true));
        displayV2=std::make_unique<DisplaySessionV2>(displayV2Access(proof));
        if(request){streamField(prepared,"requestId",request->requestId);streamField(prepared,"resizeId",request->resizeId);streamField(prepared,"epoch",double(request->epoch));std::get<Json::Object>(prepared.value).erase("initialViewport");}
        // Plain starts keep the broker's command; capture is bound by session.start.
        // The capture proof is part of the change (re-planned when it fails).
        Json bound;std::optional<DisplayState> observed;
        const auto confirm=[&]{bound=prepared;observed=displayV2Capture(bound,width,height);return observed.has_value();};
        result=displayV2->start(initialSession,width,height,proof.has_value(),GetTickCount64()+uint64_t(std::max<int64_t>(remaining-5000,1000)),confirm);
        if(result.ok){
          ok=observed.has_value();if(ok){display=*observed;if(request)prepared=std::move(bound);}
          if(!ok){result.stage=displayV2Stage::verify;result.restore=displayV2->restore();}
        }
      }catch(...){ok=false;if(!result.stage)result.stage=displayV2Stage::internal;if(displayV2&&displayV2->active())result.restore=displayV2->restore();}
      if(!ok){display.active=false;display.viewportProvisioning=false;}
      // Stage 8 carries the fixed foreign-difference code in nativeError.
      const auto fields=",\"ok\":"+std::string(ok?"true":"false")+",\"nativeStage\":"+std::to_string(ok?0u:std::min<uint32_t>(result.stage?result.stage:9u,9u))+",\"nativeError\":"+std::to_string(ok?DWORD(0):result.stage==displayV2Stage::foreign?DWORD(result.foreignDifference):result.error)+",\"consoleRemoved\":"+(result.consoleRemoved?"true":"false")+
        ",\"consoleProof\":"+(result.consoleProof?"true":"false")+",\"consoleIdentified\":"+(result.consoleIdentified?"true":"false")+",\"foreignCount\":"+std::to_string(std::min<uint32_t>(result.foreignBefore,64))+",\"desktopAvailable\":"+(interactiveDesktopAvailable()?"true":"false")+",\"display\":"+workerDisplayJson(display);
      const auto response=withV2Trace(request?"{\"type\":\"worker.initial.result\","+streamResizeFields(*request)+fields+",\"nativeReason\":0,\"unchanged\":false,\"command\":"+jsonObject(prepared)+'}':"{\"type\":\"worker.v2.started\",\"sessionId\":"+jsonQuote(initialSession)+fields+'}',1);
      if(!pipePacket(pipe.value,true,{response.begin(),response.end()},stopped,writeMutex)||!ok)break;
      v2Prepared=prepared;continue;
    }
    if(command.get("type").string()=="worker.v2.tracks.start"){
      // v2 independent outputs (tracks-v1, "모든 모니터"): snapshot -> SDK sizes
      // -> "0" primary, "1" at the client's relative place -> identified
      // console off -> verify -> capture proof. No v1 transaction, profile
      // worker, launch receipt or journal; stop/failure restore the state file.
      if(!streamSession.empty()||streamProfile||initialAttempted||displayV2||cleanupOnly)break;initialAttempted=true;
      const auto source=command.get("sourceCommand");
      std::optional<NativeTrackResize> request;std::optional<NativeConsoleModelProof> proof;int64_t remaining=0;
      try{
        initialSession=source.get("sessionId").string();*diagnosticSession=initialSession;
        if(source.get("type").string()!="session.start"||source.get("mode").string()!="stream"||source.get("mediaProtocol").string()!="tracks-v1")break;
        if(hasInitialProfileTracks(source)){request=initialProfileTrackRequest(source);if(!request||!initialProfileTracksEnabled(nativeDisplayConfig(),nativeDisplaySelected()))break;proof=nativeStreamProfileRequest(source,true);}
        else{if(!std::holds_alternative<std::nullptr_t>(source.get("consoleProfile").value))break;request=nativeInitialTrackViewport(source);}
        remaining=workerResizeDeadline(command,streamMonotonicMillis())-streamMonotonicMillis();
      }catch(...){preparationFailure(17,0);break;}
      profileRequired=proof.has_value();
      DisplayV2Result result;DisplayState display;bool ok=false;
      try{
        worker.suspendStreaming();v2RpcTimeout=15000;
        displayV2=std::make_unique<DisplaySessionV2>(displayV2Access(proof));displayV2Tracks=true;
        const auto outputs=displayV2TrackOutputs(*request);
        std::optional<TrackSourcePlan> bound;std::optional<DisplayState> observed;
        const auto confirm=[&]{bound.reset();observed=displayV2TrackCapture(source,*request,bound);return observed.has_value()&&bound.has_value();};
        result=displayV2->startGroup(initialSession,outputs,proof.has_value(),GetTickCount64()+uint64_t(std::max<int64_t>(remaining-5000,1000)),confirm);
        if(result.ok){
          ok=observed.has_value()&&bound.has_value();if(ok)display=*observed;
          if(!ok){result.stage=displayV2Stage::verify;result.restore=displayV2->restore();}
        }
      }catch(...){ok=false;if(!result.stage)result.stage=displayV2Stage::internal;if(displayV2&&displayV2->active())result.restore=displayV2->restore();}
      if(!ok){display.active=false;display.viewportProvisioning=false;}
      const auto response=withV2Trace(displayV2TracksStartedBytes(initialSession,result,ok,interactiveDesktopAvailable(),workerDisplayJson(display)),1);
      if(!pipePacket(pipe.value,true,{response.begin(),response.end()},stopped,writeMutex)||!ok)break;
      v2Prepared=source;continue;
    }
    if(command.get("type").string()=="worker.qemu-console.prepare"){
      if(!streamSession.empty()||streamProfile||initialAttempted||displayV2||cleanupOnly)break;
      initialAttempted=true;QemuConsoleResult outcome;
      const auto key=displayOperationKey(command);
      try{
        const auto proof=qemuConsoleProof(command,true);const auto lease=command.get("leaseKey").string();
        const auto deadline=workerResizeDeadline(command,streamMonotonicMillis()),remaining=deadline-streamMonotonicMillis();
        if(lease.empty()||lease.size()>2048||remaining<=0||remaining>18000)throw std::runtime_error("qemu_console_cancelled");
        QemuConsoleAccess access;access.display=displayV2Access(proof);access.proof=proof;
        access.machineUuid=[](){return machineUuid();};access.remoteSession=[](){return GetSystemMetrics(SM_REMOTESESSION)!=0;};access.resolver=nativeConsoleAdapterIdentity;
        access.deadline=GetTickCount64()+uint64_t(remaining);
        access.currentLease=[&]{
          if(stopped||WaitForSingleObject(boundService.value,0)!=WAIT_TIMEOUT||streamMonotonicMillis()>=deadline)return false;
          const auto snapshot=nativeDisplayLeaseSnapshot();
          if(snapshot.get("servicePid").number()!=servicePid||snapshot.get("configurationPending").boolean()||nativeDisplayLeaseKey(snapshot)!=lease)return false;
          // Each display mutation needs the current service's exact operation
          // authority, not just a lease sampled before browser cancellation.
          const auto request="{\"type\":\"worker.qemu-console.authority\",\"protocol\":"+jsonQuote(qemuConsoleProtocol)+','+displayOperationFields(key)+",\"leaseKey\":"+jsonQuote(lease)+'}';
          if(!pipePacket(pipe.value,true,{request.begin(),request.end()},stopped,writeMutex))return false;
          bool text=false;std::vector<uint8_t> bytes;const auto wait=deadline-streamMonotonicMillis();
          if(wait<=0||!readPipePacket(pipe.value,text,bytes,stopped,std::chrono::milliseconds(wait))||!text||bytes.size()>4096)return false;
          const auto reply=JsonParser::parse(std::string(bytes.begin(),bytes.end()));const auto* fields=std::get_if<Json::Object>(&reply.value);
          return fields&&fields->size()==5&&reply.get("type").string()=="worker.qemu-console.authority.result"&&reply.get("protocol").string()==qemuConsoleProtocol&&key.matches(reply)&&std::holds_alternative<bool>(reply.get("ok").value)&&reply.get("ok").boolean();
        };
        outcome=prepareQemuConsoleOutput(std::move(access));
      }catch(...){outcome.ok=false;outcome.reason="qemu_console_unavailable";}
      const auto response=qemuConsoleResultPacket(key,outcome.ok,outcome.attempted,false,outcome.reason,"worker.qemu-console.result");
      (void)pipePacket(pipe.value,true,{response.begin(),response.end()},stopped,writeMutex);break;
    }
    if(command.get("type").string()=="worker.v2.restore"){
      // Service-start / crash recovery: put the saved configuration back once.
      if(!streamSession.empty()||streamProfile||initialAttempted||displayV2)break;
      DisplayV2RestoreResult outcome;outcome.outcome=DisplayV2Outcome::failed;
      try{
        v2RpcTimeout=5000;auto access=displayV2Access(std::nullopt);
        const bool discardStale=command.get("discardStale").boolean();access.discardStale=[discardStale]{return discardStale;};
        DisplaySessionV2 recovery(std::move(access));outcome=recovery.restore();
      }catch(...){ }
      const auto response="{\"type\":\"worker.v2.restored\",\"outcome\":"+std::to_string(int(outcome.outcome))+",\"stage\":"+std::to_string(outcome.stage)+",\"error\":"+std::to_string(outcome.error)+
        ",\"expected\":"+std::to_string(outcome.expected)+",\"missing\":"+std::to_string(outcome.missing)+",\"missingOwned\":"+std::to_string(outcome.missingOwned)+",\"fallback\":"+std::to_string(outcome.fallback)+
        ",\"consoleExpected\":"+(outcome.consoleExpected?"true":"false")+",\"consoleActive\":"+(outcome.consoleActive?"true":"false")+
        ",\"stale\":"+std::to_string(outcome.stale)+",\"remapped\":"+(outcome.remapped?"true":"false")+'}';
      (void)pipePacket(pipe.value,true,{response.begin(),response.end()},stopped,writeMutex);break;
    }
    if(command.get("type").string()=="worker.initial.tracks.prepare"){
      if(!streamSession.empty()||streamProfile||initialAttempted||cleanupOnly)break;initialAttempted=true;
#ifdef MULTITAP_NATIVE_DISPLAY
      try{
        nativeStreamJournalFields(command,3);const auto source=command.get("sourceCommand");const auto parsed=initialProfileTrackRequest(source);
        if(!parsed||!initialProfileTracksEnabled(nativeDisplayConfig(),nativeDisplaySelected()))break;const auto request=*parsed;initialSession=request.sessionId;*diagnosticSession=initialSession;
        StreamResizeBudget budget;budget.deadline=workerResizeDeadline(command,streamMonotonicMillis());budget.cancelled=[&]{return stopped.load();};
        const auto interleave=[&](const Json& value){const auto type=value.get("type").string();if(type=="input.reset"){worker.localControl(jsonObject(value));return true;}if((type=="session.stop"||type=="session.ended")&&value.get("sessionId").string()==initialSession){stopped=true;return false;}return backlog.defer(value);};
        auto access=nativeWorkerTrackAccess(pipe.value,request,stopped,writeMutex,budget,interleave);const auto eligible=access.eligible;
        const auto result=applyInitialProfileTracks(source,std::move(access),budget,true);
        // The failure reason as a fixed code ahead of the result (hostAction 200 stage 218).
        if(!result.ok)preparationFailure(18,initialProfileTrackFailureCode(result));
        const auto response=initialProfileTrackResultPacket(request,result,eligible(),jsonObject,workerDisplayJson);
        if(!pipePacket(pipe.value,true,{response.begin(),response.end()},stopped,writeMutex)||!result.ok)break;
        initialPrepared=result.command;initialGroupPrepared=true;continue;
      }catch(...){preparationFailure(16,0);break;}
#else
      break;
#endif
    }
    if(command.get("type").string()=="worker.initial.prepare"){
      if(!streamSession.empty()||streamProfile||initialAttempted||cleanupOnly)break;initialAttempted=true;
#ifdef MULTITAP_NATIVE_DISPLAY
      const auto source=command.get("sourceCommand");const auto parsed=initialViewportRequest(source,true);if(!parsed||!nativeDisplaySelected())break;const auto request=*parsed;initialSession=request.sessionId;*diagnosticSession=initialSession;
      StreamResizeBudget budget;budget.deadline=workerResizeDeadline(command,streamMonotonicMillis());budget.cancelled=[&]{return stopped.load();};DWORD ownSession=UINT32_MAX;if(!ProcessIdToSessionId(GetCurrentProcessId(),&ownSession))break;
      WorkerViewportAccess access{[&](DWORD timeout){budget.remaining(timeout);return nativeDisplayStatus();},currentCaptureMonitors,[ownSession]{return sameInteractiveConsole(ownSession)&&sessionUnlocked(ownSession)&&interactiveDesktopAvailable();},elevated,applyViewport};
      const auto interleave=[&](const Json& value){const auto type=value.get("type").string();if(type=="input.reset"){worker.localControl(jsonObject(value));return true;}if((type=="session.stop"||type=="session.ended")&&value.get("sessionId").string()==initialSession){stopped=true;return false;}return backlog.defer(value);};
      NativeWorkerViewportAccess native{access,nativeDisplayLeaseSnapshot,windowsNativeTopologyAccess([&]{return access.interactive()&&access.privileged()&&!stopped;}),[&](const std::string& phase,const std::string& key,bool verified,const std::string& baseline){return requestNativeWorkerViewport(pipe.value,request,phase,key,verified,budget,stopped,writeMutex,interleave,baseline);}};
      native.recoveryScope=[](const NativeTopologyBaseline& baseline){return captureNativeRecoveryScope(baseline);};
      const auto result=applyInitialNativeViewport(source,std::move(native),budget,true,false,true);
      const auto response="{\"type\":\"worker.initial.result\","+streamResizeFields(request)+",\"ok\":"+(result.ok?"true":"false")+",\"nativeStage\":"+std::to_string(result.nativeStage)+",\"nativeError\":"+std::to_string(result.nativeError)+",\"nativeReason\":"+std::to_string(result.ok?0:result.nativeReason)+",\"unchanged\":"+(!result.ok&&result.unchanged?"true":"false")+",\"restored\":"+(!result.ok&&result.restored?"true":"false")+",\"desktopAvailable\":"+(access.interactive()?"true":"false")+",\"display\":"+workerDisplayJson(result.display)+",\"command\":"+jsonObject(result.command)+'}';
      if(!pipePacket(pipe.value,true,{response.begin(),response.end()},stopped,writeMutex)||!result.ok)break;initialPrepared=result.command;
      continue;
#else
      break;
#endif
    }
    if(command.get("type").string()=="stream.resize") {
      const auto request=streamResizeRequest(command);
      if(displayV2){
        // v2 resize: SDK size + owned primary at the new size. Other displays
        // may move; their active state/mode/refresh/rotation must not change.
        // Any failure restores the saved configuration (the stream then ends).
        int64_t remaining=0;try{remaining=workerResizeDeadline(command,streamMonotonicMillis())-streamMonotonicMillis();}catch(...){remaining=0;}
        // resize_stages.hpp: where this resize spends its time (diagnostics).
        ResizeStages stages;const auto resizeBegan=std::chrono::steady_clock::now();std::chrono::steady_clock::duration confirming{};
        worker.suspendStreaming();v2RpcTimeout=15000;const auto deferredBefore=backlog.deferred;
        const auto suspended=std::chrono::steady_clock::now();stages.suspendMs=resizeStageMs(resizeBegan,suspended);
        DisplayV2Result result;DisplayState display;Json next;bool ok=false;
        try{
          if(remaining>0&&displayV2->active()&&request.monitor=="0"&&request.sessionId==streamSession&&streamCommand.get("resizeProtocol").string()=="epoch-v1"&&request.epoch>streamCommand.get("epoch").number(0)){
            const auto resizeBase=resizeDecoderCommand(streamCommand,command);
            const auto limits=viewportEncoderLimits(resizeBase);
            // The capture proof is part of the change: re-planned and
            // confirmed again when GDI lags or Windows re-lays late.
            std::optional<DisplayState> observed;
            const auto confirm=[&]{
              const auto began=std::chrono::steady_clock::now();
              next=resizeBase;streamField(next,"type",std::string("session.start"));streamField(next,"requestId",request.requestId);streamField(next,"resizeId",request.resizeId);streamField(next,"epoch",double(request.epoch));
              streamField(next,"width",double(limits?limits->width:request.width));streamField(next,"height",double(limits?limits->height:request.height));streamField(next,"fps",double(request.fps));streamField(next,"monitor",std::string("0"));
              observed=displayV2Capture(next,DWORD(request.width),DWORD(request.height));confirming+=std::chrono::steady_clock::now()-began;return observed.has_value();
            };
            result=displayV2->resize(DWORD(request.width),DWORD(request.height),GetTickCount64()+uint64_t(std::max<int64_t>(remaining-5000,1000)),confirm);
            if(result.ok){
              ok=observed.has_value();if(ok)display=*observed;
              if(!ok){result.stage=displayV2Stage::verify;result.restore=displayV2->restore();}
            }
          }else result.stage=displayV2Stage::ineligible;
        }catch(...){ok=false;if(!result.stage)result.stage=displayV2Stage::internal;if(displayV2->active())result.restore=displayV2->restore();}
        if(!ok){try{display=withClipboard(nativeDisplayStatus());}catch(...){display={};}display.viewportProvisioning=false;}
        const auto response="{\"type\":\"worker.resize.result\","+streamResizeFields(request)+",\"ok\":"+(ok?"true":"false")+",\"nativeStage\":"+std::to_string(ok?0u:std::min<uint32_t>(result.stage?result.stage:9u,9u))+",\"nativeError\":"+std::to_string(ok?DWORD(0):result.stage==displayV2Stage::foreign?DWORD(result.foreignDifference):result.error)+",\"nativeReason\":0,\"consoleReturned\":"+(result.consoleReturned?"true":"false")+",\"profileStage\":0,\"desktopAvailable\":"+(interactiveDesktopAvailable()?"true":"false")+displayV2RetryFields(result,backlog.deferred-deferredBefore)+",\"display\":"+workerDisplayJson(display)+'}';
        if(!pipePacket(pipe.value,true,{response.begin(),response.end()},stopped,writeMutex))break;
        if(ok){
          stages.displayMs=resizeStageMs(suspended,std::chrono::steady_clock::now());stages.sdkMs=resizeStageMs(result.sdkMs);stages.settleMs=resizeStageMs(result.settleMs);stages.confirmMs=resizeStageMs(confirming);
          streamCommand=next;worker.localControl(jsonObject(withResizeStages(streamCommand,stages)));applyScale(streamCommand);
        }
        else{
          const bool restored=result.restore&&result.restore->ok();
          const auto failed="{\"type\":\"stream.resize.failed\","+streamResizeFields(request)+",\"reason\":"+jsonQuote(result.modeUnsupported?"unsupported_display_mode":"display_change_failed")+",\"unchanged\":"+(result.modeUnsupported?"true":"false")+",\"rollbackAttempted\":"+(result.restore?"true":"false")+",\"rollbackSucceeded\":"+(restored?"true":"false")+'}';
          if(!pipePacket(pipe.value,true,{failed.begin(),failed.end()},stopped,writeMutex))break;
          // A provider preflight refusal changed neither topology nor limits.
          // Resume the old encoder after reporting the failed request.
          if(result.modeUnsupported){worker.localControl(jsonObject(streamCommand));applyScale(streamCommand);}
        }
        continue;
      }
      if(streamProfile&&!streamProfile->reserveViewport(request)){const auto failed="{\"type\":\"stream.resize.failed\","+streamResizeFields(request)+",\"reason\":\"unsupported_display_mode\",\"unchanged\":true}";if(!pipePacket(pipe.value,true,{failed.begin(),failed.end()},stopped,writeMutex))break;continue;}
      StreamResizeBudget budget;budget.deadline=workerResizeDeadline(command,streamMonotonicMillis());budget.cancelled=[&stopped]{return stopped.load();};profileCancelDeadline=budget.deadline;ProfileCancelDeadlineReset cancelDeadlineReset{profileCancelDeadline};
      worker.suspendStreaming();
      DWORD ownSession=0xffffffff;ProcessIdToSessionId(GetCurrentProcessId(),&ownSession);
      WorkerViewportAccess access{[&stopped](DWORD timeout){return workerViewportProbe(timeout,[&stopped]{return stopped.load();});},currentCaptureMonitors,[ownSession]{return sameInteractiveConsole(ownSession)&&sessionUnlocked(ownSession)&&interactiveDesktopAvailable();},elevated,applyViewport};
      WorkerViewportResult result;bool driverChangePossible=true;
#ifdef MULTITAP_NATIVE_DISPLAY
      if(nativeDisplaySelected()){
        const auto interleave=[&](const Json& value){const auto type=value.get("type").string();if(type=="clip.packet"){if(clipboard&&value.get("sessionId").string()==clipboardSession)clipboard->message(value.get("payload"),clipboardGenerationNumber(value.get("generation").number()));return true;}if(type=="input.reset"){worker.localControl(jsonObject(value));return true;}if(type=="session.stop"||type=="session.ended"){stopped=true;return false;}return backlog.defer(value);};
        NativeWorkerViewportAccess native{access,nativeDisplayLeaseSnapshot,windowsNativeTopologyAccess([&]{return access.interactive()&&access.privileged()&&!stopped;}),[&](const std::string& phase,const std::string& key,bool verified,const std::string& baseline){return requestNativeWorkerViewport(pipe.value,request,phase,key,verified,budget,stopped,writeMutex,interleave,baseline);}};
        native.recoveryScope=[](const NativeTopologyBaseline& baseline){return captureNativeRecoveryScope(baseline);};
        // Native probing uses current PnP/CCD/source names, never the legacy helper.
        native.viewport.probe=[&](DWORD timeout){budget.remaining(timeout);return nativeDisplayStatus();};
        if(streamProfile){native.console=[&](const NativeTopologyBaseline& baseline){return streamProfile->viewportConsole(request,baseline);};native.consoleCurrent=[&](const NativeConsoleBinding& binding){return streamProfile->viewportConsoleCurrent(request,binding);};}
        result=applyNativeWorkerViewport(streamCommand,request,logicalMonitor,std::move(native),budget);
        // Only the native coordinator reports this bit; the legacy helper never does.
        driverChangePossible=result.helper.get("driverChangePossible").boolean(true);
      }else
#endif
      result=applyWorkerViewport(streamCommand,request,logicalMonitor,access,budget);
      if(streamProfile)completeNativeProfileViewport(result,*streamProfile,request,driverChangePossible);
      if(result.ok)result.command=preparedResizeDecoderCommand(streamCommand,result.command,command);
      const auto response="{\"type\":\"worker.resize.result\","+streamResizeFields(request)+",\"ok\":"+(result.ok?"true":"false")+",\"nativeStage\":"+std::to_string(result.nativeStage)+",\"nativeError\":"+std::to_string(result.nativeError)+",\"nativeReason\":"+std::to_string(result.nativeReason)+",\"consoleReturned\":"+(result.consoleReturned?"true":"false")+",\"profileStage\":"+std::to_string(streamProfile?streamProfile->viewportStage():0)+",\"desktopAvailable\":"+(access.interactive()?"true":"false")+",\"display\":"+workerDisplayJson(result.display)+'}';
      if(!pipePacket(pipe.value,true,{response.begin(),response.end()},stopped,writeMutex))break;
      if(result.ok) {streamCommand=result.command;worker.localControl(jsonObject(streamCommand));applyScale(streamCommand);}
      else {
        const auto failed="{\"type\":\"stream.resize.failed\","+streamResizeFields(request)+",\"reason\":"+jsonQuote(result.reason)+",\"rollbackAttempted\":"+(result.helper.get("rollbackAttempted").boolean()?"true":"false")+",\"rollbackSucceeded\":"+(result.helper.get("rollbackSucceeded").boolean()?"true":"false")+'}';
        if(!pipePacket(pipe.value,true,{failed.begin(),failed.end()},stopped,writeMutex))break;
      }
      continue;
    }
    if(command.get("type").string()=="worker.display.prepare") { configureWorkerDisplays(pipe.value, command, stopped, writeMutex); break; }
    if(command.get("type").string()=="worker.catalog.prepare") {configureWorkerCatalog(pipe.value,command,stopped,writeMutex);break;}
    if(command.get("type").string()=="worker.probe"||command.get("type").string()=="worker.native.activate"){
      const bool activation=command.get("type").string()=="worker.native.activate";
      const auto result=activation?bootstrapNativeDisplays(nativeWorkerBootstrapAccess()):NativeBootstrapResult{workerDisplayProbe()};
      // `privileged` tells the service whether this worker may act on the display it reports (workerDisplayAuthority).
      const auto response=workerProbeReply(result.display,interactiveDesktopAvailable(),workerDisplayAuthority(),activation?std::optional<bool>(result.applyAttempted):std::nullopt,result.diagnostic);
      if(!pipePacket(pipe.value,true,{response.begin(),response.end()},stopped,writeMutex))break;continue;
    }
    if(command.get("type").string()=="session.start"){
      if(!streamSession.empty())break;
      // v2: phase 2 must be exactly the command this worker prepared (a tracks
      // start only after worker.v2.tracks.start, a single one only after
      // worker.v2.start).
      if(displayV2){if(!v2Prepared||!initialViewportJsonEqual(*v2Prepared,command)||(command.get("mediaProtocol").string()=="tracks-v1")!=displayV2Tracks){preparationFailure(13,0);break;}v2Prepared.reset();}
      else if(initialAttempted){if(!initialPrepared||!(initialGroupPrepared?initialProfileTrackPhase2Matches(*initialPrepared,command):initialViewportPhase2Matches(*initialPrepared,command))){preparationFailure(13,0);break;}initialPrepared.reset();}
      else if(hasInitialViewport(command)||hasInitialProfileTracks(command)||(command.get("resizeProtocol").string()=="epoch-v1"&&command.get("epoch").number()==1))break;
      streamSession=command.get("sessionId").string();*diagnosticSession=streamSession;profileRequired=!std::holds_alternative<std::nullptr_t>(command.get("consoleProfile").value);
      const auto unavailable=[&]{const std::string error="{\"type\":\"stream.unavailable\",\"sessionId\":"+jsonQuote(streamSession)+",\"reason\":\"virtual_display_required\"}";pipePacket(pipe.value,true,{error.begin(),error.end()},stopped,writeMutex);};
      if(profileRequired&&!displayV2){
#ifdef MULTITAP_NATIVE_DISPLAY
        streamProfile=makeProfileWorker();
        if(!streamProfile->begin(command,command.get("profileOperation").string(),initialGroupPrepared)){preparationFailure(10,streamProfileFailureCode(streamProfile->reason()));if(streamProfile->entrySystemError())preparationFailure(14,streamProfile->entrySystemError());unavailable();break;}
#else
        unavailable();break;
#endif
      }
      const auto display=virtualDisplayStatus();if(!display.ready()){preparationFailure(11,0);unavailable();break;}
      if(streamProfile){const auto ready="{\"type\":\"worker.profile.active\",\"sessionId\":"+jsonQuote(streamSession)+",\"desktopAvailable\":true,\"display\":"+workerDisplayJson(display)+'}';if(!pipePacket(pipe.value,true,{ready.begin(),ready.end()},stopped,writeMutex))break;}
      if(command.get("mediaProtocol").string()=="tracks-v1"){
        if(displayV2){
          // v2 group already applied and verified: bind each owned output to
          // its own track source ("0" primary at the origin).
          try{
            const auto request=nativeInitialTrackViewport(command);if(!initialProfileTrackDisplayMatches(display,request,true))throw std::runtime_error("initial_profile_tracks_invalid");
            auto source=bindOwnedTrackSources(command,display,currentCaptureMonitors());
            clipboardSession=request.sessionId;clipboard=std::make_unique<ClipboardWorker>(clipboardSession,[&](const std::string& payload){if(!pipePacket(pipe.value,true,{payload.begin(),payload.end()},stopped,writeMutex))throw std::runtime_error("Clipboard worker IPC closed");});
            streamCommand=command;logicalMonitor=command.get("monitor").string();applyTrackScale(command,source);worker.localTrackStart(streamCommand,std::move(source));
          }catch(...){preparationFailure(16,0);clipboard.reset();unavailable();break;}
          continue;
        }
        if(initialGroupPrepared){
          try{
            const auto request=nativeInitialTrackViewport(command);if(!streamProfile||!initialProfileTrackDisplayMatches(display,request,true))throw std::runtime_error("initial_profile_tracks_invalid");
            auto source=bindOwnedTrackSources(command,display,currentCaptureMonitors());
            clipboardSession=request.sessionId;clipboard=std::make_unique<ClipboardWorker>(clipboardSession,[&](const std::string& payload){if(!pipePacket(pipe.value,true,{payload.begin(),payload.end()},stopped,writeMutex))throw std::runtime_error("Clipboard worker IPC closed");});
            streamCommand=command;logicalMonitor=command.get("monitor").string();applyTrackScale(command,source);worker.localTrackStart(streamCommand,std::move(source));
          }catch(...){preparationFailure(16,0);clipboard.reset();unavailable();break;}
          continue;
        }
        try{
          const auto request=nativeInitialTrackViewport(command);StreamResizeBudget budget;budget.deadline=workerResizeDeadline(command,streamMonotonicMillis());budget.cancelled=[&stopped]{return stopped.load();};profileCancelDeadline=budget.deadline;ProfileCancelDeadlineReset cancelDeadlineReset{profileCancelDeadline};worker.suspendStreaming();
          clipboardSession=request.sessionId;clipboard=std::make_unique<ClipboardWorker>(clipboardSession,[&](const std::string& payload){if(!pipePacket(pipe.value,true,{payload.begin(),payload.end()},stopped,writeMutex))throw std::runtime_error("Clipboard worker IPC closed");});
          NativeWorkerTrackResult result;std::optional<TrackSourcePlan> source;
#ifdef MULTITAP_NATIVE_DISPLAY
          if(nativeDisplaySelected()){
            const auto interleave=[&](const Json& value){const auto type=value.get("type").string();if(type=="clip.packet"){if(clipboard&&value.get("sessionId").string()==clipboardSession)clipboard->message(value.get("payload"),clipboardGenerationNumber(value.get("generation").number()));return true;}if(type=="input.reset"){worker.localControl(jsonObject(value));return true;}if(type=="session.stop"||type=="session.ended"){stopped=true;return false;}return backlog.defer(value);};
            result=applyNativeWorkerTracks(command,request,nativeWorkerTrackAccess(pipe.value,request,stopped,writeMutex,budget,interleave),budget,true);
            if(result.ok)try{source=bindOwnedTrackSources(result.command,result.display,currentCaptureMonitors());}catch(...){result.ok=false;result.display.active=false;result.display.recoveryRequired=true;result.display.error="native_topology_recovery_required";}
          }
#endif
          const auto response="{\"type\":\"worker.tracks.resize.result\","+nativeTrackResizeFields(request)+",\"ok\":"+(result.ok?"true":"false")+",\"desktopAvailable\":"+(interactiveDesktopAvailable()?"true":"false")+",\"display\":"+workerDisplayJson(result.display)+'}';
          if(!pipePacket(pipe.value,true,{response.begin(),response.end()},stopped,writeMutex))break;
          if(!result.ok||!source){clipboard.reset();unavailable();break;}
          streamCommand=result.command;logicalMonitor=streamCommand.get("monitor").string();applyTrackScale(command,*source);worker.localTrackStart(streamCommand,std::move(*source));
        }catch(...){clipboard.reset();unavailable();break;}
        continue;
      }
      if(command.get("monitor").string().empty()&&!display.monitors.empty())streamField(command,"monitor",display.monitors.front().id);
      CaptureSelection selection;try{selection=selectOwnedCapture(display,command.get("monitor").string(),currentCaptureMonitors());}catch(...){preparationFailure(12,0);unavailable();continue;}const RECT area=selection.bounds;
      auto& object=std::get<Json::Object>(command.value);const auto number=[&](const char* key,double value){Json child;child.value=value;object[key]=child;};number("captureLeft",area.left);number("captureTop",area.top);number("captureWidth",area.right-area.left);number("captureHeight",area.bottom-area.top);Json device;device.value=selection.monitors.size()==1?utf8(selection.monitors[0].name):"";object["displayDeviceName"]=device;
      Json::Array selectedDisplays;for(const auto& monitor:selection.monitors){Json item;Json::Object fields;const auto field=[&](const char* key,double value){Json child;child.value=value;fields[key]=child;};field("left",monitor.bounds.left);field("top",monitor.bounds.top);field("right",monitor.bounds.right);field("bottom",monitor.bounds.bottom);Json name;name.value=utf8(monitor.name);fields["name"]=name;Json identity;identity.value=utf8(monitor.identity);fields["identity"]=identity;item.value=std::move(fields);selectedDisplays.push_back(std::move(item));}Json captures;captures.value=std::move(selectedDisplays);object["captureDisplays"]=std::move(captures);
      clipboardSession=command.get("sessionId").string();clipboard=std::make_unique<ClipboardWorker>(clipboardSession,[&](const std::string& payload){if(!pipePacket(pipe.value,true,{payload.begin(),payload.end()},stopped,writeMutex))throw std::runtime_error("Clipboard worker IPC closed");});
      streamCommand=command;logicalMonitor=command.get("monitor").string();
    }
    if(command.get("type").string()=="stream.tracks.resize"){
      if(displayV2){
        // v2 group resize: SDK sizes + the group at the client's new places,
        // console kept off. Other displays may move; their active state/mode/
        // refresh/rotation must not change. The capture proof is part of the
        // change. Last resort when it still cannot be applied: a profile track
        // stream puts its previous group back in place and continues there
        // (stream.tracks.resize.restored, accepted by the broker and released
        // clients). Only when that fails too is the saved configuration
        // restored and the stream ended.
        const auto request=nativeTrackResizeRequest(command);
        int64_t remaining=0;try{remaining=workerResizeDeadline(command,streamMonotonicMillis())-streamMonotonicMillis();}catch(...){remaining=0;}
        worker.suspendStreaming();v2RpcTimeout=15000;const auto deferredBefore=backlog.deferred;
        DisplayV2Result result;DisplayState display;Json next;std::optional<TrackSourcePlan> source;bool ok=false;std::optional<NativeTrackResize> restored;
        try{
          const auto current=nativeInitialTrackViewport(streamCommand);
          if(remaining>0&&displayV2Tracks&&displayV2->active()&&request.sessionId==streamSession&&streamCommand.get("mediaProtocol").string()=="tracks-v1"&&nativeTrackResizeFollows(current,request)){
            std::optional<DisplayState> observed;
            const auto resizeBase=resizeDecoderCommand(streamCommand,command);
            const auto confirmFor=[&](const NativeTrackResize& target)->DisplaySessionV2::Confirm{return [&,target]{next=displayV2TrackCommand(nativeSameTrackResize(target,request)?resizeBase:streamCommand,target);source.reset();observed=displayV2TrackCapture(next,target,source);return observed.has_value()&&source.has_value();};};
            const bool fallback=profileRequired;const auto start=GetTickCount64();const auto budget=uint64_t(std::max<int64_t>(remaining-5000,1000));
            // A fallback keeps 20 s of the budget for putting the previous group back.
            result=displayV2->resizeGroup(displayV2TrackOutputs(request),start+(fallback&&budget>30000?budget-20000:budget),confirmFor(request),fallback);
            if(result.ok){ok=observed.has_value()&&source.has_value();if(ok)display=*observed;else{result.stage=displayV2Stage::verify;result.restore=displayV2->restore();}}
            else if(fallback&&displayV2->active()){
              const auto back=profileTrackRestoredRequest(current,request);
              auto reverted=displayV2->resizeGroup(displayV2TrackOutputs(back),start+budget,confirmFor(back),false);
              if(reverted.ok&&observed&&source){restored=back;display=*observed;}
              else{result.restore=std::move(reverted.restore);if(displayV2->active())result.restore=displayV2->restore();}
            }
          }else result.stage=displayV2Stage::ineligible;
        }catch(...){ok=false;restored.reset();if(!result.stage)result.stage=displayV2Stage::internal;if(displayV2->active())result.restore=displayV2->restore();}
        if(!ok&&!restored){try{display=withClipboard(nativeDisplayStatus());}catch(...){display={};}display.viewportProvisioning=false;}
        const auto response=displayV2TrackResizeResultBytes(request,result,ok,interactiveDesktopAvailable(),workerDisplayJson(display),backlog.deferred-deferredBefore);
        if(!pipePacket(pipe.value,true,{response.begin(),response.end()},stopped,writeMutex))break;
        if(ok){streamCommand=next;applyTrackScale(streamCommand,*source);worker.localTrackStart(streamCommand,std::move(*source));}
        else if(restored){
          const auto packet=profileTrackRestoredPacket(*restored,"display_change_failed");if(!pipePacket(pipe.value,true,{packet.begin(),packet.end()},stopped,writeMutex))break;
          streamCommand=next;applyTrackScale(streamCommand,*source);worker.localTrackStart(streamCommand,std::move(*source));
        }
        else{const auto failed=nativeTrackResizeFailure(request,"display_change_failed");if(!pipePacket(pipe.value,true,{failed.begin(),failed.end()},stopped,writeMutex))break;}
        continue;
      }
      if(streamProfile){
        const auto request=nativeTrackResizeRequest(command);StreamResizeBudget budget;budget.deadline=workerResizeDeadline(command,streamMonotonicMillis());budget.cancelled=[&]{return stopped.load();};profileCancelDeadline=budget.deadline;ProfileCancelDeadlineReset cancelDeadlineReset{profileCancelDeadline};
        DWORD ownSession=UINT32_MAX;ProcessIdToSessionId(GetCurrentProcessId(),&ownSession);
        ProfileTrackResizeAccess access;access.eligible=[&]{return !stopped&&sameInteractiveConsole(ownSession)&&sessionUnlocked(ownSession)&&interactiveDesktopAvailable()&&elevated();};access.reserve=[&](const auto& value){return streamProfile->reserveTracks(value);};access.stop=[&]{worker.suspendStreaming();};access.complete=[&](const auto& value,bool applied){return streamProfile->completeTracks(value,applied);};access.probe=workerDisplayProbe;access.observed=currentCaptureMonitors;
        access.apply=[&]{
          const auto interleave=[&](const Json& value){const auto type=value.get("type").string();if(type=="clip.packet"){if(clipboard&&value.get("sessionId").string()==clipboardSession)clipboard->message(value.get("payload"),clipboardGenerationNumber(value.get("generation").number()));return true;}if(type=="input.reset"){worker.localControl(jsonObject(value));return true;}if(type=="session.stop"||type=="session.ended"){stopped=true;return false;}return backlog.defer(value);};
          auto native=nativeWorkerTrackAccess(pipe.value,request,stopped,writeMutex,budget,interleave);
          native.console=[&](const NativeTopologyBaseline& baseline){return streamProfile->viewportConsole(request,baseline);};native.consoleCurrent=[&](const NativeConsoleBinding& binding){return streamProfile->viewportConsoleCurrent(request,binding);};
          return applyNativeWorkerTracks(streamCommand,request,std::move(native),budget);
        };
        auto result=changeProfileTracks(streamCommand,request,std::move(access));
        if(result.changed&&result.source){result.command=preparedResizeDecoderCommand(streamCommand,result.command,command);result.source=bindOwnedTrackSources(result.command,result.viewport.display,currentCaptureMonitors());}
        const auto response="{\"type\":\"worker.tracks.resize.result\","+nativeTrackResizeFields(request)+",\"ok\":"+(result.changed?"true":"false")+",\"desktopAvailable\":"+(interactiveDesktopAvailable()?"true":"false")+",\"display\":"+workerDisplayJson(result.viewport.display)+'}';
        if(!pipePacket(pipe.value,true,{response.begin(),response.end()},stopped,writeMutex))break;
        if(result.changed&&result.source){streamCommand=result.command;applyTrackScale(streamCommand,*result.source);worker.localTrackStart(streamCommand,std::move(*result.source));continue;}
        if(result.restored&&result.source&&result.applied){
          const auto restored=profileTrackRestoredPacket(*result.applied,result.reason);if(!pipePacket(pipe.value,true,{restored.begin(),restored.end()},stopped,writeMutex))break;
          streamCommand=result.command;applyTrackScale(streamCommand,*result.source);worker.localTrackStart(streamCommand,std::move(*result.source));continue;
        }
        const auto failed=nativeTrackResizeFailure(request,result.reason);if(!pipePacket(pipe.value,true,{failed.begin(),failed.end()},stopped,writeMutex))break;break;
      }
      if(streamCommand.get("mediaProtocol").string()=="tracks-v1"&&command.get("sessionId").string()==streamCommand.get("sessionId").string()){
        const auto request=nativeTrackResizeRequest(command);StreamResizeBudget budget;budget.deadline=workerResizeDeadline(command,streamMonotonicMillis());budget.cancelled=[&stopped]{return stopped.load();};profileCancelDeadline=budget.deadline;ProfileCancelDeadlineReset cancelDeadlineReset{profileCancelDeadline};worker.suspendStreaming();
        DWORD ownSession=0xffffffff;ProcessIdToSessionId(GetCurrentProcessId(),&ownSession);const auto eligible=[&]{return !stopped&&sameInteractiveConsole(ownSession)&&sessionUnlocked(ownSession)&&interactiveDesktopAvailable()&&elevated();};
        NativeWorkerTrackResult result;std::optional<TrackSourcePlan> source;
#ifdef MULTITAP_NATIVE_DISPLAY
        if(nativeDisplaySelected()){
          const auto interleave=[&](const Json& value){const auto type=value.get("type").string();if(type=="clip.packet"){if(clipboard&&value.get("sessionId").string()==clipboardSession)clipboard->message(value.get("payload"),clipboardGenerationNumber(value.get("generation").number()));return true;}if(type=="input.reset"){worker.localControl(jsonObject(value));return true;}if(type=="session.stop"||type=="session.ended"){stopped=true;return false;}return backlog.defer(value);};
          result=applyNativeWorkerTracks(streamCommand,request,nativeWorkerTrackAccess(pipe.value,request,stopped,writeMutex,budget,interleave),budget);
          if(result.ok)try{result.command=preparedResizeDecoderCommand(streamCommand,result.command,command);source=bindOwnedTrackSources(result.command,result.display,currentCaptureMonitors());}catch(...){result.ok=false;result.display.active=false;result.display.recoveryRequired=true;result.display.error="native_topology_recovery_required";}
        }
#endif
        const auto response="{\"type\":\"worker.tracks.resize.result\","+nativeTrackResizeFields(request)+",\"ok\":"+(result.ok?"true":"false")+",\"desktopAvailable\":"+(eligible()?"true":"false")+",\"display\":"+workerDisplayJson(result.display)+'}';
        if(!pipePacket(pipe.value,true,{response.begin(),response.end()},stopped,writeMutex))break;
        if(result.ok&&source){streamCommand=result.command;applyTrackScale(streamCommand,*source);worker.localTrackStart(streamCommand,std::move(*source));}
        else{const auto failed=nativeTrackResizeFailure(request,result.reason);if(!pipePacket(pipe.value,true,{failed.begin(),failed.end()},stopped,writeMutex))break;}
      }continue;
    }
    if(command.get("type").string()=="session.stop"||command.get("type").string()=="session.ended"){if(command.get("sessionId").string()==streamSession||(streamSession.empty()&&command.get("sessionId").string()==initialSession))break;continue;}
    // display-scale-v1: a runtime request of this worker's session.
    if(command.get("type").string()=="display.scale"){if(const auto percent=displayScaleMessage(command);percent&&!streamSession.empty()&&command.get("sessionId").string()==streamSession)try{if(trackScale.granted())publishTrackScale(trackScale.request(*percent));else publishScale(displayScale.request(*percent));}catch(...){ }continue;}
    worker.localControl(jsonObject(command));
    if(command.get("type").string()=="session.start")applyScale(streamCommand);
  }
  }catch(...){ }
  clipboard.reset();worker.suspendStreaming();try{(void)displayScale.restore();}catch(...){ }try{(void)trackScale.restore();}catch(...){ }bool restored=cleanupHandled?cleanupResult:(streamProfile?streamProfile->finish():!profileRequired);
  // v2 stop/failure: restore the saved configuration (idempotent; no state
  // file means nothing to do). A failure leaves the file for the service's
  // automatic recovery; it never blocks a later stream.
  std::optional<DisplayV2RestoreResult> v2RestoreOutcome;
  if(displayV2){v2RpcTimeout=5000;try{v2RestoreOutcome=displayV2->restore();restored=v2RestoreOutcome->ok();}catch(...){restored=false;}}
  if(streamSession.empty())streamSession=initialSession;
  if(!streamSession.empty()){const auto response=withV2Trace(nativeStreamStoppedPacket(streamSession,profileRequired,restored),2,v2RestoreOutcome);std::atomic<bool> cleanupStopped{false};(void)pipePacket(pipe.value,true,{response.begin(),response.end()},cleanupStopped,writeMutex);}
  stopped=true;worker.stop(); return 0;
}

#ifdef MULTITAP_NATIVE_DISPLAY
// One administrator reset conversation (native_recovery_reset.hpp). The
// console step is answered by the authenticated elevated settings process in
// the active console session; every other decision is made here.
inline std::string performNativeRecoveryReset(HANDLE pipe,const Json& request,ServiceBridge* bridge,NativeDisplayService& native){
  std::vector<NativeResetStep> steps;size_t moved=0;bool ok=false;
  NativeResetOrchestration access;
  access.busy=[&]{return bridge?bridge->nativeResetBusy():(nativeResetRdpSessionActive()?std::string("rdp_session_active"):std::string());};
  access.preflight=[&]{return native.resetPreflight();};
  access.console=[&]()->std::optional<NativeResetConsoleResult>{if(!nativeResetSend(pipe,"{\"type\":\"native.reset.console\"}"))return {};const auto reply=nativeResetReceive(pipe,20000);if(!reply)return {};return parseNativeResetConsoleReply(*reply);};
  access.commit=[&]{return native.resetCommit();};
  try{
    if(!nativeDisplaySelected()){steps.push_back({"sdk","not_selected",false});}
    else{const auto run=[&]{return runNativeRecoveryReset(request,access,steps,moved);};ok=bridge?bridge->underNativeResetLocks(run):run();}
  }catch(...){ok=false;steps.push_back({"service","protocol",false});}
  if(ok&&bridge)bridge->nativeResetCompleted();
  recordHostEvent("hostAction",{{"action",uint32_t(210)},{"stage",uint32_t(steps.size())},{"ok",ok},{"changed",moved>0}});
  return nativeResetResultJson(ok,steps,moved);
}
#endif
inline std::wstring serviceName = L"MultitapHost"; inline HANDLE serviceStopEvent = nullptr, serviceChangeEvent = nullptr; inline SERVICE_STATUS_HANDLE serviceStatusHandle = nullptr; inline SERVICE_STATUS serviceStatus{}; inline bool qaService = false;inline ServiceConfigurationChanges serviceConfigurationChanges;
inline std::mutex serviceStatusMutex;inline std::atomic<bool> serviceStopRequested{false};
inline std::mutex sessionEventsMutex;inline std::vector<SessionEvent> sessionEvents;
// Each status report and each WTS session notification is counted at once
// (signal_cache.hpp): the capability report's kept SCM status and session view
// are read again, before the service loop has handled the change.
inline void reportService(DWORD state, DWORD error = NO_ERROR, bool checkpoint = false) { std::lock_guard statusLock(serviceStatusMutex);if(state==SERVICE_RUNNING&&serviceStopRequested.load())return;serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS; serviceStatus.dwCurrentState = state; serviceStatus.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE : 0; serviceStatus.dwWin32ExitCode = error; serviceStatus.dwWaitHint = state == SERVICE_STOP_PENDING ? 60000 : 0;if(state==SERVICE_STOP_PENDING){if(checkpoint||!serviceStatus.dwCheckPoint)++serviceStatus.dwCheckPoint;}else serviceStatus.dwCheckPoint=0; SetServiceStatus(serviceStatusHandle, &serviceStatus); noteHostServiceStatusReported(); }
inline DWORD WINAPI serviceHandler(DWORD control, DWORD eventType, void* eventData, void*) { if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) { serviceStopRequested=true;service_workers::closeCurrentAdmission();if (auto* marker = serviceRunMarker.load()) marker->stopping(); reportService(SERVICE_STOP_PENDING); SetEvent(serviceStopEvent); } else if (control == SERVICE_CONTROL_SESSIONCHANGE) {noteHostSessionChange();DWORD session=0xffffffff;const auto* notification=static_cast<const WTSSESSION_NOTIFICATION*>(eventData);if(notification&&notification->cbSize>=sizeof(WTSSESSION_NOTIFICATION))session=notification->dwSessionId;{std::lock_guard lock(sessionEventsMutex);if(sessionEvents.size()<64)sessionEvents.push_back({eventType,session});else sessionEvents.back()={eventType,0xffffffff};}SetEvent(serviceChangeEvent);} else if(serviceConfigurationChanges.request(control))SetEvent(serviceChangeEvent); return NO_ERROR; }
inline void WINAPI serviceMain(DWORD, LPWSTR*) {
  // boot-ready-v1 (boot_timeline.hpp): its stages count from here.
  auto& boot=bootTimeline();
  serviceStopRequested=false;service_workers::ServiceLifetime physicalWorkers;
  NativeDisplayService nativeDisplay;
  std::shared_ptr<signin::broker::SigninBridge> signinRuntime;
  RenderObservation renderObservation(queryGpuEngineSamples,consoleCompositorPid);
  autologon::WindowsAutologonStartupOwner windowsAutologonRecovery;
  serviceStatusHandle = RegisterServiceCtrlHandlerExW(serviceName.c_str(), serviceHandler, nullptr); if (!serviceStatusHandle) return; reportService(SERVICE_START_PENDING);
  // Controls 128/129 for SYSTEM and Administrators only (service_control_security.hpp).
  if(!qaService)(void)restrictServiceControls(serviceName.c_str());
  std::unique_ptr<service_workers::StopPendingHeartbeat> stopHeartbeat;
  try{stopHeartbeat=std::make_unique<service_workers::StopPendingHeartbeat>([]{reportService(SERVICE_STOP_PENDING,NO_ERROR,true);});}catch(...){physicalWorkers.closeAdmission();reportService(SERVICE_STOPPED,ERROR_NOT_ENOUGH_MEMORY);return;}
  operationalLogEnabled=!qaService;
  // hostAction 275 (process_exit_record.hpp): how the previous run ended, then
  // this run's marker; an unhandled exception notes its code before Windows ends us.
  std::optional<ServiceRunMarker> runMarker;
  if(!qaService)try{const auto folder=machineDirectory()/L"private";if(trustedMachineFile(folder)){runMarker.emplace(folder/L"service-run.bin");
    if(runMarker->open()){if(runMarker->previous())recordProcessExit(*runMarker->previous());serviceRunMarker=&*runMarker;serviceRunPreviousFilter()=SetUnhandledExceptionFilter(serviceRunCrashFilter);}}}catch(...){ }
  struct RunMarkerRelease{~RunMarkerRelease(){if(serviceRunMarker.exchange(nullptr))SetUnhandledExceptionFilter(serviceRunPreviousFilter());}}runMarkerRelease;
  // Host config `displayManager`: "v2" (default) or "v1". Read once per service
  // lifetime so the SDK owner and the bridge always agree.
  {const bool v2=!qaService&&displayManagerV2Selected();nativeDisplay.setDisplayManagerV2(v2);recordHostEvent("hostAction",{{"action",uint32_t(218)},{"ok",true},{"changed",v2}});}
  serviceStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); serviceChangeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!serviceStopEvent || !serviceChangeEvent) { reportService(SERVICE_STOPPED, GetLastError()); return; }
  if(serviceStopRequested.load()){physicalWorkers.closeAdmission();SetEvent(serviceStopEvent);}
  BrokerConnectionStatus brokerStatus;std::unique_ptr<BrokerStatusServer> brokerStatusServer;std::unique_ptr<CatalogMaintenance> catalogMaintenance;std::unique_ptr<ServiceBridge> bridge; std::unique_ptr<Agent> agent;std::unique_ptr<PendingConnectionWorker> pendingConnection;
  NativeStreamProfileCleanupHeartbeat profileCleanupHeartbeat;
  // print-v1: printers/ports/documents a crashed run left (joined at exit).
  auto printSweep=std::make_unique<PrintLeftoverSweep>();
  // usb-v1: ports a crashed run left attached (a short, bounded sweep at start).
  auto usbSweep=std::make_unique<UsbLeftoverSweep>();
  // Guards bridge replacement against the administrator reset conversation.
  std::mutex bridgeLifetime;
#ifdef MULTITAP_NATIVE_DISPLAY
  std::unique_ptr<NativeRecoveryResetServer> resetServer;
  std::unique_ptr<signin::diagnostic::ServiceRuntime> privateDiagnostics;
  const auto closePrivateDiagnostics=[&]()noexcept{
    if(!privateDiagnostics)return;
    // First private trial retains the complete owner until cleanup settles.
    // Normal packages do not start this listener. Bounded product shutdown
    // requires the separate durable handoff producer AND consumer gate.
    SetEvent(serviceStopEvent);reportService(SERVICE_STOP_PENDING);
    bool pendingRecorded=false;uint64_t reported=0;
    for(;;){
      const auto now=GetTickCount64();
      if(now-reported>=1000){reportService(SERVICE_STOP_PENDING,NO_ERROR,true);reported=now;}
      if(!privateDiagnostics->closeInlet()){Sleep(50);continue;}
      try{
        std::lock_guard lifetime(bridgeLifetime);
        const auto cleanup=[&]{(void)privateDiagnostics->close();return true;};
        if(bridge){bridge->underNativeResetLocks(cleanup);bridge->diagnosticActivityChanged();}else cleanup();
        if(!privateDiagnostics->coordinator().busy()){
          if(pendingRecorded)recordHostEvent("serviceState",{{"running",true},{"pending",false},{"recovery",false}});
          privateDiagnostics.reset();return;
        }
      }catch(...){ }
      if(!pendingRecorded){recordHostEvent("serviceState",{{"running",true},{"pending",true},{"recovery",true}});pendingRecorded=true;}
      Sleep(50);
    }
  };
#endif
  try { if constexpr(signin::signinBackendCompiled){if(!qaService)try{signinRuntime=std::make_shared<signin::broker::SigninBridge>(nativeDisplay,serviceStopEvent);}catch(...){ /* Optional capability stays unavailable; ordinary Host startup continues. */ }} WSADATA data{}; if (WSAStartup(MAKEWORD(2,2), &data)) throw std::runtime_error("Socket initialization failed");if(!qaService){brokerStatusServer=std::make_unique<BrokerStatusServer>(brokerStatus);if(!nativeDisplaySelected())catalogMaintenance=std::make_unique<CatalogMaintenance>(runServiceCatalog);}
#ifdef MULTITAP_NATIVE_DISPLAY
    if(!qaService)resetServer=std::make_unique<NativeRecoveryResetServer>([&](HANDLE pipe,const Json& request){std::lock_guard lock(bridgeLifetime);return performNativeRecoveryReset(pipe,request,bridge.get(),nativeDisplay);},nativeResetClientAllowed);
#endif
    reportService(SERVICE_RUNNING);recordHostEvent("serviceState",{{"running",1}});
#if defined(MULTITAP_NATIVE_DISPLAY) && defined(MULTITAP_PRIVATE_SIGNIN_DIAGNOSTIC_TRIAL)
    // Private BuildOnly trial. Normal packages keep this listener disabled
    // while real cleanup/shutdown acceptance remains unqualified.
    // A missing/mismatched packaged Tool pin leaves the
    // inlet absent; it never grants public sign-in capability/readiness.
    if(!qaService&&nativeDisplaySelected()&&nativeDisplay.displayManagerV2()){
      if(auto pin=signin::diagnostic::loadInstalledToolPin()){
        privateDiagnostics=std::make_unique<signin::diagnostic::ServiceRuntime>(nativeDisplay,serviceStopEvent,
          signin::native::processIdentity(GetCurrentProcess()),std::move(*pin),
          [&](signin::diagnostic::Coordinator& coordinator,signin::diagnostic::VerifiedOperator&& peer,const signin::diagnostic::Request& request){
            using namespace signin::diagnostic;std::lock_guard lifetime(bridgeLifetime);
            if(!bridge)return Result{false,Code::refused};
            Result result{false,Code::failed};
            bridge->underNativeResetLocks([&]{
              result=coordinator.prepare(std::move(peer),request,[&]{
                const bool allowed=nativeDisplaySelected()&&nativeDisplay.displayManagerV2()&&bridge->nativeDiagnosticOtherBusy().empty();
                bridge->diagnosticActivityChanged();return allowed;
              });return true;
            });
            bridge->diagnosticActivityChanged();return result;
          });
      }
    }
#endif
    if(!qaService)windowsAutologonRecovery.start(serviceStopEvent);
    if(!qaService)printSweep->start(serviceStopEvent);
    if(!qaService)usbSweep->start();
    // The display probe's change signals (display_change_signals.hpp).
    std::optional<DisplayChangeSignals> displaySignals;if(!qaService)displaySignals.emplace();
    bool loaded = false; HANDLE events[] = {serviceStopEvent, serviceChangeEvent};if(!qaService)pendingConnection=std::make_unique<PendingConnectionWorker>([]{serviceConfigurationChanges.request(hostServiceReloadControl);SetEvent(serviceChangeEvent);});
    // Every step is contained (service_loop_guard.hpp): one throwing step used
    // to stop the whole service with an error the SCM recovery does not act on.
    ServiceLoopGuard loopGuard;
    // The agent holds a raw pointer to the bridge: always release it first.
    const auto releaseConnection=[&]{std::lock_guard lifetime(bridgeLifetime);agent.reset();bridge.reset();};
    // Render adapter re-arm (native_display_service.hpp): idle means the bridge
    // has nothing streaming, restoring or exiting. Evaluated inside
    // nativeDisplay.tick() on this thread, the only one that creates and resets
    // the bridge, so no bridgeLifetime lock is taken under the display mutex.
    nativeDisplay.setRenderRearmIdle([&bridge]{return bridge&&bridge->renderRearmIdle();});
    for(bool firstPass=true;;firstPass=false) {const DWORD event=WaitForMultipleObjects(2,events,FALSE,serviceLoopWaitMs(firstPass));if(event==WAIT_OBJECT_0){windowsAutologonRecovery.closeScheduling();break;}
      if (qaService) continue;
      if(runMarker)runMarker->alive();
      windowsAutologonRecovery.tick();
      nativeDisplay.tick();
#ifdef MULTITAP_NATIVE_DISPLAY
      loopGuard.run(ServiceLoopStep::privateDiagnostic,[&]{if(privateDiagnostics&&privateDiagnostics->coordinator().busy()){
        std::lock_guard lifetime(bridgeLifetime);
        const auto cleanup=[&]{(void)privateDiagnostics->coordinator().tickCleanup();return true;};
        if(bridge){bridge->underNativeResetLocks(cleanup);bridge->diagnosticActivityChanged();}else cleanup();
      }});
#endif
      if(event==WAIT_OBJECT_0+1){std::vector<SessionEvent> changes;{std::lock_guard lock(sessionEventsMutex);changes.swap(sessionEvents);}
        serviceConfigurationChanges.processSessionChanges(!changes.empty(),[&]{loopGuard.run(ServiceLoopStep::reload,[&]{
#ifdef MULTITAP_NATIVE_DISPLAY
          if(privateDiagnostics)privateDiagnostics->coordinator().cancelForward();
#endif
          loaded=false;struct Release{const decltype(releaseConnection)& release;~Release(){release();}}release{releaseConnection};
          if(catalogMaintenance)catalogMaintenance->cancel();std::lock_guard lifetime(bridgeLifetime);if(agent)agent->stop();});},
          [&]{loopGuard.run(ServiceLoopStep::sessionChange,[&]{if(catalogMaintenance)catalogMaintenance->cancel();if(bridge)bridge->sessionChanged(changes);});});}
      // A failed start must not leave an agent bound to a bridge that the next
      // attempt replaces (the old agent's teardown would reach a freed bridge).
      // host-machine-v1 (credential_binding.hpp): a registration copied from another
      // machine is kept aside and not used; the service then waits like an unregistered Host.
      if (!loaded) { try { Credentials credentials; if (loadCredentials(credentials) && bindCredentialMachine(credentials, nativeCredentialMachineAccess()) != CredentialMachine::copied) { {std::lock_guard lifetime(bridgeLifetime);bridge = std::make_unique<ServiceBridge>(credentials,catalogMaintenance.get(),&nativeDisplay,signinRuntime);bridge->readinessWake(serviceChangeEvent);
#ifdef MULTITAP_NATIVE_DISPLAY
          if(privateDiagnostics)bridge->diagnosticActivity(privateDiagnostics->busyFlag());
#endif
        } agent = std::make_unique<Agent>([](const auto&) {}, bridge.get(),AgentBridge::Send{},&brokerStatus); {const auto* active=bridge.get();agent->enableHostUpdate([active](const std::wstring& origin){return serviceHostUpdateEnvironment(origin,[active]{return active->hostUpdateSessionActive();});});agent->enableHostPower(serviceHostPowerEnvironment);} agent->onMachineChanged([held=credentials]{try{(void)holdCopiedCredentials(held);}catch(...){ }serviceConfigurationChanges.request(hostServiceReloadControl);SetEvent(serviceChangeEvent);}); agent->start(credentials); loaded = true; boot.mark(BootStage::config); }else brokerStatus.waiting(); } catch (...) {try{releaseConnection();}catch(...){ }brokerStatus.waiting("credentials_unavailable");} }
      loopGuard.run(ServiceLoopStep::maintenance,[&]{if(catalogMaintenance&&catalogMaintenance->changed()&&bridge)bridge->maintenanceChanged();});
      loopGuard.run(ServiceLoopStep::profileRecovery,[&]{if(bridge)bridge->recoverPendingStreamProfile();});
      loopGuard.run(ServiceLoopStep::displayV2Recovery,[&]{if(bridge)bridge->recoverDisplayV2();});
      loopGuard.run(ServiceLoopStep::displayAdapters,[&]{if(bridge)bridge->maintainDisplayAdapters();});
      loopGuard.run(ServiceLoopStep::renderObservation,[&]{renderObservation.streaming(bridge&&bridge->streaming());if(const auto reading=renderObservation.tick(GetTickCount64()))recordRenderObservation(*reading);});
      loopGuard.run(ServiceLoopStep::capabilities,[&]{if(agent&&bridge){const bool changed=bridge->readinessChanged(),requested=serviceConfigurationChanges.takeCapabilitiesRequest();if(requested)bridge->refreshDeviceObservations();if(changed||requested)agent->announceCapabilities();if(boot.window(GetTickCount64()))bridge->bootProbe();}});
      if(const auto record=boot.take(GetTickCount64()))recordBootTimeline(*record);
    }
    // Close the scheduling/commit gate before an outstanding bridge probe can
    // publish fresh eligibility during teardown. Keep the manager alive until
    // bridge destruction has joined those probes; shutdown itself is bounded.
    nativeDisplay.setRenderRearmIdle({});
    physicalWorkers.closeAdmission();serviceStopRequested=true;SetEvent(serviceStopEvent);
    windowsAutologonRecovery.closeSchedulingAndJoin();printSweep.reset();usbSweep.reset();
    profileCleanupHeartbeat.start([&]{return nativeDisplay.refreshOwnedCleanupLease();});
#ifdef MULTITAP_NATIVE_DISPLAY
    resetServer.reset();
    closePrivateDiagnostics();
#endif
    if(catalogMaintenance)catalogMaintenance->shutdown();brokerStatus.stop();pendingConnection.reset();if (agent) agent->stop(); agent.reset();if(bridge)bridge->closeSchedulingAndJoin();if(signinRuntime)signinRuntime->closeSchedulingAndJoin();physicalWorkers.drain([](const service_workers::DrainFacts&){});bridge.reset();signinRuntime.reset();profileCleanupHeartbeat.stop();catalogMaintenance.reset();brokerStatusServer.reset();nativeDisplay.shutdown(); WSACleanup(); if(runMarker)runMarker->stopped();stopHeartbeat->stop(); reportService(SERVICE_STOPPED);
  } catch (...) {
    nativeDisplay.setRenderRearmIdle({});
    physicalWorkers.closeAdmission();serviceStopRequested=true;SetEvent(serviceStopEvent);
    windowsAutologonRecovery.closeSchedulingAndJoin();printSweep.reset();usbSweep.reset();
#ifdef MULTITAP_NATIVE_DISPLAY
    resetServer.reset();
    closePrivateDiagnostics();
#endif
    profileCleanupHeartbeat.start([&]{return nativeDisplay.refreshOwnedCleanupLease();});if(catalogMaintenance)catalogMaintenance->shutdown();brokerStatus.stop();pendingConnection.reset(); if(agent)agent->stop();agent.reset();if(bridge)bridge->closeSchedulingAndJoin();if(signinRuntime)signinRuntime->closeSchedulingAndJoin();physicalWorkers.drain([](const service_workers::DrainFacts&){});bridge.reset();signinRuntime.reset();profileCleanupHeartbeat.stop();catalogMaintenance.reset();brokerStatusServer.reset();nativeDisplay.shutdown();if(runMarker)runMarker->failed(ERROR_SERVICE_SPECIFIC_ERROR);stopHeartbeat->stop();reportService(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR); }
  CloseHandle(serviceStopEvent); CloseHandle(serviceChangeEvent); serviceStopEvent = serviceChangeEvent = nullptr;
}
inline int runService(const std::wstring& name, bool qa) { if (name.empty() || name.size() > 64 || name.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::wstring::npos) throw std::runtime_error("Invalid host service name"); serviceName = name; qaService = qa; SERVICE_TABLE_ENTRYW table[] = {{serviceName.data(), serviceMain}, {nullptr, nullptr}}; winCheck(StartServiceCtrlDispatcherW(table), "Start Windows service dispatcher"); return 0; }
}
