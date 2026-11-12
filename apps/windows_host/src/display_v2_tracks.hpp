#pragma once
#include "display_v2_rpc.hpp"
#include "native_track_change.hpp"

// Display manager v2 for independent outputs (tracks-v1, "모든 모니터"):
// glue independent of the service/worker classes. The broker/client wire is
// unchanged; these packets never leave the private worker pipe.
namespace host {
// The owned outputs a tracks-v1 request names, at its sizes and places.
inline std::vector<DisplayV2Output> displayV2TrackOutputs(const NativeTrackResize& request){
  std::vector<DisplayV2Output> outputs;
  for(const auto& o:request.outputs)outputs.push_back({o.id,DWORD(o.width),DWORD(o.height),o.left,o.top});
  return outputs;
}
// The streaming command after a v2 group resize: the request's correlation,
// topology, fps and tracks (what the v1 coordinator produced).
inline Json displayV2TrackCommand(const Json& current,const NativeTrackResize& request){
  auto next=current;const auto fields=JsonParser::parse('{'+nativeTrackResizeFields(request)+'}');
  for(const auto* field:{"requestId","resizeId","topologyEpoch","fps","tracks"})std::get<Json::Object>(next.value)[field]=fields.get(field);
  uint32_t width=0,height=0;for(const auto& o:request.outputs){width=std::max(width,o.width);height=std::max(height,o.height);}
  streamField(next,"maxWidth",std::max(next.get("maxWidth").number(),double(width)));streamField(next,"maxHeight",std::max(next.get("maxHeight").number(),double(height)));
  return next;
}
// Fixed diagnostic values of a v2 result: stage (1..9) and error (the Windows
// code, or displayV2ForeignCode at stage 8). Zero on success.
inline std::pair<uint32_t,uint32_t> displayV2ResultCode(const DisplayV2Result& result,bool ok){
  if(ok)return {0u,0u};
  const auto stage=std::min<uint32_t>(result.stage?result.stage:displayV2Stage::internal,displayV2Stage::internal);
  return {stage,stage==displayV2Stage::foreign?result.foreignDifference:uint32_t(result.error)};
}
// worker -> service: the group start result. Fixed fields only (no reason
// text): stage/error as above, the console facts of hostAction 210 and the
// outcome of the restore a failure ran.
inline std::string displayV2TracksStartedBytes(const std::string& session,const DisplayV2Result& result,bool ok,bool desktop,const std::string& display){
  const auto [stage,error]=displayV2ResultCode(result,ok);const auto flag=[](bool b){return b?"true":"false";};
  const bool restoreAttempted=!ok&&result.restore.has_value();const bool restored=restoreAttempted&&result.restore->ok();
  return "{\"type\":\"worker.v2.tracks.started\",\"sessionId\":"+jsonQuote(session)+",\"ok\":"+flag(ok)+",\"nativeStage\":"+std::to_string(stage)+",\"nativeError\":"+std::to_string(error)+
    ",\"consoleRemoved\":"+flag(result.consoleRemoved)+",\"consoleProof\":"+flag(result.consoleProof)+",\"consoleIdentified\":"+flag(result.consoleIdentified)+",\"foreignCount\":"+std::to_string(std::min<uint32_t>(result.foreignBefore,64))+
    ",\"restoreAttempted\":"+flag(restoreAttempted)+",\"restored\":"+flag(restored)+",\"restoreStage\":"+std::to_string(restoreAttempted?std::min<uint32_t>(result.restore->stage,displayV2Stage::internal):0u)+
    ",\"desktopAvailable\":"+flag(desktop)+",\"display\":"+display+'}';
}
struct DisplayV2TracksStarted {
  bool ok=false,consoleRemoved=false,consoleProof=false,consoleIdentified=false,restoreAttempted=false,restored=false,desktopAvailable=false;
  uint32_t stage=0,error=0,foreignCount=0,restoreStage=0;DisplayState display;
};
inline std::optional<DisplayV2TracksStarted> parseDisplayV2TracksStarted(const Json& value,const std::string& session){
  try{
    const auto* fields=std::get_if<Json::Object>(&value.value);
    if(!fields||fields->size()!=14||value.get("type").string()!="worker.v2.tracks.started"||session.empty()||value.get("sessionId").string()!=session)return {};
    for(const auto* name:{"ok","consoleRemoved","consoleProof","consoleIdentified","restoreAttempted","restored","desktopAvailable"})if(!std::holds_alternative<bool>(value.get(name).value))return {};
    const auto stage=value.get("nativeStage").number(-1),error=value.get("nativeError").number(-1),count=value.get("foreignCount").number(-1),restoreStage=value.get("restoreStage").number(-1);
    if(!nativeDisplayInteger(stage,0,displayV2Stage::internal)||!nativeDisplayInteger(error,0,UINT32_MAX)||!nativeDisplayInteger(count,0,64)||!nativeDisplayInteger(restoreStage,0,displayV2Stage::internal))return {};
    if(!std::holds_alternative<Json::Object>(value.get("display").value))return {};
    DisplayV2TracksStarted result;
    result.ok=value.get("ok").boolean();result.consoleRemoved=value.get("consoleRemoved").boolean();result.consoleProof=value.get("consoleProof").boolean();result.consoleIdentified=value.get("consoleIdentified").boolean();
    result.restoreAttempted=value.get("restoreAttempted").boolean();result.restored=value.get("restored").boolean();result.desktopAvailable=value.get("desktopAvailable").boolean();
    result.stage=uint32_t(stage);result.error=uint32_t(error);result.foreignCount=uint32_t(count);result.restoreStage=uint32_t(restoreStage);
    if((result.ok&&(result.stage||result.error||result.restoreAttempted))||(!result.ok&&!result.stage)||(result.restored&&!result.restoreAttempted))return {};
    result.display=parseDisplayState(value.get("display"));
    return result;
  }catch(...){return {};}
}
// What a v2 resize needed before it succeeded or failed (private worker ->
// service fields, fixed counts only): SDK size calls asked again, capture
// confirmation rounds that re-planned, and session packets kept while an RPC
// waited (WorkerPipeBacklog). The service logs them as hostAction 202 stages
// 410 (kept packets), 411 (SDK retries) and 412 (confirmation re-plans).
inline std::string displayV2RetryFields(const DisplayV2Result& result,uint32_t deferred){
  return ",\"sdkRetries\":"+std::to_string(std::min<uint32_t>(result.sdkRetries,64))+",\"confirmRetries\":"+std::to_string(std::min<uint32_t>(result.confirmRetries,64))+",\"deferredPackets\":"+std::to_string(std::min<uint32_t>(deferred,1024));
}
struct DisplayV2RetryDiagnostic {uint32_t sdkRetries=0,confirmRetries=0,deferredPackets=0;};
// Absent fields (a v1 worker) read as zero; malformed values as nothing.
inline DisplayV2RetryDiagnostic displayV2RetryDiagnostic(const Json& value){
  DisplayV2RetryDiagnostic result;
  const auto read=[&](const char* name,double maximum,uint32_t& target){const auto number=value.get(name).number(0);if(nativeDisplayInteger(number,0,maximum))target=uint32_t(number);};
  read("sdkRetries",64,result.sdkRetries);read("confirmRetries",64,result.confirmRetries);read("deferredPackets",1024,result.deferredPackets);
  return result;
}
// worker -> service: a v2 group resize result. The v1 fields plus the same
// fixed nativeStage/nativeError/consoleReturned values as a single-output
// v2 resize (streamViewportDiagnostic reads them).
inline std::string displayV2TrackResizeResultBytes(const NativeTrackResize& request,const DisplayV2Result& result,bool ok,bool desktop,const std::string& display,uint32_t deferred=0){
  const auto [stage,error]=displayV2ResultCode(result,ok);
  return "{\"type\":\"worker.tracks.resize.result\","+nativeTrackResizeFields(request)+",\"ok\":"+(ok?"true":"false")+",\"nativeStage\":"+std::to_string(stage)+",\"nativeError\":"+std::to_string(error)+
    ",\"nativeReason\":0,\"consoleReturned\":"+(result.consoleReturned?"true":"false")+",\"profileStage\":0,\"desktopAvailable\":"+(desktop?"true":"false")+displayV2RetryFields(result,deferred)+",\"display\":"+display+'}';
}
}
