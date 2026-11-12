#pragma once
#include "native_display_topology.hpp"
#include "owned_output_size.hpp"

namespace host {
// Candidate-only pure geometry. No production caller, Windows query or mutation.
// Before applying a plan, a future coordinator MUST durably bind its checkpoint
// to the authorized stream, exact current driver lease/device and Windows logon,
// then revalidate those identities and the fresh topology at the OS boundary.
// Structural validation here is not authentication of an externally supplied
// checkpoint; protected journal integrity and lifecycle ownership are separate.
struct NativeStreamConsolePath {
  LUID sourceAdapter{},targetAdapter{};UINT32 source=0,target=0;
};
inline bool nativeStreamConsolePathMatches(const DISPLAYCONFIG_PATH_INFO& path,const NativeStreamConsolePath& identity){
  return nativeSameLuid(path.sourceInfo.adapterId,identity.sourceAdapter)&&nativeSameLuid(path.targetInfo.adapterId,identity.targetAdapter)&&path.sourceInfo.id==identity.source&&path.targetInfo.id==identity.target;
}
struct NativeStreamPrimaryProfile {
  NativeCcdData before,applied;
  std::vector<NativeDisplayTarget> targets;
  std::optional<NativeStreamConsolePath> suppressedConsole;
};
inline auto nativeStreamProfileTargetKey(const DISPLAYCONFIG_PATH_INFO& path){
  return std::tuple{path.targetInfo.adapterId.HighPart,path.targetInfo.adapterId.LowPart,path.targetInfo.id};
}
inline std::vector<std::vector<int64_t>> nativeStreamProfileSignatures(const NativeCcdData& data,std::span<const NativeDisplayTarget> omit={}){
  auto values=nativeActiveSignatures(data,omit);size_t index=0;
  for(const auto& path:data.paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&!nativeOwnedPath(path,omit)){
    // These documented fields are intentionally additional to the ordinary
    // topology verifier. Only mode-array references may change during reindex.
    auto& signature=values[index++];signature.push_back(path.sourceInfo.statusFlags);signature.push_back(path.targetInfo.statusFlags);
    signature.push_back(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE?path.sourceInfo.cloneGroupId:DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID);
  }
  return values;
}
inline NativeCcdData validateNativeStreamProfileCcd(const NativeCcdData& data,std::span<const NativeDisplayTarget> targets){
  const auto fail=[] {throw std::runtime_error("native_stream_profile_invalid");};
  if(data.paths.empty()||data.paths.size()>nativeCcdQueryPathLimit||data.modes.size()>nativeCcdQueryModeLimit||targets.empty()||targets.size()>2)fail();
  for(size_t i=0;i<targets.size();++i){const auto& target=targets[i];
    if(target.id!=std::to_string(i)||!ownedOutputSize(target.width,target.height))fail();
    for(size_t j=0;j<i;++j)if(nativeSameLuid(target.adapter,targets[j].adapter)&&target.target==targets[j].target)fail();
  }
  auto active=nativeActiveCcd(data);if(active.paths.empty()||active.paths.size()>64||active.modes.size()>256)fail();
  std::set<decltype(nativeStreamProfileTargetKey(DISPLAYCONFIG_PATH_INFO{}))> uniqueTargets;
  std::map<decltype(nativeSourceKey(DISPLAYCONFIG_PATH_INFO{})),DISPLAYCONFIG_MODE_INFO> sources;
  std::set<decltype(nativeSourceKey(DISPLAYCONFIG_PATH_INFO{}))> primary,ownedSources,foreignSources;
  std::vector<size_t> targetCounts(targets.size(),0);
  for(const auto& path:active.paths){
    if(!path.targetInfo.targetAvailable||!uniqueTargets.insert(nativeStreamProfileTargetKey(path)).second)fail();
    const auto& source=nativeCcdSource(active,path);(void)nativeCcdTarget(active,path);
    if(!displayRectangleFits(source.position.x,source.position.y,source.width,source.height))fail();
    const auto key=nativeSourceKey(path);const auto& mode=active.modes[nativeSourceIndex(path)];const auto found=sources.find(key);
    if(found==sources.end())sources.emplace(key,mode);else if(!nativeSameMode(found->second,mode))fail();
    if(!source.position.x&&!source.position.y)primary.insert(key);
    const auto owner=std::find_if(targets.begin(),targets.end(),[&](const auto& target){return nativeSameTarget(path,target);});
    if(owner==targets.end())foreignSources.insert(key);
    else{
      const auto index=size_t(owner-targets.begin());++targetCounts[index];
      if(!nativeSameLuid(path.sourceInfo.adapterId,owner->adapter)||source.width!=owner->width||source.height!=owner->height||!ownedSources.insert(key).second)fail();
    }
  }
  if(primary.size()!=1||std::any_of(targetCounts.begin(),targetCounts.end(),[](size_t count){return count!=1;}))fail();
  for(const auto& key:ownedSources){
    if(foreignSources.count(key))fail();const auto& owned=sources.at(key).sourceMode;
    for(const auto& [otherKey,mode]:sources){if(otherKey==key)continue;const auto& other=mode.sourceMode;
      if(std::max<int64_t>(owned.position.x,other.position.x)<std::min<int64_t>(int64_t(owned.position.x)+owned.width,int64_t(other.position.x)+other.width)&&std::max<int64_t>(owned.position.y,other.position.y)<std::min<int64_t>(int64_t(owned.position.y)+owned.height,int64_t(other.position.y)+other.height))fail();
    }
  }
  (void)nativeStreamProfileSignatures(active);return active;
}
inline NativeCcdData translateNativeStreamProfile(const NativeCcdData& data,int64_t dx,int64_t dy){
  const auto minimum=int64_t(INT32_MIN)-INT32_MAX,maximum=int64_t(INT32_MAX)-INT32_MIN;
  if(dx<minimum||dx>maximum||dy<minimum||dy>maximum)throw std::runtime_error("native_stream_profile_bounds");
  auto result=data;std::set<UINT32> translated;
  for(const auto& path:result.paths){
    if(!(path.flags&DISPLAYCONFIG_PATH_ACTIVE))throw std::runtime_error("native_stream_profile_invalid");
    const auto index=nativeSourceIndex(path);(void)nativeCcdSource(result,path);
    if(!translated.insert(index).second)continue;
    auto& source=result.modes[index].sourceMode;const int64_t x=int64_t(source.position.x)+dx,y=int64_t(source.position.y)+dy;
    if(!displayRectangleFits(x,y,source.width,source.height))throw std::runtime_error("native_stream_profile_bounds");
    source.position={LONG(x),LONG(y)};
  }
  return result;
}
inline NativeStreamPrimaryProfile planNativeStreamPrimaryProfile(const NativeCcdData& fresh,std::span<const NativeDisplayTarget> targets){
  NativeStreamPrimaryProfile result;result.before=validateNativeStreamProfileCcd(fresh,targets);result.targets.assign(targets.begin(),targets.end());
  const auto zero=std::find_if(result.before.paths.begin(),result.before.paths.end(),[&](const auto& path){return nativeSameTarget(path,targets.front());});
  const auto& anchor=nativeCcdSource(result.before,*zero);
  result.applied=validateNativeStreamProfileCcd(translateNativeStreamProfile(result.before,-int64_t(anchor.position.x),-int64_t(anchor.position.y)),targets);
  return result;
}
// The exact key must already be positively classified as this VM's console by
// the caller's fresh hardware/configuration proof. This function neither infers
// that role from an adapter name nor accepts a list of ambiguous candidates.
inline NativeStreamPrimaryProfile planNativeStreamPrimaryProfile(const NativeCcdData& fresh,std::span<const NativeDisplayTarget> targets,const NativeStreamConsolePath& console){
  auto result=planNativeStreamPrimaryProfile(fresh,targets);
  const auto selected=std::find_if(result.before.paths.begin(),result.before.paths.end(),[&](const auto& path){return nativeStreamConsolePathMatches(path,console);});
  if(selected==result.before.paths.end()||nativeOwnedPath(*selected,targets)||
      std::count_if(result.before.paths.begin(),result.before.paths.end(),[&](const auto& path){return nativeSourceKey(path)==nativeSourceKey(*selected);})!=1)throw std::runtime_error("native_stream_console_selection_invalid");
  result.applied.paths.erase(std::remove_if(result.applied.paths.begin(),result.applied.paths.end(),[&](const auto& path){return nativeStreamConsolePathMatches(path,console);}),result.applied.paths.end());
  result.applied=validateNativeStreamProfileCcd(result.applied,targets);result.suppressedConsole=console;return result;
}
inline void validateNativeStreamPrimaryProfile(const NativeStreamPrimaryProfile& profile){
  if(profile.before.paths.size()>64||profile.applied.paths.size()>64||profile.before.modes.size()>256||profile.applied.modes.size()>256||
      std::any_of(profile.before.paths.begin(),profile.before.paths.end(),[](const auto& path){return !(path.flags&DISPLAYCONFIG_PATH_ACTIVE);})||
      std::any_of(profile.applied.paths.begin(),profile.applied.paths.end(),[](const auto& path){return !(path.flags&DISPLAYCONFIG_PATH_ACTIVE);}))throw std::runtime_error("native_stream_profile_invalid");
  const auto expected=profile.suppressedConsole?planNativeStreamPrimaryProfile(profile.before,profile.targets,*profile.suppressedConsole):planNativeStreamPrimaryProfile(profile.before,profile.targets);
  const auto applied=validateNativeStreamProfileCcd(profile.applied,profile.targets);
  if(nativeStreamProfileSignatures(expected.applied)!=nativeStreamProfileSignatures(applied))throw std::runtime_error("native_stream_profile_checkpoint_mismatch");
}
inline bool nativeStreamPrimaryProfileMatches(const NativeStreamPrimaryProfile& profile,const NativeCcdData& fresh){
  try{validateNativeStreamPrimaryProfile(profile);const auto current=validateNativeStreamProfileCcd(fresh,profile.targets);return nativeStreamProfileSignatures(profile.applied)==nativeStreamProfileSignatures(current);}catch(...){return false;}
}
inline bool nativeStreamOriginalPrimaryMatches(const NativeStreamPrimaryProfile& profile,const NativeCcdData& current,std::span<const NativeDisplayTarget> currentTargets){
  const auto oldPrimary=std::find_if(profile.before.paths.begin(),profile.before.paths.end(),[&](const auto& path){const auto& source=nativeCcdSource(profile.before,path);return !source.position.x&&!source.position.y;});
  if(oldPrimary==profile.before.paths.end())return false;
  const auto oldOwner=std::find_if(profile.targets.begin(),profile.targets.end(),[&](const auto& target){return nativeSameTarget(*oldPrimary,target);});
  const auto primary=std::find_if(current.paths.begin(),current.paths.end(),[&](const auto& path){
    if(oldOwner!=profile.targets.end()){const auto owner=std::find_if(currentTargets.begin(),currentTargets.end(),[&](const auto& target){return target.id==oldOwner->id;});return owner!=currentTargets.end()&&nativeSameTarget(path,*owner);}
    return nativeStreamProfileTargetKey(path)==nativeStreamProfileTargetKey(*oldPrimary)&&nativeSourceKey(path)==nativeSourceKey(*oldPrimary);
  });
  if(primary==current.paths.end())return false;const auto& source=nativeCcdSource(current,*primary);return !source.position.x&&!source.position.y;
}
inline NativeCcdData nativeStreamProfileWithConsole(const NativeStreamPrimaryProfile& profile,const NativeCcdData& fresh,const NativeCcdData& active){
  const auto& identity=*profile.suppressedConsole;
  const auto original=std::find_if(profile.before.paths.begin(),profile.before.paths.end(),[&](const auto& path){return nativeStreamConsolePathMatches(path,identity);});
  if(original==profile.before.paths.end())throw std::runtime_error("native_stream_console_selection_invalid");
  const auto matches=std::count_if(fresh.paths.begin(),fresh.paths.end(),[&](const auto& path){return nativeStreamConsolePathMatches(path,identity)&&path.targetInfo.targetAvailable;});
  if(matches!=1)throw std::runtime_error("native_stream_console_unavailable");
  if(std::any_of(active.paths.begin(),active.paths.end(),[&](const auto& path){return nativeSourceKey(path)==nativeSourceKey(*original)||nativeStreamProfileTargetKey(path)==nativeStreamProfileTargetKey(*original);}))throw std::runtime_error("native_stream_console_source_reused");
  // Reinsert only the saved console path. Copy its original local modes into
  // fresh current mode storage; never replay an old owned source/target ID.
  auto result=active;auto path=*original;std::map<UINT32,UINT32> copied;
  const auto copyMode=[&](UINT32 index,DISPLAYCONFIG_MODE_INFO_TYPE type){const auto found=copied.find(index);if(found!=copied.end())return found->second;const auto destination=UINT32(result.modes.size());result.modes.push_back(nativeCcdMode(profile.before,index,type));copied.emplace(index,destination);return destination;};
  const auto source=copyMode(nativeSourceIndex(path),DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE),target=copyMode(nativeTargetIndex(path),DISPLAYCONFIG_MODE_INFO_TYPE_TARGET);
  if(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE){if(path.targetInfo.desktopModeInfoIdx!=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID)path.targetInfo.desktopModeInfoIdx=copyMode(path.targetInfo.desktopModeInfoIdx,DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE);path.sourceInfo.sourceModeInfoIdx=source;path.targetInfo.targetModeInfoIdx=target;}
  else{path.sourceInfo.modeInfoIdx=source;path.targetInfo.modeInfoIdx=target;}
  size_t lower=0,upper=result.paths.size();const auto originalIndex=size_t(original-profile.before.paths.begin());
  for(size_t i=0;i<profile.before.paths.size();++i){const auto& prior=profile.before.paths[i];if(i==originalIndex||nativeOwnedPath(prior,profile.targets))continue;
    const auto found=std::find_if(result.paths.begin(),result.paths.end(),[&](const auto& value){return nativeStreamProfileTargetKey(value)==nativeStreamProfileTargetKey(prior)&&nativeSourceKey(value)==nativeSourceKey(prior);});
    if(found==result.paths.end())throw std::runtime_error("native_stream_profile_foreign_changed");const auto index=size_t(found-result.paths.begin());
    if(i<originalIndex)lower=std::max(lower,index+1);else upper=std::min(upper,index);
  }
  if(lower>upper)throw std::runtime_error("native_stream_profile_foreign_changed");
  result.paths.insert(result.paths.begin()+std::clamp(originalIndex,lower,upper),path);return result;
}
// Plan-only exit. Current target IDs/dimensions come from the caller's freshly
// verified lease, never from a replay of retired entry targets. Their identity
// transition must be authenticated by the future lifecycle coordinator.
inline NativeCcdData planNativeStreamPrimaryRestore(const NativeStreamPrimaryProfile& profile,const NativeCcdData& fresh,std::span<const NativeDisplayTarget> currentTargets){
  validateNativeStreamPrimaryProfile(profile);const auto current=validateNativeStreamProfileCcd(fresh,currentTargets);
  const auto currentForeign=nativeStreamProfileSignatures(current,currentTargets);
  // An apply can fail before touching Windows, or restoration can have completed
  // before its acknowledgement is lost. Exact original foreign state + original
  // primary is an idempotent no-op, not permission to overwrite current geometry.
  if(profile.suppressedConsole&&nativeStreamProfileSignatures(profile.before,profile.targets)==currentForeign&&nativeStreamOriginalPrimaryMatches(profile,current,currentTargets))return current;
  const auto originalZero=std::find_if(profile.before.paths.begin(),profile.before.paths.end(),[&](const auto& path){return nativeSameTarget(path,profile.targets.front());});
  const auto& anchor=nativeCcdSource(profile.before,*originalZero);
  bool consoleActive=false;
  if(profile.suppressedConsole){
    consoleActive=std::any_of(current.paths.begin(),current.paths.end(),[&](const auto& path){return nativeStreamConsolePathMatches(path,*profile.suppressedConsole);});
    // Partial entry may translate correctly but leave the console active. Admit
    // exactly that full known profile for rollback, never an arbitrary reenable.
    const auto expected=consoleActive?translateNativeStreamProfile(profile.before,-int64_t(anchor.position.x),-int64_t(anchor.position.y)):profile.applied;
    if(nativeStreamProfileSignatures(expected,profile.targets)!=currentForeign)throw std::runtime_error("native_stream_profile_foreign_changed");
  }else if(nativeStreamProfileSignatures(profile.applied,profile.targets)!=currentForeign)throw std::runtime_error("native_stream_profile_foreign_changed");
  const auto zero=std::find_if(current.paths.begin(),current.paths.end(),[&](const auto& path){return nativeSameTarget(path,currentTargets.front());});
  const auto& zeroSource=nativeCcdSource(current,*zero);if(zeroSource.position.x||zeroSource.position.y)throw std::runtime_error("native_stream_profile_primary_changed");
  auto restored=translateNativeStreamProfile(current,anchor.position.x,anchor.position.y);
  if(profile.suppressedConsole&&!consoleActive)restored=nativeStreamProfileWithConsole(profile,fresh,restored);
  try{restored=validateNativeStreamProfileCcd(restored,currentTargets);}
  catch(...){
    // A later group viewport can put output one to the left of output zero.
    // Translating back to the old anchor may then overlap the restored console.
    // With a proven original FOREIGN primary, move only the complete owned
    // group outside the foreign bounds. Preserve every foreign mode/position,
    // current owned dimensions/relative placement and original primary identity.
    const bool foreignPrimary=std::any_of(profile.before.paths.begin(),profile.before.paths.end(),[&](const auto& path){if(nativeOwnedPath(path,profile.targets))return false;const auto& source=nativeCcdSource(profile.before,path);return !source.position.x&&!source.position.y;});
    if(!foreignPrimary)throw;
    int64_t foreignLeft=INT64_MAX,foreignTop=INT64_MAX,foreignRight=INT64_MIN,foreignBottom=INT64_MIN,ownedLeft=INT64_MAX,ownedTop=INT64_MAX,ownedRight=INT64_MIN,ownedBottom=INT64_MIN;
    for(const auto& path:restored.paths){const auto& s=nativeCcdSource(restored,path);const bool owned=nativeOwnedPath(path,currentTargets);auto& left=owned?ownedLeft:foreignLeft;auto& top=owned?ownedTop:foreignTop;auto& right=owned?ownedRight:foreignRight;auto& bottom=owned?ownedBottom:foreignBottom;left=std::min(left,int64_t(s.position.x));top=std::min(top,int64_t(s.position.y));right=std::max(right,int64_t(s.position.x)+s.width);bottom=std::max(bottom,int64_t(s.position.y)+s.height);}
    bool placed=false;
    for(const auto& [dx,dy]:std::array<std::pair<int64_t,int64_t>,4>{{{foreignRight-ownedLeft,0},{foreignLeft-ownedRight,0},{0,foreignBottom-ownedTop},{0,foreignTop-ownedBottom}}}){
      try{auto candidate=restored;std::set<UINT32> translated;for(const auto& path:candidate.paths)if(nativeOwnedPath(path,currentTargets)){const auto index=nativeSourceIndex(path);if(!translated.insert(index).second)continue;auto& s=candidate.modes[index].sourceMode;const auto x=int64_t(s.position.x)+dx,y=int64_t(s.position.y)+dy;if(!displayRectangleFits(x,y,s.width,s.height))throw std::runtime_error("native_stream_profile_bounds");s.position={LONG(x),LONG(y)};}
        candidate=validateNativeStreamProfileCcd(candidate,currentTargets);if(!nativeStreamOriginalPrimaryMatches(profile,candidate,currentTargets)||nativeStreamProfileSignatures(profile.before,profile.targets)!=nativeStreamProfileSignatures(candidate,currentTargets))continue;restored=std::move(candidate);placed=true;break;
      }catch(...){ }
    }
    if(!placed)throw std::runtime_error("native_stream_profile_restore_layout_unavailable");
  }
  if(!nativeStreamOriginalPrimaryMatches(profile,restored,currentTargets))throw std::runtime_error("native_stream_profile_original_primary_changed");
  if(nativeStreamProfileSignatures(profile.before,profile.targets)!=nativeStreamProfileSignatures(restored,currentTargets))throw std::runtime_error("native_stream_profile_foreign_changed");
  return restored;
}
}
