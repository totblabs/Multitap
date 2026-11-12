#pragma once
#include "native_display_topology.hpp"
#include "native_activation_diagnostic.hpp"
#include <mutex>
namespace host {
inline std::string nativeDisplayLeaseKey(const Json& snapshot){
  const auto pid=snapshot.get("servicePid").number(),generation=snapshot.get("generation").number();
  const auto instance=snapshot.get("deviceInstanceId").string();
  if(!nativeDisplayInteger(pid,1,UINT32_MAX)||!nativeDisplayInteger(generation,1,UINT32_MAX)||!nativeDisplayInstanceId(instance)||snapshot.get("recoveryRequired").boolean())throw std::runtime_error("native_display_snapshot_invalid");
  std::string key=instance+":"+std::to_string(DWORD(pid))+":"+std::to_string(DWORD(generation));
  for(const auto& target:nativeDisplayTargets(snapshot))key+='/'+target.id+':'+std::to_string(target.adapter.HighPart)+':'+std::to_string(target.adapter.LowPart)+':'+std::to_string(target.target)+':'+std::to_string(target.width)+':'+std::to_string(target.height);
  return key;
}
class NativeActivationGate {
 public:
  bool begin(const std::string& key){std::lock_guard lock(mutex_);if(key.empty()||key==attempted_)return false;attempted_=key;blocked_=true;return true;}
  void observed(const std::string& key,bool acknowledged,bool applied,bool verifiedReady=false){
    std::lock_guard lock(mutex_);
    if(attempted_!=key||!acknowledged)return;
    // A fresh CCD verification can succeed without SetDisplayConfig when the
    // requested topology is already active. Retain that generation just like
    // a verified apply; otherwise each periodic probe reblocks streaming.
    if(verifiedReady){blocked_=false;return;}
    if(!applied){attempted_.clear();blocked_=false;}
  }
  bool readinessAllowed(const std::string& key)const{std::lock_guard lock(mutex_);return !key.empty()&&(attempted_!=key||!blocked_);}
  bool rearmAfterConsole(const std::string& key){
    std::lock_guard lock(mutex_);
    // Only a known successful activation of this exact lease may be retried
    // after the console deliberately deactivated it. Preserve uncertain work.
    if(key.empty()||attempted_!=key||blocked_)return false;
    attempted_.clear();return true;
  }
 private:mutable std::mutex mutex_;std::string attempted_;bool blocked_=false;
};
inline bool nativeActivationPublished(const Json& snapshot){
  for(const auto* field:{"activationVerified","configurationPending","recoveryRequired"})if(!std::holds_alternative<bool>(snapshot.get(field).value))return false;
  if(!snapshot.get("activationVerified").boolean()||snapshot.get("configurationPending").boolean()||snapshot.get("recoveryRequired").boolean())return false;
  try{return !nativeDisplayLeaseKey(snapshot).empty();}catch(...){return false;}
}
struct NativeBootstrapAccess {
  std::function<bool()> selected,eligible;
  std::function<DisplayState()> probe;
  std::function<Json()> snapshot;
  std::function<NativeTopologyResult(std::span<const NativeDisplayTarget>,const std::string&)> activate;
};
struct NativeBootstrapResult {DisplayState display;bool applyAttempted=false;std::optional<NativeActivationDiagnostic> diagnostic;};
inline NativeBootstrapResult bootstrapNativeDisplays(const NativeBootstrapAccess& access){
  NativeBootstrapResult result;result.display=access.probe();
  if(!access.selected()||!access.eligible()||!result.display.installed||!result.display.deviceReady||!result.display.owned||result.display.remoteSession||result.display.rebootRequired||result.display.recoveryRequired)return result;
  try{
    const auto snapshot=access.snapshot();if(snapshot.get("configurationPending").boolean())return result;
    const auto key=nativeDisplayLeaseKey(snapshot);const auto targets=nativeDisplayTargets(snapshot);
    if(!access.eligible())return result;
    // Set this before crossing the mutation boundary. A lost/throwing reply is
    // uncertainty, not permission to repeat a display transition every probe.
    result.applyAttempted=true;result.diagnostic=NativeActivationDiagnostic{"native_topology_failed",0,true,false};
    const auto applied=access.activate(targets,key);result.applyAttempted=applied.applyAttempted;
    result.diagnostic=NativeActivationDiagnostic{applied.reason,applied.systemError,applied.applyAttempted,false};
    result.diagnostic->ccd=applied.ccdDiagnostic;
    result.display=access.probe();
    // Only this worker's fresh successful CCD verification can stage initial
    // readiness. General read-only probes keep activation pending until the
    // service durably acknowledges this exact generation.
    if(applied.ok&&access.eligible()&&result.display.active&&result.display.error=="native_display_activation_pending"){const auto fresh=access.snapshot();if(!fresh.get("configurationPending").boolean()&&nativeDisplayLeaseKey(fresh)==key){result.display.configurationPending=false;result.display.error.clear();}}
    const bool eligible=access.eligible(),sameLease=nativeDisplayLeaseKey(access.snapshot())==key;
    if(!applied.ok||!eligible||!sameLease){
      if(applied.ok)result.diagnostic->reason=!eligible?"interactive_console_required":"native_activation_lease_changed";
      result.display.active=false;result.display.viewportProvisioning=false;result.display.monitorControlSupported=false;
      result.display.recoveryRequired=result.display.recoveryRequired||applied.recoveryRequired;
      result.display.configurationPending=true;result.display.error=applied.recoveryRequired?"native_topology_recovery_required":"native_topology_not_ready";
    }else if(result.display.ready())result.diagnostic->windowsVerified=true;
    else result.diagnostic->reason="native_activation_observation_pending";
  }catch(const std::exception& error){
    if(!result.diagnostic)result.diagnostic=NativeActivationDiagnostic{};
    const std::string reason=error.what();result.diagnostic->reason=nativeActivationReasonKnown(reason)?reason:"native_topology_failed";
    result.diagnostic->attempted=result.applyAttempted;result.diagnostic->windowsVerified=false;
    result.display.active=false;result.display.viewportProvisioning=false;result.display.monitorControlSupported=false;result.display.configurationPending=true;result.display.recoveryRequired=result.display.recoveryRequired||result.applyAttempted;result.display.error="native_topology_not_ready";
  }catch(...){result.diagnostic=NativeActivationDiagnostic{"native_topology_failed",0,result.applyAttempted,false};result.display.active=false;result.display.viewportProvisioning=false;result.display.monitorControlSupported=false;result.display.configurationPending=true;result.display.recoveryRequired=result.display.recoveryRequired||result.applyAttempted;result.display.error="native_topology_not_ready";}
  return result;
}
}
