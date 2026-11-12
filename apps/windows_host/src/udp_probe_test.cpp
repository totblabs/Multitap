// udp-media-v1 Phase 2 rules: no socket and no real clock anywhere here. The expected
// datagrams were produced by the relay's reference, server/src/media-udp.mjs.
#include "udp_probe.hpp"
#include <iostream>
#include <map>
using namespace host;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
std::vector<uint8_t> hex(std::string_view text){std::vector<uint8_t> out;for(size_t i=0;i+1<text.size();i+=2)out.push_back(uint8_t(std::stoi(std::string(text.substr(i,2)),nullptr,16)));return out;}
bool same(std::span<const uint8_t> a,std::span<const uint8_t> b){return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin());}
const std::string ticketText="AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=",idText="oKGio6Slpqc=";
const auto bindBytes=hex("01a0a1a2a3a4a5a6a759844e012ec1d19ae7d67621889f47182174d56f661a744270183d3722f9a729");
const auto ackBytes=hex("02a0a1a2a3a4a5a6a723c5c7aef506575f37452a7656101e4d1c0d39f57c93688abe744d0c11e48303");
const auto pingBytes=hex("0301020304050607080100000000000007000000000001e240");
const std::string goodRelays=R"([{"host":"192.0.2.121","port":18481},{"host":"vdi.example.com","port":18481}])";
std::string grantText(const std::string& relays=goodRelays,const std::string& id="\""+idText+"\"",const std::string& ticket="\""+ticketText+"\"",const std::string& expires="1793520000000",const std::string& protocol="\"udp-media-v1\""){
  return R"({"type":"udp.grant","protocol":)"+protocol+R"(,"relays":)"+relays+R"(,"id":)"+id+R"(,"ticket":)"+ticket+R"(,"expiresAt":)"+expires+"}";
}
std::optional<UdpGrant> parsed(const std::string& text){return parseUdpGrant(JsonParser::parse(text),text.size());}
UdpGrant goodGrant(){return *parsed(grantText());}
UdpLegDatagrams goodDatagrams(){UdpLegDatagrams out;require(udpLegDatagrams(goodGrant(),out),"BCrypt HMAC failed");return out;}
// The fake network: what the probe asked for, in order, against a test clock.
struct Net {
  struct Sent{size_t relay;std::vector<uint8_t> bytes;uint64_t at;};
  std::vector<Sent> sent;std::vector<size_t> resolved;std::set<size_t> unnamed;bool refuse=false;uint64_t now=1000;
  std::unique_ptr<UdpProbe> probe;
  explicit Net(size_t relays,std::set<size_t> missing={},bool refused=false):unnamed(std::move(missing)),refuse(refused){
    probe=std::make_unique<UdpProbe>(goodDatagrams(),relays,now,[this](size_t relay){resolved.push_back(relay);return !unnamed.contains(relay);},[this](size_t relay,std::span<const uint8_t> bytes){sent.push_back({relay,{bytes.begin(),bytes.end()},now});return !refuse;});
  }
  // As the leg's loop does: sleep to the next wake, then tick.
  void run(uint64_t until){while(now<=until){const auto wake=probe->tick(now);if(!wake||*wake>until)break;now=*wake;}}
};
int run(){
  int passed=0;
  require(std::string(udpMediaProtocol)=="udp-media-v1"&&udpProbeAction==270&&udpProbeBytes==25&&udpBindBytes==41,"The contract's fixed values changed");++passed;

  // Datagram building against the reference. Fixed bytes, computed by Node.
  {const auto built=goodDatagrams();
    require(same(built.bind,bindBytes),"BIND differs from the relay's reference");
    require(same(built.ack,ackBytes),"ACK differs from the relay's reference");++passed;}

  // Base64: exact length, standard alphabet, canonical padding.
  {std::array<uint8_t,udpIdBytes> id{};
    require(udpBase64(idText,id)&&id[0]==0xa0&&id[7]==0xa7,"Valid base64 refused");++passed;
    for(const auto* bad:{"","oKGio6Slpqc","oKGio6Slpqc==","oKGio6Slpqd=","oKGio6Slpq=c","oKGio6Slpq c=","oKGio6Slpq-=","oKGio6Slpq_=","oKGio6Slpq\xc3=","=KGio6Slpqc=","oKGio6Slpqc=A","oKGio6Slpqc\n"})
      {UdpLegId out{};require(!udpBase64(std::string(bad),out),"Non-canonical base64 accepted");++passed;}}

  // Grant validation: the good grant, then one change at a time.
  {const auto grant=parsed(grantText());
    require(grant&&grant->relays.size()==2&&grant->relays[0].host=="192.0.2.121"&&grant->relays[0].port==18481&&grant->relays[1].host=="vdi.example.com"&&grant->id[3]==0xa3&&grant->ticket[31]==31,"A valid grant was refused or misread");++passed;
    std::string eight="[";for(int i=0;i<8;++i){if(i)eight+=',';eight+=R"({"host":"a","port":1})";}eight+=']';
    require(parsed(grantText(eight))&&parsed(grantText(R"([{"host":"h","port":65535}])")),"A grant at its bounds was refused");++passed;
    require(parsed(grantText(R"([{"host":")"+std::string(253,'a')+R"(","port":1}])")).has_value(),"The longest host name was refused");++passed;
    auto padded=grantText();padded.insert(padded.size()-1,std::string(2048-padded.size()-14,' ')+R"(,"sessionId":1)");require(padded.size()==2048&&parsed(padded),"A grant of exactly 2048 bytes was refused");++passed;
    auto over=padded;over.insert(over.size()-1," ");require(over.size()==2049&&!parsed(over),"A grant over 2048 bytes was accepted");++passed;
    std::string nine="[";for(int i=0;i<9;++i){if(i)nine+=',';nine+=R"({"host":"a","port":1})";}nine+=']';
    const std::vector<std::string> malformed{
      grantText(nine),grantText("[]"),grantText("{}"),grantText("\"host\""),grantText("null"),grantText(R"([1])"),grantText(R"([null])"),
      grantText(R"([{"port":1}])"),grantText(R"([{"host":"a"}])"),grantText(R"([{"host":"","port":1}])"),grantText(R"([{"host":1,"port":1}])"),
      grantText(R"([{"host":")"+std::string(254,'a')+R"(","port":1}])"),grantText(R"([{"host":"a b","port":1}])"),grantText(R"([{"host":"a/b","port":1}])"),grantText(R"([{"host":"a:1","port":1}])"),grantText(R"([{"host":"한글","port":1}])"),
      grantText(R"([{"host":"a","port":0}])"),grantText(R"([{"host":"a","port":65536}])"),grantText(R"([{"host":"a","port":-1}])"),grantText(R"([{"host":"a","port":1.5}])"),grantText(R"([{"host":"a","port":"1"}])"),
      grantText(R"([{"host":"ok","port":1},{"host":"bad host","port":1}])"),
      // 7 and 9 id bytes, 31 and 33 ticket bytes, each validly encoded.
      grantText(goodRelays,"\"oKGio6Slpg==\""),grantText(goodRelays,"\"oKGio6Slpqeo\""),grantText(goodRelays,"1"),grantText(goodRelays,"null"),
      grantText(goodRelays,"\""+idText+"\"","\"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHg==\""),grantText(goodRelays,"\""+idText+"\"","\"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8g\""),grantText(goodRelays,"\""+idText+"\"","1"),
      grantText(goodRelays,"\""+idText+"\"","\"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8\""),
      grantText(goodRelays,"\""+idText+"\"","\""+ticketText+"\"","0"),grantText(goodRelays,"\""+idText+"\"","\""+ticketText+"\"","-1"),grantText(goodRelays,"\""+idText+"\"","\""+ticketText+"\"","1.5"),grantText(goodRelays,"\""+idText+"\"","\""+ticketText+"\"","\"soon\""),grantText(goodRelays,"\""+idText+"\"","\""+ticketText+"\"","9007199254740992"),
      grantText(goodRelays,"\""+idText+"\"","\""+ticketText+"\"","1793520000000","\"udp-media-v2\""),grantText(goodRelays,"\""+idText+"\"","\""+ticketText+"\"","1793520000000","1"),
      R"({"type":"udp.grant"})",R"({"type":"other","protocol":"udp-media-v1"})"};
    for(const auto& text:malformed){require(!parsed(text),"A malformed grant was accepted");++passed;}
    // Phase 3's marker, and anything else a later Broker adds, is not this parser's business.
    {for(const auto* extra:{",\"keys\":true}",",\"keys\":false}",",\"keys\":\"yes\"}",",\"later\":{\"a\":[1]}}"}){auto text=grantText();text.pop_back();text+=extra;const auto marked=parsed(text);require(marked&&marked->relays.size()==parsed(grantText())->relays.size(),"An extra grant field refused the grant");++passed;}}
    for(const auto* field:{"\"id\"","\"ticket\"","\"expiresAt\"","\"protocol\"","\"relays\""}){auto text=grantText();const auto at=text.find(field);text[at+1]='x';require(!parsed(text),"A grant missing a field was accepted");++passed;}}

  // ACK: the exact 41 bytes, nothing near them. udpSameBytes is what the probe calls.
  {const auto built=goodDatagrams();
    require(udpSameBytes(ackBytes,built.ack),"The reference ACK was refused");++passed;
    for(size_t bit=0;bit<ackBytes.size()*8;++bit){auto forged=ackBytes;forged[bit/8]^=uint8_t(1u<<(bit%8));require(!udpSameBytes(forged,built.ack),"An ACK with one bit changed passed");}++passed;
    require(!udpSameBytes(bindBytes,built.ack),"The BIND passed as an ACK");
    require(!udpSameBytes(std::span(ackBytes).first(40),built.ack),"A short ACK passed");
    auto longer=ackBytes;longer.push_back(0);require(!udpSameBytes(longer,built.ack),"A long ACK passed");++passed;}

  // PING to PONG.
  {const UdpLegId own{0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7};
    const auto pong=udpProbePong(pingBytes,own);
    require(pong&&same(*pong,hex("03a0a1a2a3a4a5a6a70200000000000007000000000001e240")),"The PONG is not this leg's id plus the PING's payload with [0]=2");++passed;
    {auto top=pingBytes;for(size_t i=13;i<25;++i)top[i]=0xff;const auto high=udpProbePong(top,own);require(high&&same(std::span(*high).subspan(13),std::span(top).subspan(13)),"Sequence or time did not survive");++passed;}
    require(!udpProbePong(std::span(pingBytes).first(24),own),"A short PING was answered");++passed;
    {auto longer=pingBytes;longer.push_back(0);require(!udpProbePong(longer,own),"A long PING was answered");++passed;}
    for(const uint8_t type:{uint8_t(0),udpTypeBind,udpTypeAck,uint8_t(4)}){auto bad=pingBytes;bad[0]=type;require(!udpProbePong(bad,own),"A datagram that is not DATA was answered");++passed;}
    for(const uint8_t kind:{uint8_t(0),udpPong,uint8_t(3),uint8_t(255)}){auto bad=pingBytes;bad[9]=kind;require(!udpProbePong(bad,own),"A payload that is not a PING was answered");++passed;}
    for(const size_t reserved:{size_t(10),size_t(11),size_t(12)}){auto bad=pingBytes;bad[reserved]=1;require(!udpProbePong(bad,own),"A PING with reserved bytes set was answered");++passed;}
    require(!udpProbePong({},own)&&!udpProbePong(bindBytes,own),"Empty input or a BIND was answered");++passed;}

  // Rate cap: 100 in any second, on a sliding window.
  {UdpPongLimit limit;int allowed=0;for(int i=0;i<300;++i)allowed+=limit.allow(5000)?1:0;require(allowed==100,"More than 100 PONGs in one instant");++passed;
    require(!limit.allow(5999),"The window closed early");require(limit.allow(6000),"The window never opened again");++passed;}
  {UdpPongLimit limit;for(int i=0;i<50;++i)require(limit.allow(100),"Early PONG refused");for(int i=0;i<50;++i)require(limit.allow(900),"Later PONG refused");
    require(!limit.allow(1099),"Burst across a calendar second exceeded 100 per second");require(limit.allow(1100),"The oldest PONG left the window but no slot opened");++passed;}
  {UdpPongLimit limit;std::vector<uint64_t> sent;uint64_t now=0;uint32_t state=12345;
    for(int i=0;i<200000;++i){state=state*1664525u+1013904223u;now+=(state>>24)%8;if(limit.allow(now))sent.push_back(now);}
    size_t tail=0;for(size_t head=0;head<sent.size();++head){while(sent[head]-sent[tail]>=1000)++tail;require(head-tail+1<=100,"A sliding second held more than 100 PONGs");}
    require(sent.size()>5000,"The cap starved traffic it should pass");++passed;}

  // Relay schedule: 3 BINDs 200 ms apart per relay, in list order, until 5 s.
  {UdpBindSchedule one(1,1000);std::vector<size_t> relays;std::vector<uint64_t> times;uint64_t now=1000;
    while(!one.expired(now)){if(const auto relay=one.due(now)){relays.push_back(*relay);times.push_back(now-1000);}now=one.wake();}
    require(relays.size()==25&&times.front()==0&&times[1]==200&&times.back()==4800,"The schedule is not every 200 ms for 5 s");require(std::all_of(relays.begin(),relays.end(),[](size_t relay){return !relay;}),"A lone relay was not repeated");++passed;}
  {UdpBindSchedule two(2,0);std::vector<size_t> order;for(uint64_t now=0;now<5000;now+=200)if(const auto relay=two.due(now))order.push_back(*relay);
    const std::vector<size_t> expected{0,0,0,1,1,1,0,0,0,1,1,1,0,0,0,1,1,1,0,0,0,1,1,1,0};require(order==expected,"Two relays were not tried in order");++passed;}
  {UdpBindSchedule three(3,0);std::vector<size_t> order;for(uint64_t now=0;now<2000;now+=200)if(const auto relay=three.due(now))order.push_back(*relay);
    const std::vector<size_t> expected{0,0,0,1,1,1,2,2,2,0};require(order==expected,"Three relays were not tried in order");++passed;}
  {UdpBindSchedule schedule(2,0);require(schedule.due(0)==size_t(0),"First BIND missing");require(!schedule.due(199),"BIND before 200 ms");require(schedule.due(700)==size_t(1),"A late wake did not continue at the current relay");require(!schedule.due(799),"A late wake sent twice");require(schedule.wake()==800,"Wake is not the next 200 ms step");++passed;}
  {UdpBindSchedule schedule(1,0);require(schedule.due(4999)==size_t(0)&&schedule.wake()==5000,"The last step was not at 5 s");require(!schedule.due(5000)&&schedule.expired(5000)&&!schedule.expired(4999),"The schedule ran past 5 s");++passed;}

  // The whole probe through the fake network.
  {Net net(2);net.run(1400);
    require(net.sent.size()==3&&std::all_of(net.sent.begin(),net.sent.end(),[](const Net::Sent& each){return !each.relay&&same(each.bytes,bindBytes);}),"The first relay did not get three BINDs");++passed;
    net.run(1800);require(net.sent.size()==5&&net.sent[3].relay==1&&net.sent[3].at==1600,"The next relay did not start at 600 ms");++passed;
    require(net.resolved==std::vector<size_t>({0,1}),"Names were not looked up lazily and once");++passed;
    // Until ACK everything else is ignored, including a perfectly valid PING.
    const auto before=net.sent.size();net.probe->received(0,pingBytes,net.now);net.probe->received(0,bindBytes,net.now);auto forged=ackBytes;forged[20]^=1;net.probe->received(0,forged,net.now);net.probe->received(0,{},net.now);
    require(net.sent.size()==before&&!net.probe->takeOutcome(),"Something unproven moved the probe or got a reply");++passed;
    net.probe->received(1,ackBytes,net.now);const auto outcome=net.probe->takeOutcome();
    require(outcome&&outcome->stage==udpStageBound&&outcome->relay==1,"The answering relay was not kept");require(!net.probe->takeOutcome(),"The outcome was reported twice");++passed;
    require(!net.probe->failed()&&!net.probe->tick(net.now+5000),"BINDs went on after the ACK");require(net.sent.size()==before,"A BIND followed the ACK");++passed;
    net.probe->received(1,ackBytes,net.now);require(!net.probe->takeOutcome(),"A repeated ACK made a second record");++passed;
    net.probe->received(1,pingBytes,net.now);require(net.sent.size()==before+1&&net.sent.back().relay==1&&net.sent.back().bytes[9]==udpPong&&same(std::span(net.sent.back().bytes).subspan(1,8),goodGrant().id),"A valid PING got no PONG from this leg");++passed;
    net.probe->received(0,pingBytes,net.now);require(net.sent.size()==before+1,"A PING from another relay was answered");++passed;
    auto reservedSet=pingBytes;reservedSet[11]=1;net.probe->received(1,reservedSet,net.now);require(net.sent.size()==before+1,"A malformed PING was answered");++passed;}
  {Net net(1);net.probe->tick(net.now);net.probe->received(0,ackBytes,net.now);(void)net.probe->takeOutcome();const auto before=net.sent.size();
    for(int i=0;i<1000;++i)net.probe->received(0,pingBytes,net.now+uint64_t(i));
    require(net.sent.size()-before==100,"A PING flood was answered past 100 per second");++passed;
    for(int i=0;i<1000;++i)net.probe->received(0,pingBytes,net.now+1000+uint64_t(i));
    require(net.sent.size()-before>100&&net.sent.size()-before<=200,"The cap did not recover in the next second");++passed;}
  // Failures, each one record with its fixed stage.
  {Net net(2);net.run(6000);const auto outcome=net.probe->takeOutcome();
    require(net.probe->failed()&&outcome&&outcome->stage==udpStageTimeout&&outcome->relay==0&&net.sent.size()==25,"Silence did not end as a BIND timeout at 5 s");++passed;
    require(!net.probe->takeOutcome(),"The timeout was reported twice");net.probe->received(0,ackBytes,net.now);require(!net.probe->takeOutcome()&&net.probe->failed(),"A late ACK revived a finished probe");++passed;}
  {Net net(3,{0,1,2});net.run(2200);const auto outcome=net.probe->takeOutcome();
    require(outcome&&outcome->stage==udpStageName&&outcome->relay==2&&net.sent.empty()&&net.resolved.size()==3,"No usable name did not end as a lookup failure");++passed;}
  {Net net(2,{0});net.run(6000);const auto outcome=net.probe->takeOutcome();
    require(outcome&&outcome->stage==udpStageTimeout&&outcome->relay==1&&std::all_of(net.sent.begin(),net.sent.end(),[](const Net::Sent& each){return each.relay==1;})&&!net.sent.empty(),"One bad name stopped the relay that still works");
    require(std::count(net.resolved.begin(),net.resolved.end(),size_t(0))==1,"A failed name was looked up again");++passed;}
  {Net net(2,{},true);net.run(6000);const auto outcome=net.probe->takeOutcome();
    require(outcome&&outcome->stage==udpStageSocket&&outcome->relay==0&&net.sent.size()==25,"Every send failing did not end as a socket error");++passed;}
  {Net net(1);net.probe->tick(net.now);net.probe->socketFailed();const auto outcome=net.probe->takeOutcome();require(outcome&&outcome->stage==udpStageSocket&&net.probe->failed(),"A socket failure during BIND was lost");++passed;}
  {Net net(1);net.probe->tick(net.now);net.probe->received(0,ackBytes,net.now);(void)net.probe->takeOutcome();net.probe->socketFailed();require(!net.probe->takeOutcome()&&!net.probe->failed(),"A later socket failure rewrote a bound outcome");++passed;}
  return passed;
}
}
int main(){
  try{std::cout<<"{\"kind\":\"udp-probe\",\"passed\":"<<run()<<",\"sockets\":0}\n";return 0;}
  catch(const std::exception& error){std::cerr<<"UDP probe check failed: "<<error.what()<<"\n";return 1;}
}
