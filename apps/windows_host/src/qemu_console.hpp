#pragma once
#include "display_v2.hpp"
#include "native_stream_console_identity.hpp"

namespace host {
struct QemuConsoleAccess {
  DisplayV2Access display;
  NativeConsoleModelProof proof;
  std::function<std::string()> machineUuid;
  std::function<bool()> remoteSession;
  NativeConsoleIdentityResolver resolver;
  std::function<bool()> currentLease;
  uint64_t deadline=0;
};
struct QemuConsoleResult {
  bool ok=false,attempted=false;
  std::string reason="qemu_console_unavailable";
};
inline bool sameQemuConsoleOutput(const NativeQemuConsoleOutput& a,const NativeQemuConsoleOutput& b){
  return a.proof.machineUuid==b.proof.machineUuid&&a.proof.model==b.proof.model&&a.target==b.target&&
    nativeConsoleLuidEqual(a.device.adapter,b.device.adapter)&&a.device.interfacePath==b.device.interfacePath&&
    a.device.instanceId==b.device.instanceId&&a.device.enumerator==b.device.enumerator&&a.device.hardwareIds==b.device.hardwareIds;
}
// Console = the QEMU output alone. While the console is held every other
// display is off: the owned
// virtual outputs and every foreign one (a vGPU's own head such as NVIDIA GRID,
// anything else a VM has), since nobody sees them through the Proxmox console.
// A foreign display left on beside the console can stay primary-side (an
// NVIDIA output with the icons and windows on it), and the console then
// shows an empty extended desktop.
inline bool qemuConsoleTargetPath(const DISPLAYCONFIG_PATH_INFO& path,const NativeQemuConsoleOutput& console){
  return nativeSameLuid(path.targetInfo.adapterId,console.device.adapter)&&path.targetInfo.id==console.target;
}
// Satisfied: the console is active at the origin and no other path is active.
inline bool qemuConsoleOnlySatisfied(const NativeCcdData& ccd,const NativeQemuConsoleOutput& console){
  bool found=false;
  for(const auto& path:ccd.paths){
    if(!displayV2Active(path))continue;
    if(!qemuConsoleTargetPath(path,console)||found)return false;
    const auto& mode=nativeCcdSource(ccd,path);
    if(mode.position.x||mode.position.y)return false;found=true;
  }
  return found;
}
// The plan: the console's active path from `original` (its modes kept) at the
// origin and nothing else; every path the plan leaves out is switched off.
inline NativeCcdData planQemuConsoleOnly(const NativeCcdData& fresh,const NativeCcdData& original,const NativeQemuConsoleOutput& console){
  const DISPLAYCONFIG_PATH_INFO* source=nullptr;
  for(const auto& path:original.paths)if(displayV2Active(path)&&qemuConsoleTargetPath(path,console)){if(source)throw std::runtime_error("qemu_console_identity_changed");source=&path;}
  if(!source||!nativeSameLuid(source->sourceInfo.adapterId,console.device.adapter))throw std::runtime_error("qemu_console_state_invalid");
  size_t available=0;
  for(const auto& path:fresh.paths)if(path.targetInfo.targetAvailable&&qemuConsoleTargetPath(path,console)&&
      nativeSameLuid(path.sourceInfo.adapterId,source->sourceInfo.adapterId)&&path.sourceInfo.id==source->sourceInfo.id)++available;
  if(available!=1)throw std::runtime_error("qemu_console_identity_changed");
  NativeCcdData plan;std::map<UINT32,UINT32> copied;
  displayV2CopyPath(plan,copied,original,*source);
  plan.modes[nativeSourceIndex(plan.paths.front())].sourceMode.position={0,0};
  return plan;
}
// An identified console that is inactive has no modes to re-supply when no
// restore point has it active (Windows' boot topology, or a stream topology
// whose restore point was lost). Supply only the topology:
// the console target alone, on the lowest source of its own adapter. Windows
// picks the modes from its database, as DisplaySessionV2::returnConsole's last
// resort does.
inline constexpr UINT32 qemuConsoleTopologySupplied=SDC_TOPOLOGY_SUPPLIED|SDC_ALLOW_PATH_ORDER_CHANGES|SDC_VIRTUAL_MODE_AWARE;
inline NativeCcdData planQemuConsoleActivation(const NativeCcdData& fresh,const NativeQemuConsoleOutput& console){
  std::optional<DISPLAYCONFIG_PATH_INFO> chosen;
  for(const auto& path:fresh.paths){
    if(!qemuConsoleTargetPath(path,console))continue;
    if(displayV2Active(path))throw std::runtime_error("qemu_console_identity_changed");
    if(!path.targetInfo.targetAvailable||!nativeSameLuid(path.sourceInfo.adapterId,console.device.adapter))continue;
    if(!chosen||path.sourceInfo.id<chosen->sourceInfo.id)chosen=path;
  }
  if(!chosen)throw std::runtime_error("qemu_console_unavailable");
  chosen->flags|=DISPLAYCONFIG_PATH_ACTIVE;
  NativeCcdData topology;topology.paths.push_back(*chosen);
  return displayV2PathsOnly(std::move(topology));
}
// A rollback put `before` back exactly: every display active before is active
// again with its modes (foreign ones included) at its position.
inline bool qemuConsoleRollbackMatches(const NativeCcdData& before,const NativeCcdData& after){
  if(!displayV2SameForeign(displayV2Foreign(before,{},{}),displayV2Foreign(after,{},{})))return false;
  for(const auto& path:before.paths)if(displayV2Active(path)){
    const auto fresh=std::find_if(after.paths.begin(),after.paths.end(),[&](const auto& value){return displayV2Active(value)&&nativeSameLuid(value.targetInfo.adapterId,path.targetInfo.adapterId)&&value.targetInfo.id==path.targetInfo.id;});
    if(fresh==after.paths.end())return false;const auto old=nativeCcdSource(before,path).position,current=nativeCcdSource(after,*fresh).position;
    if(old.x!=current.x||old.y!=current.y)return false;
  }
  return true;
}
// The browser grant is not display authority. The service supplies this bounded
// worker with its exact current lease and a protected pre-stream snapshot.
inline QemuConsoleResult prepareQemuConsoleOutput(QemuConsoleAccess access){
  QemuConsoleResult result;
  const auto fail=[&](const char* reason){result.ok=false;result.reason=reason;return result;};
  if(!access.display.query||!access.display.configure||!access.display.owned||!access.display.now||!access.display.eligible||
     !access.machineUuid||!access.remoteSession||!access.resolver||!access.currentLease||!access.deadline)return result;
  const auto current=[&]{
    try{return access.display.now()<access.deadline&&access.display.eligible()&&access.currentLease();}catch(...){return false;}
  };
  if(!current())return fail("qemu_console_cancelled");
  try{
    const auto owned=access.display.owned();const auto before=access.display.query();
    const auto observation=observeNativeQemuConsoleOutput(access.proof,access.machineUuid(),access.remoteSession(),before.paths,owned,access.resolver);
    if(!observation.output)return fail("qemu_console_identity_changed");
    const auto held=*observation.output;
    if(!current())return fail("qemu_console_cancelled");
    if(held.active&&qemuConsoleOnlySatisfied(before,held)){result.ok=true;result.reason="qemu_console_active";return result;}
    if(!access.display.store.load||!access.display.store.remove||!access.display.store.save)return fail("qemu_console_state_missing");
    std::optional<std::string> savedBytes;
    DisplayV2State saved;bool created=false;
    try{savedBytes=access.display.store.load();if(savedBytes)saved=parseDisplayV2State(*savedBytes);}
    catch(...){return fail("qemu_console_state_invalid");}
    std::optional<DisplayV2Console> target=DisplayV2Console{held.device.adapter,held.device.adapter,0,held.target};
    // The non-owned displays besides the console, as observed now. Until the
    // console-only plan is applied they must stay exactly these; once it is,
    // none of them may be active (`expectForeign`).
    const auto foreign=displayV2Foreign(before,owned,target);const std::vector<DisplayV2Mode> none;
    // A state from an earlier boot can never be restored; like no state, it is
    // replaced by a restore point of the current topology below.
    const bool unsaved=!savedBytes||displayV2StateFromEarlierBoot(saved,before,owned);
    // An existing restore point (a stream's original whose restore is still
    // pending, or an earlier console's) is the true original: the console keeps
    // it and its end restores it. It must name this console and be restorable
    // from the current topology. Its foreign modes need not equal the current
    // ones: the console no longer keeps foreign displays, and the restore puts
    // the saved ones back, as the ordinary v2 recovery would anyway.
    bool savedConsoleModes=false;
    if(!unsaved){
      if(!saved.console||!nativeConsoleLuidEqual(saved.console->sourceAdapter,held.device.adapter)||!nativeConsoleLuidEqual(saved.console->targetAdapter,held.device.adapter)||
         saved.console->target!=held.target||!preflightDisplayV2Restore(saved,before,owned).resolved)return fail("qemu_console_state_invalid");
      savedConsoleModes=displayV2ConsoleActive(saved.ccd,*saved.console);
    }
    // Without a saved console mode, an inactive console is first activated with
    // modes Windows chooses, then placed like an active one.
    const bool activate=!held.active&&!savedConsoleModes;
    const auto plan=activate?planQemuConsoleActivation(before,held):planQemuConsoleOnly(before,held.active?before:saved.ccd,held);
    const auto key=NativeConsoleOutputKey{held.device.adapter,held.target};
    bool rollingBack=false;
    const auto expectForeign=[&](const std::vector<DisplayV2Mode>& now){
      // Before the first apply: exactly as observed. After it: none. A rollback
      // accepts either (it may follow an apply Windows refused or one it kept).
      if(rollingBack)return displayV2SameForeign(foreign,now)||now.empty();
      return displayV2SameForeign(result.attempted?none:foreign,now);
    };
    const auto checked=[&](bool requireState,bool requireIsolated=false){
      if(!current()){result.reason="qemu_console_cancelled";return false;}
      const auto fresh=access.display.query();
      const auto observed=observeNativeQemuConsoleOutput(access.proof,access.machineUuid(),access.remoteSession(),fresh.paths,owned,access.resolver,key);
      if(!observed.output||!sameQemuConsoleOutput(held,*observed.output)){result.reason="qemu_console_identity_changed";return false;}
      if(!expectForeign(displayV2Foreign(fresh,owned,target))){result.reason="qemu_console_foreign_changed";return false;}
      if(requireIsolated&&!qemuConsoleOnlySatisfied(fresh,held)){result.reason="qemu_console_inactive";return false;}
      if(requireState&&access.display.store.load()!=savedBytes){result.reason="qemu_console_state_invalid";return false;}
      return true;
    };
    if(!checked(true))return result;
    if(unsaved){
      // The restore point is the current topology, foreign displays included.
      // An active console is bound by its exact identity. An inactive one is
      // named by its target only (no path, no modes): the restore at the
      // console's end then knows the active console is this one and switches
      // it off again (preflightDisplayV2Restore counts it as known).
      std::optional<DisplayV2Console> console=target;
      if(held.active){
        const auto identity=identifyNativeStreamConsole(access.proof,access.machineUuid(),access.remoteSession(),before.paths,owned,access.resolver);
        if(!identity.binding)return fail("qemu_console_identity_changed");
        const auto& path=identity.binding->path;console=DisplayV2Console{path.sourceAdapter,path.targetAdapter,path.source,path.target};
      }
      savedBytes=displayV2StateBytes({"qemu-console",displayV2ActiveCcd(before),owned,console});
      if(!access.display.store.save(*savedBytes))return fail("qemu_console_state_missing");created=true;
    }
    const auto configure=[&](const NativeCcdData& next,UINT32 flags,const std::vector<DisplayV2Mode>& planned){
      // A topology-only plan carries no modes; the checks after it compare them.
      if(!checked(true)||(!(flags&SDC_TOPOLOGY_SUPPLIED)&&!displayV2SameForeign(planned,displayV2Foreign(next,owned,target))))return false;
      if(access.display.configure(next,SDC_VALIDATE|flags)!=ERROR_SUCCESS)return false;
      if(!checked(true))return false;
      result.attempted=true;return access.display.configure(next,SDC_APPLY|flags)==ERROR_SUCCESS;
    };
    // Bounded reads until `satisfied` holds `reads` times in a row.
    const auto settle=[&](const auto& satisfied,unsigned reads){
      const auto until=std::min(access.deadline,access.display.now()+2500);unsigned stable=0;
      do{
        if(!checked(true))return false;
        if(satisfied(access.display.query())){if(++stable==reads)return true;}else stable=0;
        if(access.display.now()>=until)return false;access.display.pause();
      }while(true);
    };
    const auto isolated=[&](const NativeCcdData& ccd){return qemuConsoleOnlySatisfied(ccd,held);};
    bool applied=false,ready=false;
    try{
      applied=configure(plan,activate?qemuConsoleTopologySupplied:displayV2Supplied,none);
      bool placed=applied;
      if(applied&&activate){
        // Windows chose the console's modes; unless it already sits alone at the
        // origin, the ordinary supplied plan places it there with those modes.
        placed=settle([&](const NativeCcdData& ccd){return displayV2TargetActive(ccd,held.device.adapter,held.target);},1);
        if(placed){const auto fresh=access.display.query();if(!isolated(fresh))placed=configure(planQemuConsoleOnly(fresh,fresh,held),displayV2Supplied,none);}
      }
      if(placed)ready=settle(isolated,2);
      // The restore point stays: it is the topology from before the console,
      // foreign displays included, and the console's end puts it back (the
      // service's ordinary v2 recovery once the hold is released,
      // ServiceBridge::rearmConsoleActivation). Were it removed
      // here, only the owned outputs would come back afterwards.
      if(ready&&checked(true,true)){result.ok=true;result.reason="qemu_console_restored";return result;}
    }catch(...){ }
    // Restore only while the same operation still owns the original devices.
    // On cancellation/identity loss, retain protected recovery evidence rather
    // than using expired authority to issue another display mutation.
    bool returned=false;rollingBack=true;
    try{
      if(result.attempted&&checked(true)&&configure(displayV2ActiveCcd(before),displayV2Supplied,foreign)){
        const auto after=access.display.query();
        returned=qemuConsoleRollbackMatches(displayV2ActiveCcd(before),after);
      }else returned=!result.attempted;
      if(returned&&created&&checked(true))(void)access.display.store.remove();
    }catch(...){ }
    if(result.reason=="qemu_console_unavailable")result.reason=applied?"qemu_console_inactive":"qemu_console_unavailable";
    return result;
  }catch(...){return result;}
}
}
