#pragma once
#include <cfgmgr32.h>
#include "native_display_contract.hpp"
#include "native_display_files.hpp"
#include "signal_cache.hpp"
// A selected native provider never falls back to the console/legacy adapter.
// A protected lease snapshot identifies targets; fresh Windows CCD data proves
// which outputs are actually active in this interactive session.
namespace host {
// The separately selected trial provider advertises viewport changes only
// after the current interactive output has passed the ordinary readiness gate.
inline bool nativeViewportTrialReady(const DisplayState& display,const Json& config,bool interactive){
  return interactive&&config.get("provider").string()=="libvirtualdisplay-1.6.4"&&
    config.get("enabled").boolean()&&config.get("viewportTrialEnabled").boolean()&&
    display.ready()&&!display.remoteSession;
}
// The service loop reads the marker every second and each readiness check reads
// it again; Host setup and settings change it rarely. Its trusted read is kept
// until the file changes (native_display_files.hpp). `present`: the marker exists.
inline Json nativeDisplayConfig(bool* present=nullptr){
  const auto path=machineDirectory()/L"native-display.json";
  const bool exists=std::filesystem::exists(path);if(present)*present=exists;
  if(!exists)return {};
  static TrustedFileCache<Json> cache;
  return cache.read(path,[&]{
    if(!trustedMachineFile(path)||std::filesystem::file_size(path)>4096)throw std::runtime_error("native_display_configuration_untrusted");
    std::ifstream file(path,std::ios::binary);return JsonParser::parse(std::string((std::istreambuf_iterator<char>(file)),{}));
  });
}
inline bool nativeDisplaySelected(){try{bool present=false;const auto config=nativeDisplayConfig(&present);return nativeProviderSelected(config,present);}catch(...){return true;}}
inline bool nativeDisplayConfirmed(){try{bool present=false;const auto config=nativeDisplayConfig(&present);return nativeProviderConfirmed(config,present);}catch(...){return false;}}
inline SERVICE_STATUS_PROCESS queryHostServiceStatus(){
  ServiceHandle manager(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT));ServiceHandle service(OpenServiceW(manager.value,L"MultitapHost",SERVICE_QUERY_STATUS));SERVICE_STATUS_PROCESS status{};DWORD bytes=0;
  if(!QueryServiceStatusEx(service.value,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&bytes))throw std::runtime_error("native_display_snapshot_invalid");
  return status;
}
// The lease snapshot's service check: the snapshot's writer must be the running
// Host service. Five SCM calls (~5 wakeups) for each of the capability report's
// three snapshot reads. In the service itself SCM answers "running, this
// process" for its whole run, and only this process's own status report
// (reportService) can change that: so that one answer is kept, at most 60 s,
// until the next report. Any other answer (another process, a stop pending) is
// read again every time, as before.
class LeaseServiceStatus {
 public:
  static constexpr std::chrono::seconds maxAge{60};
  template<class Query> SERVICE_STATUS_PROCESS read(std::chrono::steady_clock::time_point now,DWORD ownPid,Query&& query){
    return cache_.get(now,hostServiceStatusEpoch().load(),query,[ownPid](const SERVICE_STATUS_PROCESS& status){return status.dwCurrentState==SERVICE_RUNNING&&status.dwProcessId==ownPid;});
  }
 private:
  SignalCache<SERVICE_STATUS_PROCESS,uint64_t> cache_{maxAge};
};
inline SERVICE_STATUS_PROCESS nativeLeaseServiceStatus(){
  static LeaseServiceStatus status;
  return status.read(std::chrono::steady_clock::now(),GetCurrentProcessId(),queryHostServiceStatus);
}
inline Json nativeDisplayLeaseSnapshot(){
  const auto path=machineDirectory()/L"native-display-state.json";
  if(!trustedMachineFile(path)||std::filesystem::file_size(path)>4096)throw std::runtime_error("native_display_snapshot_invalid");
  std::ifstream file(path,std::ios::binary);const auto snapshot=JsonParser::parse(std::string((std::istreambuf_iterator<char>(file)),{}));
  const auto status=nativeLeaseServiceStatus();
  if(status.dwCurrentState!=SERVICE_RUNNING||!nativeDisplaySnapshotFresh(snapshot,GetTickCount64(),status.dwProcessId))throw std::runtime_error("native_display_snapshot_invalid");
  if(!nativeConfigMatchesLease(nativeDisplayConfig(),snapshot))throw std::runtime_error("native_display_snapshot_invalid");
  return snapshot;
}
inline Json nativeDisplayCleanupLeaseSnapshot(DWORD expectedServicePid,const std::string& instance,uint32_t generation){
  const auto path=machineDirectory()/L"native-display-state.json";
  if(!trustedMachineFile(path)||std::filesystem::file_size(path)>4096)throw std::runtime_error("native_display_snapshot_invalid");
  std::ifstream file(path,std::ios::binary);const auto snapshot=JsonParser::parse(std::string((std::istreambuf_iterator<char>(file)),{}));
  ServiceHandle manager(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT));ServiceHandle service(OpenServiceW(manager.value,L"MultitapHost",SERVICE_QUERY_STATUS));SERVICE_STATUS_PROCESS status{};DWORD bytes=0;
  if(!QueryServiceStatusEx(service.value,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&bytes)||
     !nativeDisplayCleanupSnapshotAllowed(nativeDisplayConfig(),snapshot,status.dwCurrentState,status.dwProcessId,expectedServicePid,instance,generation,GetTickCount64()))throw std::runtime_error("native_display_snapshot_invalid");
  return snapshot;
}
inline DisplayState nativeDisplayStatus(){
  DisplayState state;state.owned=true;state.configurationPending=true;state.error="native_display_not_ready";state.foreign=0;
#ifndef MULTITAP_NATIVE_DISPLAY
  state.error="native_display_provider_unavailable";return state;
#else
  DWORD session=0;state.remoteSession=!ProcessIdToSessionId(GetCurrentProcessId(),&session)||session==0||session!=WTSGetActiveConsoleSessionId()||GetSystemMetrics(SM_REMOTESESSION)!=0;
  try{
    const auto config=nativeDisplayConfig();if(config.get("provider").string()!="libvirtualdisplay-1.6.4"){state.error="native_display_provider_unavailable";return state;}
    auto instance=wide(config.get("deviceInstanceId").string());DEVINST node=0;ULONG flags=0,problem=0;
    if(!instance.empty()&&CM_Locate_DevNodeW(&node,instance.data(),CM_LOCATE_DEVNODE_NORMAL)==CR_SUCCESS&&CM_Get_DevNode_Status(&flags,&problem,node,0)==CR_SUCCESS){
      state.installed=true;state.deviceReady=(flags&DN_STARTED)!=0&&problem==0;state.rebootRequired=(flags&DN_NEED_RESTART)!=0||problem==CM_PROB_NEED_RESTART;
    }
    const auto snapshot=nativeDisplayLeaseSnapshot();
    state.recoveryRequired=snapshot.get("recoveryRequired").boolean();
    if(state.recoveryRequired)state.error="native_topology_recovery_required";
    if(!snapshot.get("leaseActive").boolean())return state;
    const auto targets=nativeDisplayTargets(snapshot);
    if(state.remoteSession){state.error="interactive_console_required";return state;}
    UINT32 pathCount=0,modeCount=0;const UINT32 queryFlags=QDC_ONLY_ACTIVE_PATHS|QDC_VIRTUAL_MODE_AWARE;
    if(GetDisplayConfigBufferSizes(queryFlags,&pathCount,&modeCount)!=ERROR_SUCCESS||pathCount>64||modeCount>256)return state;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if(QueryDisplayConfig(queryFlags,&pathCount,paths.data(),&modeCount,modes.data(),nullptr)!=ERROR_SUCCESS)return state;paths.resize(pathCount);modes.resize(modeCount);
    state.monitors=nativeDisplayMonitors(targets,paths,modes,[](const auto& path){
      DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};name.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(name),path.sourceInfo.adapterId,path.sourceInfo.id};
      if(DisplayConfigGetDeviceInfo(&name.header)!=ERROR_SUCCESS)throw std::runtime_error("native_display_topology_mismatch");return std::wstring(name.viewGdiDeviceName);
    });
    const auto& first=state.monitors.front();state.name=first.name;state.left=first.left;state.top=first.top;LONG right=first.left+LONG(first.width),bottom=first.top+LONG(first.height);
    for(const auto& monitor:state.monitors){state.left=std::min(state.left,monitor.left);state.top=std::min(state.top,monitor.top);right=std::max(right,monitor.left+LONG(monitor.width));bottom=std::max(bottom,monitor.top+LONG(monitor.height));}
    state.width=DWORD(right-state.left);state.height=DWORD(bottom-state.top);state.active=true;const bool activationPending=!snapshot.get("activationVerified").boolean();state.configurationPending=snapshot.get("configurationPending").boolean()||activationPending;state.error=state.recoveryRequired?"native_topology_recovery_required":snapshot.get("configurationPending").boolean()?"native_display_change_pending":activationPending?"native_display_activation_pending":"";state.viewportProvisioning=nativeViewportTrialReady(state,config,true);state.monitorControlSupported=state.ready()&&!state.remoteSession&&snapshot.get("countControlAvailable").boolean();return state;
  }catch(const std::exception& error){const std::string reason=error.what();if(reason=="native_display_snapshot_invalid"||reason=="native_display_configuration_untrusted"||reason=="native_display_topology_mismatch")state.error=reason;return state;}
  catch(...){return state;}
#endif
}
}
