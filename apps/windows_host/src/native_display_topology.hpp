#pragma once
#include "machine.hpp"
#include "owned_output_size.hpp"
#include "native_activation_diagnostic.hpp"
#include "native_ccd_mode_reference.hpp"
#include <map>
#include <set>
#include <tuple>
#include <cstring>

namespace host {
struct NativeCcdData {
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};
// QDC_ALL_PATHS counts possible source/target combinations, not monitors.
// Keep its bounded read allocation separate from the small applied topology.
inline constexpr size_t nativeCcdQueryPathLimit=4096,nativeCcdQueryModeLimit=16384;
class NativeTopologyWindowsError final:public std::runtime_error {
 public:explicit NativeTopologyWindowsError(DWORD error):std::runtime_error("native_topology_query_failed"),code(error){}
  DWORD code;
};
inline bool nativeSameLuid(const LUID& a,const LUID& b){return a.LowPart==b.LowPart&&a.HighPart==b.HighPart;}
inline bool nativeSameTarget(const DISPLAYCONFIG_PATH_INFO& path,const NativeDisplayTarget& target){return nativeSameLuid(path.targetInfo.adapterId,target.adapter)&&path.targetInfo.id==target.target;}
inline auto nativeSourceKey(const DISPLAYCONFIG_PATH_INFO& path){return std::tuple{path.sourceInfo.adapterId.HighPart,path.sourceInfo.adapterId.LowPart,path.sourceInfo.id};}
inline UINT32 nativeSourceIndex(const DISPLAYCONFIG_PATH_INFO& path){return path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE?path.sourceInfo.sourceModeInfoIdx:path.sourceInfo.modeInfoIdx;}
inline UINT32 nativeTargetIndex(const DISPLAYCONFIG_PATH_INFO& path){return path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE?path.targetInfo.targetModeInfoIdx:path.targetInfo.modeInfoIdx;}
inline size_t normalizeNativeCcdModeReferences(NativeCcdData& ccd){
  return normalizeNativeCcdModeReferences(ccd.paths,ccd.modes);
}
inline const DISPLAYCONFIG_MODE_INFO& nativeCcdMode(const NativeCcdData& ccd,UINT32 index,DISPLAYCONFIG_MODE_INFO_TYPE type){
  if(index>=ccd.modes.size()||ccd.modes[index].infoType!=type)throw std::runtime_error("native_topology_mode_invalid");return ccd.modes[index];
}
inline const DISPLAYCONFIG_SOURCE_MODE& nativeCcdSource(const NativeCcdData& ccd,const DISPLAYCONFIG_PATH_INFO& path){
  const auto& mode=nativeCcdMode(ccd,nativeSourceIndex(path),DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE);
  if(!nativeSameLuid(mode.adapterId,path.sourceInfo.adapterId)||mode.id!=path.sourceInfo.id)throw std::runtime_error("native_topology_mode_invalid");return mode.sourceMode;
}
// CCD source sizes are DWORD; desktop endpoints are LONG. Use a widened sum
// before narrowing instead of imposing a product-resolution/coordinate limit.
inline std::optional<RECT> nativeSourceBounds(const DISPLAYCONFIG_SOURCE_MODE& source){
  const int64_t right=int64_t(source.position.x)+source.width,bottom=int64_t(source.position.y)+source.height;
  if(!displayRectangleFits(source.position.x,source.position.y,source.width,source.height))return {};
  return RECT{source.position.x,source.position.y,LONG(right),LONG(bottom)};
}
inline const DISPLAYCONFIG_TARGET_MODE& nativeCcdTarget(const NativeCcdData& ccd,const DISPLAYCONFIG_PATH_INFO& path){
  const auto& mode=nativeCcdMode(ccd,nativeTargetIndex(path),DISPLAYCONFIG_MODE_INFO_TYPE_TARGET);
  if(!nativeSameLuid(mode.adapterId,path.targetInfo.adapterId)||mode.id!=path.targetInfo.id)throw std::runtime_error("native_topology_mode_invalid");return mode.targetMode;
}
inline bool nativeOwnedPath(const DISPLAYCONFIG_PATH_INFO& path,std::span<const NativeDisplayTarget> targets){return std::any_of(targets.begin(),targets.end(),[&](const auto& target){return nativeSameTarget(path,target);});}

// Compare documented fields rather than structure padding or reindexed mode
// references. QueryDisplayConfig may return a different mode-array ordering.
inline std::vector<int64_t> nativePathSignature(const NativeCcdData& ccd,const DISPLAYCONFIG_PATH_INFO& path){
  const auto& source=nativeCcdSource(ccd,path);const auto& signal=nativeCcdTarget(ccd,path).targetVideoSignalInfo;
  if(signal.pixelRate>INT64_MAX)throw std::runtime_error("native_topology_mode_invalid");
  std::vector<int64_t> out{path.sourceInfo.adapterId.HighPart,path.sourceInfo.adapterId.LowPart,path.sourceInfo.id,
    path.targetInfo.adapterId.HighPart,path.targetInfo.adapterId.LowPart,path.targetInfo.id,
    path.flags,path.targetInfo.targetAvailable,path.targetInfo.outputTechnology,path.targetInfo.rotation,path.targetInfo.scaling,
    path.targetInfo.refreshRate.Numerator,path.targetInfo.refreshRate.Denominator,path.targetInfo.scanLineOrdering,
    source.width,source.height,source.pixelFormat,source.position.x,source.position.y,int64_t(signal.pixelRate),
    signal.hSyncFreq.Numerator,signal.hSyncFreq.Denominator,signal.vSyncFreq.Numerator,signal.vSyncFreq.Denominator,
    signal.activeSize.cx,signal.activeSize.cy,signal.totalSize.cx,signal.totalSize.cy,signal.videoStandard,signal.scanLineOrdering};
  const auto desktop=path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE?path.targetInfo.desktopModeInfoIdx:DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
  out.push_back(desktop!=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID);
  if(desktop!=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID){const auto& mode=nativeCcdMode(ccd,desktop,DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE);const auto& image=mode.desktopImageInfo;
    for(const auto value:{mode.adapterId.HighPart,LONG(mode.adapterId.LowPart),LONG(mode.id),image.PathSourceSize.x,image.PathSourceSize.y,
        image.DesktopImageRegion.left,image.DesktopImageRegion.top,image.DesktopImageRegion.right,image.DesktopImageRegion.bottom,
        image.DesktopImageClip.left,image.DesktopImageClip.top,image.DesktopImageClip.right,image.DesktopImageClip.bottom})out.push_back(value);
  }
  return out;
}
inline std::vector<std::vector<int64_t>> nativeActiveSignatures(const NativeCcdData& ccd,std::span<const NativeDisplayTarget> omit={}){
  std::vector<std::vector<int64_t>> result;
  for(const auto& path:ccd.paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&!nativeOwnedPath(path,omit))result.push_back(nativePathSignature(ccd,path));return result;
}
inline NativeCcdData nativeActiveCcd(const NativeCcdData& ccd){
  NativeCcdData active;std::map<UINT32,UINT32> copied;
  const auto copyMode=[&](UINT32 index,DISPLAYCONFIG_MODE_INFO_TYPE type){
    const auto& mode=nativeCcdMode(ccd,index,type);const auto found=copied.find(index);if(found!=copied.end())return found->second;
    const auto destination=UINT32(active.modes.size());active.modes.push_back(mode);copied.emplace(index,destination);return destination;
  };
  for(auto path:ccd.paths)if(path.flags&DISPLAYCONFIG_PATH_ACTIVE){
    const auto source=copyMode(nativeSourceIndex(path),DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE),target=copyMode(nativeTargetIndex(path),DISPLAYCONFIG_MODE_INFO_TYPE_TARGET);
    if(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE){
      if(path.targetInfo.desktopModeInfoIdx!=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID)path.targetInfo.desktopModeInfoIdx=copyMode(path.targetInfo.desktopModeInfoIdx,DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE);
      path.sourceInfo.sourceModeInfoIdx=source;path.targetInfo.targetModeInfoIdx=target;
    }else{path.sourceInfo.modeInfoIdx=source;path.targetInfo.modeInfoIdx=target;}
    active.paths.push_back(path);
  }
  return active;
}
inline bool nativeSameMode(const DISPLAYCONFIG_MODE_INFO& a,const DISPLAYCONFIG_MODE_INFO& b){
  if(a.infoType!=b.infoType||a.id!=b.id||!nativeSameLuid(a.adapterId,b.adapterId))return false;
  if(a.infoType==DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE){const auto& x=a.sourceMode;const auto& y=b.sourceMode;return std::tuple{x.width,x.height,x.pixelFormat,x.position.x,x.position.y}==std::tuple{y.width,y.height,y.pixelFormat,y.position.x,y.position.y};}
  if(a.infoType==DISPLAYCONFIG_MODE_INFO_TYPE_TARGET){const auto& x=a.targetMode.targetVideoSignalInfo;const auto& y=b.targetMode.targetVideoSignalInfo;
    return std::tuple{x.pixelRate,x.hSyncFreq.Numerator,x.hSyncFreq.Denominator,x.vSyncFreq.Numerator,x.vSyncFreq.Denominator,x.activeSize.cx,x.activeSize.cy,x.totalSize.cx,x.totalSize.cy,x.videoStandard,x.scanLineOrdering}==std::tuple{y.pixelRate,y.hSyncFreq.Numerator,y.hSyncFreq.Denominator,y.vSyncFreq.Numerator,y.vSyncFreq.Denominator,y.activeSize.cx,y.activeSize.cy,y.totalSize.cx,y.totalSize.cy,y.videoStandard,y.scanLineOrdering};
  }
  if(a.infoType==DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE){const auto& x=a.desktopImageInfo;const auto& y=b.desktopImageInfo;
    return std::tuple{x.PathSourceSize.x,x.PathSourceSize.y,x.DesktopImageRegion.left,x.DesktopImageRegion.top,x.DesktopImageRegion.right,x.DesktopImageRegion.bottom,x.DesktopImageClip.left,x.DesktopImageClip.top,x.DesktopImageClip.right,x.DesktopImageClip.bottom}==std::tuple{y.PathSourceSize.x,y.PathSourceSize.y,y.DesktopImageRegion.left,y.DesktopImageRegion.top,y.DesktopImageRegion.right,y.DesktopImageRegion.bottom,y.DesktopImageClip.left,y.DesktopImageClip.top,y.DesktopImageClip.right,y.DesktopImageClip.bottom};
  }
  return false;
}
struct NativeOutputPlacement {std::string id;LONG left=0,top=0;bool primary=false;};
struct NativeTopologyPlan {NativeCcdData supplied;std::vector<NativeDisplayTarget> targets;std::vector<NativeOutputPlacement> placements;};
inline NativeTopologyPlan planNativeTopology(const NativeCcdData& before,std::span<const NativeDisplayTarget> targets,std::span<const NativeOutputPlacement> previous={}){
  const auto fail=[]{throw std::runtime_error("native_topology_plan_invalid");};
  if(targets.empty()||targets.size()>2||before.paths.size()>nativeCcdQueryPathLimit||before.modes.size()>nativeCcdQueryModeLimit||previous.size()>2)fail();
  for(size_t i=0;i<targets.size();++i){const auto& target=targets[i];
    if(target.id!=std::to_string(i)||!ownedOutputSize(target.width,target.height))fail();
    for(size_t j=0;j<i;++j)if(nativeSameLuid(target.adapter,targets[j].adapter)&&target.target==targets[j].target)fail();
  }
  std::set<std::string> placementIds;for(const auto& item:previous)if((item.id!="0"&&item.id!="1")||!placementIds.insert(item.id).second||(item.primary&&(item.left||item.top)))fail();
  std::set<decltype(nativeSourceKey(DISPLAYCONFIG_PATH_INFO{}))> foreignSources;
  std::vector<RECT> occupied;std::set<std::tuple<LONG,DWORD,UINT32>> activeTargets;bool hasPrimary=false;
  for(const auto& path:before.paths)if(path.flags&DISPLAYCONFIG_PATH_ACTIVE){
    if(!path.targetInfo.targetAvailable||!activeTargets.emplace(path.targetInfo.adapterId.HighPart,path.targetInfo.adapterId.LowPart,path.targetInfo.id).second)fail();
    const auto& source=nativeCcdSource(before,path);
    const auto bounds=nativeSourceBounds(source);if(!bounds)fail();
    if(!nativeOwnedPath(path,targets)){
      (void)nativeCcdTarget(before,path);foreignSources.insert(nativeSourceKey(path));
      occupied.push_back(*bounds);
      hasPrimary=hasPrimary||(!source.position.x&&!source.position.y);
    }
  }
  std::vector<std::vector<size_t>> choices(targets.size());
  for(size_t i=0;i<targets.size();++i){
    for(size_t j=0;j<before.paths.size();++j){const auto& path=before.paths[j];
      if(nativeSameTarget(path,targets[i])&&path.targetInfo.targetAvailable&&nativeSameLuid(path.sourceInfo.adapterId,targets[i].adapter)&&!foreignSources.count(nativeSourceKey(path)))choices[i].push_back(j);
    }
    std::stable_sort(choices[i].begin(),choices[i].end(),[&](size_t a,size_t b){const auto& x=before.paths[a];const auto& y=before.paths[b];if(bool(x.flags&DISPLAYCONFIG_PATH_ACTIVE)!=bool(y.flags&DISPLAYCONFIG_PATH_ACTIVE))return bool(x.flags&DISPLAYCONFIG_PATH_ACTIVE);return nativeSourceKey(x)<nativeSourceKey(y);});
    if(choices[i].empty())fail();
  }
  std::vector<size_t> chosen;
  for(const auto first:choices[0]){
    if(targets.size()==1){chosen={first};break;}
    for(const auto second:choices[1])if(nativeSourceKey(before.paths[first])!=nativeSourceKey(before.paths[second])){chosen={first,second};break;}
    if(!chosen.empty())break;
  }
  if(chosen.size()!=targets.size())fail();
  NativeTopologyPlan result;result.targets.assign(targets.begin(),targets.end());std::map<UINT32,UINT32> copied;
  std::map<std::tuple<int,LONG,DWORD,UINT32>,UINT32> modeIdentities;
  const auto copyMode=[&](UINT32 index,DISPLAYCONFIG_MODE_INFO_TYPE type){
    const auto& mode=nativeCcdMode(before,index,type);const auto found=copied.find(index);if(found!=copied.end())return found->second;
    const auto key=std::tuple{int(mode.infoType),mode.adapterId.HighPart,mode.adapterId.LowPart,mode.id};
    const auto identity=modeIdentities.find(key);if(identity!=modeIdentities.end()){
      if(!nativeSameMode(result.supplied.modes[identity->second],mode))throw std::runtime_error("native_topology_mode_invalid");copied.emplace(index,identity->second);return identity->second;
    }
    const auto destination=UINT32(result.supplied.modes.size());result.supplied.modes.push_back(mode);copied.emplace(index,destination);modeIdentities.emplace(key,destination);return destination;
  };
  // All original foreign active paths and their mode bytes remain untouched.
  for(auto path:before.paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&!nativeOwnedPath(path,targets)){
    const auto source=copyMode(nativeSourceIndex(path),DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE),target=copyMode(nativeTargetIndex(path),DISPLAYCONFIG_MODE_INFO_TYPE_TARGET);
    if(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE){
      if(path.targetInfo.desktopModeInfoIdx!=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID)path.targetInfo.desktopModeInfoIdx=copyMode(path.targetInfo.desktopModeInfoIdx,DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE);
      path.sourceInfo.sourceModeInfoIdx=source;path.targetInfo.targetModeInfoIdx=target;
    }else{path.sourceInfo.modeInfoIdx=source;path.targetInfo.modeInfoIdx=target;}
    result.supplied.paths.push_back(path);
  }
  std::vector<NativeOutputPlacement> hints;std::vector<bool> explicitPlacement;
  for(size_t i=0;i<targets.size();++i){NativeOutputPlacement hint{targets[i].id};
    const auto old=std::find_if(previous.begin(),previous.end(),[&](const auto& value){return value.id==hint.id;});
    const auto& path=before.paths[chosen[i]];
    explicitPlacement.push_back(old!=previous.end());if(old!=previous.end())hint=*old;
    else if(path.flags&DISPLAYCONFIG_PATH_ACTIVE){const auto& mode=nativeCcdSource(before,path);hint.left=mode.position.x;hint.top=mode.position.y;hint.primary=!hint.left&&!hint.top;}
    else if(activeTargets.empty()&&i==0){hint.primary=true;}
    else{hint.left=LONG_MAX;hint.top=LONG_MAX;}
    hints.push_back(hint);
  }
  std::vector<size_t> order(targets.size());for(size_t i=0;i<order.size();++i)order[i]=i;
  std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return hints[a].primary&&!hints[b].primary;});
  std::vector<DISPLAYCONFIG_PATH_INFO> ownedPaths(targets.size());result.placements.resize(targets.size());
  for(const auto i:order){const auto& target=targets[i];auto hint=hints[i];
    const auto fits=[&](int64_t x,int64_t y){if(!displayRectangleFits(x,y,target.width,target.height))return false;return std::none_of(occupied.begin(),occupied.end(),[&](const RECT& r){return std::max<int64_t>(x,r.left)<std::min<int64_t>(x+target.width,r.right)&&std::max<int64_t>(y,r.top)<std::min<int64_t>(y+target.height,r.bottom);});};
    if(hint.primary&&hasPrimary&&!explicitPlacement[i])hint.primary=false;
    if(hint.primary){if(hasPrimary||hint.left||hint.top||!fits(0,0))fail();hasPrimary=true;}
    else if(!fits(hint.left,hint.top)){
      if(occupied.empty()){hint.left=0;hint.top=0;hint.primary=true;hasPrimary=true;}
      else{const auto edge=std::max_element(occupied.begin(),occupied.end(),[](const RECT& a,const RECT& b){return a.right<b.right;});hint.left=edge->right;hint.top=edge->top;if(!fits(hint.left,hint.top))fail();}
    }
    if(!hint.left&&!hint.top){if(!hint.primary&&hasPrimary)fail();hint.primary=true;hasPrimary=true;}
    occupied.push_back({hint.left,hint.top,LONG(int64_t(hint.left)+target.width),LONG(int64_t(hint.top)+target.height)});result.placements[i]=hint;
    auto path=before.paths[chosen[i]];
    DISPLAYCONFIG_MODE_INFO mode{};mode.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;mode.adapterId=path.sourceInfo.adapterId;mode.id=path.sourceInfo.id;
    mode.sourceMode={target.width,target.height,DISPLAYCONFIG_PIXELFORMAT_32BPP,{hint.left,hint.top}};
    const auto source=UINT32(result.supplied.modes.size());result.supplied.modes.push_back(mode);
    if(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE){path.sourceInfo.cloneGroupId=DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;path.sourceInfo.sourceModeInfoIdx=source;path.targetInfo.desktopModeInfoIdx=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;path.targetInfo.targetModeInfoIdx=DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;}
    else{path.sourceInfo.modeInfoIdx=source;path.targetInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;}
    path.flags|=DISPLAYCONFIG_PATH_ACTIVE;path.flags&=~DISPLAYCONFIG_PATH_BOOST_REFRESH_RATE;
    path.targetInfo.rotation=DISPLAYCONFIG_ROTATION_IDENTITY;path.targetInfo.scaling=DISPLAYCONFIG_SCALING_IDENTITY;path.targetInfo.refreshRate={60,1};path.targetInfo.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;ownedPaths[i]=path;
  }
  if(!hasPrimary)fail();
  // Retain the original path priority where the target is still present.
  for(const auto& original:before.paths)if((original.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativeOwnedPath(original,targets)){
    for(size_t i=0;i<targets.size();++i)if(nativeSameTarget(original,targets[i])){
      const auto originalAt=std::find_if(before.paths.begin(),before.paths.end(),[&](const auto& p){return &p==&original;});
      size_t priority=0;for(auto at=before.paths.begin();at!=originalAt;++at)if(at->flags&DISPLAYCONFIG_PATH_ACTIVE)++priority;
      result.supplied.paths.insert(result.supplied.paths.begin()+std::min(priority,result.supplied.paths.size()),ownedPaths[i]);ownedPaths[i].flags&=~DISPLAYCONFIG_PATH_ACTIVE;
    }
  }
  for(const auto& path:ownedPaths)if(path.flags&DISPLAYCONFIG_PATH_ACTIVE)result.supplied.paths.push_back(path);
  if(result.supplied.paths.size()>64||result.supplied.modes.size()>256)fail();return result;
}
inline bool nativeTopologyMatches(const NativeTopologyPlan& plan,const NativeCcdData& before,const NativeCcdData& after,std::string* failure=nullptr){
  const auto fail=[&](const char* reason){if(failure)*failure=reason;return false;};
  try{
    if(nativeActiveSignatures(before,plan.targets)!=nativeActiveSignatures(after,plan.targets))return fail("native_topology_foreign_changed");
    for(size_t i=0;i<plan.targets.size();++i){const auto& target=plan.targets[i];const auto& placement=plan.placements[i];size_t matches=0;
      const auto expected=std::find_if(plan.supplied.paths.begin(),plan.supplied.paths.end(),[&](const auto& p){return nativeSameTarget(p,target);});
      for(const auto& path:after.paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativeSameTarget(path,target)){
        if(!path.targetInfo.targetAvailable)return fail("native_topology_target_unavailable");
        if(expected==plan.supplied.paths.end()||nativeSourceKey(path)!=nativeSourceKey(*expected))return fail("native_topology_source_changed");
        if(path.targetInfo.rotation!=DISPLAYCONFIG_ROTATION_IDENTITY||path.targetInfo.scaling!=DISPLAYCONFIG_SCALING_IDENTITY)return fail("native_topology_transform_mismatch");
        if(path.flags&DISPLAYCONFIG_PATH_BOOST_REFRESH_RATE||!path.targetInfo.refreshRate.Denominator||uint64_t(path.targetInfo.refreshRate.Numerator)!=60ull*path.targetInfo.refreshRate.Denominator)return fail("native_topology_refresh_mismatch");
        if(path.targetInfo.scanLineOrdering!=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE)return fail("native_topology_scanline_mismatch");
        const auto& source=nativeCcdSource(after,path);const auto& signal=nativeCcdTarget(after,path).targetVideoSignalInfo;
        if(source.width!=target.width||source.height!=target.height||signal.activeSize.cx!=target.width||signal.activeSize.cy!=target.height)return fail("native_topology_size_mismatch");
        if(source.pixelFormat!=DISPLAYCONFIG_PIXELFORMAT_32BPP)return fail("native_topology_format_mismatch");
        if(source.position.x!=placement.left||source.position.y!=placement.top)return fail("native_topology_placement_mismatch");
        if(!signal.vSyncFreq.Denominator||uint64_t(signal.vSyncFreq.Numerator)!=60ull*signal.vSyncFreq.Denominator)return fail("native_topology_refresh_mismatch");
        if(signal.scanLineOrdering!=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE)return fail("native_topology_scanline_mismatch");
        ++matches;
      }
      if(matches!=1)return fail("native_topology_target_unavailable");
    }
    if(failure)failure->clear();
    return true;
  }catch(...){return fail("native_topology_mode_invalid");}
}
struct NativeTopologyBaseline {
  NativeCcdData ccd;
  std::vector<NativeDisplayTarget> targets;
  std::vector<NativeOutputPlacement> placements;
  std::string replacedMonitor;
};
inline NativeCcdData nativeForeignCcd(const NativeCcdData& data,std::span<const NativeDisplayTarget> owned){
  auto result=nativeActiveCcd(data);result.paths.erase(std::remove_if(result.paths.begin(),result.paths.end(),[&](const auto& path){return nativeOwnedPath(path,owned);}),result.paths.end());return result;
}
inline NativeTopologyBaseline captureNativeTopologyBaseline(const NativeCcdData& ccd,std::span<const NativeDisplayTarget> targets,const std::string& replacedMonitor){
  if(targets.empty()||targets.size()>2||ccd.paths.size()>nativeCcdQueryPathLimit||ccd.modes.size()>nativeCcdQueryModeLimit||(replacedMonitor!="0"&&replacedMonitor!="1"))throw std::runtime_error("native_topology_baseline_invalid");
  NativeTopologyBaseline result;result.ccd=nativeActiveCcd(ccd);result.targets.assign(targets.begin(),targets.end());result.replacedMonitor=replacedMonitor;
  if(result.ccd.paths.size()>64||result.ccd.modes.size()>256)throw std::runtime_error("native_topology_baseline_invalid");
  std::set<decltype(nativeSourceKey(DISPLAYCONFIG_PATH_INFO{}))> sources;
  for(size_t i=0;i<targets.size();++i){const auto& target=targets[i];if(target.id!=std::to_string(i))throw std::runtime_error("native_topology_baseline_invalid");size_t count=0;
    for(const auto& path:ccd.paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativeSameTarget(path,target)){
      const auto& source=nativeCcdSource(ccd,path);(void)nativeCcdTarget(ccd,path);
      if(!path.targetInfo.targetAvailable||source.width!=target.width||source.height!=target.height||!sources.insert(nativeSourceKey(path)).second)throw std::runtime_error("native_topology_baseline_invalid");
      result.placements.push_back({target.id,source.position.x,source.position.y,!source.position.x&&!source.position.y});++count;
    }
    if(count!=1)throw std::runtime_error("native_topology_baseline_invalid");
  }
  if(std::none_of(targets.begin(),targets.end(),[&](const auto& value){return value.id==replacedMonitor;}))throw std::runtime_error("native_topology_baseline_invalid");
  // Verify every original path before an SDK change can invalidate its IDs.
  (void)nativeActiveSignatures(ccd);return result;
}
// Windows' automatic re-layout after an owned output changes size: foreign
// outputs that sat right of (or below) the changed owned output follow its
// width (height) change so the desktop stays adjacent. Returns the original
// foreign CCD with ONLY those source positions moved, or nothing. Every other
// documented path/mode field (adapter/target/source identity, active state,
// size, refresh, format, rotation, scaling, scanline) must stay identical.
inline std::optional<NativeCcdData> nativeForeignReflowExpectation(const NativeTopologyBaseline& baseline,const NativeCcdData& original,const NativeCcdData& current,std::span<const NativeDisplayTarget> targets){
  const auto changedBefore=std::find_if(baseline.targets.begin(),baseline.targets.end(),[&](const auto& value){return value.id==baseline.replacedMonitor;});
  const auto changedAfter=std::find_if(targets.begin(),targets.end(),[&](const auto& value){return value.id==baseline.replacedMonitor;});
  const auto anchor=std::find_if(baseline.placements.begin(),baseline.placements.end(),[&](const auto& value){return value.id==baseline.replacedMonitor;});
  if(changedBefore==baseline.targets.end()||changedAfter==targets.end()||anchor==baseline.placements.end()||original.paths.empty()||original.paths.size()!=current.paths.size())return {};
  const int64_t dw=int64_t(changedAfter->width)-changedBefore->width,dh=int64_t(changedAfter->height)-changedBefore->height;if(!dw&&!dh)return {};
  const int64_t right=int64_t(anchor->left)+changedBefore->width,bottom=int64_t(anchor->top)+changedBefore->height;
  auto shifted=original;std::map<UINT32,POINTL> moved;
  for(const auto& path:original.paths){
    size_t count=0;const DISPLAYCONFIG_PATH_INFO* match=nullptr;
    for(const auto& candidate:current.paths)if(nativeSameLuid(candidate.targetInfo.adapterId,path.targetInfo.adapterId)&&candidate.targetInfo.id==path.targetInfo.id&&nativeSourceKey(candidate)==nativeSourceKey(path)){match=&candidate;++count;}
    if(count!=1)return {};
    const auto& before=nativeCcdSource(original,path);const auto& after=nativeCcdSource(current,*match);
    const int64_t dx=int64_t(after.position.x)-before.position.x,dy=int64_t(after.position.y)-before.position.y;
    if(dx&&(dx!=dw||before.position.x<right))return {};
    if(dy&&(dy!=dh||before.position.y<bottom))return {};
    const auto index=nativeSourceIndex(path);const auto previous=moved.find(index);
    if(previous!=moved.end()){if(previous->second.x!=after.position.x||previous->second.y!=after.position.y)return {};continue;}
    moved.emplace(index,after.position);shifted.modes[index].sourceMode.position=after.position;
  }
  const auto asSet=[](const auto& values){return std::multiset<std::vector<int64_t>>(values.begin(),values.end());};
  if(asSet(nativeActiveSignatures(shifted))!=asSet(nativeActiveSignatures(current)))return {};
  return shifted;
}
inline bool nativeForeignTranslationMatches(const NativeTopologyBaseline& baseline,const NativeCcdData& fresh,std::span<const NativeDisplayTarget> targets);
// Expected foreign geometry for a planned transition (see the two rules).
inline std::optional<NativeCcdData> nativeForeignTransitionExpectation(const NativeTopologyBaseline& baseline,const NativeCcdData& fresh,std::span<const NativeDisplayTarget> targets){
  const auto original=nativeForeignCcd(baseline.ccd,baseline.targets);
  if(nativeForeignTranslationMatches(baseline,fresh,targets))return original;
  return nativeForeignReflowExpectation(baseline,original,nativeForeignCcd(fresh,targets),targets);
}
inline bool nativeForeignTransitionMatches(const NativeTopologyBaseline& baseline,const NativeCcdData& fresh,std::span<const NativeDisplayTarget> targets){
  return nativeForeignTransitionExpectation(baseline,fresh,targets).has_value();
}
// Foreign-only active CCD carried by a plan (its expected foreign geometry).
inline NativeCcdData nativePlanForeign(const NativeTopologyPlan& plan){
  NativeCcdData only;only.modes=plan.supplied.modes;
  for(const auto& path:plan.supplied.paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&!nativeOwnedPath(path,plan.targets))only.paths.push_back(path);
  return nativeActiveCcd(only);
}
inline bool nativeForeignTranslationMatches(const NativeTopologyBaseline& baseline,const NativeCcdData& fresh,std::span<const NativeDisplayTarget> targets){
  const auto original=nativeForeignCcd(baseline.ccd,baseline.targets);auto current=nativeForeignCcd(fresh,targets);
  const auto asSet=[](const auto& values){return std::multiset<std::vector<int64_t>>(values.begin(),values.end());};
  const auto expected=asSet(nativeActiveSignatures(original));
  if(expected==asSet(nativeActiveSignatures(current)))return true;
  const auto primary=std::find_if(baseline.placements.begin(),baseline.placements.end(),[&](const auto& value){return value.id==baseline.replacedMonitor&&value.primary;});
  if(primary==baseline.placements.end()||original.paths.empty()||current.paths.size()!=original.paths.size())return false;
  const auto& first=original.paths.front();const auto match=std::find_if(current.paths.begin(),current.paths.end(),[&](const auto& value){return value.targetInfo.id==first.targetInfo.id&&nativeSameLuid(value.targetInfo.adapterId,first.targetInfo.adapterId)&&nativeSourceKey(value)==nativeSourceKey(first);});
  if(match==current.paths.end())return false;
  const auto& oldSource=nativeCcdSource(original,first);const auto& newSource=nativeCcdSource(current,*match);
  const int64_t dx=int64_t(newSource.position.x)-oldSource.position.x,dy=int64_t(newSource.position.y)-oldSource.position.y;
  if(!dx&&!dy)return false;
  // Replacing an owned primary can move the desktop origin to a surviving
  // known output. Accept only that uniform translation; all other documented
  // path/mode fields must still match the pre-change foreign configuration.
  bool knownRoot=false;
  for(const auto& path:fresh.paths)if(path.flags&DISPLAYCONFIG_PATH_ACTIVE){const auto& source=nativeCcdSource(fresh,path);if(source.position.x||source.position.y)continue;
    for(const auto& old:baseline.ccd.paths)if((old.flags&DISPLAYCONFIG_PATH_ACTIVE)&&old.targetInfo.id==path.targetInfo.id&&nativeSameLuid(old.targetInfo.adapterId,path.targetInfo.adapterId)&&nativeSourceKey(old)==nativeSourceKey(path)){
      const auto changed=std::find_if(baseline.targets.begin(),baseline.targets.end(),[&](const auto& value){return value.id==baseline.replacedMonitor;});
      if(changed!=baseline.targets.end()&&nativeSameTarget(old,*changed))continue;
      const auto& prior=nativeCcdSource(baseline.ccd,old);if(int64_t(prior.position.x)+dx==0&&int64_t(prior.position.y)+dy==0)knownRoot=true;
    }
  }
  if(!knownRoot)return false;
  std::set<UINT32> adjusted;
  for(const auto& path:current.paths){const auto index=nativeSourceIndex(path);if(!adjusted.insert(index).second)continue;auto& position=current.modes[index].sourceMode.position;
    const auto x=int64_t(position.x)-dx,y=int64_t(position.y)-dy;if(x<LONG_MIN||x>LONG_MAX||y<LONG_MIN||y>LONG_MAX)return false;position={LONG(x),LONG(y)};
  }
  return expected==asSet(nativeActiveSignatures(current));
}
inline NativeTopologyPlan planNativeTopologyTransition(const NativeTopologyBaseline& baseline,const NativeCcdData& fresh,std::span<const NativeDisplayTarget> targets,std::span<const NativeOutputPlacement> exact={}){
  if(fresh.paths.size()>nativeCcdQueryPathLimit||fresh.modes.size()>nativeCcdQueryModeLimit)throw std::runtime_error("native_topology_plan_invalid");
  for(const auto& path:fresh.paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativeOwnedPath(path,baseline.targets)&&!nativeOwnedPath(path,targets))throw std::runtime_error("native_topology_departure_pending");
  const auto expectedForeign=nativeForeignTransitionExpectation(baseline,fresh,targets);if(!expectedForeign)throw std::runtime_error("native_topology_foreign_changed");
  NativeCcdData supplied;std::map<std::pair<int,UINT32>,UINT32> copied;
  const auto copyPath=[&](DISPLAYCONFIG_PATH_INFO path,const NativeCcdData& from,int origin){
    const auto copyMode=[&](UINT32 index,DISPLAYCONFIG_MODE_INFO_TYPE type){const auto key=std::pair{origin,index};const auto found=copied.find(key);if(found!=copied.end())return found->second;const auto& mode=nativeCcdMode(from,index,type);const auto destination=UINT32(supplied.modes.size());supplied.modes.push_back(mode);copied.emplace(key,destination);return destination;};
    if(path.flags&DISPLAYCONFIG_PATH_ACTIVE){
      const auto source=copyMode(nativeSourceIndex(path),DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE),target=copyMode(nativeTargetIndex(path),DISPLAYCONFIG_MODE_INFO_TYPE_TARGET);
      if(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE){if(path.targetInfo.desktopModeInfoIdx!=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID)path.targetInfo.desktopModeInfoIdx=copyMode(path.targetInfo.desktopModeInfoIdx,DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE);path.sourceInfo.sourceModeInfoIdx=source;path.targetInfo.targetModeInfoIdx=target;}
      else{path.sourceInfo.modeInfoIdx=source;path.targetInfo.modeInfoIdx=target;}
    }else if(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE){path.sourceInfo.cloneGroupId=DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;path.sourceInfo.sourceModeInfoIdx=DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID;path.targetInfo.targetModeInfoIdx=DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;path.targetInfo.desktopModeInfoIdx=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;}
    else path.sourceInfo.modeInfoIdx=path.targetInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    supplied.paths.push_back(path);
  };
  for(const auto& path:fresh.paths)if(!(path.flags&DISPLAYCONFIG_PATH_ACTIVE)||nativeOwnedPath(path,targets))copyPath(path,fresh,0);
  // Foreign paths keep their original modes; only an accepted automatic
  // re-layout position (expectation) differs from the baseline bytes.
  const auto& foreign=*expectedForeign;for(const auto& path:foreign.paths)copyPath(path,foreign,1);
  std::vector<NativeOutputPlacement> placements;for(const auto& target:targets){const auto found=std::find_if(baseline.placements.begin(),baseline.placements.end(),[&](const auto& value){return value.id==target.id;});placements.push_back(found==baseline.placements.end()?NativeOutputPlacement{target.id,LONG_MAX,LONG_MAX,false}:*found);}
  const auto changedBefore=std::find_if(baseline.targets.begin(),baseline.targets.end(),[&](const auto& value){return value.id==baseline.replacedMonitor;});
  const auto changedAfter=std::find_if(targets.begin(),targets.end(),[&](const auto& value){return value.id==baseline.replacedMonitor;});
  const auto anchor=std::find_if(baseline.placements.begin(),baseline.placements.end(),[&](const auto& value){return value.id==baseline.replacedMonitor;});
  // Removing the owned primary must hand the origin to a surviving owned
  // output. Keep the foreign paths at their original positions; the planner
  // refuses this handoff if the survivor would overlap one of them.
  if(changedBefore!=baseline.targets.end()&&changedAfter==targets.end()&&anchor!=baseline.placements.end()&&anchor->primary&&!placements.empty()){
    placements.front().left=0;placements.front().top=0;placements.front().primary=true;
  }
  if(changedBefore!=baseline.targets.end()&&changedAfter!=targets.end()&&anchor!=baseline.placements.end())for(auto& placement:placements)if(placement.id!=anchor->id&&!placement.primary){
    const auto old=std::find_if(baseline.placements.begin(),baseline.placements.end(),[&](const auto& value){return value.id==placement.id;});if(old==baseline.placements.end())continue;
    if(int64_t(old->left)==int64_t(anchor->left)+changedBefore->width&&old->top==anchor->top){const auto next=int64_t(anchor->left)+changedAfter->width;if(next<LONG_MIN||next>LONG_MAX)throw std::runtime_error("native_topology_plan_invalid");placement.left=LONG(next);}
    if(int64_t(old->top)==int64_t(anchor->top)+changedBefore->height&&old->left==anchor->left){const auto next=int64_t(anchor->top)+changedAfter->height;if(next<LONG_MIN||next>LONG_MAX)throw std::runtime_error("native_topology_plan_invalid");placement.top=LONG(next);}
  }
  if(!exact.empty()){
    if(exact.size()!=targets.size())throw std::runtime_error("native_topology_plan_invalid");
    placements.assign(exact.begin(),exact.end());
  }
  auto result=planNativeTopology(supplied,targets,placements);
  // A group viewport is one exact relative layout. The general bootstrap
  // planner may move a colliding output; that is not an acceptable resize ACK.
  if(!exact.empty())for(const auto& actual:result.placements){
    const auto requested=std::find_if(exact.begin(),exact.end(),[&](const auto& value){return value.id==actual.id;});
    if(requested==exact.end()||requested->left!=actual.left||requested->top!=actual.top||requested->primary!=actual.primary)throw std::runtime_error("native_topology_exact_layout_failed");
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> ordered;std::set<size_t> used;
  for(const auto& old:baseline.ccd.paths)if(old.flags&DISPLAYCONFIG_PATH_ACTIVE){
    const auto owner=std::find_if(baseline.targets.begin(),baseline.targets.end(),[&](const auto& value){return nativeSameTarget(old,value);});
    for(size_t i=0;i<result.supplied.paths.size();++i){const auto& path=result.supplied.paths[i];bool matches=false;
      if(owner==baseline.targets.end())matches=old.targetInfo.id==path.targetInfo.id&&nativeSameLuid(old.targetInfo.adapterId,path.targetInfo.adapterId)&&nativeSourceKey(old)==nativeSourceKey(path);
      else{const auto target=std::find_if(targets.begin(),targets.end(),[&](const auto& value){return value.id==owner->id;});matches=target!=targets.end()&&nativeSameTarget(path,*target);}
      if(matches&&used.insert(i).second)ordered.push_back(path);
    }
  }
  for(size_t i=0;i<result.supplied.paths.size();++i)if(used.insert(i).second)ordered.push_back(result.supplied.paths[i]);
  result.supplied.paths=std::move(ordered);return result;
}
struct NativeCcdQueryAccess {
  std::function<LONG(UINT32,UINT32*,UINT32*)> sizes=GetDisplayConfigBufferSizes;
  std::function<LONG(UINT32,UINT32*,DISPLAYCONFIG_PATH_INFO*,UINT32*,DISPLAYCONFIG_MODE_INFO*)> read=[](UINT32 flags,UINT32* paths,DISPLAYCONFIG_PATH_INFO* pathData,UINT32* modes,DISPLAYCONFIG_MODE_INFO* modeData){return QueryDisplayConfig(flags,paths,pathData,modes,modeData,nullptr);};
};
inline NativeCcdData queryNativeCcdRaw(const NativeCcdQueryAccess& access={},UINT32 flags=QDC_ALL_PATHS|QDC_VIRTUAL_MODE_AWARE){
  for(int attempt=0;attempt<3;++attempt){UINT32 paths=0,modes=0;
    const auto sized=access.sizes(flags,&paths,&modes);
    if(sized!=ERROR_SUCCESS)throw NativeTopologyWindowsError(DWORD(sized));
    if(paths>nativeCcdQueryPathLimit||modes>nativeCcdQueryModeLimit)throw NativeTopologyWindowsError(ERROR_BUFFER_OVERFLOW);
    NativeCcdData result;result.paths.resize(paths);result.modes.resize(modes);
    const auto error=access.read(flags,&paths,result.paths.data(),&modes,result.modes.data());
    if(error==ERROR_INSUFFICIENT_BUFFER)continue;if(error!=ERROR_SUCCESS)throw NativeTopologyWindowsError(DWORD(error));
    if(paths>result.paths.size()||modes>result.modes.size())throw NativeTopologyWindowsError(ERROR_INVALID_DATA);
    result.paths.resize(paths);result.modes.resize(modes);return result;
  }
  throw NativeTopologyWindowsError(ERROR_INSUFFICIENT_BUFFER);
}
inline NativeCcdData queryNativeCcd(const NativeCcdQueryAccess& access={}){
  auto result=queryNativeCcdRaw(access);normalizeNativeCcdModeReferences(result);return result;
}
inline NativeCcdData queryNativeCcdDatabaseRaw(){
  DISPLAYCONFIG_TOPOLOGY_ID topology{};NativeCcdQueryAccess access;
  access.read=[&topology](UINT32 flags,UINT32* paths,DISPLAYCONFIG_PATH_INFO* p,UINT32* modes,DISPLAYCONFIG_MODE_INFO* m){return QueryDisplayConfig(flags,paths,p,modes,m,&topology);};
  return queryNativeCcdRaw(access,QDC_DATABASE_CURRENT|QDC_VIRTUAL_MODE_AWARE);
}
using NativePackedCcdKey=std::tuple<LONG,DWORD,UINT32,LONG,DWORD,UINT32>;
inline NativePackedCcdKey nativePackedCcdKey(const DISPLAYCONFIG_PATH_INFO& path){
  return {path.sourceInfo.adapterId.HighPart,path.sourceInfo.adapterId.LowPart,path.sourceInfo.id,path.targetInfo.adapterId.HighPart,path.targetInfo.adapterId.LowPart,path.targetInfo.id};
}
inline std::set<NativePackedCcdKey> nativeObservedPackedCcdKeys(const NativeCcdData& raw){
  // Retain only the exact unflagged legacy packing proven by the read-side
  // validator. Never infer this representation from a provider or GPU name.
  std::set<NativePackedCcdKey> keys;
  for(const auto& path:raw.paths){
    std::array<DISPLAYCONFIG_PATH_INFO,1> copy{path};
    if(normalizeNativeCcdModeReferences(copy,raw.modes)!=1)continue;
    const auto key=nativePackedCcdKey(path);
    if(std::count_if(raw.paths.begin(),raw.paths.end(),[&](const auto& other){return (other.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativePackedCcdKey(other)==key;})==1)keys.insert(key);
  }
  return keys;
}
inline size_t encodeNativeCcdWireReferences(NativeCcdData& wire,const std::set<NativePackedCcdKey>& observed){
  size_t changed=0;
  for(auto& path:wire.paths){
    if(!(path.flags&DISPLAYCONFIG_PATH_ACTIVE)||(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)||!observed.count(nativePackedCcdKey(path)))continue;
    const auto source=path.sourceInfo.modeInfoIdx,target=path.targetInfo.modeInfoIdx;
    if(source>=wire.modes.size()||target>=wire.modes.size()||source>=0xffff||target>=0xffff)continue;
    const auto& s=wire.modes[source];const auto& t=wire.modes[target];
    if(s.infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE||t.infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET||
       !nativeSameLuid(s.adapterId,path.sourceInfo.adapterId)||s.id!=path.sourceInfo.id||
       !nativeSameLuid(t.adapterId,path.targetInfo.adapterId)||t.id!=path.targetInfo.id)continue;
    path.sourceInfo.modeInfoIdx=(source<<16)|0xffff;path.targetInfo.modeInfoIdx=(target<<16)|0xffff;++changed;
  }
  return changed;
}
inline bool nativeSameSuppliedCcdBytes(const NativeCcdData& a,const NativeCcdData& b){
  // These are copies of the SAME supplied plan, not independently queried CCD.
  return a.paths.size()==b.paths.size()&&a.modes.size()==b.modes.size()&&
    (a.paths.empty()||std::memcmp(a.paths.data(),b.paths.data(),a.paths.size()*sizeof(DISPLAYCONFIG_PATH_INFO))==0)&&
    (a.modes.empty()||std::memcmp(a.modes.data(),b.modes.data(),a.modes.size()*sizeof(DISPLAYCONFIG_MODE_INFO))==0);
}
inline std::function<LONG(const NativeCcdData&,UINT32)> nativeCcdWireConfigure(
    std::function<NativeCcdData()> rawQuery,std::function<LONG(const NativeCcdData&,UINT32)> configure,
    std::function<NativeCcdData()> availableQuery={},std::function<NativeCcdData()> databaseQuery={}){
  struct Validated{NativeCcdData canonical,wire;UINT32 flags=0;};
  return [rawQuery=std::move(rawQuery),configure=std::move(configure),availableQuery=std::move(availableQuery),databaseQuery=std::move(databaseQuery),history=std::set<NativePackedCcdKey>{},seen=std::set<NativePackedCcdKey>{},databaseAllowed=true,validated=std::optional<Validated>{}](const NativeCcdData& plan,UINT32 flags)mutable->LONG{
    const bool checking=(flags&SDC_VALIDATE)!=0,applying=(flags&SDC_APPLY)!=0;
    if(checking==applying)return ERROR_INVALID_PARAMETER;
    try{
      auto wire=plan;
      if((flags&SDC_USE_SUPPLIED_DISPLAY_CONFIG)&&!wire.modes.empty()){
        const auto raw=rawQuery();const auto current=nativeObservedPackedCcdKeys(raw);
        if(raw.paths.size()>nativeCcdQueryPathLimit)throw NativeTopologyWindowsError(ERROR_BUFFER_OVERFLOW);
        std::set<NativePackedCcdKey> active;
        for(const auto& path:raw.paths)if(path.flags&DISPLAYCONFIG_PATH_ACTIVE)active.insert(nativePackedCcdKey(path));
        // Fresh active observations override older packed evidence, including
        // ordinary flat, virtual-aware, malformed and duplicate active paths.
        for(const auto& key:active)history.erase(key);
        history.insert(current.begin(),current.end());
        seen.insert(active.begin(),active.end());
        // Keep negative observations too: an older persisted database must not
        // resurrect packing contradicted by a newer active flat/virtual path.
        // If the bounded worker cache fills, disable database fallback rather
        // than forget a negative observation and trust older evidence again.
        if(seen.size()>nativeCcdQueryPathLimit){databaseAllowed=false;seen=active;history=current;}
        NativeCcdData available=raw;bool availabilityRead=true;
        if(availableQuery)try{available=availableQuery();}catch(...){availabilityRead=false;}
        const auto availableOnce=[&](const NativePackedCcdKey& key){return availabilityRead&&std::count_if(available.paths.begin(),available.paths.end(),[&](const auto& path){return path.targetInfo.targetAvailable&&nativePackedCcdKey(path)==key;})==1;};
        auto proven=current;
        for(const auto& key:history)if(!active.count(key)&&availableOnce(key))proven.insert(key);
        const bool inactiveLegacy=std::any_of(wire.paths.begin(),wire.paths.end(),[&](const auto& path){const auto key=nativePackedCcdKey(path);return (path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&!(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)&&!seen.count(key)&&!proven.count(key)&&availableOnce(key);});
        if(inactiveLegacy&&databaseAllowed&&databaseQuery)try{
          // Cold recovery: the Windows database is another RAW API witness,
          // never the normalized saved state or an adapter/provider name.
          // Re-read it for VALIDATE and APPLY; do not retain database evidence.
          const auto database=nativeObservedPackedCcdKeys(databaseQuery());
          for(const auto& key:database)if(!seen.count(key)&&availableOnce(key))proven.insert(key);
        }catch(...){ }
        encodeNativeCcdWireReferences(wire,proven);
      }
      const auto commonFlags=flags&~(SDC_VALIDATE|SDC_APPLY);
      if(checking){
        validated.reset();const auto result=configure(wire,flags);
        if(result==ERROR_SUCCESS)validated=Validated{plan,std::move(wire),commonFlags};
        return result;
      }
      // Fresh RAW identity/encoding must still produce the validated buffers.
      if(!validated||validated->flags!=commonFlags||!nativeSameSuppliedCcdBytes(plan,validated->canonical)||!nativeSameSuppliedCcdBytes(wire,validated->wire)){validated.reset();return ERROR_RETRY;}
      auto applied=std::move(validated->wire);validated.reset();return configure(applied,flags);
    }catch(const NativeTopologyWindowsError& error){validated.reset();return LONG(error.code);}
    catch(...){validated.reset();return ERROR_INVALID_DATA;}
  };
}
inline std::function<LONG(const NativeCcdData&,UINT32)> windowsNativeCcdConfigure(){
  return nativeCcdWireConfigure(
    // ACTIVE includes the current mode records; ALL may omit their references.
    []{return queryNativeCcdRaw({},QDC_ONLY_ACTIVE_PATHS|QDC_VIRTUAL_MODE_AWARE);},
    [](const NativeCcdData& ccd,UINT32 flags){auto copy=ccd;return SetDisplayConfig(UINT32(copy.paths.size()),copy.paths.empty()?nullptr:copy.paths.data(),UINT32(copy.modes.size()),copy.modes.empty()?nullptr:copy.modes.data(),flags);},
    []{return queryNativeCcdRaw();},queryNativeCcdDatabaseRaw);
}
inline bool nativePathsAvailable(const NativeCcdData& requested,const NativeCcdData& fresh){
  return std::all_of(requested.paths.begin(),requested.paths.end(),[&](const auto& path){return std::any_of(fresh.paths.begin(),fresh.paths.end(),[&](const auto& candidate){return candidate.targetInfo.targetAvailable&&nativeSameLuid(path.targetInfo.adapterId,candidate.targetInfo.adapterId)&&path.targetInfo.id==candidate.targetInfo.id&&nativeSourceKey(path)==nativeSourceKey(candidate);});});
}
struct NativeTopologyAccess {
  std::function<NativeCcdData()> query;
  std::function<LONG(const NativeCcdData&,UINT32)> configure;
  std::function<bool()> eligible;
  std::function<uint64_t()> now=[] {return GetTickCount64();};
  std::function<void()> pause=[] {std::this_thread::sleep_for(std::chrono::milliseconds(50));};
};
// Clone paths share one desktop source. Adjacency needs a positive edge overlap;
// touching a corner alone does not connect two Windows desktop regions.
inline bool nativeCountLayoutConnected(const NativeCcdData& ccd){
  std::set<decltype(nativeSourceKey(DISPLAYCONFIG_PATH_INFO{}))> sources;std::vector<RECT> screens;
  for(const auto& path:ccd.paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&sources.insert(nativeSourceKey(path)).second){const auto bounds=nativeSourceBounds(nativeCcdSource(ccd,path));if(!bounds)return false;screens.push_back(*bounds);}
  if(screens.empty())return false;std::set<size_t> reached{0};bool changed=true;
  while(changed){changed=false;for(size_t i=0;i<screens.size();++i)if(!reached.count(i))for(const auto j:reached){const auto& a=screens[i];const auto& b=screens[j];
    if(((a.right==b.left||b.right==a.left)&&std::max(a.top,b.top)<std::min(a.bottom,b.bottom))||((a.bottom==b.top||b.bottom==a.top)&&std::max(a.left,b.left)<std::min(a.right,b.right))){reached.insert(i);changed=true;break;}
  }}return reached.size()==screens.size();
}
inline NativeTopologyPlan planNativeCountTopologyTransition(const NativeTopologyBaseline& baseline,const NativeCcdData& fresh,std::span<const NativeDisplayTarget> targets){
  auto original=planNativeTopologyTransition(baseline,fresh,targets);if(nativeCountLayoutConnected(original.supplied))return original;
  // Removing owned output1 can remove the bridge between output0 and the
  // untouched console. Only the remaining nonprimary owned output may move.
  if(baseline.targets.size()!=2||targets.size()!=1||original.placements.size()!=1||original.placements[0].primary)throw std::runtime_error("native_count_topology_disconnected");
  const auto anchor=original.placements[0];const auto& target=targets[0];std::optional<NativeTopologyPlan> best;int64_t distance=INT64_MAX;
  const auto consider=[&](int64_t x,int64_t y){
    if(!displayRectangleFits(x,y,target.width,target.height))return;
    try{const std::array<NativeOutputPlacement,1> placement{{{target.id,LONG(x),LONG(y),false}}};auto candidate=planNativeTopologyTransition(baseline,fresh,targets,placement);if(!nativeCountLayoutConnected(candidate.supplied))return;
      const auto delta=std::abs(x-anchor.left)+std::abs(y-anchor.top);if(delta<distance){distance=delta;best=std::move(candidate);}
    }catch(...){ }
  };
  std::set<decltype(nativeSourceKey(DISPLAYCONFIG_PATH_INFO{}))> sources;
  for(const auto& path:original.supplied.paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&!nativeOwnedPath(path,targets)&&sources.insert(nativeSourceKey(path)).second){const auto& s=nativeCcdSource(original.supplied,path);const int64_t right=int64_t(s.position.x)+s.width,bottom=int64_t(s.position.y)+s.height;
    for(const auto y:{int64_t(s.position.y),bottom-target.height,std::clamp<int64_t>(anchor.top,int64_t(s.position.y)-target.height+1,bottom-1)}){consider(right,y);consider(int64_t(s.position.x)-target.width,y);}
    for(const auto x:{int64_t(s.position.x),right-target.width,std::clamp<int64_t>(anchor.left,int64_t(s.position.x)-target.width+1,right-1)}){consider(x,bottom);consider(x,int64_t(s.position.y)-target.height);}
  }
  if(!best)throw std::runtime_error("native_count_topology_disconnected");return std::move(*best);
}
inline NativeTopologyAccess windowsNativeTopologyAccess(std::function<bool()> eligible){
  return {[]{return queryNativeCcd();},windowsNativeCcdConfigure(),std::move(eligible)};
}
struct NativeTopologyResult {bool ok=false,applyAttempted=false,restoreAttempted=false,topologyRestored=false,recoveryRequired=false;std::string reason="native_topology_failed";DWORD systemError=0;std::optional<NativeCcdDiagnostic> ccdDiagnostic;};
inline NativeTopologyResult activateNativeTopology(std::span<const NativeDisplayTarget> targets,NativeTopologyAccess access,DWORD timeout=5000,std::span<const NativeOutputPlacement> previous={},const NativeTopologyBaseline* baseline=nullptr,std::span<const NativeOutputPlacement> exact={},std::function<NativeTopologyPlan(const NativeCcdData&)> planner={}){
  NativeTopologyResult result;NativeCcdData before;bool captured=false;
  constexpr UINT32 options=SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_VIRTUAL_MODE_AWARE;
  const auto deadline=access.now()+std::min<DWORD>(timeout,10000);
  const auto eligible=[&]{return timeout&&access.now()<deadline&&access.eligible();};
  try{
    if(!exact.empty()&&!baseline){result.reason="native_topology_baseline_required";return result;}
    if(!eligible()){result.reason="interactive_console_required";return result;}
    NativeTopologyPlan plan;const auto gatherUntil=access.now()+std::min<DWORD>(timeout/2,1500);
    for(;;){try{before=access.query();plan=planner?planner(before):baseline?planNativeTopologyTransition(*baseline,before,targets,exact):planNativeTopology(before,targets,previous);break;}catch(...){if(!baseline||!eligible()||access.now()>=gatherUntil)throw;access.pause();}}
    // The plan carries the expected foreign geometry (baseline bytes, or the
    // accepted automatic re-layout). Verify exactly that, never a guess.
    captured=true;const auto preservation=baseline?nativePlanForeign(plan):before;
    if(nativeTopologyMatches(plan,preservation,before)){result.ok=true;result.reason.clear();return result;}
    if(!eligible()){result.reason="interactive_console_required";return result;}
    const auto validation=access.configure(plan.supplied,SDC_VALIDATE|options);
    if(validation!=ERROR_SUCCESS){result.reason="native_topology_validate_failed";result.systemError=DWORD(validation);return result;}
    if(!eligible()){result.reason="interactive_console_required";return result;}
    const auto prepared=access.query();
    if(nativeActiveSignatures(before)!=nativeActiveSignatures(prepared)||!nativePathsAvailable(plan.supplied,prepared)){result.reason="native_topology_changed";return result;}
    if(!eligible()){result.reason="interactive_console_required";return result;}
    result.applyAttempted=true;
    const auto application=access.configure(plan.supplied,SDC_APPLY|options);
    if(application!=ERROR_SUCCESS){result.systemError=DWORD(application);throw std::runtime_error("native_topology_apply_failed");}
    const auto verifyUntil=deadline>1000?deadline-1000:deadline;
    result.reason="native_topology_not_applied";
    do{if(!eligible())break;if(nativeTopologyMatches(plan,preservation,access.query(),&result.reason)){result.ok=true;result.reason.clear();return result;}if(access.now()>=verifyUntil)break;access.pause();}while(eligible());
  }catch(const NativeTopologyWindowsError& error){result.reason=error.what();result.systemError=error.code;
  }catch(const std::exception& error){
    const std::string reason=error.what();
    static const std::set<std::string> known{"native_topology_query_failed","native_topology_plan_invalid","native_topology_mode_invalid","native_topology_apply_failed","native_topology_foreign_changed","native_topology_departure_pending","native_topology_exact_layout_failed","native_stream_console_profile_changed"};
    result.reason=known.count(reason)?reason:"native_topology_failed";
  }catch(...){result.reason="native_topology_failed";}
  if(!result.applyAttempted)return result;
  result.recoveryRequired=true;
  // A resized SDK target needs a coordinated SDK rollback before its original
  // Windows modes can be restored. Never apply the now-retired target IDs here.
  if(baseline)return result;
  try{
    // Never overwrite an independent foreign-display edit during recovery.
    if(!captured||!eligible()||nativeActiveSignatures(before,targets)!=nativeActiveSignatures(access.query(),targets))return result;
    const auto restore=nativeActiveCcd(before);if(restore.paths.empty())return result;
    if(access.configure(restore,SDC_VALIDATE|options)!=ERROR_SUCCESS||!eligible())return result;
    const auto current=access.query();if(nativeActiveSignatures(before,targets)!=nativeActiveSignatures(current,targets)||!nativePathsAvailable(restore,current))return result;
    result.restoreAttempted=true;
    if(access.configure(restore,SDC_APPLY|options)!=ERROR_SUCCESS)return result;
    result.topologyRestored=nativeActiveSignatures(before)==nativeActiveSignatures(access.query());
    result.recoveryRequired=!result.topologyRestored;
  }catch(...){ }
  return result;
}
}
