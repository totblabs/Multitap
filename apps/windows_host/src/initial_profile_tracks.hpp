#pragma once
#include "initial_viewport.hpp"
#include "native_display_worker_tracks.hpp"
#include "native_stream_profile_worker.hpp"

namespace host {
// Private candidate gate. Do not advertise group-profile support until the
// broker/client tuple and subsequent resize/stop path have been verified.
inline bool initialProfileTracksEnabled(const Json& config,bool nativeSelected){
  if(!nativeSelected||config.get("provider").string()!="libvirtualdisplay-1.6.4")return false;
  for(const auto* name:{"enabled","streamProfileTrialEnabled","streamProfileTracksTrialEnabled"})
    if(!std::holds_alternative<bool>(config.get(name).value)||!config.get(name).boolean())return false;
  return true;
}
inline bool hasInitialProfileTracks(const Json& command){const auto* fields=std::get_if<Json::Object>(&command.value);return fields&&fields->contains("streamProfileTracksProtocol");}
inline std::optional<NativeTrackResize> initialProfileTrackRequest(const Json& command){
  if(!hasInitialProfileTracks(command))return {};
  const auto& fields=std::get<Json::Object>(command.value);
  (void)streamSizeRequest(command);
  if(command.get("streamProfileTracksProtocol").string()!="group-v1"||command.get("type").string()!="session.start"||command.get("mode").string()!="stream"||command.get("mediaProtocol").string()!="tracks-v1")throw std::runtime_error("initial_profile_tracks_invalid");
  for(const auto* field:{"initialViewport","initialViewportProtocol","viewportEncoderLimitsProtocol","resizeProtocol","epoch","requestId","resizeId","profileOperation","profileLaunchSha256","workerDeadlineMs"})if(fields.contains(field))throw std::runtime_error("initial_profile_tracks_invalid");
  (void)nativeStreamProfileRequest(command,true);const auto request=nativeInitialTrackViewport(command);
  if(!streamSizeV2(command)&&!legacyTrackRequestSize(request))throw std::runtime_error("initial_profile_tracks_invalid");
  if(request.topologyEpoch!=1||std::any_of(request.epochs.begin(),request.epochs.end(),[](auto epoch){return epoch!=1;})||request.outputs.front().id!="0"||command.get("monitor").string()!=(request.outputs.size()==2?"all":"0"))throw std::runtime_error("initial_profile_tracks_invalid");
  const auto limit=[&](const char* key,int maximum){const auto value=command.get(key).number(-1);if(!std::isfinite(value)||value<(command.get("streamSizeProtocol").string()=="stream-size-v2"?16:128)||value>maximum||value!=std::floor(value)||(int(value)&1))throw std::runtime_error("initial_profile_tracks_invalid");return int(value);};
  const bool v2=command.get("streamSizeProtocol").string()=="stream-size-v2";
  const auto width=limit("maxWidth",v2?int(ownedOutputMaxSide):3840),height=limit("maxHeight",v2?int(ownedOutputMaxSide):2160);for(const auto& output:request.outputs)if(!ownedOutputSize(output.width,output.height)||(!v2&&(output.width>uint32_t(width)||output.height>uint32_t(height))))throw std::runtime_error("initial_profile_tracks_invalid");
  return request;
}
inline bool initialProfileTrackDisplayMatches(const DisplayState& display,const NativeTrackResize& request,bool activeProfile){
  if(!display.ready()||display.remoteSession||!display.viewportProvisioning||display.monitors.size()!=request.outputs.size())return false;
  const auto root=std::find_if(display.monitors.begin(),display.monitors.end(),[](const auto& m){return m.id=="0";});if(root==display.monitors.end()||(activeProfile&&(!root->primary||root->left||root->top)))return false;
  const auto& requestedRoot=request.outputs.front();
  for(const auto& output:request.outputs){const auto m=std::find_if(display.monitors.begin(),display.monitors.end(),[&](const auto& value){return value.id==output.id;});if(m==display.monitors.end()||m->width!=output.width||m->height!=output.height||int64_t(m->left)-root->left!=int64_t(output.left)-requestedRoot.left||int64_t(m->top)-root->top!=int64_t(output.top)-requestedRoot.top)return false;}
  return true;
}
inline bool initialProfileTrackPreparedMatches(const Json& original,const Json& prepared,const DisplayState& display,const NativeTrackResize& request){
  try{
    if(!initialProfileTrackDisplayMatches(display,request,false))return false;
    auto expected=original;const auto canonical=JsonParser::parse('{'+nativeTrackResizeFields(request)+'}');std::get<Json::Object>(expected.value)["tracks"]=canonical.get("tracks");
    return initialViewportJsonEqual(expected,prepared);
  }catch(...){return false;}
}
inline bool initialProfileTrackLeaseMatches(const Json& snapshot,const DisplayState& display,const NativeTrackResize& request,DWORD servicePid,std::string_view instance){
  try{
    for(const auto* name:{"leaseActive","activationVerified","configurationPending","recoveryRequired"})if(!std::holds_alternative<bool>(snapshot.get(name).value))return false;
    const auto lease=nativeStreamProfileLease(snapshot,servicePid);
    if(snapshot.get("deviceInstanceId").string()!=instance||lease.targets.size()!=request.outputs.size()||!initialProfileTrackDisplayMatches(display,request,false))return false;
    for(const auto& output:request.outputs){const auto target=std::find_if(lease.targets.begin(),lease.targets.end(),[&](const auto& value){return value.id==output.id;});if(target==lease.targets.end()||target->width!=output.width||target->height!=output.height)return false;}
    return true;
  }catch(...){return false;}
}
inline bool initialProfileTrackPhase2Matches(const Json& prepared,const Json& start){
  try{
    if(!initialProfileTrackRequest(prepared))return false;auto actual=start;auto& fields=std::get<Json::Object>(actual.value);
    for(const auto* name:{"profileOperation","profileLaunchSha256"}){const auto digest=start.get(name).string();if(!nativeRecoveryHex(digest)||std::get<Json::Object>(prepared.value).contains(name))return false;fields.erase(name);}
    return initialViewportJsonEqual(prepared,actual);
  }catch(...){return false;}
}
inline NativeWorkerTrackResult applyInitialProfileTracks(const Json& command,NativeWorkerTrackAccess access,const StreamResizeBudget& budget,bool supported,bool profileReserved=false){
  NativeWorkerTrackResult result;result.reason="unsupported_display_mode";
  try{
    const auto request=initialProfileTrackRequest(command);if(!request||!supported||profileReserved)return result;
    budget.remaining(1);if(!access.eligible()||!access.topology.eligible())return result;const auto before=access.probe();budget.remaining(1);if(!before.ready()||before.remoteSession||!before.viewportProvisioning||before.monitors.size()!=request->outputs.size())return result;
    result=applyNativeWorkerTracks(command,*request,access,budget,true);
    if(result.ok&&!initialProfileTrackPreparedMatches(command,result.command,result.display,*request)){result.ok=false;result.reason="initial_profile_tracks_invalid";}
    return result;
  }catch(...){return result;}
}
// Fixed private code of a failed v1 group preparation (worker.stream.failure
// stage 18 -> hostAction 200 stage 218): the reason (1 unsupported mode,
// 2 display change failed, 3 invalid group, 15 other) + 16 rollback attempted
// + 32 rollback succeeded. Never free text.
inline uint32_t initialProfileTrackFailureCode(const NativeWorkerTrackResult& result){
  const uint32_t reason=result.reason=="unsupported_display_mode"?1:result.reason=="display_change_failed"?2:result.reason=="initial_profile_tracks_invalid"?3:15;
  return reason|(result.rollbackAttempted?16u:0u)|(result.rollbackSucceeded?32u:0u);
}
// Display manager v2 group start (worker.v2.tracks.started): the reported
// outputs are the request's ("0" primary at the origin, "1" at its relative
// place) on the service's own fresh lease. No profile reservation or journal.
inline bool initialProfileTrackV2Matches(const DisplayState& display,const NativeTrackResize& request,const Json& snapshot,DWORD servicePid,std::string_view instance){
  return initialProfileTrackDisplayMatches(display,request,true)&&initialProfileTrackLeaseMatches(snapshot,display,request,servicePid,instance);
}
inline std::string initialProfileTrackFields(const NativeTrackResize& request){return "\"sessionId\":"+jsonQuote(request.sessionId)+",\"requestId\":"+jsonQuote(request.requestId)+",\"resizeId\":"+jsonQuote(request.resizeId)+",\"topologyEpoch\":"+std::to_string(request.topologyEpoch);}
template<class Serialize,class SerializeDisplay>
inline std::string initialProfileTrackResultPacket(const NativeTrackResize& request,const NativeWorkerTrackResult& result,bool desktop,Serialize serialize,SerializeDisplay serializeDisplay){
  return "{\"type\":\"worker.initial.tracks.result\","+initialProfileTrackFields(request)+",\"ok\":"+(result.ok?"true":"false")+",\"desktopAvailable\":"+(desktop?"true":"false")+",\"display\":"+serializeDisplay(result.display)+",\"command\":"+serialize(result.command)+'}';
}
class InitialProfileTrackCompletion {
 public:
  bool arm(const Json& command,bool supported,bool profileReserved=false){
    if(armed_)return false;
    try{const auto request=initialProfileTrackRequest(command);if(!request||!supported||profileReserved)return false;request_=*request;original_=command;armed_=true;return true;}catch(...){return false;}
  }
  const std::optional<NativeTrackResize>& request()const{return request_;}
  const std::optional<Json>& preparedCommand()const{return prepared_;}
  bool pending()const{return armed_&&!complete_;}
  bool workerVerified()const{return prepared_.has_value();}
  bool complete()const{return complete_;}
  bool acceptWorker(const Json& packet,bool eligible,const std::function<bool(const DisplayState&)>& publish){
    try{
      nativeStreamJournalFields(packet,9);
      if(!pending()||workerVerified()||!eligible||!publish||packet.get("type").string()!="worker.initial.tracks.result"||packet.get("sessionId").string()!=request_->sessionId||packet.get("requestId").string()!=request_->requestId||packet.get("resizeId").string()!=request_->resizeId||packet.get("topologyEpoch").number()!=1||!std::holds_alternative<bool>(packet.get("ok").value)||!packet.get("ok").boolean()||!std::holds_alternative<bool>(packet.get("desktopAvailable").value)||!packet.get("desktopAvailable").boolean())return false;
      const auto display=parseDisplayState(packet.get("display"));const auto& prepared=packet.get("command");if(!initialProfileTrackPreparedMatches(original_,prepared,display,*request_)||!publish(display))return false;prepared_=prepared;return true;
    }catch(...){return false;}
  }
  // Display manager v2: the service verified the worker's group start
  // (initialProfileTrackV2Matches); phase two sends the broker's command
  // unchanged, so the prepared command is the original. Metadata then
  // completes exactly as for v1 ("0" primary at the origin).
  bool acceptV2Worker(const DisplayState& display,bool eligible,const std::function<bool(const DisplayState&)>& publish){
    try{
      if(!pending()||workerVerified()||!eligible||!publish||!initialProfileTrackDisplayMatches(display,*request_,true)||!publish(display))return false;
      prepared_=original_;return true;
    }catch(...){return false;}
  }
  bool acceptMetadata(const Json& value,const DisplayState& display,bool eligible,bool scaled=false){
    if(!pending()||!workerVerified()||!eligible||!initialProfileTrackDisplayMatches(display,*request_,true)||!nativeTrackMetadataMayComplete(value,*request_,display,true,true,scaled))return false;complete_=true;return true;
  }
 private:
  Json original_;std::optional<Json> prepared_;std::optional<NativeTrackResize> request_;bool armed_=false,complete_=false;
};
}
