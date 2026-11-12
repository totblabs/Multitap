#include "../../../native/windows_video/udp_direct.hpp"
#include "../../../native/windows_video/udp_stun.hpp"
#include <iostream>
#include <queue>
#include <stdexcept>
#include <string>
using namespace multitap::udp;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static void key(DirectionKey& value,uint8_t byte){std::fill_n(value.key.data(),keyBytes,byte);value.ready=true;}
struct Link {
  DirectPath a,b;DirectionKey as,ar,bs,br;
  DirectEndpoint ea{1,1001},eb{2,1002},relay{3,18481};
  struct Flight{bool toA;DirectEndpoint from;DirectPath::Packet bytes;uint64_t due;};
  std::vector<Flight> queue;uint64_t now=1000,directDelay=2000,relayDelay=20000;bool directWorks=true;
  Link(){key(as,1);key(ar,2);key(bs,2);key(br,1);require(a.configure(as,ar)&&b.configure(bs,br),"configure");a.relay(relay);b.relay(relay);a.candidate(eb);b.candidate(ea);}
  void send(bool fromA,DirectEndpoint to,const DirectPath::Packet& bytes){if(to==relay)queue.push_back({!fromA,relay,bytes,now+relayDelay});else if(directWorks&&to==(fromA?eb:ea))queue.push_back({!fromA,fromA?ea:eb,bytes,now+directDelay});}
  void run(uint64_t until){for(;now<until;now+=1000){a.tick(now,[&](auto to,const auto& bytes){send(true,to,bytes);});b.tick(now,[&](auto to,const auto& bytes){send(false,to,bytes);});auto held=std::move(queue);queue.clear();for(const auto& f:held){if(f.due>now){queue.push_back(f);continue;}auto& path=f.toA?a:b;path.received(f.bytes.data(),f.bytes.size(),f.from,now,[&](auto to,const auto& bytes){send(f.toA,to,bytes);});}}}
};
int main(){try{
  size_t checks=0;
  {Link link;const std::string hex="5456440101000102030405060708090a0b0c0d0e0f3bee1b52544d6b8f4b01d4787080ebb46fbafa3aad919634be540a5d6b70b620";std::vector<uint8_t> vector;for(size_t n=0;n<hex.size();n+=2)vector.push_back(uint8_t(std::stoul(hex.substr(n,2),nullptr,16)));bool answered=false;
    require(link.b.received(vector.data(),vector.size(),link.ea,1000,[&](auto,const auto&){answered=true;}),"Node/javax HMAC reference refused");require(answered,"reference ping not answered");checks+=2;}
  {StunBinding stun;DirectEndpoint server{9,3478};stun.server(server);StunBinding::Request request{};stun.tick(1000,[&](auto,const auto& bytes){request=bytes;});
    std::vector<uint8_t> response(request.begin(),request.end());response[0]=1;response[1]=1;response[3]=12;response.insert(response.end(),{0,0x20,0,8,0,1,0x21,0x12,0xe1,0x12,0xa6,0x43});response[26]^=0x30;response[27]^=0x39;
    const auto wrong=DirectEndpoint{10,3478};require(!stun.received(response.data(),response.size(),wrong,2000),"STUN accepted another source");++checks;
    auto invalid=response;invalid[8]^=1;require(!stun.received(invalid.data(),invalid.size(),server,2000),"STUN accepted another transaction");++checks;
    const auto mapped=stun.received(response.data(),response.size(),server,2000);require(mapped&&mapped->port==12345,"STUN mapped port");const std::array<uint8_t,4> expected{192,0,2,1};require(std::memcmp(&mapped->address,expected.data(),4)==0,"STUN mapped IPv4");checks+=2;
    require(!stun.received(response.data(),response.size(),server,3000),"STUN accepted a repeated response");++checks;
  }
  {Link link;require(!link.a.target(link.now),"an address is not proof");++checks;link.run(1000000);require(link.a.target(link.now)==link.eb&&link.b.target(link.now)==link.ea,"fast direct not selected");require(link.a.relayRtt()>link.a.rtt(link.eb),"round trip not measured");checks+=2;
    link.directWorks=false;link.run(3000000);require(!link.a.target(link.now)&&!link.b.target(link.now),"dead path did not return to relay");++checks;
    link.directWorks=true;link.run(4000000);require(link.a.target(link.now)==link.eb,"recovered path did not return");++checks;}
  {Link link;link.directDelay=100000;link.relayDelay=10000;link.run(2000000);require(!link.a.target(link.now)&&!link.b.target(link.now),"slower direct displaced relay");++checks;}
  {Link link;DirectPath::Packet captured{};link.a.tick(link.now,[&](auto to,const auto& packet){if(to==link.eb)captured=packet;});auto tampered=captured;tampered.back()^=1;bool answered=false;
    require(!link.b.received(tampered.data(),tampered.size(),link.ea,link.now,[&](auto,const auto&){answered=true;}),"forged tag accepted");require(!answered,"forgery reflected");checks+=2;
    DirectPath::Packet pong{};require(link.b.received(captured.data(),captured.size(),link.ea,link.now+1000,[&](auto,const auto& packet){pong=packet;}),"authentic ping refused");require(!link.b.target(link.now+1000),"incoming ping selected its source");checks+=2;
    link.a.received(pong.data(),pong.size(),link.eb,link.now+2000,[](auto,const auto&){});require(link.a.target(link.now+2000)==link.eb,"fresh pong refused");++checks;
    link.a.received(pong.data(),pong.size(),link.eb,link.now+2000000,[](auto,const auto&){});require(!link.a.target(link.now+2000000),"replay revived stale path");++checks;
    require(!link.b.received(captured.data(),captured.size()-1,link.ea,link.now,[](auto,const auto&){}),"truncated packet accepted");++checks;
  }
  // Freshness and targets.
  {const auto ip=[](uint8_t a,uint8_t b,uint8_t c,uint8_t d){uint32_t out=0;const uint8_t bytes[4]{a,b,c,d};std::memcpy(&out,bytes,4);return out;};
    DirectPath path;DirectionKey s,r;key(s,1);key(r,2);require(path.configure(s,r),"configure");path.relay({ip(203,0,113,9),18481});
    for(const auto bad:{ip(0,0,0,0),ip(0,1,2,3),ip(127,0,0,1),ip(224,0,0,1),ip(239,255,255,250),ip(240,0,0,1),ip(255,255,255,255)}){
      require(!path.routable({bad,5000}),"non-routable target accepted");path.candidate({bad,5000});++checks;}
    require(path.candidates()==0,"a refused target became a candidate");require(!path.routable({ip(10,0,0,5),0}),"port 0 accepted");checks+=2;
    require(path.routable({ip(10,0,0,5),5000})&&path.routable({ip(198,51,100,7),5000}),"ordinary addresses refused");++checks;
    DirectPath local;require(local.configure(s,r),"configure");local.relay({ip(127,0,0,1),18481});require(local.routable({ip(127,0,0,1),5000}),"loopback beside a loopback relay refused");++checks;}
  {Link link;DirectPath::Packet captured{};link.a.tick(link.now,[&](auto to,const auto& packet){if(to==link.eb)captured=packet;});
    const auto at=[](uint8_t last){uint32_t out=0;const uint8_t bytes[4]{198,51,100,last};std::memcpy(&out,bytes,4);return out;};
    size_t pongs=0;const auto count=[&](auto,const auto&){++pongs;};
    require(link.b.received(captured.data(),captured.size(),link.ea,link.now,count)&&pongs==1,"authentic ping not answered");++checks;
    require(link.b.received(captured.data(),captured.size(),link.ea,link.now+250000,count)&&pongs==2,"retransmitted ping from the same address not answered");++checks;
    // Played back from other addresses: consumed, never answered or learned.
    for(uint8_t n=1;n<=6;++n)require(link.b.received(captured.data(),captured.size(),{at(n),40000},link.now+300000,count),"replay not consumed");
    require(pongs==2&&link.b.candidates()==1&&link.b.learned()==0,"a replayed ping filled a slot or was reflected");checks+=2;
    // Fresh pings from new addresses (a NAT's reflexive address): at most two are learned.
    for(uint8_t n=1;n<=4;++n){const uint64_t when=link.now+n*DirectPath::aliveUs+1000;DirectPath::Packet fresh{};
      link.a.tick(when,[&](auto to,const auto& packet){if(to==link.eb)fresh=packet;});require(fresh[0]=='T'&&fresh!=captured,"a new challenge per round");
      link.b.received(fresh.data(),fresh.size(),{at(uint8_t(10+n)),40000},when,[](auto,const auto&){});}
    require(link.b.learned()==2&&link.b.candidates()==3,"learned addresses not bounded");checks+=2;
    // Unanswered learned addresses leave after the trial; broker candidates stay.
    link.b.tick(link.now+4*DirectPath::aliveUs+DirectPath::learnedTrialUs+1000,[](auto,const auto&){});
    require(link.b.learned()==0&&link.b.candidates()==1,"unconfirmed learned addresses kept");++checks;}
  std::cout<<"{\"kind\":\"udp-direct-core\",\"checks\":"<<checks<<"}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
