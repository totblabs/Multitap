#pragma once
#include "display_v2.hpp"
#include "operational_log.hpp"

namespace host {
inline constexpr uint32_t displayV2TraceAction=280;
inline constexpr const char* displayV2TraceType="worker.v2.trace";
using DisplayV2TraceValues=std::initializer_list<std::pair<const char*,int64_t>>;
struct DisplayV2TracePacket {
  uint32_t phase=0;
  DisplayV2SnapshotEvidence snapshot;
  std::optional<DisplayV2RestoreResult> restore;
  DisplayV2GeometrySet after;
  std::optional<bool> statePreserved;
};
inline std::string displayV2GeometryBytes(const DisplayV2GeometrySet& set){
  std::string out="{\"captured\":"+std::string(set.captured?"true":"false")+",\"rows\":[";
  for(uint32_t i=0;i<set.count&&i<set.rows.size();++i){
    const auto& r=set.rows[i];if(i)out+=',';
    out+="{\"track\":"+std::to_string(r.track)+",\"width\":"+std::to_string(r.width)+",\"height\":"+std::to_string(r.height)+",\"left\":"+std::to_string(r.left)+",\"top\":"+std::to_string(r.top)+
      ",\"observed\":"+(r.observed?"true":"false")+",\"active\":"+(r.active?"true":"false")+",\"primary\":"+(r.primary?"true":"false")+'}';
  }
  return out+"]}";
}
inline std::string displayV2TraceBytes(const std::string& session,const DisplayV2TracePacket& packet){
  const auto& s=packet.snapshot;
  const auto restore=packet.restore?"{\"outcome\":"+std::to_string(int(packet.restore->outcome))+",\"stage\":"+std::to_string(packet.restore->stage)+",\"error\":"+std::to_string(packet.restore->error)+'}':"null";
  return "{\"type\":\"worker.v2.trace\",\"protocol\":\"display-v2-trace-v1\",\"sessionId\":"+jsonQuote(session)+",\"phase\":"+std::to_string(packet.phase)+
    ",\"before\":"+displayV2GeometryBytes(s.before)+",\"saved\":"+displayV2GeometryBytes(s.saved)+",\"reusedExisting\":"+(s.reusedExisting?"true":"false")+
    ",\"olderSession\":"+(s.olderSession?"true":"false")+",\"restore\":"+restore+",\"after\":"+displayV2GeometryBytes(packet.after)+
    ",\"statePreserved\":"+(packet.statePreserved?(*packet.statePreserved?"true":"false"):"null")+'}';
}
inline bool displayV2TraceKeys(const Json& value,std::initializer_list<const char*> names){
  const auto* object=std::get_if<Json::Object>(&value.value);if(!object||object->size()!=names.size())return false;
  for(const auto* name:names)if(!object->contains(name))return false;return true;
}
inline bool displayV2TraceNumber(const Json& value,uint32_t maximum,uint32_t& out){
  const auto* n=std::get_if<double>(&value.value);
  if(!n||!std::isfinite(*n)||*n<0||*n>maximum||std::floor(*n)!=*n)return false;
  out=uint32_t(*n);return true;
}
inline bool displayV2TraceBit(const Json& value,bool& out){const auto* bit=std::get_if<bool>(&value.value);if(!bit)return false;out=*bit;return true;}
inline bool parseDisplayV2Geometry(const Json& value,DisplayV2GeometrySet& out){
  if(!displayV2TraceKeys(value,{"captured","rows"})||!displayV2TraceBit(value.get("captured"),out.captured))return false;
  const auto* rows=std::get_if<Json::Array>(&value.get("rows").value);
  if(!rows||rows->size()>2||out.captured!=!rows->empty())return false;
  for(const auto& item:*rows){
    if(!displayV2TraceKeys(item,{"track","width","height","left","top","observed","active","primary"}))return false;
    auto& r=out.rows[out.count++];
    if(!displayV2TraceNumber(item.get("track"),1,r.track)||!displayV2TraceNumber(item.get("width"),32768,r.width)||!displayV2TraceNumber(item.get("height"),32768,r.height)||
       !displayV2TraceNumber(item.get("left"),UINT32_MAX,r.left)||!displayV2TraceNumber(item.get("top"),UINT32_MAX,r.top)||
       !displayV2TraceBit(item.get("observed"),r.observed)||!displayV2TraceBit(item.get("active"),r.active)||!displayV2TraceBit(item.get("primary"),r.primary))return false;
    if(out.count>1&&out.rows[out.count-2].track>=r.track)return false;
    if(r.observed){if(!r.active||!r.width||!r.height||r.primary!=(r.left==0&&r.top==0))return false;}
    else if(r.width||r.height||r.left||r.top||r.primary)return false;
  }
  return true;
}
inline std::optional<DisplayV2TracePacket> parseDisplayV2Trace(const Json& value,const std::string& expected){
  if(!hostObservationSession(expected)||value.get("type").string()!=displayV2TraceType||value.get("protocol").string()!="display-v2-trace-v1"||value.get("sessionId").string()!=expected||
     !displayV2TraceKeys(value,{"type","protocol","sessionId","phase","before","saved","reusedExisting","olderSession","restore","after","statePreserved"}))return {};
  DisplayV2TracePacket p;
  if(!displayV2TraceNumber(value.get("phase"),2,p.phase)||!p.phase||!parseDisplayV2Geometry(value.get("before"),p.snapshot.before)||!parseDisplayV2Geometry(value.get("saved"),p.snapshot.saved)||
     !parseDisplayV2Geometry(value.get("after"),p.after)||!displayV2TraceBit(value.get("reusedExisting"),p.snapshot.reusedExisting)||!displayV2TraceBit(value.get("olderSession"),p.snapshot.olderSession)||
     (p.snapshot.olderSession&&!p.snapshot.reusedExisting))return {};
  if(!std::holds_alternative<std::nullptr_t>(value.get("restore").value)){
    const auto& result=value.get("restore");DisplayV2RestoreResult restore;uint32_t outcome=0,error=0;
    if(!displayV2TraceKeys(result,{"outcome","stage","error"})||!displayV2TraceNumber(result.get("outcome"),3,outcome)||!displayV2TraceNumber(result.get("stage"),9,restore.stage)||!displayV2TraceNumber(result.get("error"),UINT32_MAX,error))return {};
    restore.outcome=DisplayV2Outcome(outcome);restore.error=error;p.restore=restore;
  }
  if(!std::holds_alternative<std::nullptr_t>(value.get("statePreserved").value)){bool present=false;if(!displayV2TraceBit(value.get("statePreserved"),present))return {};p.statePreserved=present;}
  if(p.phase==1&&(p.restore||p.after.captured||p.statePreserved))return {};
  return p;
}
template<class Emit> void emitDisplayV2Geometry(uint32_t stage,const DisplayV2GeometrySet& set,const DisplayV2SnapshotEvidence& snapshot,Emit&& emit){
  const auto row=[&](const DisplayV2GeometryEvidence& r,uint32_t item){emit(DisplayV2TraceValues{{"action",displayV2TraceAction},{"stage",stage},{"items",item},{"monitors",set.count},{"owned",set.captured},{"ok",r.observed},{"active",r.active},{"changed",r.primary},{"width",r.width},{"height",r.height},{"expected",r.left},{"missing",r.top},{"recovery",snapshot.reusedExisting},{"stale",snapshot.olderSession}});};
  if(!set.captured){row({},2);return;}
  for(uint32_t i=0;i<set.count;++i)row(set.rows[i],set.rows[i].track);
}
// The collector belongs to one already authenticated UserWorker pipe. Duplicate
// phases are ignored and mismatched snapshots never get mixed into its session.
// A malformed diagnostic packet is consumed by the caller, never a stream error.
class DisplayV2TraceCollector {
 public:
  template<class Emit> bool accept(const Json& value,const std::string& session,Emit&& emit)noexcept{
    try{
      const auto packet=parseDisplayV2Trace(value,session);if(!packet)return false;
      if(snapshot_&&*snapshot_!=packet->snapshot)return false;
      if(!snapshot_){snapshot_=packet->snapshot;emitDisplayV2Geometry(1,snapshot_->before,*snapshot_,emit);emitDisplayV2Geometry(2,snapshot_->saved,*snapshot_,emit);}
      if(packet->phase==2&&!stopped_){
        stopped_=true;const auto& result=packet->restore;
        emit(DisplayV2TraceValues{{"action",displayV2TraceAction},{"stage",3},{"active",result.has_value()},{"ok",result&&result->ok()},{"format",result?uint32_t(result->outcome):0},{"cause",result?result->stage:0},{"error",result?result->error:0},{"owned",packet->statePreserved.has_value()},{"pending",packet->statePreserved.value_or(false)}});
        emitDisplayV2Geometry(4,packet->after,*snapshot_,emit);
      }
      return true;
    }catch(...){return false;}
  }
  template<class Emit> void finished(Emit&& emit)noexcept{
    try{if(snapshot_&&!stopped_&&!missing_){missing_=true;emit(DisplayV2TraceValues{{"action",displayV2TraceAction},{"stage",5},{"ok",false},{"cause",1}});}}catch(...){ }
  }
 private:std::optional<DisplayV2SnapshotEvidence> snapshot_;bool stopped_=false,missing_=false;
};
// Piggyback on existing reliable private replies/ACKs: no extra pipe writes,
// frame prefixes, deadlines, or cancellation operations for diagnostics.
inline std::string withDisplayV2Trace(std::string response,const std::string& session,const DisplayV2TracePacket& packet)noexcept{
  try{if(response.empty()||response.back()!='}')return response;const auto trace=displayV2TraceBytes(session,packet);if(response.size()+trace.size()+16>4096)return response;return response.substr(0,response.size()-1)+",\"displayTrace\":"+trace+'}';}catch(...){return response;}
}
template<class Emit> void consumeDisplayV2Trace(Json& envelope,bool v2,const std::string& session,DisplayV2TraceCollector& collector,Emit&& emit)noexcept{
  try{
    auto* object=std::get_if<Json::Object>(&envelope.value);if(!object)return;
    const auto found=object->find("displayTrace");if(found==object->end())return;
    const auto* type=std::get_if<std::string>(&envelope.get("type").value);
    if(!type||(*type!="worker.v2.started"&&*type!="worker.v2.tracks.started"&&*type!="worker.initial.result"&&*type!="worker.session.stopped"))return;
    Json trace=std::move(found->second);
    // Even a malformed trace leaves the original authoritative packet shape.
    object->erase(found);
    if(v2&&envelope.get("sessionId").string()==session)(void)collector.accept(trace,session,emit);
  }catch(...){ }
}
}
