#pragma once
#include "owned_output_size.hpp"
#include "native_ccd_mode_reference.hpp"
#include <functional>
#include <span>
// Public monitor IDs stay compatible with the released host/relay protocol.
// Driver display IDs and the lease capability never leave the protected service.
namespace host {
inline bool nativeProviderSelected(const Json& config,bool markerExists){
  if(!markerExists)return false;
  // A damaged/future marker cannot silently select the legacy helper.
  const auto* enabled=std::get_if<bool>(&config.get("enabled").value);
  return !enabled||*enabled;
}
// Stricter than selection: an uncertain marker must never count as "the legacy
// device is no longer in use", so only the exact enabled provider passes.
inline bool nativeProviderConfirmed(const Json& config,bool markerExists){
  if(!markerExists)return false;
  const auto* enabled=std::get_if<bool>(&config.get("enabled").value);
  const auto* provider=std::get_if<std::string>(&config.get("provider").value);
  return enabled&&*enabled&&provider&&*provider=="libvirtualdisplay-1.6.4";
}
// What the service can prove about the native output: session 0 reads no console
// topology, so DisplayState::ready() is never true there.
inline bool nativeLeaseHealthy(const Json& snapshot,bool deviceStarted){
  return deviceStarted&&snapshot.get("leaseActive").boolean()&&snapshot.get("activationVerified").boolean()&&
    !snapshot.get("configurationPending").boolean()&&!snapshot.get("recoveryRequired").boolean();
}
struct NativeDisplayTarget {
  std::string id;
  LUID adapter{};
  UINT32 target=0;
  DWORD width=0,height=0;
};
inline bool nativeDisplayInteger(double value,double low,double high) {
  return std::isfinite(value)&&value==std::floor(value)&&value>=low&&value<=high;
}
inline std::vector<NativeDisplayTarget> nativeDisplayTargets(const Json& snapshot) {
  const auto* array=std::get_if<Json::Array>(&snapshot.get("targets").value);
  if(snapshot.get("schema").number()!=1||snapshot.get("provider").string()!="libvirtualdisplay-1.6.4"||
     !snapshot.get("leaseActive").boolean()||!array||array->empty()||array->size()>2)
    throw std::runtime_error("native_display_snapshot_invalid");
  std::vector<NativeDisplayTarget> result;
  for(const auto& value:*array) {
    const auto id=value.get("id").string();
    const auto low=value.get("adapterLow").number(-1),high=value.get("adapterHigh").number(-2147483649.0),
      target=value.get("target").number(-1),width=value.get("width").number(),height=value.get("height").number();
    if((id!="0"&&id!="1")||!nativeDisplayInteger(low,0,4294967295.0)||
       !nativeDisplayInteger(high,-2147483648.0,2147483647.0)||!nativeDisplayInteger(target,0,4294967295.0)||
       !nativeDisplayInteger(width,320,ownedOutputMaxSide)||!nativeDisplayInteger(height,180,ownedOutputMaxSide)||
       !ownedOutputSize(uint64_t(width),uint64_t(height)))
      throw std::runtime_error("native_display_snapshot_invalid");
    NativeDisplayTarget entry{id,{DWORD(low),LONG(high)},UINT32(target),DWORD(width),DWORD(height)};
    for(const auto& prior:result)if(prior.id==id||
       (prior.adapter.LowPart==entry.adapter.LowPart&&prior.adapter.HighPart==entry.adapter.HighPart&&prior.target==entry.target))
      throw std::runtime_error("native_display_snapshot_invalid");
    result.push_back(std::move(entry));
  }
  std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.id<b.id;});
  if(result.front().id!="0"||(result.size()==2&&result.back().id!="1"))
    throw std::runtime_error("native_display_snapshot_invalid");
  return result;
}
// Every reader of native-display-state.json goes through this check
// (native_display_files.hpp keeps an unchanged snapshot's tick within it).
inline constexpr uint64_t nativeDisplaySnapshotMaxAgeMs=5000;
inline bool nativeDisplaySnapshotFresh(const Json& snapshot,uint64_t now,DWORD servicePid) {
  const auto tick=snapshot.get("tick").number(-1),pid=snapshot.get("servicePid").number(-1);
  return servicePid&&nativeDisplayInteger(pid,1,4294967295.0)&&DWORD(pid)==servicePid&&
    nativeDisplayInteger(tick,0,double(now))&&double(now)-tick<=double(nativeDisplaySnapshotMaxAgeMs);
}
inline bool nativeDisplayInstanceId(const std::string& instance){
  const std::string prefix="ROOT\\DISPLAY\\";
  return instance.rfind(prefix,0)==0&&instance.size()>=prefix.size()+4&&instance.size()<=prefix.size()+8&&instance.find_first_not_of("0123456789abcdefABCDEF",prefix.size())==std::string::npos;
}
inline bool nativeConfigMatchesLease(const Json& config,const Json& snapshot){
  const auto instance=config.get("deviceInstanceId").string();
  return std::holds_alternative<bool>(config.get("enabled").value)&&config.get("enabled").boolean()&&config.get("provider").string()=="libvirtualdisplay-1.6.4"&&nativeDisplayInstanceId(instance)&&instance==snapshot.get("deviceInstanceId").string();
}
struct NativeDisplayHeldLease {std::string instance;uint32_t generation=0;std::vector<NativeDisplayTarget> targets;};
inline NativeDisplayHeldLease nativeDisplayHeldLease(const Json& snapshot){
  auto targets=nativeDisplayTargets(snapshot);
  const auto instance=snapshot.get("deviceInstanceId").string();const auto generation=snapshot.get("generation").number(-1);
  if(!nativeDisplayInstanceId(instance)||!nativeDisplayInteger(generation,1,UINT32_MAX))throw std::runtime_error("native_display_snapshot_invalid");
  return {instance,uint32_t(generation),std::move(targets)};
}
// Cleanup admission is intentionally separate from normal readiness. It may
// finish the exact held lease while SCM is stopping, never admit a new stream.
inline bool nativeDisplayCleanupSnapshotAllowed(const Json& config,const Json& snapshot,DWORD state,DWORD actualPid,DWORD expectedPid,const std::string& instance,uint32_t generation,uint64_t now){
  return (state==SERVICE_RUNNING||state==SERVICE_STOP_PENDING)&&actualPid&&actualPid==expectedPid&&generation&&
    nativeDisplayInstanceId(instance)&&nativeConfigMatchesLease(config,snapshot)&&snapshot.get("deviceInstanceId").string()==instance&&
    nativeDisplayInteger(snapshot.get("generation").number(-1),1,UINT32_MAX)&&uint32_t(snapshot.get("generation").number())==generation&&
    nativeDisplaySnapshotFresh(snapshot,now,actualPid)&&snapshot.get("leaseActive").boolean()&&
    !snapshot.get("configurationPending").boolean()&&!snapshot.get("recoveryRequired").boolean();
}
inline std::vector<VirtualMonitor> nativeDisplayMonitors(
    std::span<const NativeDisplayTarget> targets,std::span<const DISPLAYCONFIG_PATH_INFO> paths,
    std::span<const DISPLAYCONFIG_MODE_INFO> modes,
    const std::function<std::wstring(const DISPLAYCONFIG_PATH_INFO&)>& sourceName){
  const auto fail=[]{throw std::runtime_error("native_display_topology_mismatch");};
  if(targets.empty()||targets.size()>2||paths.size()>64||modes.size()>256)fail();
  // Readiness consumes raw CCD as well as normalized activation snapshots.
  // Normalize a copy so source-name callbacks and caller-owned bytes stay intact.
  std::vector<DISPLAYCONFIG_PATH_INFO> normalized(paths.begin(),paths.end());
  normalizeNativeCcdModeReferences(normalized,modes);paths=normalized;
  std::vector<VirtualMonitor> verified;
  for(const auto& target:targets){
    size_t matches=0;VirtualMonitor monitor;
    for(const auto& path:paths){
      if(!(path.flags&DISPLAYCONFIG_PATH_ACTIVE)||!path.targetInfo.targetAvailable||
         path.targetInfo.adapterId.LowPart!=target.adapter.LowPart||path.targetInfo.adapterId.HighPart!=target.adapter.HighPart||path.targetInfo.id!=target.target)continue;
      const auto index=(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)?path.sourceInfo.sourceModeInfoIdx:path.sourceInfo.modeInfoIdx;
      if(index>=modes.size())fail();const auto& source=modes[index];
      if(source.infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE||source.id!=path.sourceInfo.id||
         source.adapterId.LowPart!=path.sourceInfo.adapterId.LowPart||source.adapterId.HighPart!=path.sourceInfo.adapterId.HighPart)fail();
      const auto& mode=source.sourceMode;
      if(mode.width!=target.width||mode.height!=target.height||!displayRectangleFits(mode.position.x,mode.position.y,mode.width,mode.height))fail();
      monitor={target.id,sourceName(path),mode.position.x,mode.position.y,mode.width,mode.height,mode.position.x==0&&mode.position.y==0};++matches;
    }
    if(matches!=1||monitor.name.rfind(L"\\\\.\\DISPLAY",0)!=0)fail();
    for(const auto& prior:verified)if(prior.id==monitor.id||prior.name==monitor.name||
        (std::max<int64_t>(prior.left,monitor.left)<std::min<int64_t>(int64_t(prior.left)+prior.width,int64_t(monitor.left)+monitor.width)&&
         std::max<int64_t>(prior.top,monitor.top)<std::min<int64_t>(int64_t(prior.top)+prior.height,int64_t(monitor.top)+monitor.height)))fail();
    verified.push_back(std::move(monitor));
  }
  // A target ID alone does not prove an independent desktop. A console/foreign
  // clone occupying the same area cannot be offered as an owned virtual screen.
  for(const auto& path:paths){
    if(!(path.flags&DISPLAYCONFIG_PATH_ACTIVE)||std::any_of(targets.begin(),targets.end(),[&](const auto& target){return path.targetInfo.id==target.target&&path.targetInfo.adapterId.LowPart==target.adapter.LowPart&&path.targetInfo.adapterId.HighPart==target.adapter.HighPart;}))continue;
    const auto index=(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)?path.sourceInfo.sourceModeInfoIdx:path.sourceInfo.modeInfoIdx;
    if(index>=modes.size())fail();const auto& mode=modes[index];
    if(mode.infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE||mode.id!=path.sourceInfo.id||mode.adapterId.LowPart!=path.sourceInfo.adapterId.LowPart||mode.adapterId.HighPart!=path.sourceInfo.adapterId.HighPart||!mode.sourceMode.width||!mode.sourceMode.height)fail();
    const auto& foreign=mode.sourceMode;if(!displayRectangleFits(foreign.position.x,foreign.position.y,foreign.width,foreign.height))fail();
    for(const auto& owned:verified)if(std::max<int64_t>(foreign.position.x,owned.left)<std::min<int64_t>(int64_t(foreign.position.x)+foreign.width,int64_t(owned.left)+owned.width)&&std::max<int64_t>(foreign.position.y,owned.top)<std::min<int64_t>(int64_t(foreign.position.y)+foreign.height,int64_t(owned.top)+owned.height))fail();
  }
  return verified;
}
}
