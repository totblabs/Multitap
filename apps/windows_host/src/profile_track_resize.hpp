#pragma once
#include "initial_profile_tracks.hpp"
#include "track_source.hpp"

namespace host {
inline Json profileTrackCommand(const Json& current,const NativeTrackResize& request){
  if(current.get("mode").string()!="stream"||current.get("mediaProtocol").string()!="tracks-v1"||current.get("sessionId").string()!=request.sessionId||!nativeTrackResizeValid(request))throw std::runtime_error("invalid_track_request");
  auto result=current;const auto fields=JsonParser::parse('{'+nativeTrackResizeFields(request)+'}');
  for(const auto* name:{"requestId","resizeId","topologyEpoch","fps","tracks"})std::get<Json::Object>(result.value)[name]=fields.get(name);
  uint32_t width=0,height=0;for(const auto& output:request.outputs){width=std::max(width,output.width);height=std::max(height,output.height);}
  streamField(result,"maxWidth",std::max(current.get("maxWidth").number(),double(width)));streamField(result,"maxHeight",std::max(current.get("maxHeight").number(),double(height)));return result;
}
inline NativeTrackResize profileTrackRestoredRequest(const NativeTrackResize& previous,const NativeTrackResize& failed){
  if(!nativeTrackResizeFollows(previous,failed))throw std::runtime_error("stale_topology");
  // Consume the authorized NEW topology even when dimensions roll back. Old
  // topology packets/input cannot be accepted by newly created capture/decoders.
  auto restored=previous;restored.requestId=failed.requestId;restored.resizeId=failed.resizeId;restored.topologyEpoch=failed.topologyEpoch;
  if(!nativeTrackResizeFollows(previous,restored))throw std::runtime_error("invalid_track_request");return restored;
}
inline NativeTrackResize profileTrackRestoredRequest(const Json& current,const NativeTrackResize& failed){(void)nativeStreamProfileRequest(current,true);return profileTrackRestoredRequest(nativeInitialTrackViewport(current),failed);}
inline std::string profileTrackRestoreReason(const std::string& reason){
  for(const auto* allowed:{"unsupported_display_mode","display_change_failed","virtual_display_required","monitor_not_available","resize_failed"})if(reason==allowed)return reason;return "resize_failed";
}
inline std::string profileTrackRestoredPacket(const NativeTrackResize& restored,const std::string& reason){
  return "{\"type\":\"stream.tracks.resize.restored\","+nativeTrackResizeFields(restored)+",\"reason\":"+jsonQuote(profileTrackRestoreReason(reason))+'}';
}
inline bool profileTrackRestoredMatches(const NativeTrackResize& previous,const NativeTrackResize& pending,const Json& value){
  try{nativeStreamJournalFields(value,8);const auto reason=value.get("reason").string();return value.get("type").string()=="stream.tracks.resize.restored"&&reason==profileTrackRestoreReason(reason)&&nativeSameTrackResize(profileTrackRestoredRequest(previous,pending),nativeTrackResizeRequest(value));}catch(...){return false;}
}
inline bool profileTrackRestoreReceiptMatches(const NativeStreamProfileRecord& profile,const NativeTrackResize& pending,const NativeStreamViewportReceipt& receipt,const std::string& runtimeBytes,const std::string& baselineBytes,const NativeStreamProfileLease& fresh){
  try{
    if(profile.phase!=NativeStreamProfilePhase::active||profile.resize||profile.stopRequested||profile.key.stream!=pending.sessionId||!sameNativeStreamLease(profile.current,fresh))return false;
    const bool completed=std::visit([&](const auto& value){using T=std::decay_t<decltype(value)>;
      if constexpr(std::is_same_v<T,NativeStreamViewportCommit>)return false;
      else{
        bool exact=false;if constexpr(std::is_same_v<T,NativeStreamTrackCommit>)exact=value.rollback&&nativeSameTrackResize(value.request,pending)&&nativeStreamTrackCommitBaselineMatches(value,baselineBytes);
        else if(const auto* request=std::get_if<NativeTrackResize>(&value.request))exact=nativeSameTrackResize(*request,pending);
        return exact&&nativeStreamViewportCommittedProfileMatches(profile,value)&&nativeBaselineDigest(runtimeBytes)==value.runtimeSha256;
      }
    },receipt);
    if(!completed)return false;const auto runtime=parseNativeRuntimeRecord(runtimeBytes);
    if(runtime.instance!=profile.key.instance||runtime.phase!="stable"||runtime.generation!=fresh.generation||!runtime.baselineHash.empty()||runtime.outputs.size()!=fresh.targets.size())return false;
    for(size_t i=0;i<runtime.outputs.size();++i)if(runtime.outputs[i].id!=fresh.targets[i].id||runtime.outputs[i].width!=fresh.targets[i].width||runtime.outputs[i].height!=fresh.targets[i].height)return false;return true;
  }catch(...){return false;}
}
struct ProfileTrackResizeAccess {
  std::function<bool()> eligible;
  std::function<bool(const NativeTrackResize&)> reserve;
  std::function<void()> stop;
  std::function<NativeWorkerTrackResult()> apply;
  std::function<bool(const NativeTrackResize&,bool applied)> complete;
  std::function<DisplayState()> probe;
  std::function<std::vector<CaptureMonitor>()> observed;
};
struct ProfileTrackResizeResult {
  bool changed=false,restored=false;
  std::string reason="unsupported_display_mode";
  NativeWorkerTrackResult viewport;
  Json command;
  std::optional<NativeTrackResize> applied;
  std::optional<TrackSourcePlan> source;
};
inline ProfileTrackResizeResult changeProfileTracks(const Json& current,const NativeTrackResize& request,ProfileTrackResizeAccess access){
  ProfileTrackResizeResult result;
  try{
    const auto restored=profileTrackRestoredRequest(current,request);
    if(!access.eligible||!access.reserve||!access.stop||!access.apply||!access.complete||!access.probe||!access.observed||!access.eligible()||!access.reserve(request))return result;
    access.stop();
    try{result.viewport=access.apply();}catch(...){ }
    const bool changed=result.viewport.ok;
    // A rollback/no-op ACK alone cannot resume media: the active profile must
    // complete its exact SDK/runtime receipt and durable journal first.
    if(!access.complete(request,changed)||!access.eligible()){result.reason="virtual_display_required";return result;}
    const auto applied=changed?request:restored;const auto command=profileTrackCommand(current,applied);
    if(changed&&!initialViewportJsonEqual(command,result.viewport.command)){result.reason="virtual_display_required";return result;}
    auto display=access.probe();
    if(!initialProfileTrackDisplayMatches(display,applied,true)){result.reason="virtual_display_required";return result;}
    auto source=bindOwnedTrackSources(command,display,access.observed());
    result.changed=changed;result.restored=!changed;result.reason=changed?"":result.viewport.reason;result.viewport.display=std::move(display);result.command=command;result.applied=applied;result.source=std::move(source);return result;
  }catch(...){result.reason="virtual_display_required";return result;}
}
}
