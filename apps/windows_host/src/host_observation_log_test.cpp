#include "host_observation_log.hpp"
#include <iostream>
#include <thread>

namespace {
using host::Json;
const std::string session="12345678-1234-4234-8234-000000000001";
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
Json snapshot(unsigned epoch=1,const std::string& id=session,bool tracks=false){
  return host::JsonParser::parse("{\"type\":\""+std::string(tracks?"stream.tracks.stats":"stream.stats")+"\",\"sessionId\":"+host::jsonQuote(id)+
    (tracks?",\"topologyEpoch\":":",\"epoch\":")+std::to_string(epoch)+R"(,"observability":{"protocol":"stream-observability-v1","metrics":{"hostQueuePackets":2,"hostQueueBytes":1024,"hostQueueOldestUs":3000,"serviceCpuPct10":10,"workerCpuPct10":50,"readbackUs":100,"readbackMaxUs":200,"password":"PRIVATE_SENTINEL","relayQueueBytes":999}}})");
}
std::vector<Json> drain(host::HostObservationLogBuffer& buffer){std::vector<Json> rows;while(buffer.pending())buffer.drain([&](const std::string& row){require(row.find("PRIVATE_SENTINEL")==std::string::npos,"Log retained private payload");rows.push_back(host::JsonParser::parse(row));});return rows;}
}
int main(){try{
  int checks=0;
  require(!host::operationalLogEnabled,"Fixture must never write installed Host journal");
  {host::queueHostObservationLog(host::streamObservationJson(snapshot()));require(!host::hostObservationLogsPending(),"OFF queued a real journal record");++checks;}
  {host::HostObservationLogBuffer buffer;buffer.observe(snapshot(),0);buffer.observe(snapshot(),29999);require(!buffer.pending(),"Snapshot logged before its batching interval");buffer.observe(snapshot(),30000);const auto rows=drain(buffer);require(rows.size()==2,"Service and worker sources were not split");
    for(const auto& row:rows){require(row.get("sessionId").string()==session&&row.get("epoch").number()==1&&row.get("windowMs").number()==30000&&row.get("samples").number()==3&&!row.get("final").boolean(),"Window metadata incorrect");
      const auto& values=row.get("values");require(std::holds_alternative<std::nullptr_t>(values.get("password").value)&&std::holds_alternative<std::nullptr_t>(values.get("relayQueueBytes").value),"Log accepted unallowlisted metrics");
      if(row.get("source").string()=="host-service")require(values.get("hostQueueBytes").number()==1024&&std::holds_alternative<std::nullptr_t>(values.get("workerCpuPct10").value),"Service record confused worker provenance");
      else require(values.get("workerCpuPct10").number()==50&&std::holds_alternative<std::nullptr_t>(values.get("serviceCpuPct10").value),"Worker record confused service provenance");}++checks;}
  {host::HostObservationLogBuffer buffer;buffer.observe(snapshot(),100);buffer.finish("other-session",500);require(!buffer.pending(),"Other session ended active diagnostics");buffer.finish(session,5100);const auto rows=drain(buffer);require(rows.size()==2&&rows.front().get("final").boolean()&&rows.front().get("windowMs").number()==5000,"Short disconnect lost final summary");buffer.finish(session,6000);require(!buffer.pending(),"Final summary was duplicated");++checks;}
  {host::HostObservationLogBuffer buffer;buffer.observe(snapshot(),0);buffer.observe(snapshot(2),1000);const auto first=drain(buffer);require(first.size()==2&&first.front().get("epoch").number()==1&&first.front().get("final").boolean(),"Resize relabeled previous epoch");buffer.finish(session,2000);const auto second=drain(buffer);require(second.size()==2&&second.front().get("epoch").number()==2,"Resize mixed observations across epochs");++checks;}
  {host::HostObservationLogBuffer buffer;buffer.observe(snapshot(7,session,true),100);buffer.finish(session,200);const auto rows=drain(buffer);require(rows.size()==2&&rows.front().get("topologyEpoch").number()==7&&std::holds_alternative<std::nullptr_t>(rows.front().get("epoch").value),"Track log invented shared output epoch");++checks;}
  {host::HostObservationLogBuffer buffer;buffer.observe(snapshot(1,"PRIVATE_SENTINEL"),0);buffer.finish("PRIVATE_SENTINEL",30001);require(!buffer.pending(),"Log accepted a non-UUID correlation string");auto bad=snapshot();std::get<Json::Object>(bad.value)["epoch"].value=-1.0;buffer.observe(bad,0);require(!buffer.pending(),"Log accepted invalid epoch");++checks;}
  {host::HostObservationLogBuffer buffer;for(unsigned n=0;n<40;++n)buffer.observe(snapshot(n),n*1000);buffer.finish(session,40000);require(buffer.dropped()>0,"Stalled writer did not bound diagnostic backlog");
    unsigned rows=0;while(buffer.pending()){unsigned batch=0;buffer.drain([&](const std::string& row){++batch;++rows;require(!host::safeHostLogRecord(host::JsonParser::parse(row)).empty(),"Staged row failed safe projection");});require(batch<=4,"Collector drain exceeded batch bound");}require(rows<=16,"Pending journal memory exceeded record bound");++checks;}
  {host::HostObservationLogBuffer buffer;buffer.observe(snapshot(),1000);buffer.observe(snapshot(),10);const auto rows=drain(buffer);require(rows.size()==2&&rows.front().get("windowMs").number()==0&&rows.front().get("final").boolean(),"Clock reset produced negative duration");++checks;}
  {host::HostObservationLogBuffer buffer;const std::string next="12345678-1234-4234-8234-000000000002";buffer.observe(snapshot(),0);buffer.finish(session,1);buffer.drain([&](const std::string&){buffer.observe(snapshot(2,next),2);throw std::runtime_error("injected disk sink failure");});buffer.finish(next,3);require(drain(buffer).size()==2,"Writer failure held producer lock or broke next session");++checks;}
  {host::HostObservationLogBuffer buffer;buffer.observe(snapshot(2),0);buffer.observe(snapshot(1),1);buffer.finish(session,2);buffer.observe(snapshot(3),3);const auto rows=drain(buffer);require(rows.size()==2&&rows.front().get("epoch").number()==2&&rows.front().get("samples").number()==1,"Late epoch or retired session polluted diagnostics");++checks;}
  {const auto source=host::JsonParser::parse(R"({"time":"2026-10-30T10:00:00.000Z","event":"displayObserved","values":{"reboot":true,"monitors":2,"password":"PRIVATE_SENTINEL"},"token":"PRIVATE_SENTINEL"})");const auto raw=host::safeHostLogRecord(source);require(raw==R"({"time":"2026-10-30T10:00:00.000Z","event":"displayObserved","values":{"monitors":2,"reboot":true}})","Legacy safe log shape changed");++checks;}
  std::cout<<"Host observation log: "<<checks<<" cases passed; memory-only fixture, real journal disabled\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
