#include "udp_tracks_session.hpp"
#include <iostream>
#include <stdexcept>
using namespace host;
static void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
static bool until(const std::function<bool()>& test){for(int n=0;n<1000;++n){if(test())return true;Sleep(1);}return test();}
int main(){try{
  unsigned checks=0;const UdpTrackScope primary{"0",{0,7,11,true},1,12000000},secondary{"1",{1,7,12,false},1,12000000};
  const auto key=JsonParser::parse(R"({"type":"input","event":"key","vk":65,"scan":30,"down":true,"extended":false,"topologyEpoch":7})");
  require(validDirectTrackInput(key,primary,false)&&!validDirectTrackInput(key,secondary,false),"global input ownership");++checks;
  const auto move=JsonParser::parse(R"({"type":"input","event":"move","x":0.25,"y":0.5,"track":"1","epoch":12,"topologyEpoch":7})");
  require(validDirectTrackInput(move,secondary,false)&&!validDirectTrackInput(move,primary,false),"pointer crossed output");++checks;
  const auto relative=JsonParser::parse(R"({"type":"input","event":"moveRelative","dx":3,"dy":-2,"topologyEpoch":7})");
  require(validDirectTrackInput(relative,primary,true)&&!validDirectTrackInput(relative,primary,false)&&!validDirectTrackInput(relative,secondary,true),"relative input grant");++checks;
  auto old=secondary;old.binding.epoch=13;require(!validDirectTrackInput(move,old,false),"stale input epoch");old=secondary;old.binding.topology=8;require(!validDirectTrackInput(move,old,false),"stale input topology");checks+=2;
  require(!validDirectTrackInput(JsonParser::parse(R"({"type":"host.power.request","action":"shutdown"})"),primary,true),"privileged command admitted");++checks;
  require(udpTrackScopeMatches(JsonParser::parse(R"({"track":"0","epoch":11,"topologyEpoch":7,"generation":1})"),primary)&&!udpTrackScopeMatches(JsonParser::parse(R"({"track":"0","epoch":11,"topologyEpoch":7,"generation":2})"),primary),"key/candidate generation scope");++checks;
  std::mutex mutex;std::vector<Json> injected;
  UdpTracksSession session("session",{{},[&](const Json& value){std::lock_guard lock(mutex);injected.push_back(value);},{}});
  const auto count=[&]{std::lock_guard lock(mutex);return injected.size();};
  const auto packet=[&](uint32_t sequence,bool down){return JsonParser::parse("{\"type\":\"input.direct\",\"protocol\":\"udp-direct-v1\",\"sessionId\":\"session\",\"sequence\":"+std::to_string(sequence)+",\"epoch\":11,\"input\":{\"type\":\"input\",\"event\":\"key\",\"vk\":65,\"scan\":30,\"down\":"+(down?"true":"false")+",\"extended\":false,\"topologyEpoch\":7}}");};
  session.control(packet(1,true));session.control(packet(1,true));require(until([&]{return count()==1;}),"reliable copy did not inject exactly once");++checks;
  session.suspend();session.control(packet(1,true));session.control(packet(2,false));require(until([&]{return count()==2;}),"sequence reset after regrant/loss");++checks;
  session.control(JsonParser::parse(R"({"type":"input.direct","protocol":"udp-direct-v1","sessionId":"session","sequence":3,"epoch":11,"input":{"type":"input.fence"}})"));
  session.control(packet(4,true));require(until([&]{return count()==3;}),"fence injected an action");++checks;
  auto foreign=packet(5,true);std::get<Json::Object>(foreign.value)["sessionId"]=Json{std::string("other")};session.control(foreign);session.close();session.control(packet(6,true));require(count()==3,"retired/foreign owner injected");++checks;
  {std::lock_guard lock(mutex);require(injected[0].get("sessionId").string()=="session"&&injected[0].get("down").boolean()&&!injected[1].get("down").boolean(),"ordered pipe owner or key direction");}++checks;
  {
    std::mutex resetMutex;std::vector<Json> actions;
    UdpTracksSession retired("retired",{{},[&](const Json& value){std::lock_guard lock(resetMutex);actions.push_back(value);},{}});
    const auto actionCount=[&]{std::lock_guard lock(resetMutex);return actions.size();};
    const auto reliable=[&](uint32_t sequence,uint16_t epoch,const std::string& body){return retired.control(JsonParser::parse("{\"type\":\"input.direct\",\"protocol\":\"udp-direct-v1\",\"sessionId\":\"retired\",\"sequence\":"+std::to_string(sequence)+",\"epoch\":"+std::to_string(epoch)+",\"input\":"+body+'}'));};
    retired.suspend();
    const std::string metadata=R"({"type":"stream.tracks.info","sessionId":"retired","topologyEpoch":8,"audio":{"format":"s16le"},"tracks":[{"id":"0","epoch":21,"primary":true,"bitrateTarget":12000000}]})";
    std::vector<uint8_t> bytes(metadata.begin(),metadata.end());require(!retired.outgoing(true,bytes),"metadata left reliable tunnel");
    require(reliable(1,11,R"({"type":"input.reset"})")&&reliable(2,11,R"({"type":"input.fence"})"),"retired epoch reset/fence was refused");++checks;
    const std::string current=R"({"type":"input","event":"move","x":0.2,"y":0.7,"track":"0","topologyEpoch":8,"epoch":21})";
    require(reliable(3,21,current)&&until([&]{return actionCount()==2;}),"new pointer did not follow old-epoch reset/fence");++checks;
    reliable(1,11,R"({"type":"input.reset"})");reliable(3,21,current);retired.close();
    {std::lock_guard lock(resetMutex);require(actions.size()==2&&actions[0].get("type").string()=="input.reset"&&actions[1].get("event").string()=="move"&&actions[1].get("epoch").number()==21,"fence injected an action or retired duplicate repeated");}++checks;
  }
  std::cout<<"{\"kind\":\"udp-tracks-input\",\"checks\":"<<checks<<",\"oldEpochResetFence\":true,\"realInput\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
