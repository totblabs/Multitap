// relative-mouse-v1 on the host: the INPUT of a moveRelative message, its bounds
// and refusals, the session gate on the actual worker Agent path, independent
// outputs, sampling and Session0 forwarding. Absolute packets of a granted
// session stay exactly as they were. The injector is substituted: no OS input.
#include "agent.hpp"
#include "track_input.hpp"
#include <iostream>
using namespace host;
namespace {
unsigned checks=0;
void require(bool condition,const char* reason){++checks;if(!condition)throw std::runtime_error(reason);}
const std::string sessionKey="00000000-0000-4000-8000-000000000091";
const RECT screen{0,0,1920,1080};
std::string start(const std::string& extra){
 return "{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\""+sessionKey+"\",\"resizeProtocol\":\"epoch-v1\",\"epoch\":0,\"monitor\":\"0\""+extra+'}';
}
const std::string granted=",\"relativeMouseProtocol\":\"relative-mouse-v1\"";
std::string relative(int dx,int dy,const std::string& extra=""){
 return "{\"type\":\"input\",\"event\":\"moveRelative\",\"dx\":"+std::to_string(dx)+",\"dy\":"+std::to_string(dy)+extra+'}';
}
RemoteInput::Injector record(std::vector<INPUT>& calls){return [&calls](INPUT& value)->UINT{calls.push_back(value);return 1U;};}
void injectsWithoutAbsolute(){
 std::vector<INPUT> calls;RemoteInput input(record(calls),false,[]{return screen;});input.area(screen);
 const std::pair<int,int> moves[]={{12,-3},{-1,0},{0,1},{16384,-16384},{-16384,16384}};
 size_t count=0;
 for(const auto [dx,dy]:moves){
  require(input.apply(JsonParser::parse(relative(dx,dy)))&&calls.size()==++count,"A message was not exactly one INPUT");
  const auto& mi=calls.back().mi;
  require(calls.back().type==INPUT_MOUSE&&mi.dwFlags==MOUSEEVENTF_MOVE&&mi.dx==dx&&mi.dy==dy&&mi.mouseData==0,"Movement carried other flags or changed its delta");
  require(!(mi.dwFlags&(MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK)),"Movement was injected as a position");
 }
 require(input.pending()==0,"Movement held something");
}
void noGeometryLookup(){
 std::vector<INPUT> calls;unsigned queries=0;RemoteInput input(record(calls),false,[&]{++queries;return RECT{};});input.area(screen);
 require(input.apply(JsonParser::parse(relative(5,5)))&&queries==0,"Movement looked up the desktop");
}
void boundsAndMalformed(){
 std::vector<INPUT> calls;RemoteInput input(record(calls),false,[]{return screen;});input.area(screen);
 for(const auto* bad:{
   R"({"type":"input","event":"moveRelative","dx":0,"dy":0})",R"({"type":"input","event":"moveRelative","dx":-0,"dy":0})",
   R"({"type":"input","event":"moveRelative","dx":16385,"dy":0})",R"({"type":"input","event":"moveRelative","dx":0,"dy":16385})",
   R"({"type":"input","event":"moveRelative","dx":-16385,"dy":1})",R"({"type":"input","event":"moveRelative","dx":1,"dy":-16385})",
   R"({"type":"input","event":"moveRelative","dx":1e300,"dy":1})",
   R"({"type":"input","event":"moveRelative","dx":1.5,"dy":0})",R"({"type":"input","event":"moveRelative","dx":0,"dy":0.5})",
   R"({"type":"input","event":"moveRelative","dx":1})",R"({"type":"input","event":"moveRelative","dy":1})",R"({"type":"input","event":"moveRelative"})",
   R"({"type":"input","event":"moveRelative","dx":"1","dy":1})",R"({"type":"input","event":"moveRelative","dx":1,"dy":true})",R"({"type":"input","event":"moveRelative","dx":null,"dy":1})",
   R"({"type":"input","event":"moveRelative","dx":[1],"dy":1})",
   R"({"type":"input","event":"moveRelative","dx":1,"dy":1,"x":0.5,"y":0.5})",R"({"type":"input","event":"moveRelative","dx":1,"dy":1,"x":0.5})",
   R"([1,2])"})
  require(!input.apply(JsonParser::parse(bad))&&calls.empty(),"A malformed movement reached the injector");
}
void needsBoundSource(){
 std::vector<INPUT> calls;RemoteInput input(record(calls),false,[]{return screen;});
 require(!input.apply(JsonParser::parse(relative(1,1)))&&calls.empty(),"Movement without a bound source was injected");
 input.area({0,0,1,10});require(!input.apply(JsonParser::parse(relative(1,1)))&&calls.empty(),"A one-pixel source accepted movement");
}
void reportsRefusedInjection(){
 unsigned calls=0;RemoteInput input([&](INPUT&)->UINT{++calls;return 0U;},false,[]{return screen;});input.area(screen);
 require(!input.apply(JsonParser::parse(relative(1,1)))&&calls==1,"A refused injection was reported as applied");
}
void absoluteUnchanged(){
 std::vector<INPUT> calls;RemoteInput input(record(calls),false,[]{return screen;});input.area(screen);
 require(input.apply(JsonParser::parse(R"({"type":"input","event":"move","x":0.5,"y":0.5})")),"Absolute movement failed");
 require(calls.back().mi.dwFlags==(MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK),"Absolute movement changed its flags");
 // The buttons and wheel of a captured mouse act at the current cursor.
 require(input.apply(JsonParser::parse(R"({"type":"input","event":"button","button":"left","down":true})"))&&calls.back().mi.dwFlags==MOUSEEVENTF_LEFTDOWN&&input.pending()==1,"A bare press moved the cursor or was not tracked");
 require(input.apply(JsonParser::parse(relative(3,4))),"Movement with a held button failed");
 require(input.pending()==1,"Movement changed a held button");
 require(input.apply(JsonParser::parse(R"({"type":"input","event":"button","button":"left","down":false})"))&&calls.back().mi.dwFlags==MOUSEEVENTF_LEFTUP&&input.pending()==0,"A bare release moved the cursor");
 require(input.apply(JsonParser::parse(R"({"type":"input","event":"wheel","delta":-120})"))&&calls.back().mi.dwFlags==MOUSEEVENTF_WHEEL,"A bare wheel step moved the cursor");
}
struct Worker{std::vector<INPUT> calls;std::vector<std::string> texts;};
Worker runWorker(const std::string& startExtra,const std::vector<std::string>& inputs){
 Worker log;std::mutex mutex;
 {
  Agent agent([](const std::wstring&){},[&](bool text,std::vector<uint8_t> bytes){std::lock_guard lock(mutex);if(text)log.texts.emplace_back(bytes.begin(),bytes.end());},
              [&](INPUT& value)->UINT{std::lock_guard lock(mutex);log.calls.push_back(value);return 1U;},[]{return screen;});
  agent.inputLatencyTestStream(JsonParser::parse(start(startExtra)),screen);
  for(const auto& input:inputs)agent.localControl(input);
  agent.localControl("{\"type\":\"session.stop\",\"sessionId\":\""+sessionKey+"\"}");
 }
 return log;
}
void sessionGate(){
 const auto move=relative(7,-2);
 require(relativeMouseSessionRequested(JsonParser::parse(start(granted))),"A granted start was not recognized");
 require(runWorker(granted,{move}).calls.size()==1,"A granted session did not inject");
 require(runWorker("",{move}).calls.empty(),"An old Broker's session injected movement");
 require(runWorker(",\"relativeMouseProtocol\":\"relative-mouse-v2\"",{move}).calls.empty(),"Another protocol value enabled movement");
 require(runWorker(",\"relativeMouseProtocol\":true",{move}).calls.empty(),"A non-string marker enabled movement");
 require(runWorker(granted+",\"mediaProtocol\":\"tracks-v1\"",{move}).calls.empty(),"Independent outputs injected movement");
 require(!relativeMouseSessionRequested(JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"rdp\",\"sessionId\":\"x\""+granted+'}')),"An RDP session was granted movement");
 require(runWorker(granted,{"{\"type\":\"input\",\"event\":\"moveRelative\",\"dx\":1,\"dy\":1,\"sessionId\":\"another\"}"}).calls.empty(),"Another session's movement was injected");
 const auto bad=runWorker(granted,{relative(0,0),relative(16385,0)});
 require(bad.calls.empty(),"A granted session injected malformed movement");
 // A session without the grant keeps its ordinary pointer input.
 const auto plain=runWorker("",{R"({"type":"input","event":"move","x":0.5,"y":0.5})"});
 require(plain.calls.size()==1&&(plain.calls[0].mi.dwFlags&MOUSEEVENTF_ABSOLUTE),"Ordinary movement changed");
}
void independentOutputsRefuse(){
 TrackSourcePlan plan;plan.sessionId=sessionKey;plan.topology=1;
 TrackSourceInput input(plan);input.encodedReady();
 require(input.acknowledge(JsonParser::parse("{\"type\":\"stream.tracks.ready\",\"sessionId\":\""+sessionKey+"\",\"topologyEpoch\":1,\"tracks\":[]}")),"Independent input did not become ready");
 const std::string head="{\"type\":\"input\",\"sessionId\":\""+sessionKey+"\",\"topologyEpoch\":1,";unsigned injected=0;const auto receive=[&](const Json&){++injected;};
 require(input.apply(JsonParser::parse(head+R"("event":"key","vk":65,"scan":30,"down":true,"extended":false})"),receive)&&injected==1,"A key of the ready source was refused");
 require(!input.apply(JsonParser::parse(head+R"("event":"moveRelative","dx":1,"dy":1})"),receive)&&injected==1,"An independent output injected movement");
}
void sampling(){
 const auto id=",\"sample\":1,\"latencyReceivedUs\":"+std::to_string(hostMonotonicUs());
 const auto log=runWorker(granted+",\"inputLatencyProtocol\":\"input-latency-v1\",\"cursorProtocol\":\"cursor-v1\"",{relative(1,1,id)});
 const auto report=std::find_if(log.texts.begin(),log.texts.end(),[](const std::string& text){return JsonParser::parse(text).get("sample").number()==1;});
 require(log.calls.size()==1&&report!=log.texts.end()&&JsonParser::parse(*report).get("ok").boolean(),"A sampled movement was not reported like a move");
 require(inputFrameCandidate(JsonParser::parse(relative(1,1)),true,false),"Movement of a hidden cursor names no frame");
 require(!inputFrameInterrupts(JsonParser::parse(relative(1,1))),"Movement dropped a pending candidate");
}
struct Bridge final:AgentBridge{
 std::vector<Json> controls;
 void start(const Json&,Send)override{}
 void control(const Json& value)override{controls.push_back(value);}
 void stop()override{}
 std::string capabilities()override{return "{\"service\":true}";}
 void displayControl(const Json&,Send)override{}
};
void serviceForwarding(){
 Bridge bridge;Agent agent([](const std::wstring&){},&bridge);agent.publicationTestOpen();
 agent.localControl(start(granted));agent.localControl(relative(9,-8));
 require(bridge.controls.size()==1&&bridge.controls[0].get("event").string()=="moveRelative"&&bridge.controls[0].get("dx").number()==9&&bridge.controls[0].get("dy").number()==-8,"The service changed or dropped movement before its worker");
}
}
int main(){
 const struct{const char* name;void(*run)();}cases[]={{"injects-without-absolute",injectsWithoutAbsolute},{"no-geometry-lookup",noGeometryLookup},{"bounds-and-malformed",boundsAndMalformed},{"needs-bound-source",needsBoundSource},{"reports-refused-injection",reportsRefusedInjection},{"absolute-unchanged",absoluteUnchanged},{"session-gate",sessionGate},{"independent-outputs-refuse",independentOutputsRefuse},{"sampling",sampling},{"service-forwarding",serviceForwarding}};
 unsigned passed=0,failed=0;
 for(const auto& value:cases)try{value.run();++passed;}catch(const std::exception& error){++failed;std::cerr<<value.name<<": "<<error.what()<<'\n';}
 std::cout<<"{\"kind\":\"relative-mouse-v1\",\"passed\":"<<passed<<",\"failed\":"<<failed<<",\"checks\":"<<checks<<",\"productionAgent\":true,\"injector\":\"substituted\",\"osInputEmitted\":false}\n";return failed?1:0;
}
