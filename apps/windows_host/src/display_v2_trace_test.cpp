// Production trace producer -> private parser -> journal formatter/reader ->
// actual diagnostic delivery. Default/--fixture uses in-memory CCD/SDK/store.
// --measure-ccd-query is a separate, explicit local read-only timing probe.
#include "json_object.hpp"
#include "display_v2_trace.hpp"
#include "display_v2_tracks.hpp"
#include "native_stream_profile_worker.hpp"
#include "host_diagnostics.hpp"
#include "../tests/display-v2-world.hpp"
#include <iostream>
#include <set>
#include <chrono>
using namespace host;
namespace {
using namespace v2world;
constexpr const char* session="12345678-1234-4234-8234-123456789abc";
int checks=0;Json::Array journal;
void require(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
void emit(DisplayV2TraceValues fields){
  const auto time="2026-11-04T00:00:"+std::string(journal.size()<10?"0":"")+std::to_string(journal.size())+".000Z";
  const auto bytes="{\"time\":"+jsonQuote(time)+",\"event\":\"hostAction\",\"values\":"+hostLogEventFields(fields)+'}';
  const auto safe=safeHostLogRecord(JsonParser::parse(bytes));
  require(!safe.empty()&&safe.find(session)==std::string::npos,"Public journal lost a row or included private session data");
  journal.push_back(JsonParser::parse(safe));
}
DisplayV2TracePacket prepared(DisplaySessionV2& display){DisplayV2TracePacket p;p.phase=1;p.snapshot=display.snapshotEvidence();return p;}
DisplayV2TracePacket stopped(DisplaySessionV2& display,const DisplayV2SnapshotEvidence& snapshot,const DisplayV2RestoreResult& outcome){
  DisplayV2TracePacket p;p.phase=2;p.snapshot=snapshot;p.restore=outcome;p.after=display.currentGeometryEvidence();p.statePreserved=display.savedStatePresentEvidence();return p;
}
void baselineCases(){
  for(const bool older:{false,true}){
    World w;Memory m;
    if(older){m.bytes=displayV2StateBytes({"older-private-session",displayV2ActiveCcd(w.query()),w.targets(),DisplayV2Console{consoleAdapter,consoleAdapter,0,consoleTarget}});}
    w.owned().x=3840;w.owned().width=w.ownedWidth=1280;w.owned().height=w.ownedHeight=720;
    DisplaySessionV2 display(worldAccess(w,m));
    const std::array<DisplayV2Output,1> outputs{{{"0",1280,720,0,0}}};
    require(display.startGroup(session,outputs,true,deadline(w)).ok,"Trace baseline fixture could not start");
    const auto p=prepared(display);const auto& evidence=p.snapshot;
    require(evidence.before.captured&&evidence.before.count==1&&evidence.before.rows[0].left==3840&&!evidence.before.rows[0].primary,"Actual pre-start CCD geometry was not retained");
    require(evidence.reusedExisting==older&&evidence.olderSession==older,"Older-state reuse flags were not actual snapshot decisions");
    require(evidence.saved.captured&&evidence.saved.rows[0].left==(older?1024u:3840u)&&evidence.saved.rows[0].width==(older?1920u:1280u),"Saved-original geometry was replaced by current viewport/SDK data");
    DisplayV2TraceCollector collector;
    require(collector.accept(JsonParser::parse(displayV2TraceBytes(session,p)),session,emit),"Actual private prepared packet was rejected");
    const auto n=journal.size();require(collector.accept(JsonParser::parse(displayV2TraceBytes(session,p)),session,emit)&&journal.size()==n,"Duplicate prepared trace flooded the journal");
    const auto outcome=display.restore();const auto end=stopped(display,evidence,outcome);
    require(outcome.outcome==DisplayV2Outcome::restored&&end.statePreserved==false&&end.after.rows[0].left==(older?1024u:3840u),"Restore fixture did not expose its actual original result");
    const auto mutationCount=w.applies+w.resizes+w.sizeCalls+m.saves+m.removes+m.discards;
    require(collector.accept(JsonParser::parse(displayV2TraceBytes(session,end)),session,emit),"Actual private stopped packet was rejected");
    collector.finished(emit);
    require(w.applies+w.resizes+w.sizeCalls+m.saves+m.removes+m.discards==mutationCount,"Trace publication changed display/store behavior");
    require(displayV2TraceBytes(session,end).size()<4096,"Private trace packet exceeded its fixed bound");
  }
}
void failureCases(){
  World w;Memory m;DisplaySessionV2 display(worldAccess(w,m));
  require(display.start(session,1280,720,true,deadline(w)).ok,"Failure trace fixture could not start");
  const auto p=prepared(display);w.eligible=false;
  const auto failed=display.restore();const auto end=stopped(display,p.snapshot,failed);
  require(failed.outcome==DisplayV2Outcome::failed&&end.statePreserved==true,"Failed restore lost its preserved-state evidence");
  DisplayV2TraceCollector collector;
  require(collector.accept(JsonParser::parse(displayV2TraceBytes(session,end)),session,emit),"Stopped trace could not recover a missing prepared packet");
  const auto previous=journal.size();collector.finished(emit);require(journal.size()==previous,"A received stop trace emitted a missing marker");
  const auto presence=display.savedStatePresentEvidence();m.throwLoad=true;const auto discards=m.discards;
  require(display.savedStatePresentEvidence()==presence&&m.discards==discards,"Diagnostic presence getter reread/discarded state instead of reusing actual restore evidence");
  DisplayV2TraceCollector missing;
  require(missing.accept(JsonParser::parse(displayV2TraceBytes(session,p)),session,emit),"Missing-stop fixture rejected its prepared packet");
  const auto count=journal.size();missing.finished(emit);missing.finished(emit);
  require(journal.size()==count+1&&journal.back().get("values").get("stage").number()==5,"Missing-stop evidence was absent or repeated");
}
void parserCases(){
  World w;Memory m;DisplaySessionV2 display(worldAccess(w,m));
  require(display.start(session,1280,720,true,deadline(w)).ok,"Parser fixture could not start");
  auto p=prepared(display);p.snapshot.before.rows[0].left=uint32_t(-1280);p.snapshot.before.rows[0].top=uint32_t(-720);p.snapshot.before.rows[0].primary=false;
  const auto good=JsonParser::parse(displayV2TraceBytes(session,p));
  const auto decoded=parseDisplayV2Trace(good,session);
  require(decoded&&decoded->snapshot.before.rows[0].left==4294966016u&&decoded->snapshot.before.rows[0].top==4294966576u,"Signed coordinate bits did not survive the private codec");
  DisplayV2TraceCollector signedCoordinates;require(signedCoordinates.accept(good,session,emit),"Signed geometry was rejected");
  const auto reject=[&](Json bad){const auto before=journal.size();DisplayV2TraceCollector c;require(!c.accept(bad,session,emit)&&journal.size()==before,"Malformed private trace changed journal state");};
  {auto v=good;std::get<Json::Object>(v.value)["sessionId"].value=std::string("87654321-1234-4234-8234-123456789abc");reject(v);}
  {auto v=good;std::get<Json::Object>(v.value)["path"].value=std::string("PRIVATE-PATH-DO-NOT-LOG");reject(v);}
  {auto v=good;std::get<Json::Object>(v.value)["phase"].value=3.0;reject(v);}
  {auto v=good;std::get<Json::Object>(v.value)["olderSession"].value=true;reject(v);}
  {auto v=good;std::get<Json::Object>(v.value)["statePreserved"].value=false;reject(v);}
  for(const auto* field:{"left","width","observed","primary"}){
    auto v=good;auto& before=std::get<Json::Object>(std::get<Json::Object>(v.value).at("before").value);auto& object=std::get<Json::Object>(std::get<Json::Array>(before.at("rows").value)[0].value);
    if(std::string(field)=="left")object[field].value=-1.0;else if(std::string(field)=="width")object[field].value=0.0;else if(std::string(field)=="observed")object[field].value=1.0;else object[field].value=true;
    reject(v);
  }
  require(hostLogEventFields({{"ok",1},{"error",4294967295LL},{"width",-1},{"path",12}})=="{\"ok\":true,\"error\":4294967295}","Production journal formatter changed its existing bool/numeric allowlist");
  volatile bool sinkFails=true;DisplayV2TraceCollector throwing;require(!throwing.accept(good,session,[&](DisplayV2TraceValues){if(sinkFails)throw std::runtime_error("injected sink");}),"Diagnostic sink exception escaped its best-effort boundary");
}
void envelopeCases(){
  World w;Memory m;DisplaySessionV2 display(worldAccess(w,m));
  require(display.start(session,1280,720,true,deadline(w)).ok,"Envelope fixture could not start");
  const auto packet=prepared(display);
  DisplayV2Result failure;failure.stage=displayV2Stage::state;
  const auto base=displayV2TracksStartedBytes(session,failure,false,true,"{}");
  auto wrapped=JsonParser::parse(withDisplayV2Trace(base,session,packet));DisplayV2TraceCollector collector;
  consumeDisplayV2Trace(wrapped,true,session,collector,emit);
  require(jsonObject(wrapped)==jsonObject(JsonParser::parse(base)),"Valid diagnostic extraction changed the authoritative reply");
  const auto authoritative=parseDisplayV2TracksStarted(wrapped,session);require(authoritative&&!authoritative->ok&&authoritative->stage==3,"The actual authoritative v2 parser changed after trace extraction");
  for(const bool enabled:{false,true}){
    auto bad=JsonParser::parse(base);std::get<Json::Object>(bad.value)["displayTrace"].value=std::string("PRIVATE-INVALID-TRACE");const auto before=journal.size();
    DisplayV2TraceCollector ignored;consumeDisplayV2Trace(bad,enabled,session,ignored,emit);
    require(jsonObject(bad)==jsonObject(JsonParser::parse(base))&&journal.size()==before,"Malformed/disabled diagnostics affected authoritative failure or journal");
  }
  auto throwing=JsonParser::parse(withDisplayV2Trace(base,session,packet));DisplayV2TraceCollector failedSink;
  volatile bool sinkFails=true;consumeDisplayV2Trace(throwing,true,session,failedSink,[&](DisplayV2TraceValues){if(sinkFails)throw std::runtime_error("sink unavailable");});
  require(jsonObject(throwing)==jsonObject(JsonParser::parse(base)),"A diagnostic sink exception left extra ACK/reply fields behind");
  const auto large="{\"type\":\"worker.v2.started\",\"padding\":\""+std::string(4096,'x')+"\"}";
  require(withDisplayV2Trace(large,session,packet)==large,"Telemetry extended an already large private reply");
  const auto outcome=display.restore();const auto end=stopped(display,packet.snapshot,outcome);
  const auto ack=nativeStreamStoppedPacket(session,true,true);
  const auto ackBytes=withDisplayV2Trace(ack,session,end);require(ackBytes.size()<=4096,"Stop ACK escaped its conservative private bound");
  auto parsed=JsonParser::parse(ackBytes);consumeDisplayV2Trace(parsed,true,session,collector,emit);
  require(jsonObject(parsed)==jsonObject(JsonParser::parse(ack)),"Stopped diagnostics changed the authoritative ACK");
  NativeStreamStopFence fence;fence.reset(session,true);require(fence.acknowledge(parsed),"The actual stop fence rejected the stripped ACK");fence.finished(true);require(fence.released(),"Successful restore ACK changed meaning");
  auto badAck=JsonParser::parse(nativeStreamStoppedPacket(session,true,false));std::get<Json::Object>(badAck.value)["displayTrace"].value=std::string("invalid");DisplayV2TraceCollector badAckTrace;
  consumeDisplayV2Trace(badAck,true,session,badAckTrace,emit);NativeStreamStopFence failedFence;failedFence.reset(session,true);require(failedFence.acknowledge(badAck),"Malformed telemetry broke a valid failed-restore ACK");failedFence.finished(true);require(!failedFence.released(),"Telemetry turned a failed restore into success");
  World failedWorld;Memory absent;absent.failSave=true;DisplaySessionV2 failed(worldAccess(failedWorld,absent));
  const auto failedResult=failed.start(session,1280,720,true,deadline(failedWorld));require(!failedResult.ok&&!absent.bytes,"Failed-start fixture unexpectedly created state");
  const auto failedPacket=prepared(failed);require(failedPacket.snapshot.before.captured&&!failedPacket.snapshot.saved.captured,"Failed-start evidence invented a saved original");
  const auto noState=failed.restore();require(noState.outcome==DisplayV2Outcome::nothing,"No-existing-state restore changed behavior");
  DisplayV2TraceCollector failedStart;const auto failedEnd=stopped(failed,failedPacket.snapshot,noState);
  require(failedStart.accept(JsonParser::parse(displayV2TraceBytes(session,failedEnd)),session,emit),"Failed-start/no-state evidence was rejected");
  auto failedReply=JsonParser::parse(withDisplayV2Trace(displayV2TracksStartedBytes(session,failedResult,false,true,"{}"),session,failedPacket));DisplayV2TraceCollector failedReplyTrace;
  consumeDisplayV2Trace(failedReply,true,session,failedReplyTrace,emit);const auto parsedFailure=parseDisplayV2TracksStarted(failedReply,session);require(parsedFailure&&!parsedFailure->ok&&parsedFailure->stage==failedResult.stage,"Actual failed-start semantics changed with telemetry");
}
void postReadCases(){
  World w;Memory m;auto access=worldAccess(w,m);const auto owned=access.owned;const auto query=access.query;
  int ownedReads=0,ccdReads=0;bool forbidOwned=false,failQuery=false;
  access.owned=[&]{++ownedReads;if(forbidOwned)throw std::runtime_error("unexpected post enumeration");return owned();};
  access.query=[&]{++ccdReads;if(failQuery)throw NativeTopologyWindowsError(ERROR_TIMEOUT);return query();};
  DisplaySessionV2 display(std::move(access));require(display.start(session,1280,720,true,deadline(w)).ok,"Post-read fixture could not start");
  const auto snapshot=display.snapshotEvidence();const auto outcome=display.restore();const auto beforeOwned=ownedReads,beforeCcd=ccdReads;
  failQuery=true;forbidOwned=true;const auto after=display.currentGeometryEvidence();
  require(!after.captured&&ownedReads==beforeOwned&&ccdReads==beforeCcd+1,"Diagnostic CCD failure retried or enumerated SDK/lease state");
  const auto presence=display.savedStatePresentEvidence();m.throwLoad=true;
  require(display.savedStatePresentEvidence()==presence&&presence==false,"Presence evidence did additional file IO or changed the successful restore result");
  DisplayV2TracePacket packet;packet.phase=2;packet.snapshot=snapshot;packet.restore=outcome;packet.after=after;packet.statePreserved=presence;
  auto ack=JsonParser::parse(withDisplayV2Trace(nativeStreamStoppedPacket(session,true,outcome.ok()),session,packet));DisplayV2TraceCollector trace;
  consumeDisplayV2Trace(ack,true,session,trace,emit);NativeStreamStopFence fence;fence.reset(session,true);require(fence.acknowledge(ack),"Unknown post geometry invalidated the authoritative ACK");fence.finished(true);
  require(fence.released(),"Failed diagnostic read changed a successful restore into cleanup failure");
}
Json reports(){
  // Actual report selection/ACK backlog must carry every produced trace row,
  // even with older maintenance errors and more than one8-row report of data.
  Json events;events.value=journal;HostDiagnosticDelivery delivery;Json::Array reports;
  DisplayState display;display.installed=display.deviceReady=display.owned=display.active=true;display.width=1280;display.height=720;
  std::set<std::string> required;for(const auto& row:journal)required.insert(safeHostLogRecord(row));
  std::set<std::string> seen;
  for(unsigned pass=0;pass<16&&seen.size()<required.size();++pass){
    const auto bytes=buildHostDiagnosticReport(display,true,true,true,"",Json{},events,true,0,{},{},{},&delivery);
    require(bytes.size()<=6144,"Trace delivery escaped the existing broker report bound");
    const auto report=JsonParser::parse(bytes);const auto& rows=std::get<Json::Array>(report.get("events").value);
    require(rows.size()<=8,"Trace delivery escaped the existing event bound");
    for(const auto& row:rows)seen.insert(safeHostLogRecord(row));reports.push_back(report);
    (void)delivery.acknowledged(true);
  }
  require(seen==required,"Actual report/ACK delivery dropped a trace stage or track");
  Json result;result.value=std::move(reports);return result;
}
}
int main(int argc,char** argv){try{
  if(argc==2&&std::string(argv[1])=="--measure-ccd-query"){
    bool all=true;std::cout<<"{\"kind\":\"local-read-only-ccd-query-cost\",\"scope\":\"local QA machine, not VM149\",\"osDisplayChanges\":0,\"samples\":[";
    for(unsigned i=0;i<3;++i){
      NativeCcdQueryAccess access;const auto sizes=access.sizes;const auto read=access.read;unsigned sizeCalls=0,readCalls=0;DWORD error=0;
      access.sizes=[&](UINT32 flags,UINT32* p,UINT32* m){++sizeCalls;return sizes(flags,p,m);};
      access.read=[&](UINT32 flags,UINT32* p,DISPLAYCONFIG_PATH_INFO* pd,UINT32* m,DISPLAYCONFIG_MODE_INFO* md){++readCalls;return read(flags,p,pd,m,md);};
      const auto start=std::chrono::steady_clock::now();try{(void)queryNativeCcd(access);}catch(const NativeTopologyWindowsError& e){error=e.code;}catch(...){error=ERROR_INVALID_DATA;}
      const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();all=all&&!error;
      if(i)std::cout<<',';std::cout<<"{\"microseconds\":"<<us<<",\"error\":"<<error<<",\"sizeCalls\":"<<sizeCalls<<",\"readCalls\":"<<readCalls<<'}';
    }
    std::cout<<"],\"hardWallClockBoundClaimed\":false}\n";return all?0:1;
  }
  baselineCases();failureCases();parserCases();envelopeCases();postReadCases();const auto delivered=reports();
  Json rows;rows.value=journal;
  if(argc==2&&std::string(argv[1])=="--fixture")std::cout<<"{\"kind\":\"display-v2-trace-native-fixture\",\"checks\":"<<checks<<",\"osDisplayChanges\":0,\"journal\":"<<jsonObject(rows)<<",\"reports\":"<<jsonObject(delivered)<<"}\n";
  else std::cout<<"Display v2 trace checks passed: "<<checks<<" (in-memory CCD/SDK; no OS changes)\n";
  return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
