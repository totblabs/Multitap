#pragma once
#include "native_stream_primary_profile.hpp"
#include "native_display_baseline_codec.hpp"

namespace host {
// Candidate-only codec and injectable transaction coordinator. This header has
// no production caller and never creates a Windows access implementation.
// Journal integrity, stream authorization, console hardware identity, logon and
// current driver lease must be independently proved by the caller. Geometry is
// not an authorization token. A payload stores only the immutable entry state;
// its applied state is always recomputed, never accepted from serialized input.
// The caller must serialize profile, viewport and monitor-count transactions.
inline constexpr size_t nativeStreamProfilePayloadLimit=60000;

inline NativeCcdData parseNativeStreamProfilePaths(const Json& value){
  const auto* paths=std::get_if<Json::Array>(&value.value);
  if(!paths||paths->empty()||paths->size()>64)throw std::runtime_error("native_stream_profile_payload_invalid");
  NativeCcdData ccd;std::map<std::tuple<int,LONG,DWORD,UINT32>,UINT32> modes;std::set<std::tuple<LONG,DWORD,UINT32>> targetIds;
  const auto modeIndex=[&](const DISPLAYCONFIG_MODE_INFO& mode){
    const auto key=std::tuple{int(mode.infoType),mode.adapterId.HighPart,mode.adapterId.LowPart,mode.id};
    const auto found=modes.find(key);if(found!=modes.end()){if(!nativeSameMode(ccd.modes[found->second],mode))throw std::runtime_error("native_stream_profile_payload_invalid");return found->second;}
    if(ccd.modes.size()>=256)throw std::runtime_error("native_stream_profile_payload_invalid");
    const auto index=UINT32(ccd.modes.size());ccd.modes.push_back(mode);modes.emplace(key,index);return index;
  };
  for(const auto& encoded:*paths){
    const auto* row=std::get_if<Json::Array>(&encoded.value);if(!row||(row->size()!=34&&row->size()!=47))throw std::runtime_error("native_stream_profile_payload_invalid");
    const auto integer=[&](size_t index,int64_t low,int64_t high){return nativeBaselineInteger((*row)[index],low,high);};
    const auto u32=[&](size_t index){return UINT32(integer(index,0,UINT32_MAX));};const auto i32=[&](size_t index){return LONG(integer(index,INT32_MIN,INT32_MAX));};
    DISPLAYCONFIG_PATH_INFO path{};path.sourceInfo.adapterId={u32(1),i32(0)};path.sourceInfo.id=u32(2);path.targetInfo.adapterId={u32(4),i32(3)};path.targetInfo.id=u32(5);path.flags=u32(6);path.targetInfo.targetAvailable=BOOL(integer(7,1,1));
    if(!(path.flags&DISPLAYCONFIG_PATH_ACTIVE)||!targetIds.emplace(path.targetInfo.adapterId.HighPart,path.targetInfo.adapterId.LowPart,path.targetInfo.id).second)throw std::runtime_error("native_stream_profile_payload_invalid");
    path.targetInfo.outputTechnology=static_cast<DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY>(i32(8));path.targetInfo.rotation=static_cast<DISPLAYCONFIG_ROTATION>(integer(9,1,4));
    const auto scaling=integer(10,1,128);if(scaling>5&&scaling!=128)throw std::runtime_error("native_stream_profile_payload_invalid");
    path.targetInfo.scaling=static_cast<DISPLAYCONFIG_SCALING>(scaling);path.targetInfo.refreshRate={u32(11),u32(12)};path.targetInfo.scanLineOrdering=static_cast<DISPLAYCONFIG_SCANLINE_ORDERING>(integer(13,0,3));
    DISPLAYCONFIG_MODE_INFO source{};source.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;source.adapterId=path.sourceInfo.adapterId;source.id=path.sourceInfo.id;
    source.sourceMode={DWORD(integer(14,1,UINT32_MAX)),DWORD(integer(15,1,UINT32_MAX)),static_cast<DISPLAYCONFIG_PIXELFORMAT>(integer(16,1,5)),{i32(17),i32(18)}};
    if(!displayRectangleFits(source.sourceMode.position.x,source.sourceMode.position.y,source.sourceMode.width,source.sourceMode.height))throw std::runtime_error("native_stream_profile_payload_invalid");
    DISPLAYCONFIG_MODE_INFO target{};target.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;target.adapterId=path.targetInfo.adapterId;target.id=path.targetInfo.id;auto& signal=target.targetMode.targetVideoSignalInfo;
    signal.pixelRate=UINT64(integer(19,0,INT64_MAX));signal.hSyncFreq={u32(20),u32(21)};signal.vSyncFreq={u32(22),u32(23)};signal.activeSize={UINT32(integer(24,1,UINT32_MAX)),UINT32(integer(25,1,UINT32_MAX))};signal.totalSize={UINT32(integer(26,1,UINT32_MAX)),UINT32(integer(27,1,UINT32_MAX))};signal.videoStandard=u32(28);signal.scanLineOrdering=static_cast<DISPLAYCONFIG_SCANLINE_ORDERING>(integer(29,0,3));
    const bool desktop=integer(30,0,1)!=0,virtualMode=bool(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE);
    if(row->size()!=(desktop?47:34)||(desktop&&!virtualMode))throw std::runtime_error("native_stream_profile_payload_invalid");
    const auto sourceIndex=modeIndex(source),targetIndex=modeIndex(target);const auto tail=row->size()-3;path.sourceInfo.statusFlags=u32(tail);path.targetInfo.statusFlags=u32(tail+1);
    if(virtualMode){
      path.sourceInfo.cloneGroupId=UINT32(integer(tail+2,0,65535));path.sourceInfo.sourceModeInfoIdx=sourceIndex;path.targetInfo.targetModeInfoIdx=targetIndex;path.targetInfo.desktopModeInfoIdx=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
      if(desktop){
        DISPLAYCONFIG_MODE_INFO image{};image.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE;image.adapterId={DWORD(i32(32)),i32(31)};image.id=DWORD(i32(33));auto& info=image.desktopImageInfo;
        info.PathSourceSize={LONG(integer(34,1,INT32_MAX)),LONG(integer(35,1,INT32_MAX))};info.DesktopImageRegion={i32(36),i32(37),i32(38),i32(39)};info.DesktopImageClip={i32(40),i32(41),i32(42),i32(43)};
        if(info.DesktopImageRegion.right<info.DesktopImageRegion.left||info.DesktopImageRegion.bottom<info.DesktopImageRegion.top||info.DesktopImageClip.right<info.DesktopImageClip.left||info.DesktopImageClip.bottom<info.DesktopImageClip.top)throw std::runtime_error("native_stream_profile_payload_invalid");
        path.targetInfo.desktopModeInfoIdx=modeIndex(image);
      }
    }else{(void)integer(tail+2,-1,-1);path.sourceInfo.modeInfoIdx=sourceIndex;path.targetInfo.modeInfoIdx=targetIndex;}
    ccd.paths.push_back(path);
  }
  return ccd;
}

inline NativeStreamPrimaryProfile parseNativeStreamProfilePayload(const Json& root){
  const auto* object=std::get_if<Json::Object>(&root.value);const auto* targets=std::get_if<Json::Array>(&root.get("targets").value);
  if(!object||object->size()!=4||!object->contains("suppressedConsole")||!std::holds_alternative<double>(root.get("schema").value)||root.get("schema").number()!=1||!targets||targets->empty()||targets->size()>2)throw std::runtime_error("native_stream_profile_payload_invalid");
  std::vector<NativeDisplayTarget> owned;
  const auto number=[](const Json& item,const char* key,int64_t low,int64_t high){const auto& value=item.get(key);const auto result=value.number(-9007199254740991.0);if(!std::holds_alternative<double>(value.value)||!nativeDisplayInteger(result,double(low),double(high)))throw std::runtime_error("native_stream_profile_payload_invalid");return int64_t(result);};
  for(const auto& item:*targets){
    const auto* fields=std::get_if<Json::Object>(&item.value);if(!fields||fields->size()!=6||!std::holds_alternative<std::string>(item.get("id").value))throw std::runtime_error("native_stream_profile_payload_invalid");
    owned.push_back({item.get("id").string(),{DWORD(number(item,"adapterLow",0,UINT32_MAX)),LONG(number(item,"adapterHigh",INT32_MIN,INT32_MAX))},UINT32(number(item,"target",0,UINT32_MAX)),DWORD(number(item,"width",320,ownedOutputMaxSide)),DWORD(number(item,"height",180,ownedOutputMaxSide))});
  }
  const auto before=parseNativeStreamProfilePaths(root.get("paths"));const auto& suppressed=root.get("suppressedConsole");
  if(std::holds_alternative<std::nullptr_t>(suppressed.value))return planNativeStreamPrimaryProfile(before,owned);
  const auto* fields=std::get_if<Json::Object>(&suppressed.value);if(!fields||fields->size()!=6)throw std::runtime_error("native_stream_profile_payload_invalid");
  const NativeStreamConsolePath console{{DWORD(number(suppressed,"sourceAdapterLow",0,UINT32_MAX)),LONG(number(suppressed,"sourceAdapterHigh",INT32_MIN,INT32_MAX))},{DWORD(number(suppressed,"targetAdapterLow",0,UINT32_MAX)),LONG(number(suppressed,"targetAdapterHigh",INT32_MIN,INT32_MAX))},UINT32(number(suppressed,"source",0,UINT32_MAX)),UINT32(number(suppressed,"target",0,UINT32_MAX))};
  return planNativeStreamPrimaryProfile(before,owned,console);
}

inline std::string nativeStreamProfilePayloadBytes(const NativeStreamPrimaryProfile& profile){
  validateNativeStreamPrimaryProfile(profile);std::string targets="[",paths="[",suppressed="null";
  for(const auto& target:profile.targets){if(targets.size()>1)targets+=',';targets+="{\"id\":"+jsonQuote(target.id)+",\"adapterHigh\":"+std::to_string(target.adapter.HighPart)+",\"adapterLow\":"+std::to_string(target.adapter.LowPart)+",\"target\":"+std::to_string(target.target)+",\"width\":"+std::to_string(target.width)+",\"height\":"+std::to_string(target.height)+'}';}
  for(const auto& path:profile.before.paths){if(paths.size()>1)paths+=',';paths+='[';const auto row=nativeBaselinePathRow(profile.before,path);for(size_t i=0;i<row.size();++i){if(i)paths+=',';paths+=jsonQuote(std::to_string(row[i]));}paths+=']';}
  if(profile.suppressedConsole){const auto& key=*profile.suppressedConsole;suppressed="{\"sourceAdapterHigh\":"+std::to_string(key.sourceAdapter.HighPart)+",\"sourceAdapterLow\":"+std::to_string(key.sourceAdapter.LowPart)+",\"source\":"+std::to_string(key.source)+",\"targetAdapterHigh\":"+std::to_string(key.targetAdapter.HighPart)+",\"targetAdapterLow\":"+std::to_string(key.targetAdapter.LowPart)+",\"target\":"+std::to_string(key.target)+'}';}
  const auto bytes="{\"schema\":1,\"targets\":"+targets+"],\"paths\":"+paths+"],\"suppressedConsole\":"+suppressed+'}';
  if(bytes.size()>nativeStreamProfilePayloadLimit)throw std::runtime_error("native_stream_profile_payload_invalid");
  // Also enforce the codec's documented field bounds on in-memory inputs.
  const auto decoded=parseNativeStreamProfilePayload(JsonParser::parse(bytes));
  if(nativeStreamProfileSignatures(decoded.before)!=nativeStreamProfileSignatures(profile.before))throw std::runtime_error("native_stream_profile_payload_invalid");
  return bytes;
}
inline NativeStreamPrimaryProfile parseNativeStreamProfilePayload(const std::string& bytes){
  if(bytes.empty()||bytes.size()>nativeStreamProfilePayloadLimit)throw std::runtime_error("native_stream_profile_payload_invalid");
  const auto result=parseNativeStreamProfilePayload(JsonParser::parse(bytes));
  if(nativeStreamProfilePayloadBytes(result)!=bytes)throw std::runtime_error("native_stream_profile_payload_noncanonical");
  return result;
}

struct NativeStreamProfileTransactionAccess {
  NativeTopologyAccess topology;
  // Revalidate the exact stream/operation, current lease, Windows logon/device
  // binding and positively classified console hardware on EVERY invocation.
  std::function<bool()> identityCurrent;
  // Callbacks retain the journal's typed CAS acknowledgement in their closure.
  // A false return may mean a lost acknowledgement, never permission to apply.
  std::function<bool(const NativeCcdData& before,const NativeCcdData& requested)> durablePrepared;
  std::function<bool(const NativeCcdData& observed)> durableComplete;
};
struct NativeStreamProfileTransactionResult {
  bool ok=false,prepareAttempted=false,preparedAcknowledged=false,applyAttempted=false,verified=false,completedAcknowledged=false,recoveryRequired=false;
  std::string reason="native_stream_profile_failed";DWORD systemError=0;
};
// Shared pure coordinator; Windows access is explicitly injected by the caller.
// No blind rollback after a failed apply: leave the durable entry record intact
// and invoke the separately authenticated restore operation with fresh targets.
inline NativeStreamProfileTransactionResult transactNativeStreamPrimaryProfile(const NativeStreamPrimaryProfile& profile,std::span<const NativeDisplayTarget> currentTargets,bool restore,NativeStreamProfileTransactionAccess access,DWORD timeout=5000){
  NativeStreamProfileTransactionResult result;
  const auto fail=[&](const char* reason){result.reason=reason;result.recoveryRequired=result.prepareAttempted;return result;};
  try{
    validateNativeStreamPrimaryProfile(profile);
    if(!access.topology.query||!access.topology.configure||!access.topology.eligible||!access.topology.now||!access.topology.pause||!access.identityCurrent||!access.durablePrepared||!access.durableComplete)return fail("native_stream_profile_access_missing");
    const auto started=access.topology.now();const auto limit=std::min<DWORD>(timeout,10000);
    const auto eligible=[&]{const auto now=access.topology.now();return limit&&now>=started&&now-started<limit&&access.topology.eligible()&&access.identityCurrent();};
    if(!eligible())return fail("native_stream_profile_identity_changed");
    const auto fresh=access.topology.query();const auto before=validateNativeStreamProfileCcd(fresh,currentTargets);
    if(!restore&&nativeStreamProfileSignatures(before)!=nativeStreamProfileSignatures(profile.before))return fail("native_stream_profile_topology_changed");
    const auto requested=restore?planNativeStreamPrimaryRestore(profile,fresh,currentTargets):profile.applied;
    if(!nativePathsAvailable(requested,fresh))return fail("native_stream_profile_path_unavailable");
    const auto unchanged=[&](const NativeCcdData& latest){
      const auto active=validateNativeStreamProfileCcd(latest,currentTargets);
      if(nativeStreamProfileSignatures(active)!=nativeStreamProfileSignatures(before)||!nativePathsAvailable(requested,latest))return false;
      // Reprove the inactive console's exact source/target availability for an
      // exit, including ambiguity and source-reuse checks in the geometry plan.
      return !restore||nativeStreamProfileSignatures(planNativeStreamPrimaryRestore(profile,latest,currentTargets))==nativeStreamProfileSignatures(requested);
    };
    if(!eligible()||!unchanged(access.topology.query()))return fail("native_stream_profile_topology_changed");
    result.prepareAttempted=true;
    if(!access.durablePrepared(before,requested))return fail("native_stream_profile_prepare_unacknowledged");
    result.preparedAcknowledged=true;
    if(!eligible())return fail("native_stream_profile_identity_changed");
    if(!unchanged(access.topology.query()))return fail("native_stream_profile_topology_changed");
    constexpr UINT32 options=SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_VIRTUAL_MODE_AWARE;
    if(!eligible())return fail("native_stream_profile_identity_changed");
    const auto validation=access.topology.configure(requested,SDC_VALIDATE|options);
    if(validation!=ERROR_SUCCESS){result.systemError=DWORD(validation);return fail("native_stream_profile_validate_failed");}
    if(!eligible())return fail("native_stream_profile_identity_changed");
    if(!unchanged(access.topology.query()))return fail("native_stream_profile_topology_changed");
    if(!eligible())return fail("native_stream_profile_identity_changed");
    if(nativeStreamProfileSignatures(before)!=nativeStreamProfileSignatures(requested)){
      result.applyAttempted=true;const auto applied=access.topology.configure(requested,SDC_APPLY|options);
      if(applied!=ERROR_SUCCESS){result.systemError=DWORD(applied);return fail("native_stream_profile_apply_failed");}
    }
    NativeCcdData observed;
    for(;;){
      if(!eligible())return fail("native_stream_profile_verification_expired");
      const auto snapshot=access.topology.query();observed=validateNativeStreamProfileCcd(snapshot,currentTargets);
      if(nativeStreamProfileSignatures(observed)==nativeStreamProfileSignatures(requested))break;
      // A changed foreign path is not asynchronous owned-mode convergence.
      // Do not wait/reapply over an independent administrator display change.
      if(nativeStreamProfileSignatures(observed,currentTargets)!=nativeStreamProfileSignatures(before,currentTargets)&&nativeStreamProfileSignatures(observed,currentTargets)!=nativeStreamProfileSignatures(requested,currentTargets))return fail("native_stream_profile_foreign_changed");
      access.topology.pause();
    }
    result.verified=true;
    if(!eligible())return fail("native_stream_profile_identity_changed");
    // Completion callbacks must CAS their held journal ACK and revalidate the
    // bound scope/lease themselves. No boolean is proof of a durable write.
    if(!access.durableComplete(observed))return fail("native_stream_profile_complete_unacknowledged");
    result.completedAcknowledged=true;result.ok=true;result.reason.clear();return result;
  }catch(const NativeTopologyWindowsError& error){result.systemError=error.code;return fail("native_stream_profile_query_failed");}
  catch(...){return fail("native_stream_profile_failed");}
}
inline NativeStreamProfileTransactionResult enterNativeStreamPrimaryProfile(const NativeStreamPrimaryProfile& profile,NativeStreamProfileTransactionAccess access,DWORD timeout=5000){return transactNativeStreamPrimaryProfile(profile,profile.targets,false,std::move(access),timeout);}
inline NativeStreamProfileTransactionResult restoreNativeStreamPrimaryProfile(const NativeStreamPrimaryProfile& profile,std::span<const NativeDisplayTarget> currentTargets,NativeStreamProfileTransactionAccess access,DWORD timeout=5000){return transactNativeStreamPrimaryProfile(profile,currentTargets,true,std::move(access),timeout);}
}
