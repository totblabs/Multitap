#include "agent.hpp"
#include "service_configuration_change.hpp"
#include "native_stream_profile_capability.hpp"
#include <iostream>
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
bool primary(const host::Json& packet){return std::get<host::Json::Array>(packet.get("capabilities").get("monitors").value).front().get("primary").boolean();}
struct Bridge final:host::AgentBridge {
  Send output;int reads=0,keyFrames=0,starts=0,stops=0;bool primary=false,ready=true,throwRead=false,credentialsConfigured=false;std::vector<host::Json> controls;std::vector<int> rates;
  void bitrate(const std::string& session,int value)override{require(session=="00000000-0000-0000-0000-000000000001","Adaptive target for another session");rates.push_back(value);}
  void keyFrame()override{++keyFrames;}
  void start(const host::Json&,Send send)override{++starts;output=std::move(send);}
  void control(const host::Json& value)override{controls.push_back(value);}
  void stop()override{++stops;}
  std::string capabilities()override{++reads;if(throwRead)throw std::runtime_error("injected capability failure");return "{\"service\":true,\"rdpCredentialsConfigured\":"+std::string(credentialsConfigured?"true":"false")+",\"streaming\":"+std::string(ready?"true":"false")+",\"monitors\":[{\"id\":\"0\",\"width\":1264,\"height\":760,\"primary\":"+(primary?"true":"false")+"}]}";}
  void displayControl(const host::Json&,Send)override{}
  std::atomic<int> diagnosticCalls{0};int diagnosticDelayMs=0;std::vector<bool> acks;
  std::string diagnostics()override{++diagnosticCalls;Sleep(DWORD(diagnosticDelayMs));return "{\"probe\":1}";}
  bool diagnosticsAcknowledged(bool accepted)override{acks.push_back(accepted);return false;}
  void text(const std::string& bytes){require(bool(output),"Missing actual Agent bridge callback");output(true,{bytes.begin(),bytes.end()});}
  void info(int epoch=1){text("{\"type\":\"stream.info\",\"codec\":\"h264\",\"sessionId\":\"00000000-0000-0000-0000-000000000001\",\"epoch\":"+std::to_string(epoch)+"}");}
};
struct Fixture {
  Bridge bridge;host::Agent agent;std::vector<host::Json> sent;int binary=0;
  explicit Fixture(const std::string& start=R"({"type":"session.start","mode":"stream","sessionId":"00000000-0000-0000-0000-000000000001","resizeProtocol":"epoch-v1","epoch":0,"monitor":"0"})"):agent([](const std::wstring&){},&bridge){agent.publicationTestOpen();agent.localControl(start);}
  bool drain(bool refuse=false){return agent.publicationTestDrain([&](bool text,const std::vector<uint8_t>& bytes){if(text){sent.push_back(host::JsonParser::parse(std::string(bytes.begin(),bytes.end())));return !refuse;}++binary;return true;});}
};
}
int main(){try{
  int checks=0;
  // A peer close has its own status, distinct from a WinHTTP receive error.
  // Failed queries and invalid statuses must not become invented peer causes.
  for(const USHORT code:{USHORT(1000),USHORT(1001),USHORT(1006),USHORT(4999),USHORT(0),USHORT(999),USHORT(5000),USHORT(65535)}){
    int calls=0;
    const auto query=[&](USHORT* status,void* reason,DWORD capacity,DWORD* used){++calls;require(capacity==128,"Close reason buffer is unbounded");*status=code;const std::string privateReason="private-close-reason";std::memcpy(reason,privateReason.data(),privateReason.size());*used=DWORD(privateReason.size());return DWORD(NO_ERROR);};
    require(host::hostChannelRemoteCloseCode(query)==(code>=1000&&code<=4999?uint32_t(code):0)&&calls==1,"Peer close status was lost or invented");++checks;
  }
  require(host::hostChannelRemoteCloseCode([](USHORT* code,void*,DWORD,DWORD* used){*code=1006;*used=129;return DWORD(ERROR_INSUFFICIENT_BUFFER);})==0,"Failed close query became a peer status");++checks;
  // Count 1 enables the viewport profile. Its fields join the service's base
  // capabilities inside a display result, which the actual sender parses.
  for(const bool profile:{false,true}){
    Fixture f;const auto caps=std::string("{\"service\":true,\"streamSizeProtocol\":\"stream-size-v1\"")+host::nativeStreamProfileCapabilityFields(profile)+'}';
    f.bridge.text("{\"type\":\"host.display.result\",\"operationId\":\"00000000-0000-0000-0000-000000000003\",\"ok\":true,\"count\":"+std::to_string(profile?1:2)+",\"capabilities\":"+caps+'}');
    require(f.drain()&&f.sent.size()==1&&f.sent[0].get("type").string()=="host.display.result"&&f.sent[0].get("count").number()==(profile?1:2)&&f.sent[0].get("capabilities").get("streamSizeProtocol").string()=="stream-size-v1"&&f.bridge.reads==0&&!f.agent.publicationTestClosed(),"Display count completion could not cross the actual Agent queue");++checks;
  }
  // The same settings signal/dispatch used by serviceMain queues a fresh
  // snapshot through the actual Agent while an existing stream stays active.
  // Saving or clearing credentials cannot reach reload or WTS lifecycle work.
  for(const bool configured:{true,false}){
    Fixture f;host::ServiceConfigurationChanges changes;int reloads=0,sessions=0;
    const int starts=f.bridge.starts,stops=f.bridge.stops;
    f.bridge.credentialsConfigured=!configured;
    const bool requested=host::applyThenRequestCapabilities([&]{f.bridge.credentialsConfigured=configured;},[&]{require(f.bridge.credentialsConfigured==configured,"Credential notification preceded persistence");return changes.request(host::hostServiceCapabilitiesControl);});
    // Repeated notifications coalesce and never become a fake session event.
    require(requested&&changes.request(host::hostServiceCapabilitiesControl),"Refresh signal not accepted");
    changes.processSessionChanges(false,[&]{++reloads;f.bridge.stop();},[&]{++sessions;f.bridge.stop();});
    if(changes.takeCapabilitiesRequest())f.agent.announceCapabilities();
    require(!changes.takeCapabilitiesRequest()&&!reloads&&!sessions&&f.bridge.starts==starts&&f.bridge.stops==stops,"Account refresh interrupted/reloaded the active stream");
    require(f.bridge.reads==0&&f.drain()&&f.sent.size()==1&&f.sent[0].get("type").string()=="host.capabilities"&&f.sent[0].get("capabilities").get("rdpCredentialsConfigured").boolean()==configured,"Account refresh did not publish the newly saved/cleared state");
    f.bridge.output(false,std::vector<uint8_t>(16));require(f.drain()&&f.binary==1&&f.bridge.stops==stops,"Existing media could not continue after the account refresh");++checks;
  }
  {host::ServiceConfigurationChanges changes;int reloads=0,sessions=0;
    require(!changes.request(127)&&!changes.request(130),"Unrecognized service controls became settings changes");
    changes.processSessionChanges(false,[&]{++reloads;},[&]{++sessions;});require(!reloads&&!sessions&&!changes.takeCapabilitiesRequest(),"Unknown/spurious signal mutated session state");
    changes.request(host::hostServiceCapabilitiesControl);changes.processSessionChanges(true,[&]{++reloads;},[&]{++sessions;});
    require(!reloads&&sessions==1&&changes.takeCapabilitiesRequest(),"A simultaneous real WTS event was lost or turned into a reload");
    changes.request(host::hostServiceCapabilitiesControl);changes.request(host::hostServiceReloadControl);
    changes.processSessionChanges(true,[&]{++reloads;},[&]{++sessions;});require(reloads==1&&sessions==1&&changes.takeCapabilitiesRequest(),"Explicit legacy reload lost its distinct behavior");
    changes.processSessionChanges(false,[&]{++reloads;},[&]{++sessions;});require(reloads==1&&sessions==1,"Consumed reload ran twice");++checks;
  }
  // The diagnostics snapshot runs off the send thread: a slow one neither holds
  // the send step nor runs twice at once, and its report is queued when ready.
  {Fixture f;f.bridge.diagnosticDelayMs=300;f.agent.localControl(R"({"type":"host.capabilities.ack","accepted":true,"diagnosticsProtocol":"host-diagnostics-v1"})");
    const auto began=std::chrono::steady_clock::now();f.agent.diagnosticsTestTick();f.agent.diagnosticsTestTick();
    require(std::chrono::steady_clock::now()-began<std::chrono::milliseconds(150),"A diagnostics snapshot held the send step");
    f.agent.diagnosticsTestJoin();require(f.bridge.diagnosticCalls==1,"Diagnostics ran twice at once or not at all");
    require(f.drain()&&f.sent.size()==1&&f.sent[0].get("type").string()=="host.diagnostics"&&f.sent[0].get("report").get("probe").number()==1,"The diagnostics report was not queued");
    f.agent.diagnosticsTestTick();f.agent.diagnosticsTestJoin();require(f.bridge.diagnosticCalls==1,"The schedule's rate limit was bypassed");++checks;}
  {Fixture f;f.agent.diagnosticsTestTick();f.agent.diagnosticsTestJoin();require(f.bridge.diagnosticCalls==0,"Diagnostics ran without the broker's acknowledgement");++checks;}
  // The broker's answer to a report reaches the delivery tracking (tuning #8).
  {Fixture f;f.agent.localControl(R"({"type":"host.diagnostics.ack","accepted":true})");f.agent.localControl(R"({"type":"host.diagnostics.ack","accepted":false})");
    require(f.bridge.acks==std::vector<bool>{true,false}&&f.drain()&&f.sent.empty(),"A diagnostics ACK was lost or answered");++checks;}
  {Fixture f;f.agent.announceCapabilities();f.bridge.primary=true;f.bridge.info();require(f.drain(),"Publication failed");auto info=std::find_if(f.sent.begin(),f.sent.end(),[](const auto& p){return p.get("type").string()=="stream.info";});require(info!=f.sent.begin()&&info!=f.sent.end()&&(info-1)->get("type").string()=="host.capabilities"&&primary(*(info-1)),"Actual Agent queue published stream.info before fresh profile capabilities");for(const auto& p:f.sent)if(p.get("type").string()=="host.capabilities")require(primary(p),"Queued announcement regressed fresh profile state");++checks;}
  {Fixture f;f.bridge.primary=true;f.bridge.info();require(f.drain()&&f.sent.size()==2&&f.sent[0].get("type").string()=="host.capabilities"&&f.sent[1].get("type").string()=="stream.info","Metadata without timer announcement skipped capability boundary");++checks;}
  {Fixture f;f.bridge.output(false,std::vector<uint8_t>(16));require(f.drain()&&f.binary==1&&f.bridge.reads==0&&f.sent.empty(),"Per-frame publication added capability/CPU work");++checks;}
  {Fixture f;f.bridge.throwRead=true;f.bridge.info();bool threw=false;try{f.drain();}catch(...){threw=true;}require(threw&&f.sent.empty(),"Capability read failure forwarded media metadata");++checks;}
  {Fixture f;f.bridge.info();require(!f.drain(true)&&f.sent.size()==1&&f.sent[0].get("type").string()=="host.capabilities","Failed capability write forwarded media metadata");++checks;}
  {Fixture f;f.bridge.text(R"({"type":"stream.unavailable","reason":"capture_failed"})");require(f.drain()&&f.sent.size()==1&&f.bridge.reads==0,"Nonmetadata packet acquired a readiness side effect");++checks;}
  {Fixture f;f.bridge.ready=false;f.bridge.primary=false;f.bridge.info();require(f.drain()&&!f.sent[0].get("capabilities").get("streaming").boolean()&&!primary(f.sent[0]),"Publication promoted unready/secondary capabilities");++checks;}
  {Fixture f;f.bridge.primary=true;f.bridge.info(1);require(f.drain(),"Initial publication failed");f.bridge.primary=false;f.agent.announceCapabilities();f.bridge.primary=true;f.bridge.info(2);require(f.drain()&&f.sent.size()==5&&f.sent.back().get("epoch").number()==2&&f.sent[3].get("type").string()=="host.capabilities","Resize metadata or late announcement crossed publication order");++checks;}
  // Lossless tiles opt-in: Session0 answers every ping as before and forwards
  // only a tiles-v1 prefixed nonce to the worker; ordinary pings forward nothing.
  {Fixture f;f.agent.localControl(R"({"type":"ping","id":"17"})");f.agent.localControl(R"({"type":"ping","id":"lt1-18"})");
    require(f.drain()&&f.sent.size()==2&&f.sent[0].get("id").string()=="17"&&f.sent[1].get("type").string()=="pong"&&f.sent[1].get("id").string()=="lt1-18","Ping/pong changed for the lossless opt-in");
    require(f.bridge.controls.size()==1&&f.bridge.controls[0].get("type").string()=="stream.lossless"&&f.bridge.controls[0].get("protocol").string()=="tiles-v1","Lossless opt-in was not forwarded exactly once");
    f.agent.localControl(R"({"type":"stream.lossless","protocol":"tiles-v1"})");require(f.bridge.controls.size()==1,"Session0 re-forwarded a worker-only command");
    // 게임 마우스 holds the mouse: an "lt0-" nonce is answered like any ping and
    // forwarded as the pause; the next "lt1-" opts in again.
    f.agent.localControl(R"({"type":"ping","id":"lt0-19"})");f.agent.localControl(R"({"type":"ping","id":"lt1-20"})");
    require(f.drain()&&f.sent.size()==4&&f.sent[2].get("type").string()=="pong"&&f.sent[2].get("id").string()=="lt0-19"&&f.sent[3].get("id").string()=="lt1-20","Ping/pong changed for the lossless pause");
    require(f.bridge.controls.size()==3&&f.bridge.controls[1].get("type").string()=="stream.lossless"&&f.bridge.controls[1].get("protocol").string()=="tiles-v1"&&
            std::holds_alternative<bool>(f.bridge.controls[1].get("on").value)&&!f.bridge.controls[1].get("on").boolean()&&std::holds_alternative<std::nullptr_t>(f.bridge.controls[2].get("on").value),"Lossless pause/resume was not forwarded in order");++checks;}
  // The worker that owns the encoder: the forwarded state (or a track
  // tunnel's own pings) turns tiles on and off for its bound session only.
  {RECT area{0,0,1280,720};host::Agent worker([](const std::wstring&){},[](bool,std::vector<uint8_t>){},[](INPUT&)->UINT{return 1U;},[area]{return area;});const std::string session="00000000-0000-0000-0000-000000000001";
    worker.inputLatencyTestStream(host::JsonParser::parse(R"({"type":"session.start","sessionId":"00000000-0000-0000-0000-000000000001"})"),area);
    require(!worker.losslessTestRequested(session),"Tiles without an opt-in");
    worker.localControl(R"({"type":"stream.lossless","protocol":"tiles-v1"})");require(worker.losslessTestRequested(session)&&!worker.losslessTestRequested("00000000-0000-0000-0000-000000000002"),"Worker opt-in not bound to its session");
    worker.localControl(R"({"type":"stream.lossless","protocol":"tiles-v1","on":false})");require(!worker.losslessTestRequested(session),"Worker kept tiles through a pause");
    worker.localControl(R"({"type":"ping","id":"lt1-3"})");require(worker.losslessTestRequested(session),"Track-tunnel opt-in did not resume tiles");
    worker.localControl(R"({"type":"ping","id":"lt0-4"})");require(!worker.losslessTestRequested(session),"Track-tunnel pause kept tiles");
    worker.localControl(R"({"type":"ping","id":"5"})");require(!worker.losslessTestRequested(session),"A plain ping changed the tiles state");
    worker.localControl(R"({"type":"stream.lossless","protocol":"tiles-v1","on":true})");require(worker.losslessTestRequested(session),"Explicit opt-in was not applied");++checks;}
  // Adaptive bitrate (bitrate-adaptive-v1) in the socket-owning Agent: the
  // relay's arrival feedback drives the controller and its target reaches the
  // bridge; stats are annotated; a fixed session is untouched.
  const std::string autoStart=R"({"type":"session.start","mode":"stream","sessionId":"00000000-0000-0000-0000-000000000001","resizeProtocol":"epoch-v1","epoch":0,"monitor":"0","bitrate":20000000,"bitrateMode":"auto","bitrateMax":100000000})";
  const auto video=[](uint64_t pts,size_t size){return host::mediaPacket(1,pts,std::vector<uint8_t>(size,0),false);};
  const auto feedback=[](uint64_t first,uint64_t count,uint64_t arrivalBase){std::string frames;for(uint64_t n=0;n<count;++n)frames+=(n?",":"")+std::string("[")+std::to_string(first+n)+","+std::to_string(arrivalBase+n*1000)+"]";
    return R"({"type":"stream.feedback","sessionId":"00000000-0000-0000-0000-000000000001","epoch":0,"bytes":100000,"intervalUs":100000,"relayQueuedBytes":0,"frames":[)"+frames+"]}";};
  // udp-media-v1: the worker's mark on a LOSS answer (flag bit 1) never leaves the Host.
  {Fixture f;const auto marked=host::mediaPacket(1,7,std::vector<uint8_t>(64,5),true,true);require(marked[1]==3,"mediaPacket did not mark the answer");
    f.bridge.output(false,marked);std::vector<uint8_t> seen;
    require(f.agent.publicationTestDrain([&](bool text,const std::vector<uint8_t>& bytes){if(!text)seen=bytes;return true;})&&seen.size()==marked.size()&&seen[1]==1&&std::equal(seen.begin()+2,seen.end(),marked.begin()+2),"The LOSS answer mark reached the broker channel, or the packet changed");++checks;}
  {Fixture f(autoStart);f.bridge.text(R"({"type":"stream.info","codec":"h264","width":1920,"height":1080,"sourceWidth":1920,"sourceHeight":1080,"fpsTarget":60,"sessionId":"00000000-0000-0000-0000-000000000001","epoch":0})");
    for(uint64_t pts=1;pts<=40;++pts)f.bridge.output(false,video(pts,1000));  // more than the fixed 32-packet limit
    require(f.drain()&&f.binary==40,"Adaptive session lost packets below its 128-packet queue limit");
    // Growing one-way delay over three intervals (no burst explains it).
    for(int k=0;k<3;++k)f.agent.localControl(feedback(1+uint64_t(k)*10,10,5000000+uint64_t(k)*200000));
    require(!f.bridge.rates.empty()&&f.bridge.rates.back()<20000000&&f.bridge.rates.back()>=host::adaptiveBitrateFloor(1920,1080,60),"Congestion feedback did not lower the adaptive target within the geometry floor");
    f.bridge.text(R"({"type":"stream.stats","capturedFps":60,"encodedFps":60,"bitrateKbps":19000,"bitrateTarget":20000000})");require(f.drain(),"Stats publication failed");
    const auto& stats=f.sent.back();require(stats.get("type").string()=="stream.stats"&&stats.get("networkDelayMs").number(-1)>=0&&stats.get("deliveredKbps").number()>0,"Adaptive stats were not annotated");
    f.agent.localControl(R"({"type":"stream.feedback","sessionId":"00000000-0000-0000-0000-000000000002","epoch":0,"bytes":1,"intervalUs":100000,"relayQueuedBytes":0,"frames":[]})");
    f.agent.localControl(R"({"type":"stream.bitrate","sessionId":"00000000-0000-0000-0000-000000000001","bitrate":90000000})");
    ++checks;}
  // A fixed session's backlog beyond the released 32-packet limit used to close
  // the whole broker channel (every session of the Host). It now drops the
  // stale video, asks the encoder owner for an IDR once, keeps control traffic
  // and resumes exactly at that IDR.
  {Fixture f;for(uint64_t pts=1;pts<=40;++pts)f.bridge.output(false,video(pts,1000));
    require(!f.agent.publicationTestClosed()&&f.agent.publicationTestAwaitingKeyFrame()&&f.bridge.keyFrames==1,"A media backlog closed the broker channel or did not ask for an IDR");
    f.bridge.text(R"({"type":"stream.stats","capturedFps":24,"encodedFps":24,"bitrateKbps":30000})");
    f.bridge.output(false,video(41,1000));
    f.bridge.output(false,host::mediaPacket(1,42,std::vector<uint8_t>(5000,0),true));
    f.bridge.output(false,video(43,1000));
    require(f.drain()&&f.binary==2&&f.sent.size()==1&&f.sent[0].get("type").string()=="stream.stats"&&!f.agent.publicationTestAwaitingKeyFrame()&&!f.agent.publicationTestClosed(),"The stream did not resume at the IDR with control traffic kept");
    ++checks;}
  // Every write leaves as WebSocket fragments of at most 8 KiB (a relay PING
  // answered inside a larger frame corrupted the channel: host_error).
  {Fixture f;f.bridge.output(false,video(1,100000));f.bridge.text(R"({"type":"stream.unavailable","reason":"capture_failed"})");
    std::vector<std::pair<WINHTTP_WEB_SOCKET_BUFFER_TYPE,DWORD>> calls;
    require(f.agent.publicationTestWrite([&](WINHTTP_WEB_SOCKET_BUFFER_TYPE type,const uint8_t*,DWORD size){calls.emplace_back(type,size);return DWORD(NO_ERROR);}),"Fragmented write path failed");
    require(calls.size()==14,"Unexpected number of WebSocket writes");
    size_t videoBytes=0;bool ordered=true;for(size_t n=0;n<13;++n){videoBytes+=calls[n].second;ordered=ordered&&calls[n].second<=host::webSocketFragmentBytes&&calls[n].first==(n<12?WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE:WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE);}
    require(ordered&&videoBytes==100012&&calls[13].first==WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,"A large media write was not split into 8 KiB fragments");
    bool failed=false;f.bridge.output(false,video(2,20000));
    require(!f.agent.publicationTestWrite([&](WINHTTP_WEB_SOCKET_BUFFER_TYPE,const uint8_t*,DWORD){failed=true;return DWORD(ERROR_WINHTTP_CONNECTION_ERROR);})&&failed,"A failed fragment was not reported as a failed send");
    ++checks;}
  // host-closing-v1: only a broker whose capabilities ACK names it gets the
  // reason of a deliberate close; queued media gives way to it.
  {Fixture f;require(!f.agent.publicationTestClosingNotice("service_stopping"),"A released broker was sent host.closing");
    f.agent.localControl(R"({"type":"host.capabilities.ack","accepted":true,"closingProtocol":"host-closing-v1"})");
    f.bridge.output(false,video(1,1000));
    require(f.agent.publicationTestClosingNotice("service_stopping")&&f.drain()&&f.binary==0&&f.sent.size()==1&&f.sent[0].get("type").string()=="host.closing"&&f.sent[0].get("reason").string()=="service_stopping","The closing notice was not queued ahead of media");
    f.agent.localControl(R"({"type":"host.capabilities.ack","accepted":true})");
    require(!f.agent.publicationTestClosingNotice("service_stopping"),"A later ACK without host-closing-v1 kept the notice");
    ++checks;}
  // host-resources-v2: the heartbeat's uptime only for a broker whose ACK names it.
  {Fixture f;require(!f.agent.publicationTestResourcesUptime(),"Uptime without any ACK");
    f.agent.localControl(R"({"type":"host.capabilities.ack","accepted":true,"closingProtocol":"host-closing-v1"})");
    require(!f.agent.publicationTestResourcesUptime(),"A released broker's ACK enabled uptime");
    f.agent.localControl(R"({"type":"host.capabilities.ack","accepted":false,"resourcesProtocol":"host-resources-v2"})");
    require(!f.agent.publicationTestResourcesUptime(),"A refused ACK enabled uptime");
    f.agent.localControl(R"({"type":"host.capabilities.ack","accepted":true,"resourcesProtocol":"host-resources-v2"})");
    require(f.agent.publicationTestResourcesUptime(),"The broker's host-resources-v2 was ignored");
    f.agent.localControl(R"({"type":"host.capabilities.ack","accepted":true})");
    require(!f.agent.publicationTestResourcesUptime(),"A later ACK without host-resources-v2 kept uptime");
    ++checks;}
  // Opted-in statistics are decorated on the actual service send path. Old
  // sessions retain their exact statistics bytes and gain no observation.
  {Fixture f;const std::string raw=R"({"type":"stream.stats","capturedFps":60,"encodedFps":60,"bitrateKbps":12000})";
    f.bridge.text(raw);require(f.drain()&&std::holds_alternative<std::nullptr_t>(f.sent.back().get("observability").value),"Default session gained deep telemetry");++checks;}
  {Fixture f(R"({"type":"session.start","mode":"stream","sessionId":"00000000-0000-0000-0000-000000000001","resizeProtocol":"epoch-v1","epoch":0,"monitor":"0","observabilityProtocol":"stream-observability-v1"})");
    f.bridge.text(R"({"type":"stream.stats","sessionId":"00000000-0000-0000-0000-000000000001","epoch":1,"encodedFps":60,"observability":{"protocol":"stream-observability-v1","metrics":{"workerCpuPct10":30,"readbackUs":15,"readbackMaxUs":20}}})");
    f.bridge.output(false,video(1,1000));require(f.drain(),"Deep statistics publication failed");const auto& metrics=f.sent.front().get("observability").get("metrics");
    require(metrics.get("hostQueuePackets").number(-1)==1&&metrics.get("workerCpuPct10").number(-1)==30&&metrics.get("readbackUs").number(-1)==15,"Service lost worker facts or counted wrong outgoing queue");
    require(f.sent.front().get("epoch").number()==1&&f.binary==1,"Telemetry changed epoch or media delivery");++checks;}
  // The worker's capture owner turns stream.keyframe of its own session into an IDR request.
  {RECT area{0,0,1280,720};host::Agent worker([](const std::wstring&){},[](bool,std::vector<uint8_t>){},[](INPUT&)->UINT{return 1U;},[area]{return area;});
    worker.inputLatencyTestStream(host::JsonParser::parse(R"({"type":"session.start","sessionId":"00000000-0000-0000-0000-000000000001"})"),area);
    worker.localControl(R"({"type":"stream.keyframe","sessionId":"00000000-0000-0000-0000-000000000002"})");
    require(!worker.publicationTestKeyFrameRequested(),"Another session's keyframe request reached this encoder");
    worker.localControl(R"({"type":"stream.keyframe","sessionId":"00000000-0000-0000-0000-000000000001"})");
    require(worker.publicationTestKeyFrameRequested(),"The worker ignored its keyframe request");
    // udp-media-v1 LOSS: the oldest last good PTS wins, another session's is ignored, junk too.
    worker.localControl(R"({"type":"stream.recover","sessionId":"00000000-0000-0000-0000-000000000002","pts":5})");
    require(!worker.publicationTestRecoverRequested(),"Another session's LOSS reached this encoder");
    for(const char* pts:{"-1","1.5","\"7\"","9007199254740992"})worker.localControl(std::string(R"({"type":"stream.recover","sessionId":"00000000-0000-0000-0000-000000000001","pts":)")+pts+"}");
    require(!worker.publicationTestRecoverRequested(),"A malformed LOSS reached the encoder");
    worker.localControl(R"({"type":"stream.recover","sessionId":"00000000-0000-0000-0000-000000000001","pts":900})");
    worker.localControl(R"({"type":"stream.recover","sessionId":"00000000-0000-0000-0000-000000000001","pts":700})");
    worker.localControl(R"({"type":"stream.recover","sessionId":"00000000-0000-0000-0000-000000000001","pts":800})");
    require(worker.publicationTestRecoverRequested()==701,"The worker did not keep the oldest last good frame");
    ++checks;}
  {Fixture f;f.bridge.output(false,video(1,1000));require(f.drain(),"Fixed publication failed");
    for(int k=0;k<3;++k)f.agent.localControl(feedback(1,1,5000000+uint64_t(k)*200000));
    f.bridge.text(R"({"type":"stream.stats","capturedFps":60,"encodedFps":60,"bitrateKbps":19000})");require(f.drain()&&f.bridge.rates.empty()&&f.sent.back().get("networkDelayMs").number(-1)<0,"A fixed session reacted to feedback or was annotated");++checks;}
  // udp-media-v1 Phase 2: a grant starts one leg in the service and changes nothing
  // else. The relay address is a closed local port: only the leg's lifetime matters.
  {const auto grant=[](const std::string& extra=""){return R"({"type":"udp.grant","protocol":"udp-media-v1","relays":[{"host":"127.0.0.1","port":9}],"id":"oKGio6Slpqc=","ticket":"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=","expiresAt":1793520000000)"+extra+"}";};
    const auto unmoved=[&](Fixture& f,const char* reason){f.bridge.text(R"({"type":"stream.stats","encodedFps":60})");f.bridge.output(false,video(1,1000));const auto sent=f.sent.size();require(f.drain()&&f.sent.size()==sent+1&&f.binary==1&&f.bridge.stops==1&&!f.agent.publicationTestClosed(),reason);f.binary=0;};
    {Fixture f;require(!f.agent.publicationTestUdpLeg(),"A leg existed with no grant");
      for(const auto* bad:{R"({"type":"udp.grant"})",R"({"type":"udp.grant","protocol":"udp-media-v2","relays":[]})",R"({"type":"udp.grant","protocol":"udp-media-v1","relays":[{"host":"127.0.0.1","port":0}],"id":"oKGio6Slpqc=","ticket":"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=","expiresAt":1})",R"({"type":"udp.grant","relays":7})"}){f.agent.localControl(bad);require(!f.agent.publicationTestUdpLeg(),"A malformed grant started a leg");}
      unmoved(f,"A malformed grant disturbed the stream");++checks;}
    {Bridge bridge;host::Agent silent([](const std::wstring&){},&bridge);silent.localControl(grant());require(!silent.publicationTestUdpLeg(),"A grant with no stream started a leg");++checks;}
    {Fixture f;f.agent.localControl(grant(R"(,"sessionId":"00000000-0000-0000-0000-000000000002")"));require(!f.agent.publicationTestUdpLeg(),"Another session's grant started a leg");++checks;}
    {RECT area{0,0,1280,720};host::Agent worker([](const std::wstring&){},[](bool,std::vector<uint8_t>){},[](INPUT&)->UINT{return 1U;},[area]{return area;});
      worker.inputLatencyTestStream(host::JsonParser::parse(R"({"type":"session.start","sessionId":"00000000-0000-0000-0000-000000000001"})"),area);
      worker.localControl(grant());require(!worker.publicationTestUdpLeg(),"The interactive worker started a leg");++checks;}
    {Fixture f;require(f.drain(),"Publication failed");const auto sent=f.sent.size();
      f.agent.localControl(grant(R"(,"sessionId":"00000000-0000-0000-0000-000000000001")"));const auto first=f.agent.publicationTestUdpLeg();require(bool(first),"A valid grant started no leg");
      require(f.drain()&&f.sent.size()==sent&&f.bridge.controls.empty()&&f.bridge.starts==1,"A grant reached the channel or the worker");
      unmoved(f,"A running leg disturbed the stream");++checks;
      f.agent.localControl(R"({"type":"udp.grant","protocol":"udp-media-v2"})");require(f.agent.publicationTestUdpLeg()==first&&!first->waitClosed(0),"A malformed grant replaced the running leg");++checks;
      f.agent.localControl(grant());const auto second=f.agent.publicationTestUdpLeg();require(second&&second!=first&&first->waitClosed(2000)&&!second->waitClosed(0),"A new grant did not replace the old leg");++checks;
      unmoved(f,"Replacing the leg disturbed the stream");
      f.agent.localControl(R"({"type":"session.stop"})");require(!f.agent.publicationTestUdpLeg()&&second->waitClosed(2000),"The session's end left its leg running");++checks;}
    {Fixture f;f.agent.localControl(grant());const auto leg=f.agent.publicationTestUdpLeg();require(bool(leg),"A valid grant started no leg");f.agent.localControl(R"({"type":"session.start","mode":"stream","sessionId":"00000000-0000-0000-0000-000000000002","resizeProtocol":"epoch-v1","epoch":0,"monitor":"0"})");require(!f.agent.publicationTestUdpLeg()&&leg->waitClosed(2000),"A new session kept the old session's leg");++checks;}
    {Fixture f;f.agent.localControl(grant());const auto leg=f.agent.publicationTestUdpLeg();require(bool(leg),"A valid grant started no leg");f.agent.stop();require(!f.agent.publicationTestUdpLeg()&&leg->waitClosed(2000),"Stopping the service left the leg running");++checks;}}
  // #98 and audio-stats-v1 on the service's WebSocket path: audio queued behind
  // video leaves first; the next stream.stats carries the audio sent, dropped
  // and its wait in the send queue; a window without audio carries nothing.
  {Fixture f;f.bridge.info(0);require(f.drain(),"Publication failed");
    const auto audio=[](uint64_t pts){return host::mediaPacket(2,pts,std::vector<uint8_t>(1920,0),false);};
    f.bridge.output(false,video(1,30000));f.bridge.output(false,video(2,30000));f.bridge.output(false,audio(3));f.bridge.output(false,audio(4));
    std::vector<int> kinds;require(f.agent.publicationTestDrain([&](bool text,const std::vector<uint8_t>& bytes){kinds.push_back(text?0:bytes[0]);return true;}),"Drain failed");
    require((kinds==std::vector<int>{2,2,1,1}),"Audio waited behind queued video on the broker channel");
    const auto stats=[&]{f.bridge.text(R"({"type":"stream.stats","capturedFps":60,"encodedFps":60,"bitrateKbps":12000})");require(f.drain(),"Stats publication failed");return f.sent.back().get("audioSend");};
    auto send=stats();
    require(send.get("sent").number(-1)==2&&send.get("dropped").number(-1)==0&&send.get("queuePackets").number(-1)==2&&send.get("queueAvgUs").number(-1)>=0&&send.get("queueMaxUs").number(-1)>=send.get("queueAvgUs").number(-1),"stream.stats did not carry the audio send window");
    require(std::holds_alternative<std::nullptr_t>(stats().value),"A window without audio carried the member");
    for(uint64_t pts=10;pts<43;++pts)f.bridge.output(false,audio(pts));  // one past the fixed 32-packet limit
    require(f.drain(),"Audio publication failed");send=stats();
    require(send.get("sent").number(-1)==32&&send.get("dropped").number(-1)==1,"A dropped audio packet was not counted");
    ++checks;}
  // host-resume-v1: a lost channel keeps the granted stream (its worker is not
  // stopped); the next channel offers it once and the Broker's answer continues
  // it at an IDR. Refused channels and Brokers without the protocol stop it.
  {const std::string ack=R"({"type":"host.capabilities.ack","accepted":true,"hostResumeProtocol":"host-resume-v1"})",released=R"({"type":"host.capabilities.ack","accepted":true})";
    const auto offer=[](const std::vector<host::Json>& sent){for(const auto& message:sent)if(message.get("type").string()=="host.session.resume"){std::vector<std::string> ids;for(const auto& id:std::get<host::Json::Array>(message.get("sessions").value))ids.push_back(id.string());return ids;}throw std::runtime_error("No host.session.resume was sent");};
    {Fixture f;f.agent.localControl(ack);require(f.drain()&&offer(f.sent).empty(),"A channel without a kept stream offered one");
      const int stops=f.bridge.stops;f.sent.clear();
      f.agent.publicationTestChannelLost("transport_closed");
      require(f.bridge.stops==stops,"A lost channel stopped the granted stream");
      f.bridge.output(false,std::vector<uint8_t>(16));require(f.drain()&&f.binary==0,"Media of the lost channel waited for the next one");
      f.agent.publicationTestNewChannel();f.agent.localControl(ack);
      require(f.drain()&&offer(f.sent)==std::vector<std::string>{"00000000-0000-0000-0000-000000000001"},"The next channel did not offer the kept stream");
      f.sent.clear();f.agent.localControl(ack);require(f.drain()&&f.sent.empty(),"The kept stream was offered twice on one channel");
      const int keys=f.bridge.keyFrames;
      f.agent.localControl(R"({"type":"host.session.resumed","sessionId":"00000000-0000-0000-0000-000000000002"})");require(f.bridge.keyFrames==keys,"Another session's answer resumed the stream");
      f.agent.localControl(R"({"type":"host.session.resumed","sessionId":"00000000-0000-0000-0000-000000000001"})");
      require(f.bridge.keyFrames==keys+1&&f.bridge.stops==stops,"The resumed stream did not restart at an IDR");
      f.bridge.output(false,std::vector<uint8_t>(16));require(f.drain()&&f.binary==1,"Media did not continue on the new channel");++checks;
      f.agent.publicationTestChannelLost("transport_closed");f.agent.publicationTestNewChannel();f.agent.localControl(ack);
      f.agent.localControl(R"({"type":"session.stop","sessionId":"00000000-0000-0000-0000-000000000001","reason":"resume_refused"})");
      require(f.bridge.stops==stops+1,"A refused resume kept the stream");++checks;}
    {Fixture f;f.agent.localControl(ack);const int stops=f.bridge.stops;f.agent.publicationTestChannelLost("protocol_rejected");require(f.bridge.stops==stops+1,"A refused channel kept the stream");++checks;}
    {Fixture f;f.agent.localControl(released);const int stops=f.bridge.stops;f.agent.publicationTestChannelLost("transport_closed");require(f.bridge.stops==stops+1,"A released Broker's stream was kept");++checks;}
    {Fixture f;f.agent.localControl(ack);const int stops=f.bridge.stops;f.agent.publicationTestChannelLost("transport_closed");f.agent.publicationTestNewChannel();
      f.agent.localControl(released);require(f.bridge.stops==stops+1,"A Broker without the protocol left the stream kept");++checks;}}
  // Restart recovery (broker_reconnect.hpp): run()'s wait after a channel end.
  // A broker restart (1012 close, or a released broker's session.stop
  // "server_stopping" before it drops the channel) is dialled again after about
  // 250 ms, 500 ms, then 1 s; any other end keeps the released 2 s.
  {const auto within=[](uint32_t wait,uint32_t base){return wait>=base*4/5&&wait<=base*6/5;};
    {Fixture f;require(f.agent.publicationTestChannelEndWait(0)==2000&&f.agent.publicationTestChannelEndWait(1006)==2000&&f.agent.publicationTestChannelEndWait(1000)==2000,"An ordinary end did not keep the released 2 s");++checks;}
    {Fixture f;f.agent.localControl(R"({"type":"session.stop","sessionId":"00000000-0000-0000-0000-000000000001","reason":"client_stopped"})");
      require(f.agent.publicationTestChannelEndWait(0)==2000,"A client's stop was taken for a broker restart");++checks;}
    {Fixture f;f.agent.localControl(R"({"type":"session.stop","sessionId":"00000000-0000-0000-0000-000000000001","reason":"server_stopping"})");
      const auto first=f.agent.publicationTestChannelEndWait(0),second=f.agent.publicationTestChannelEndWait(0),third=f.agent.publicationTestChannelEndWait(0);
      require(within(first,250)&&within(second,500)&&within(third,1000),"A released broker's restart was not dialled again at once");
      f.agent.publicationTestDialed();require(f.agent.publicationTestChannelEndWait(0)==2000,"The restart steps outlived the channel that came back");++checks;}
    {Fixture f;require(within(f.agent.publicationTestChannelEndWait(1012),250),"A 1012 Service Restart close was not dialled again at once");
      uint32_t total=0;int dials=0;for(uint32_t wait=0;(wait=f.agent.publicationTestChannelEndWait(0))!=2000;++dials)total+=wait;
      require(dials==int(host::BrokerReconnectWait::restartMs.size())-1&&total<=7000,"The restart steps were not capped");++checks;}}
  // boot-ready-v1: only a capabilities report with "streaming":true that was
  // actually written marks the boot timeline's ready stage, once.
  {Fixture f;host::BootTimeline boot(GetTickCount64());f.agent.publicationTestBoot(boot);
    f.bridge.ready=false;f.agent.announceCapabilities();require(f.drain()&&f.sent.size()==1&&!boot.reached(host::BootStage::ready),"A not-ready report marked the desktop ready");
    f.bridge.ready=true;f.agent.announceCapabilities();require(!f.drain(true)&&!boot.reached(host::BootStage::ready),"A report that was never written marked the desktop ready");
    f.agent.announceCapabilities();require(f.drain()&&f.sent.back().get("capabilities").get("streaming").boolean()&&boot.reached(host::BootStage::ready),"The first written ready report did not mark the boot timeline");
    const auto record=boot.take();require(record&&record->ready&&record->at[size_t(host::BootStage::ready)]>=0,"The ready report produced no boot record");
    ++checks;}
  // A heartbeat sent early for a CPU/memory change repeats the newest report the
  // channel wrote instead of reading the capabilities again (70-96 service
  // wakeups a read); never a report older than an announcement or a result
  // that carried its own.
  {Fixture f;const int before=f.bridge.reads;const auto first=f.agent.publicationTestHeartbeatCapabilities();require(f.bridge.reads==before+1,"A channel's first heartbeat did not read the capabilities");
    for(int i=0;i<5;++i)require(f.agent.publicationTestHeartbeatCapabilities()==first,"An early heartbeat changed the repeated report");
    require(f.bridge.reads==before+1,"Early heartbeats read the capabilities again");++checks;
    f.bridge.ready=false;f.agent.announceCapabilities();require(f.drain()&&f.bridge.reads==before+2,"Announcement fixture failed");
    require(!host::JsonParser::parse(f.agent.publicationTestHeartbeatCapabilities()).get("streaming").boolean()&&f.bridge.reads==before+2,"A heartbeat repeated a report older than the announcement");++checks;
    f.bridge.ready=true;f.bridge.text(R"({"type":"host.display.result","operationId":"00000000-0000-0000-0000-000000000003","ok":true,"count":1,"capabilities":{"service":true,"streaming":true}})");require(f.drain(),"Display result fixture failed");
    require(host::JsonParser::parse(f.agent.publicationTestHeartbeatCapabilities()).get("streaming").boolean()&&f.bridge.reads==before+3,"A heartbeat after a result with its own report repeated an older one");++checks;}
  std::cout<<"{\"actualAgentQueuePublicationCases\":"<<checks<<",\"transport\":\"recorded sink\",\"capabilities\":\"injected bridge\",\"perVideoFrameCapabilityReads\":0,\"windowsDisplayChanges\":0,\"actualNetwork\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
