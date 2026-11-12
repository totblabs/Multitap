#pragma once
#include "native_display_topology.hpp"

namespace host {
class NativeActivationCcdError final:public std::runtime_error {
 public:NativeActivationCcdError(const char* reason,NativeCcdDiagnostic detail):std::runtime_error(reason),diagnostic(detail){}
  NativeCcdDiagnostic diagnostic;
};
inline const DISPLAYCONFIG_MODE_INFO& nativeActivationMode(const NativeCcdData& ccd,UINT32 index,UINT32 invalid,DISPLAYCONFIG_MODE_INFO_TYPE type,LUID adapter,UINT32 id,uint32_t sample,bool identity=true){
  uint32_t issue=index==invalid?1:index>=ccd.modes.size()?2:0,observed=0;
  if(!issue){const auto& mode=ccd.modes[index];observed=uint32_t(mode.infoType);if(mode.infoType!=type)issue=3;
    else if(identity&&(!nativeSameLuid(mode.adapterId,adapter)||mode.id!=id))issue=4;}
  if(issue)throw NativeActivationCcdError("native_topology_mode_invalid",{sample*10+uint32_t(type),index,uint32_t(type),observed,issue});
  return ccd.modes[index];
}
inline auto nativeActivationPathKey(const DISPLAYCONFIG_PATH_INFO& path){
  return std::tuple{nativeSourceKey(path),path.targetInfo.adapterId.HighPart,path.targetInfo.adapterId.LowPart,path.targetInfo.id};
}
inline auto nativeActivationPathFields(const DISPLAYCONFIG_PATH_INFO& path){
  return std::tuple{path.flags,path.sourceInfo.statusFlags,path.targetInfo.statusFlags,path.targetInfo.targetAvailable,
    path.targetInfo.outputTechnology,path.targetInfo.rotation,path.targetInfo.scaling,path.targetInfo.refreshRate.Numerator,
    path.targetInfo.refreshRate.Denominator,path.targetInfo.scanLineOrdering,
    path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE?path.sourceInfo.cloneGroupId:DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID};
}
inline void validateNativeActivationActiveCcd(const NativeCcdData& active,uint32_t sample){
  if(active.paths.size()>nativeCcdQueryPathLimit||active.modes.size()>nativeCcdQueryModeLimit)throw std::runtime_error("native_topology_plan_invalid");
  std::set<std::tuple<LONG,DWORD,UINT32>> targets;
  std::map<std::tuple<int,LONG,DWORD,UINT32>,DISPLAYCONFIG_MODE_INFO> identities;
  const auto mode=[&](const DISPLAYCONFIG_PATH_INFO& path,UINT32 index,DISPLAYCONFIG_MODE_INFO_TYPE type){
    const bool source=type==DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE,desktop=type==DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE;
    const UINT32 invalid=path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE?DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID:DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    const auto& value=nativeActivationMode(active,index,desktop?DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID:invalid,type,source?path.sourceInfo.adapterId:path.targetInfo.adapterId,source?path.sourceInfo.id:path.targetInfo.id,sample,!desktop);
    if(type==DISPLAYCONFIG_MODE_INFO_TYPE_TARGET&&value.targetMode.targetVideoSignalInfo.pixelRate>INT64_MAX)throw NativeActivationCcdError("native_topology_mode_invalid",{sample*10+uint32_t(type),index,uint32_t(type),uint32_t(type),7});
    const auto key=std::tuple{int(type),value.adapterId.HighPart,value.adapterId.LowPart,value.id};
    const auto [at,inserted]=identities.emplace(key,value);
    if(!inserted&&!nativeSameMode(at->second,value))throw NativeActivationCcdError("native_topology_mode_invalid",{sample*10+uint32_t(type),index,uint32_t(type),uint32_t(type),5});
  };
  for(const auto& path:active.paths){
    if(!(path.flags&DISPLAYCONFIG_PATH_ACTIVE)||!path.targetInfo.targetAvailable||
       !targets.emplace(path.targetInfo.adapterId.HighPart,path.targetInfo.adapterId.LowPart,path.targetInfo.id).second)throw std::runtime_error("native_topology_changed");
    mode(path,nativeSourceIndex(path),DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE);mode(path,nativeTargetIndex(path),DISPLAYCONFIG_MODE_INFO_TYPE_TARGET);
    if((path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)&&path.targetInfo.desktopModeInfoIdx!=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID)
      mode(path,path.targetInfo.desktopModeInfoIdx,DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE);
  }
}
// ALL_PATHS may omit active mode records. Only two identical complete ACTIVE
// observations may supply those records; ALL still proves every path identity.
inline NativeCcdData mergeNativeActivationCcd(const NativeCcdData& activeBefore,const NativeCcdData& all,const NativeCcdData& activeAfter){
  validateNativeActivationActiveCcd(activeBefore,1);validateNativeActivationActiveCcd(activeAfter,3);
  if(all.paths.size()>nativeCcdQueryPathLimit||all.modes.size()>nativeCcdQueryModeLimit)throw std::runtime_error("native_topology_plan_invalid");
  if(nativeActiveSignatures(activeBefore)!=nativeActiveSignatures(activeAfter))throw std::runtime_error("native_topology_changed");
  for(size_t i=0;i<activeBefore.paths.size();++i)if(nativeActivationPathFields(activeBefore.paths[i])!=nativeActivationPathFields(activeAfter.paths[i]))throw std::runtime_error("native_topology_changed");
  std::map<decltype(nativeActivationPathKey(DISPLAYCONFIG_PATH_INFO{})),size_t> active;
  for(size_t i=0;i<activeBefore.paths.size();++i)active.emplace(nativeActivationPathKey(activeBefore.paths[i]),i);
  std::set<decltype(nativeActivationPathKey(DISPLAYCONFIG_PATH_INFO{}))> seen;
  NativeCcdData result=nativeActiveCcd(activeBefore);size_t matched=0;
  const auto compareMode=[&](UINT32 index,UINT32 invalid,DISPLAYCONFIG_MODE_INFO_TYPE type,const DISPLAYCONFIG_MODE_INFO& canonical){
    if(index==invalid||index>=all.modes.size())return;
    const auto& observed=nativeActivationMode(all,index,invalid,type,canonical.adapterId,canonical.id,2);
    if(!nativeSameMode(observed,canonical))throw NativeActivationCcdError("native_topology_changed",{20+uint32_t(type),index,uint32_t(type),uint32_t(observed.infoType),6});
  };
  for(auto path:all.paths){
    const auto key=nativeActivationPathKey(path);
    if(!seen.insert(key).second)throw std::runtime_error("native_topology_changed");
    if(path.flags&DISPLAYCONFIG_PATH_ACTIVE){
      const auto found=active.find(key);if(found==active.end())throw std::runtime_error("native_topology_changed");
      const auto& canonical=activeBefore.paths[found->second];
      if(nativeActivationPathFields(path)!=nativeActivationPathFields(canonical))throw std::runtime_error("native_topology_changed");
      const UINT32 invalid=path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE?DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID:DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
      compareMode(nativeSourceIndex(path),invalid,DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE,nativeCcdMode(activeBefore,nativeSourceIndex(canonical),DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE));
      compareMode(nativeTargetIndex(path),invalid,DISPLAYCONFIG_MODE_INFO_TYPE_TARGET,nativeCcdMode(activeBefore,nativeTargetIndex(canonical),DISPLAYCONFIG_MODE_INFO_TYPE_TARGET));
      if((path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)&&path.targetInfo.desktopModeInfoIdx!=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID){
        if(canonical.targetInfo.desktopModeInfoIdx==DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID)throw std::runtime_error("native_topology_changed");
        compareMode(path.targetInfo.desktopModeInfoIdx,DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID,DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE,nativeCcdMode(activeBefore,canonical.targetInfo.desktopModeInfoIdx,DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE));
      }
      ++matched;
    }else{
      if(active.count(key))throw std::runtime_error("native_topology_changed");
      if(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE){
        path.sourceInfo.cloneGroupId=DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;path.sourceInfo.sourceModeInfoIdx=DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID;
        path.targetInfo.targetModeInfoIdx=DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;path.targetInfo.desktopModeInfoIdx=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
      }else path.sourceInfo.modeInfoIdx=path.targetInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
      result.paths.push_back(path);
    }
  }
  if(matched!=active.size())throw std::runtime_error("native_topology_changed");return result;
}
inline NativeCcdData queryNativeActivationCcd(const NativeCcdQueryAccess& access={}){
  NativeCcdQueryAccess active;
  active.sizes=[&](UINT32,UINT32* paths,UINT32* modes){return access.sizes(QDC_ONLY_ACTIVE_PATHS|QDC_VIRTUAL_MODE_AWARE,paths,modes);};
  active.read=[&](UINT32,UINT32* paths,DISPLAYCONFIG_PATH_INFO* pathData,UINT32* modes,DISPLAYCONFIG_MODE_INFO* modeData){return access.read(QDC_ONLY_ACTIVE_PATHS|QDC_VIRTUAL_MODE_AWARE,paths,pathData,modes,modeData);};
  const auto before=queryNativeCcd(active);const auto all=queryNativeCcd(access);const auto after=queryNativeCcd(active);
  return mergeNativeActivationCcd(before,all,after);
}
}
