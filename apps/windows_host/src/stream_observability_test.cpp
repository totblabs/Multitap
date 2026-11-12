#include "support.hpp"
#include "stream_observability.hpp"
#include <iostream>
#include <limits>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
host::Json start(bool enabled=true,bool tracks=false,const char* mode="stream"){
  return host::JsonParser::parse(std::string("{\"type\":\"session.start\",\"mode\":\"")+mode+"\",\"sessionId\":\"session-a\",\"epoch\":2"+
    (tracks?",\"mediaProtocol\":\"tracks-v1\"":"")+(enabled?",\"observabilityProtocol\":\"stream-observability-v1\"":"")+'}');
}
const char* single=R"({"type":"stream.stats","sessionId":"session-a","epoch":2,"encodedFps":60})";
std::string augment(host::StreamObservability& observer,const std::string& raw,bool service=false,
                    std::optional<host::ObservedSendQueue> queue={},std::optional<std::pair<uint64_t,uint64_t>> readback={}){
  return observer.augment<host::Json>(raw,service,queue,readback,host::JsonParser::parse,host::streamObservationJson);
}
}
int main(){try{
  int checks=0;
  {host::ProcessCpuObservation cpu;
    require(!cpu.observe(100,10,4),"CPU fabricated a first sample");
    require(cpu.observe(200,110,4)==uint32_t(250),"CPU is not normalized to whole-machine capacity");
    require(!cpu.observe(190,120,4),"CPU accepted backwards monotonic time");
    require(!cpu.observe(300,5,4),"CPU accepted reset process counters");
    require(!cpu.observe(400,15,0),"CPU fabricated missing processor capacity");
    cpu.reset();require(!cpu.observe(500,25,4),"CPU reset retained old baseline");++checks;}
  {host::ReadbackObservation window;
    require(!window.take(),"Readback fabricated a missing sample");window.observe({});
    window.observe(std::numeric_limits<double>::quiet_NaN());window.observe(-1);require(!window.take(),"Readback accepted invalid durations");
    window.observe(1);window.observe(3);const auto sample=window.take();require(sample&&sample->first==2000&&sample->second==3000,"Readback mean/max incorrect");
    require(!window.take(),"Readback reused previous reporting window");++checks;}
  {host::StreamObservability observer;int parsed=0;
    observer.configure(start(false));const auto raw=observer.augment<host::Json>(single,false,{}, {},[&](const std::string& text){++parsed;return host::JsonParser::parse(text);},host::streamObservationJson);
    require(raw==single&&parsed==0,"Disabled observer parsed or changed the released wire");
    observer.configure(start(true,false,"rdp"));require(!observer.enabled()&&augment(observer,single)==single,"RDP enabled stream observations");++checks;}
  {host::StreamObservability observer;observer.configure(start());
    auto value=host::JsonParser::parse(augment(observer,R"({"type":"stream.stats","encodedFps":60})",false,{},std::pair<uint64_t,uint64_t>{1000,3000}));
    require(value.get("sessionId").string()=="session-a"&&value.get("epoch").number(-1)==2,"Worker did not bind non-resize observations to exact session/epoch");
    const auto& metrics=value.get("observability").get("metrics");require(metrics.get("readbackUs").number(-1)==1000&&metrics.get("readbackMaxUs").number(-1)==3000,"Worker readback snapshot missing");
    require(value.get("encodedFps").number()==60,"Observer changed media statistics");++checks;}
  {host::StreamObservability observer;observer.configure(start());
    const std::string stale=R"({"type":"stream.stats","sessionId":"session-old","epoch":2})";
    require(augment(observer,stale)==stale,"A retired worker report inherited current session");
    const std::string epoch=R"({"type":"stream.stats","sessionId":"session-a","epoch":1})";
    require(augment(observer,epoch)==epoch,"Worker relabeled an old epoch");
    const std::string unbound=R"({"type":"stream.stats","encodedFps":60})";
    require(augment(observer,unbound,true)==unbound,"Service invented binding for an unbound worker packet");
    require(augment(observer,"not-json")=="not-json","Observer failure broke base transport");
    observer.reset();require(augment(observer,single)==single,"Stopped session continued observation");++checks;}
  {host::StreamObservability observer;observer.configure(start());
    const std::string raw=R"({"type":"stream.stats","sessionId":"session-a","epoch":3,"observability":{"protocol":"stream-observability-v1","metrics":{"workerCpuPct10":50,"serviceCpuPct10":999,"hostQueueBytes":999,"readbackUs":2,"readbackMaxUs":3,"secret":123,"relayQueueBytes":999}}})";
    host::ObservedSendQueue queue{4,8192,1200,123};const auto value=host::JsonParser::parse(augment(observer,raw,true,queue));const auto& metrics=value.get("observability").get("metrics");
    require(metrics.get("workerCpuPct10").number(-1)==50&&metrics.get("readbackUs").number(-1)==2,"Service lost valid worker observations");
    require(metrics.get("hostQueueBytes").number(-1)==8192&&metrics.get("hostQueueOldestUs").number(-1)==1200&&metrics.get("hostWriteUs").number(-1)==123,"Service queue snapshot incorrect");
    require(std::holds_alternative<std::nullptr_t>(metrics.get("serviceCpuPct10").value)&&std::holds_alternative<std::nullptr_t>(metrics.get("relayQueueBytes").value)&&std::holds_alternative<std::nullptr_t>(metrics.get("secret").value),"Worker spoofed service/relay metrics or unallowlisted content");
    require(value.get("epoch").number()==3,"Service rewrote worker's active resize epoch");++checks;}
  {host::StreamObservability observer;observer.configure(start(true,true));
    const std::string raw=R"({"type":"stream.tracks.stats","sessionId":"session-a","topologyEpoch":12,"tracks":[{"id":"0","epoch":7},{"id":"1","epoch":9}]})";
    const auto value=host::JsonParser::parse(augment(observer,raw,true,host::ObservedSendQueue{0,0,0,{}}));
    require(value.get("topologyEpoch").number()==12&&std::get<host::Json::Array>(value.get("tracks").value).size()==2,"Track topology was changed");
    require(value.get("observability").get("metrics").get("hostQueuePackets").number(-1)==0,"Empty track tunnel queue not observed");
    require(std::holds_alternative<std::nullptr_t>(value.get("epoch").value),"Track snapshot fabricated a shared media epoch");
    require(augment(observer,single)==single,"Track session accepted unrelated single-output statistics");++checks;}
  {host::StreamObservability observer;observer.configure(start());
    const auto value=host::JsonParser::parse(augment(observer,single,true,host::ObservedSendQueue{999999,99999999,999999999,999999999}));
    const auto& metrics=value.get("observability").get("metrics");
    for(const auto* name:{"hostQueuePackets","hostQueueBytes","hostQueueOldestUs","hostWriteUs"})require(std::holds_alternative<std::nullptr_t>(metrics.get(name).value),"Observer emitted an out-of-contract measurement");++checks;}
  std::cout<<"Host observability: "<<checks<<" cases passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
