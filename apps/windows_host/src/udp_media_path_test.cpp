// The Host's UDP media path (udp_media_path.hpp) with injected time and no socket. The
// client's half is the shared core itself, joined to it in memory: a simulated link,
// not a network measurement. udp_media_loopback_test.cpp runs the real socket leg.
#include "udp_media_path.hpp"
#include "bitrate_session.hpp"
#include <iostream>
#include <map>

namespace multitap::udp {
struct TestAccess {static void nextSequence(Sealer& sealer,uint64_t next){sealer.next_=next;}};
}
namespace host {
struct UdpMediaPathTest {static void nextSequence(UdpMediaPath& path,uint64_t next){multitap::udp::TestAccess::nextSequence(*path.sealer_,next);}};
}
using namespace host;
namespace core=multitap::udp;
namespace {
int passed=0;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
using Bytes=std::vector<uint8_t>;
constexpr uint16_t streamEpoch=1;
const std::string sessionId="00000000-0000-0000-0000-000000000001";
const UdpLegId hostLeg{1,2,3,4,5,6,7,8},clientLeg{9,10,11,12,13,14,15,16};
std::string base64(std::span<const uint8_t> bytes){char text[128]{};DWORD length=DWORD(sizeof(text));require(CryptBinaryToStringA(bytes.data(),DWORD(bytes.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,text,&length),"Base64 failed");return std::string(text,length);}
Json keyMessage(const std::string& publicKey,const std::string& protocol=udpMediaProtocol){return JsonParser::parse("{\"type\":\"udp.key\",\"protocol\":\""+protocol+"\",\"publicKey\":\""+publicKey+"\",\"sessionId\":\""+sessionId+"\"}");}
// A WSS media message: the 12-byte envelope, then a payload that says where it came from.
Bytes media(uint8_t kind,bool key,uint64_t pts,size_t payload,uint16_t epoch=streamEpoch){
  Bytes bytes(12+payload);bytes[0]=kind;bytes[1]=key?1:0;bytes[2]=uint8_t(epoch);bytes[3]=uint8_t(epoch>>8);for(int n=0;n<8;++n)bytes[4+n]=uint8_t(pts>>(8*n));
  for(size_t n=12;n<bytes.size();++n)bytes[n]=uint8_t(pts*31+n*7);return bytes;
}
Bytes video(uint64_t pts,bool key=false,size_t payload=2000){return media(1,key,pts,payload);}
Bytes textBytes(const std::string& text){return {text.begin(),text.end()};}
const Bytes streamInfo=textBytes(R"({"type":"stream.info","codec":"h264","epoch":1})");
// The client: the shared core's Receiver behind its own keys, as the design has it.
struct Client {
  core::X25519 pair;core::SessionKeys keys;std::unique_ptr<core::Sealer> sealer;std::unique_ptr<core::Opener> opener;std::optional<core::Receiver> receiver;
  std::vector<core::Receiver::Message> delivered;std::vector<Bytes> outbound;uint64_t challenge=0;size_t opened=0;
  std::string publicKey()const{return base64(pair.publicKey());}
  void adopt(const std::string& reply,uint64_t now){
    core::PublicKey host;require(udpBase64(JsonParser::parse(reply).get("publicKey").string(),host),"The reply's key is not 32 bytes of base64");
    core::Secret<core::keyBytes> shared;require(pair.agree(host,shared),"No agreement");
    require(core::deriveKeys(shared,pair.publicKey(),host,reinterpret_cast<const uint8_t*>(sessionId.data()),sessionId.size(),core::Role::client,keys),"No keys");
    sealer=std::make_unique<core::Sealer>(keys.send,clientLeg);opener=std::make_unique<core::Opener>(keys.receive);receiver.emplace(streamEpoch,now);
  }
  void take(core::Receiver::Output& output){
    for(auto& message:output.messages)delivered.push_back(std::move(message));
    for(const auto& plain:output.control)outbound.push_back(seal(plain));
  }
  Bytes seal(const Bytes& plain){Bytes out(core::maxDatagram);out.resize(sealer->seal(plain.data(),plain.size(),out.data()));require(!out.empty(),"The client could not seal");return out;}
  void datagram(std::span<const uint8_t> sealed,uint64_t now){
    if(!opener)return;
    std::array<uint8_t,core::maxPlaintext> plain;const size_t size=opener->open(sealed.data(),sealed.size(),plain.data());if(!size)return;
    ++opened;if(plain[0]==core::kindTail)challenge=core::get64(&plain[14]);
    core::Receiver::Output output;receiver->datagram(plain.data(),size,now,output);take(output);
  }
  void tick(uint64_t now){if(!receiver)return;core::Receiver::Output output;receiver->tick(now,output);take(output);}
};
struct Link {
  int keyFrames=0,backlogs=0,inputs=0,target=20000000;std::vector<bool> paths;std::vector<std::pair<uint32_t,uint32_t>> records;std::vector<uint32_t> causes;std::vector<uint64_t> recoveries;
  struct Sample {uint64_t rtt;uint32_t received,lost;};std::vector<Sample> samples;
  UdpMediaPath path;Client client;uint64_t now=10000000,upDelayUs=0;size_t wire=0;bool upCut=false,downCut=false,wssFeedback=false;std::function<bool(size_t)> dropDown;std::deque<std::pair<uint64_t,Bytes>> flying;
  UdpMediaHooks hooks(){UdpMediaHooks out{[this]{++keyFrames;},[this](uint64_t rtt,uint32_t received,uint32_t lost){samples.push_back({rtt,received,lost});return target;},[this]{++backlogs;},[this](bool onUdp){paths.push_back(onUdp);return target;},[this](uint64_t lastGood){recoveries.push_back(lastGood);}};out.input=[this](uint32_t,uint16_t,const std::string&){++inputs;};return out;}
  explicit Link(bool direct=false):path(hostLeg,hooks(),[this](uint32_t stage,uint32_t reason,uint32_t cause){records.push_back({stage,reason});causes.push_back(cause);},direct){}
  void step(int milliseconds=1){
    for(int n=0;n<milliseconds;++n){
      now+=1000;
      if(wssFeedback&&now%100000==0)path.wssHeard(now);  // the client's stream.feedback on WSS
      path.pump(now,[&](std::span<const uint8_t> bytes){++wire;if(!downCut&&!(dropDown&&dropDown(wire)))client.datagram(bytes,now);});
      client.tick(now);
      for(auto& bytes:client.outbound)if(!upCut)flying.push_back({now+upDelayUs,std::move(bytes)});
      client.outbound.clear();
      while(!flying.empty()&&flying.front().first<=now){path.received(flying.front().second,now);flying.pop_front();}
    }
  }
  void handshake(){
    path.probePassed();const auto reply=path.exchange(keyMessage(client.publicKey()),sessionId,target);
    require(bool(reply),"The key exchange failed");client.adopt(*reply,now);
  }
  bool stepUntil(const std::function<bool()>& done,int milliseconds){for(int n=0;n<milliseconds&&!done();++n)step();return done();}
  void armed(){const int before=keyFrames;require(stepUntil([&]{return keyFrames>before;},2000),"Proof never armed the switch");}
  bool hasRecord(uint32_t stage,uint32_t reason)const{return std::find(records.begin(),records.end(),std::pair{stage,reason})!=records.end();}
  double stat(const char* key){auto text=textBytes(R"({"type":"stream.stats","encodedFps":60})");path.outgoing(true,text);return JsonParser::parse(std::string(text.begin(),text.end())).get(key).number(-1);}
};
bool route(Link& link,const Bytes& message){auto copy=message;const bool taken=link.path.outgoing(false,copy);if(!taken)require(copy==message,"A message left on WebSocket was altered");return taken;}
// Session, first IDR on WebSocket, keys, proof, then the key frame that switches.
uint64_t onUdp(Link& link){
  require(!route(link,video(1000,true)),"The first IDR did not stay on WebSocket");
  link.handshake();link.armed();require(route(link,video(2000,true)),"The armed key frame did not go on UDP");link.step(5);require(link.hasRecord(udpPathOnUdp,udpPathProven),"No record of the switch");return 2000;
}
void play(Link& link,int milliseconds,uint64_t& pts){for(int at=0;at<milliseconds;at+=16){pts+=16667;route(link,video(pts));link.step(16);}}
Bytes feedbackPlain(uint64_t challenge,uint32_t highest=0,uint32_t received=0){Bytes plain(core::feedbackBytes);plain[0]=core::kindFeedback;core::put32(&plain[2],highest);core::put32(&plain[6],received);core::put64(&plain[26],challenge);return plain;}
Bytes keyframePlain(uint16_t epoch){Bytes plain(core::keyframeBytes);plain[0]=core::kindKeyframe;core::put16(&plain[2],epoch);return plain;}
Bytes nackPlain(uint32_t id){Bytes plain(2+core::nackEntryBytes);plain[0]=core::kindNack;plain[1]=1;core::put32(&plain[2],id);core::put16(&plain[6],0);core::put16(&plain[8],1);return plain;}
// The right shape and any sequence, but no key: what anyone on the path could send.
Bytes forged(const Bytes& plain,uint64_t sequence){Bytes out(core::headerBytes+plain.size()+core::tagBytes);out[0]=3;std::copy(clientLeg.begin(),clientLeg.end(),out.begin()+1);core::put64(&out[9],sequence);std::copy(plain.begin(),plain.end(),out.begin()+core::headerBytes);return out;}

void exchangeRules(){
  {Link link;require(!link.path.exchange(keyMessage(link.client.publicKey()),sessionId,link.target),"A key before any PING was answered was accepted");
    require(!link.path.exchange(keyMessage(link.client.publicKey()),sessionId,link.target),"A refused key used up the grant");}
  {Link link;link.path.probePassed();const auto key=link.client.publicKey();
    require(!link.path.exchange(keyMessage(key),sessionId,0),"A key with no bitrate controller was accepted");
    require(!link.path.exchange(keyMessage(key,"udp-media-v2"),sessionId,link.target),"Another protocol's key was accepted");
    require(!link.path.exchange(keyMessage(key.substr(0,43)),sessionId,link.target),"A key without its padding was accepted");
    require(!link.path.exchange(keyMessage(std::string(44,'-')),sessionId,link.target),"A key in the URL alphabet was accepted");
    const auto reply=link.path.exchange(keyMessage(key),sessionId,link.target);require(bool(reply),"The valid key was refused after malformed ones");
    const auto parsed=JsonParser::parse(*reply);const auto* fields=std::get_if<Json::Object>(&parsed.value);require(fields&&fields->size()==4&&parsed.get("type").string()=="udp.key"&&parsed.get("protocol").string()==udpMediaProtocol&&parsed.get("sessionId").string()==sessionId,"The reply is not exactly the contract's message");
    const auto text=parsed.get("publicKey").string();require(text.size()==44&&text.back()=='='&&text!=key,"The reply's key has the wrong shape");
    require(!link.path.exchange(keyMessage(key),sessionId,link.target),"A second key was accepted for one grant");
    link.client.adopt(*reply,link.now);require(link.stepUntil([&]{return link.client.opened>0;},300),"The two sides did not derive the same keys");
    require(!route(link,video(1,true))&&link.keyFrames==0,"Keys alone moved something");}
  // The small-order point gives an all-zero secret, which aborts and spends the grant.
  {Link link;link.path.probePassed();const std::array<uint8_t,32> zero{};require(!link.path.exchange(keyMessage(base64(zero)),sessionId,link.target),"An all-zero shared secret was accepted");
    require(!link.path.exchange(keyMessage(link.client.publicKey()),sessionId,link.target),"A grant was reused after a failed exchange");
    link.step(500);require(link.wire==0&&link.path.paceMs()==0,"A failed exchange started a sender");}
  ++passed;
}
void proofRequired(){
  Link link;require(!route(link,video(1,true)),"The first IDR did not stay on WebSocket");link.handshake();link.upCut=true;
  const auto tails=[&]{const size_t before=link.wire;link.step(1000);return link.wire-before;};
  // No FEEDBACK: TAILs on the 200 ms timer and nothing else, for as long as it takes.
  for(int second=0;second<3;++second){const auto count=tails();require(count>=4&&count<=6,"Not a TAIL every 200 ms before the proof");require(link.keyFrames==0,"The switch armed with no proof of the address");require(!route(link,video(uint64_t(second)+2,true)),"Media left WebSocket with no proof");}
  // Holding the keys proves nothing: only an echo of a challenge sent to that address does.
  link.upCut=false;link.client.outbound.clear();
  link.path.received(link.client.seal(feedbackPlain(0x1234567890abcdefULL)),link.now);link.path.received(link.client.seal(feedbackPlain(0)),link.now);link.client.receiver.reset();link.step(50);
  require(link.keyFrames==0,"FEEDBACK with a made-up challenge counted as proof");
  link.path.received(link.client.seal(feedbackPlain(link.client.challenge)),link.now);link.client.opener.reset();link.step(5);require(link.keyFrames==1,"A real echo did not arm the switch");++passed;
}
void switching(){
  Link link;require(!route(link,video(1000,true)),"The first IDR did not stay on WebSocket");link.handshake();link.armed();
  require(!route(link,video(1100)),"A P-frame left WebSocket before the key frame");require(!route(link,media(2,false,1110,1920)),"Audio left WebSocket before the key frame");require(link.paths.empty(),"The path moved before the key frame");
  const auto key=video(2000,true,90000),next=video(2100),sound=media(2,false,2110,1920);
  require(route(link,key),"The armed key frame stayed on WebSocket");require(route(link,next),"The next P-frame went back to WebSocket");require(route(link,sound),"Audio did not move with video");
  const auto cursor=media(3,false,2120,40);require(!route(link,cursor),"A cursor record left WebSocket");
  require(!route(link,textBytes(R"({"type":"pong"})")),"Text left WebSocket");
  require(link.stepUntil([&]{return link.client.delivered.size()>=3;},500),"Media did not arrive");
  require(link.paths==std::vector<bool>{true}&&link.records.size()==1&&link.records[0]==std::pair{udpPathOnUdp,uint32_t(udpPathProven)},"The switch was not recorded once");
  const auto bytesOf=[&](uint64_t pts){for(const auto& message:link.client.delivered)if(message.pts==pts)return message.bytes;return Bytes{};};
  require(bytesOf(2000)==key&&bytesOf(2100)==next&&bytesOf(2110)==sound,"Delivered bytes differ from the WSS message");require(bytesOf(1000).empty()&&bytesOf(1100).empty(),"A WebSocket message reached the UDP receiver");
  require(link.stat("udpPath")==1&&link.path.paceMs()==1,"Stats or pacing do not show UDP");++passed;
}
void firstIdrStaysOnWss(){
  // The leg started after the stream's first IDR: it asks for one on WebSocket, once,
  // and only then for the one that goes on UDP.
  {Link link;link.handshake();link.armed();link.step(1500);require(link.keyFrames==1,"The first-IDR request repeated");
    require(!route(link,video(1,true)),"An unseen first IDR went on UDP");link.armed();require(route(link,video(2,true)),"The next key frame stayed on WebSocket");++passed;}
  // A new picture while on UDP: its stream.info is text on WebSocket, so its IDR follows
  // it there, then the path comes back at once. This is no failure: no backoff.
  {Link link;auto pts=onUdp(link);auto info=streamInfo;require(!link.path.outgoing(true,info)&&info==streamInfo,"stream.info changed");
    require(!route(link,video(pts+1)),"A frame after stream.info took UDP");link.step(2);require(link.hasRecord(udpPathOnWss,udpPathNewStream)&&!link.paths.back(),"No record of the new stream");
    require(link.stat("udpFallbacks")==0,"A new stream counted as a failure");
    require(!route(link,video(pts+2,true)),"The new stream's first IDR went on UDP");link.armed();require(route(link,video(pts+3,true)),"The path did not come back after the new stream");link.step(2);require(link.paths.back(),"The return was not recorded");++passed;}
  // stream.info while armed: the key frame that was on its way is the first IDR.
  {Link link;require(!route(link,video(1,true)),"First IDR");link.handshake();link.armed();auto info=streamInfo;link.path.outgoing(true,info);require(!route(link,video(2,true)),"An armed switch took a new stream's first IDR");++passed;}
}
void directInputGates(){
  Link link(true);onUdp(link);link.path.wssHeard(link.now);
  const auto send=[&](uint16_t epoch,bool direct,uint64_t now){Bytes plain(core::directInputHeader+2);plain[0]=core::kindDirectInput;core::put32(plain.data()+1,1);core::put16(plain.data()+5,epoch);core::put16(plain.data()+7,2);plain[9]='{';plain[10]='}';link.path.received(link.client.seal(plain),now,direct);};
  send(1,false,link.now);require(link.inputs==0,"fast input arrived over the relay");
  send(1,true,link.now);require(link.inputs==1,"authorized direct input refused");
  send(2,true,link.now);require(link.inputs==1,"unannounced input epoch admitted");
  link.path.pauseInputs();send(1,true,link.now);require(link.inputs==1,"old coordinates crossed resize");
  // A queued old on-UDP notification cannot undo the minimum epoch gate.
  route(link,media(1,true,3000,2000,2));link.step(2);send(2,true,link.now);require(link.inputs==2,"new output never regained input");
  send(2,true,link.now+2000001);require(link.inputs==2,"direct input outlived broker feedback");++passed;
}
void staleProof(){
  Link link;require(!route(link,video(1,true)),"First IDR");link.handshake();link.armed();link.upCut=true;link.step(1100);
  require(!route(link,video(2,true)),"A key frame went on UDP on a proof over a second old");require(link.paths.empty(),"A stale switch moved the path");++passed;
}
void fallback(){
  // Both ways dead: silence for 1 s, not sooner, then WSS at a forced IDR.
  {Link link;auto pts=onUdp(link);play(link,500,pts);require(link.stepUntil([&]{return link.client.delivered.size()>=20;},300),"Video did not settle");
    link.upCut=link.downCut=true;const int before=link.keyFrames;const auto cut=link.now;
    uint64_t left=0;for(int ms=0;ms<1500&&!left;ms+=16){pts+=16667;if(!route(link,video(pts)))left=link.now;link.step(16);}
    require(left,"Silence did not end UDP");require(left-cut>=900000&&left-cut<=1300000,"The fallback was not about 1 s after the silence");
    require(link.keyFrames==before+1,"No IDR was forced");require(link.hasRecord(udpPathOnWss,udpPathSilent)&&!link.paths.back(),"The fallback was not recorded");
    require(link.causes.back()==1,"Silence with no FEEDBACK was not recorded as cause 1");
    require(link.stat("udpPath")==0&&link.stat("udpFallbacks")==1,"Stats missed the fallback");
    require(!route(link,video(pts+1,true)),"The forced IDR did not go on WebSocket");require(!route(link,media(2,false,pts+2,1920)),"Audio stayed on UDP");++passed;}
  // Host to client dead: FEEDBACK keeps coming and shows nothing arrived.
  {Link link;auto pts=onUdp(link);play(link,500,pts);link.step(100);link.downCut=true;const int before=link.keyFrames;
    uint64_t left=0;const auto cut=link.now;for(int ms=0;ms<2000&&!left;ms+=16){pts+=16667;if(!route(link,video(pts)))left=link.now;link.step(16);}
    require(left&&left-cut<=1500000&&link.keyFrames==before+1,"A one-way outage froze on UDP");
    require(link.records.back()==std::pair{udpPathOnWss,uint32_t(udpPathSilent)}&&link.causes.back()==2,"A Host-to-client outage was not recorded as cause 2");++passed;}
  // An unchanged screen is not silence: FEEDBACK runs on its timer, TAIL keeps running
  // after the switch, and there is nothing newer to report. Right after the switch, past
  // many challenges, with TAILs lost.
  {Link link;const auto pts=onUdp(link);link.step(60000);require(link.stat("udpPath")==1&&link.stat("udpFallbacks")==0&&link.keyFrames==1,"An idle screen read as a failed path");require(route(link,video(pts+1)),"Idle ended UDP");++passed;}
  {Link link;auto pts=onUdp(link);play(link,200,pts);link.step(100);const auto tails=link.wire;link.step(10000);require(link.wire-tails>=48&&link.wire-tails<=52,"TAIL stopped, or changed its pace, after the switch");
    link.dropDown=[](size_t n){return n%4==0;};link.step(30000);require(link.stat("udpPath")==1&&link.stat("udpFallbacks")==0,"Lost TAILs on an idle screen read as a failed path");++passed;}
  // That must not come from giving up on silence: real outages under a still screen count.
  {Link link;auto pts=onUdp(link);play(link,200,pts);link.step(3000);link.upCut=true;const auto cut=link.now;require(link.stepUntil([&]{return link.stat("udpPath")==0;},1300),"Silence on an idle screen was missed");require(link.now-cut>=900000&&link.hasRecord(udpPathOnWss,udpPathSilent),"The idle fallback came early, or unrecorded");++passed;}
  {Link link;auto pts=onUdp(link);play(link,200,pts);link.step(3000);link.downCut=true;require(link.stepUntil([&]{return link.stat("udpPath")==0;},6000),"A one-way outage on an idle screen was missed");
    require(link.causes.back()==2,"An idle one-way outage was not recorded as cause 2");++passed;}
  // The client's whole network goes dark: its stream.feedback on WSS stops with UDP, so WSS
  // has nothing to offer. The path holds, recorded once, and UDP carries on when it returns.
  {Link link;link.wssFeedback=true;auto pts=onUdp(link);play(link,300,pts);link.step(50);
    link.upCut=link.downCut=true;link.wssFeedback=false;
    for(int ms=0;ms<4000;ms+=16){pts+=16667;require(route(link,video(pts)),"A dark network left UDP");link.step(16);}
    const auto held=std::count(link.records.begin(),link.records.end(),std::pair{udpPathHeld,uint32_t(udpPathSilent)});
    require(held==1&&link.causes.back()==1&&link.stat("udpFallbacks")==0,"The hold was not recorded once, or it fell back");
    link.upCut=link.downCut=false;link.wssFeedback=true;const int asked=link.keyFrames;const size_t recovering=link.recoveries.size();
    // A LOSS now, which the encoder answers with an IDR: its last good frame is 4 s old.
    require(link.stepUntil([&]{return link.keyFrames>asked||link.recoveries.size()>recovering;},1000),"No key frame was asked for after the dark spell");
    pts+=1000;const auto before=link.client.delivered.size();const auto key=video(pts,true);require(route(link,key),"The key frame after the dark spell left UDP");play(link,2000,pts);
    require(link.client.delivered.size()>before+100&&link.client.delivered[before].bytes==key,"Video did not resume on UDP");
    require(link.stat("udpPath")==1&&link.stat("udpFallbacks")==0,"UDP did not carry on after the dark spell");++passed;}
  // WSS still heard: the outage is UDP's alone, so the path leaves as before.
  {Link link;link.wssFeedback=true;auto pts=onUdp(link);play(link,300,pts);link.step(50);link.upCut=link.downCut=true;
    uint64_t left=0;const auto cut=link.now;for(int ms=0;ms<1500&&!left;ms+=16){pts+=16667;if(!route(link,video(pts)))left=link.now;link.step(16);}
    require(left&&left-cut<=1300000&&link.causes.back()==1&&link.records.size()==2,"A UDP-only outage with WSS alive did not leave at once");++passed;}
  // Dark, then WSS comes back but UDP stays dead: it leaves within about a second more.
  {Link link;link.wssFeedback=true;auto pts=onUdp(link);play(link,300,pts);link.step(50);link.upCut=link.downCut=true;link.wssFeedback=false;
    play(link,2000,pts);require(link.stat("udpPath")==1,"Fixture: the hold");link.wssFeedback=true;const auto back=link.now;
    uint64_t left=0;for(int ms=0;ms<2500&&!left;ms+=16){pts+=16667;if(!route(link,video(pts)))left=link.now;link.step(16);}
    require(left&&left-back<=1300000&&link.records.back()==std::pair{udpPathOnWss,uint32_t(udpPathSilent)},"UDP stayed dead under a live WSS and did not leave");++passed;}
  // Every other reason records no cause.
  {Link link;onUdp(link);require(link.causes==std::vector<uint32_t>{0},"The switch to UDP carried a cause");++passed;}
}
void retry(){
  Link link;auto pts=onUdp(link);play(link,300,pts);link.step(50);
  const auto fail=[&]{link.upCut=link.downCut=true;const auto before=link.records.size();require(link.stepUntil([&]{return link.records.size()>before;},1500),"No fallback");require(link.records.back()==std::pair{udpPathOnWss,uint32_t(udpPathSilent)},"Wrong fallback");return link.now;};
  // The client sees the forced IDR on WebSocket, as the path rule says.
  const auto wssKey=[&]{pts+=1000;require(!route(link,video(pts,true)),"The forced IDR went on UDP");link.client.receiver->keyFrameOtherPath();};
  // One grant, one binding: nothing but this binding delivering again can bring UDP back,
  // here an outage outliving the backoff.
  auto left=fail();wssKey();link.step(40000);int forced=link.keyFrames;require(link.now-left>=30000000,"Test timing");require(link.keyFrames==forced&&!route(link,video(pts+1)),"A retry ran with the binding still dead");
  link.upCut=link.downCut=false;link.armed();pts+=1000;
  const auto before=link.client.delivered.size();const auto key=video(pts,true);require(route(link,key),"The retry's key frame stayed on WebSocket");play(link,3000,pts);
  require(link.client.delivered.size()>before+100&&link.client.delivered[before].bytes==key,"Video did not resume on the same grant");require(link.stat("udpPath")==1&&link.stat("udpFallbacks")==1,"The retry fell back");++passed;
  // The second failure waits 60 s, not 30, with the binding fine the whole time.
  left=fail();wssKey();link.upCut=link.downCut=false;forced=link.keyFrames;
  link.step(59000);require(link.keyFrames==forced,"The backoff did not double");link.armed();require(link.now-left>=60000000,"The doubled backoff was not waited out");pts+=1000;require(route(link,video(pts,true)),"The second retry did not switch");play(link,300,pts);link.step(50);
  // An outage that swallowed frames: nothing the client reports can move the Sender's clock
  // while the path is down, so it is as old as the outage when the retry starts. The key
  // frame still gets its second.
  {Link swallowed;auto at=onUdp(swallowed);play(swallowed,300,at);swallowed.step(50);swallowed.upCut=swallowed.downCut=true;uint64_t gone=0;
    for(int ms=0;ms<1500&&!gone;ms+=16){at+=16667;if(!route(swallowed,video(at)))gone=swallowed.now;swallowed.step(16);}require(gone,"No fallback under load");
    at+=1000;require(!route(swallowed,video(at,true)),"The forced IDR went on UDP");swallowed.client.receiver->keyFrameOtherPath();swallowed.upCut=swallowed.downCut=false;
    const int asked=swallowed.keyFrames;require(swallowed.stepUntil([&]{return swallowed.keyFrames>asked;},31000),"No retry after the backoff");at+=1000;require(route(swallowed,video(at,true)),"The retry's key frame stayed on WebSocket");
    play(swallowed,1500,at);require(swallowed.stat("udpPath")==1&&swallowed.stat("udpFallbacks")==1,"A retry fell back on the outage's old clock");++passed;}
  // Doubling stops at 5 minutes.
  for(const uint64_t wait:{120000000ull,240000000ull,300000000ull,300000000ull}){
    left=fail();wssKey();link.upCut=link.downCut=false;forced=link.keyFrames;link.step(int(wait/1000)-1000);require(link.keyFrames==forced,"Retried inside the backoff");link.armed();require(link.now-left<=wait+1000000,"The backoff ran past its cap");pts+=1000;require(route(link,video(pts,true)),"Later retry");play(link,300,pts);link.step(50);
  }
  ++passed;
}
void legEnds(){
  {Link link;onUdp(link);const int before=link.keyFrames;link.path.ended();require(!route(link,video(9000))&&link.keyFrames==before+1&&link.hasRecord(udpPathOnWss,udpPathSocketFailed)&&!link.paths.back(),"A dead leg left media on UDP");++passed;}
  // A leg closed on UDP hands media back at a forced IDR, then nothing reaches the session.
  {Link link;onUdp(link);const int before=link.keyFrames;link.path.detach();require(link.keyFrames==before+1&&!link.paths.back()&&!route(link,video(9000)),"A closed leg kept media on UDP");
    const auto paths=link.paths.size();const auto records=link.records.size();link.path.ended();require(link.keyFrames==before+1&&link.paths.size()==paths&&link.records.size()==records,"The session was called after detach");++passed;}
  {Link link;require(!route(link,video(1,true)),"First IDR");link.handshake();link.armed();link.path.detach();require(!route(link,video(2,true)),"A closed leg took a key frame");link.step(1500);require(link.keyFrames==1&&link.paths.empty(),"A closed leg armed again");++passed;}
  {Link link;link.path.ended();require(!route(link,video(1,true))&&link.keyFrames==0&&link.records.empty(),"A leg that never carried media reported a change");++passed;}
}
void tooLarge(){
  const auto huge=core::maxMessageBytes+1;
  {Link link;auto pts=onUdp(link);const int before=link.keyFrames;require(!route(link,video(pts+1,false,huge)),"An oversized P-frame took UDP");link.step(2);require(link.keyFrames==before+1&&link.hasRecord(udpPathOnWss,udpPathTooLarge),"An oversized P-frame forced no IDR");require(!route(link,video(pts+2)),"UDP kept the stream");++passed;}
  {Link link;auto pts=onUdp(link);const int before=link.keyFrames;require(!route(link,video(pts+1,true,huge)),"An oversized key frame took UDP");link.step(2);require(link.keyFrames==before&&link.hasRecord(udpPathOnWss,udpPathTooLarge),"An oversized key frame forced another");++passed;}
  {Link link;require(!route(link,video(1,true)),"First IDR");link.handshake();link.armed();require(!route(link,video(2,true,huge)),"An oversized key frame switched");link.step(2);const int before=link.keyFrames;link.step(5000);require(link.keyFrames==before,"An oversized key frame did not back off");++passed;}
  {Link link;auto pts=onUdp(link);require(!route(link,media(2,false,pts+1,core::fragmentPayload*core::maxAudioFragments)),"Oversized audio took UDP");require(route(link,video(pts+2)),"Oversized audio moved the path");++passed;}
}
void stalledLeg(){
  // The leg stops pumping: the pipe reader never waits, and asks for the IDR itself.
  Link link;auto pts=onUdp(link);const int before=link.keyFrames;size_t taken=0;
  for(;taken<64;++taken)if(!route(link,video(pts+taken+1,false,1024*1024)))break;
  require(taken>=8&&taken<20&&link.keyFrames==before+1,"A stalled leg was not bounded, or asked no IDR");require(!route(link,video(pts+100)),"UDP kept taking media");
  link.step(2);require(link.hasRecord(udpPathOnWss,udpPathStalled)&&!link.paths.back(),"The stall was not recorded");++passed;
}
void hostile(){
  // Before keys there is nothing to verify with, so nothing is read at all.
  {Link link;link.path.probePassed();for(const auto& plain:{feedbackPlain(1),keyframePlain(streamEpoch),nackPlain(1)}){const auto bytes=forged(plain,1);link.path.received(bytes,link.now);}link.step(50);require(link.keyFrames==0&&link.wire==0&&link.samples.empty(),"A datagram acted before keys");++passed;}
  // Everything right but the key, including the live challenge: no proof.
  {Link link;require(!route(link,video(1,true)),"First IDR");link.handshake();link.upCut=true;require(link.stepUntil([&]{return link.client.opened>0;},300),"No TAIL");
    core::DirectionKey other;std::fill_n(other.key.data(),core::keyBytes,uint8_t(0x5a));other.ready=true;core::Sealer wrong(other,clientLeg);
    const auto echo=feedbackPlain(link.client.challenge);Bytes sealedWrong(core::maxDatagram);sealedWrong.resize(wrong.seal(echo.data(),echo.size(),sealedWrong.data()));
    for(uint64_t n=1;n<=3;++n){link.path.received(forged(echo,n),link.now);link.path.received(sealedWrong,link.now);}
    Bytes tampered=link.client.seal(echo);tampered[core::headerBytes]^=1;link.path.received(tampered,link.now);
    const Bytes empty;link.path.received(empty,link.now);link.path.received(std::vector<uint8_t>(16,3),link.now);link.path.received(std::vector<uint8_t>(1200,3),link.now);link.path.received(std::vector<uint8_t>(5000,3),link.now);
    link.step(50);require(link.keyFrames==0&&!route(link,video(2,true)),"A forged FEEDBACK proved the address");
    // Control: the same plaintext, sealed, proves it.
    link.path.received(link.client.seal(echo),link.now);link.step(2);require(link.keyFrames==1,"The control FEEDBACK did not prove the address");++passed;}
  // On UDP: FEEDBACK, NACK and KEYFRAME without the key move nothing; with it they do.
  {Link link;auto pts=onUdp(link);play(link,100,pts);link.step(20);const auto retransmits=link.stat("udpRetransmits");const int keys=link.keyFrames;const auto samples=link.samples.size();
    link.upCut=true;  // only what this test hands over
    uint64_t sequence=1ull<<40;const auto attack=[&]{link.path.received(forged(feedbackPlain(link.client.challenge,1,1),sequence++),link.now);link.path.received(forged(nackPlain(1),sequence++),link.now);link.path.received(forged(keyframePlain(streamEpoch),sequence++),link.now);};
    attack();link.step(2);require(link.stat("udpRetransmits")==retransmits&&link.keyFrames==keys&&link.samples.size()==samples,"A forged control acted");
    // A forged FEEDBACK cannot hold the path open either.
    uint64_t left=0;for(int ms=0;ms<1500&&!left;ms+=16){attack();pts+=16667;if(!route(link,video(pts)))left=link.now;link.step(16);}require(left,"Forged FEEDBACK kept a dead path alive");++passed;}
  {Link link;auto pts=onUdp(link);play(link,100,pts);link.step(20);link.upCut=true;const int keys=link.keyFrames;
    link.path.received(link.client.seal(nackPlain(1)),link.now);link.step(50);require(link.stat("udpRetransmits")>=1,"The sealed control NACK was not answered");
    const auto sealed=link.client.seal(keyframePlain(streamEpoch));link.path.received(sealed,link.now);require(link.keyFrames==keys+1,"The sealed control KEYFRAME was not answered");
    link.step(500);link.path.received(sealed,link.now);require(link.keyFrames==keys+1,"A replayed KEYFRAME acted");++passed;}
  {Link link;auto pts=onUdp(link);play(link,100,pts);link.step(20);link.upCut=true;const auto sealed=link.client.seal(feedbackPlain(link.client.challenge,5,uint32_t(link.stat("udpDatagrams"))+1));const auto samples=link.samples.size();link.path.received(sealed,link.now);require(link.samples.size()==samples+1,"The control FEEDBACK was not counted");link.path.received(sealed,link.now);require(link.samples.size()==samples+1,"A replayed FEEDBACK counted");++passed;}
}
void keyframeRequests(){
  Link link;auto pts=onUdp(link);play(link,100,pts);link.step(20);link.upCut=true;const int keys=link.keyFrames;
  link.path.received(link.client.seal(keyframePlain(streamEpoch+1)),link.now);require(link.keyFrames==keys,"A KEYFRAME of another epoch forced an IDR");
  link.path.received(link.client.seal(keyframePlain(streamEpoch)),link.now);require(link.keyFrames==keys+1,"A KEYFRAME of this epoch forced no IDR");
  for(int n=0;n<50;++n)link.path.received(link.client.seal(keyframePlain(streamEpoch)),link.now);require(link.keyFrames==keys+1,"KEYFRAMEs were not rate limited");
  link.step(core::keyframeIntervalUs/1000);link.path.received(link.client.seal(keyframePlain(streamEpoch)),link.now);require(link.keyFrames==keys+2,"The client's repeat was starved by the limit");++passed;
  // While a key frame is on its way the request is already answered.
  {Link busy;auto at=onUdp(busy);busy.step(400);const int asked=busy.keyFrames;require(route(busy,video(at+1,true,300000)),"Key frame");busy.step(2);busy.path.received(busy.client.seal(keyframePlain(streamEpoch)),busy.now);require(busy.keyFrames==asked,"A KEYFRAME during a key frame forced a second IDR");++passed;}
  // Off UDP, WebSocket has its own way to ask.
  {Link wss;require(!route(wss,video(1,true)),"First IDR");wss.handshake();wss.upCut=true;require(wss.stepUntil([&]{return wss.client.opened>0;},300),"No TAIL");wss.path.received(wss.client.seal(feedbackPlain(wss.client.challenge)),wss.now);wss.step(2);require(wss.keyFrames==1,"Arming");wss.path.received(wss.client.seal(keyframePlain(streamEpoch)),wss.now);require(wss.keyFrames==1,"A KEYFRAME acted off UDP");++passed;}
}
// LOSS: the encoder is asked to code from the client's last good frame, the KEYFRAME
// that came with it is merged, and the marked answer reaches the client as a recovery
// frame that ends its wait. Unanswered, the client's repeat still gets an IDR.
void lossAnswers(){
  const auto lose=[](Link& link,uint64_t& pts){
    // Downlink gone for a few frames and their repairs, then the uplink too, past the deadline.
    link.downCut=true;play(link,80,pts);link.downCut=false;link.upCut=true;play(link,120,pts);link.upCut=false;
  };
  {Link link;auto pts=onUdp(link);play(link,300,pts);link.step(20);
    const uint64_t good=link.client.delivered.back().pts;const int keys=link.keyFrames;
    lose(link,pts);require(link.stepUntil([&]{return !link.recoveries.empty();},700),"No LOSS reached the encoder");
    require(link.recoveries==std::vector<uint64_t>{good},"The encoder was not told the client's last good frame");
    link.step(20);require(link.keyFrames==keys,"The KEYFRAME that came with the LOSS forced an IDR");
    require(link.client.receiver->awaitingRecovery(),"Fixture: the client is not waiting for a recovery");
    pts+=16667;auto answer=video(pts,false,3000);require(link.path.outgoing(false,answer,true),"The answer left UDP");
    require(link.stepUntil([&]{return !link.client.receiver->awaitingKey();},200)&&link.client.delivered.back().pts==pts,"The marked answer did not end the client's wait");
    play(link,300,pts);require(link.keyFrames==keys&&link.client.delivered.back().pts==pts&&link.stat("udpRecoveries")==1,"Video did not go on after the recovery, or it cost an IDR");
    require(link.stat("udpFallbacks")==0,"A loss answered by recovery fell back");++passed;}
  {Link link;auto pts=onUdp(link);play(link,300,pts);link.step(20);const int keys=link.keyFrames;
    lose(link,pts);require(link.stepUntil([&]{return !link.recoveries.empty();},700),"No LOSS reached the encoder");
    // The worker never answers: the next repeat is not merged and forces an IDR.
    require(link.stepUntil([&]{return link.keyFrames>keys;},700),"An unanswered LOSS left the client without an IDR");++passed;}
  {Link link;auto pts=onUdp(link);play(link,100,pts);link.step(20);link.upCut=true;
    Bytes loss(core::lossBytes);loss[0]=core::kindLoss;core::put16(&loss[2],streamEpoch);core::put64(&loss[8],pts);
    link.path.received(forged(loss,1ull<<41),link.now);require(link.recoveries.empty(),"A forged LOSS reached the encoder");
    core::put16(&loss[2],streamEpoch+1);link.path.received(link.client.seal(loss),link.now);require(link.recoveries.empty(),"A LOSS of another epoch reached the encoder");
    core::put16(&loss[2],streamEpoch);core::put64(&loss[8],0);link.path.received(link.client.seal(loss),link.now);require(link.recoveries.empty(),"A LOSS naming no frame reached the encoder");
    core::put64(&loss[8],pts);for(int n=0;n<20;++n)link.path.received(link.client.seal(loss),link.now);require(link.recoveries.size()==1,"LOSS was not rate limited");++passed;}
}
void bitrate(){
  // No FEEDBACK reaches the controller until media is on UDP.
  Link link;link.upDelayUs=20000;require(!route(link,video(1,true)),"First IDR");link.handshake();link.armed();link.step(500);require(link.samples.empty(),"FEEDBACK fed the controller off UDP");
  uint64_t pts=2;require(route(link,video(pts,true)),"Switch");play(link,1000,pts);link.step(50);
  require(link.samples.size()>=8,"FEEDBACK did not reach the controller");
  uint64_t received=0,lost=0,rtts=0;for(const auto& sample:link.samples){received+=sample.received;lost+=sample.lost;if(sample.rtt){++rtts;require(sample.rtt>=19000&&sample.rtt<=22000,"The round trip is not the link's 20 ms");}}
  require(received*2>=uint64_t(link.stat("udpDatagrams"))&&lost==0&&rtts>=4,"FEEDBACK deltas or round trips were wrong");++passed;
  require(link.client.receiver->rttUs()>=19000&&link.client.receiver->rttUs()<=22000,"TAIL did not carry the Host's RTT to the client");++passed;
  // A report the network reordered does not reach the controller: its deltas would wrap.
  {Link late;auto at=onUdp(late);play(late,100,at);late.step(20);late.upCut=true;
   const auto samples=late.samples.size();const uint32_t count=uint32_t(late.stat("udpDatagrams"))+100;
   late.path.received(late.client.seal(feedbackPlain(late.client.challenge,5,count)),late.now);require(late.samples.size()==samples+1,"A new report was not counted");
   late.path.received(late.client.seal(feedbackPlain(late.client.challenge,5,count-50)),late.now);require(late.samples.size()==samples+1,"A reordered report reached the controller");++passed;}
  // Its output paces: 15 Mbps of frames pass at a 50 Mbps target and not at 2 Mbps.
  const auto rate=[&](int target){link.target=target;link.step(150);const auto before=link.wire;for(int at=0;at<300;at+=16){pts+=16667;route(link,video(pts,false,30000));link.step(16);}return link.wire-before;};
  const auto high=rate(50000000);const auto low=rate(2000000);require(low*2<high,"setTarget did not reach the pacer");++passed;
  // A refused P-frame is the backlog signal, and asks for the key frame it needs.
  Link slow;slow.target=20000000;auto at=onUdp(slow);slow.target=1000000;play(slow,200,at);const int keys=slow.keyFrames;for(int n=0;n<30;++n){at+=16667;route(slow,video(at,false,300000));slow.step(16);}
  require(slow.backlogs>=1&&slow.keyFrames>keys,"A pacer backlog raised nothing");++passed;
  require(slow.stat("udpBacklogKeys")>=1&&slow.stat("udpClientKeys")==0,"Stats did not count the pacer's key frame request");++passed;
  // A client KEYFRAME request the path honours is counted apart from the pacer's.
  {Link asked;auto from=onUdp(asked);play(asked,100,from);asked.step(600);const int before=asked.keyFrames;
   asked.path.received(asked.client.seal(keyframePlain(1)),asked.now);asked.step(5);
   require(asked.keyFrames==before+1&&asked.stat("udpClientKeys")==1&&asked.stat("udpBacklogKeys")==0,"A client KEYFRAME request was not counted");++passed;}
}
void statistics(){
  Link link;const auto clean=textBytes(R"({"type":"stream.stats","encodedFps":60})");auto text=clean;link.path.probePassed();link.step(5);require(!link.path.outgoing(true,text)&&text==clean,"Stats changed with no key exchange");
  auto other=textBytes(R"({"type":"stream.info"})");require(!link.path.outgoing(true,other),"Text was taken");
  link.handshake();link.step(5);require(link.stat("udpPath")==0&&link.stat("udpDatagrams")>=1&&link.stat("udpRetransmits")==0&&link.stat("udpFallbacks")==0&&link.stat("udpBacklogKeys")==0&&link.stat("udpClientKeys")==0&&link.stat("udpRefused")==0,"Stats fields missing after the exchange");++passed;
}
void exhausted(){
  Link link;auto pts=onUdp(link);play(link,100,pts);const int keys=link.keyFrames;UdpMediaPathTest::nextSequence(link.path,core::sequenceLimit);link.step(2);
  require(link.hasRecord(udpPathOnWss,udpPathExhausted)&&link.keyFrames==keys+1&&!route(link,video(pts+1)),"The sequence limit left media on UDP");
  link.step(400000);require(link.keyFrames==keys+1,"A spent sequence space was retried");++passed;
}
void sessionSignals(){
  // What replaces the WebSocket queue while on UDP.
  AdaptiveBitrateSession session;std::vector<int> published;session.start("s",{true,20000000,100000000},60,[&](const std::string&,int value){published.push_back(value);});
  Sleep(200);session.queue(0,8*1024*1024,1);require(published.size()==1,"The queue guard did not act on WebSocket");Sleep(200);
  session.udpPath(true);const auto cuts=published.size();session.queue(0,8*1024*1024,1);require(published.size()==cuts,"The WebSocket queue moved a UDP stream");
  Sleep(350);  // past the guard's own 300 ms, or this report could not cut with or without the gate
  session.feedback(JsonParser::parse(R"({"type":"stream.feedback","sessionId":"s","epoch":0,"bytes":1,"intervalUs":100000,"relayQueuedBytes":99999999,"frames":[]})"));require(published.size()==cuts,"stream.feedback moved a UDP stream");
  require(session.udpFeedback(1000,10,0)==session.target(),"FEEDBACK did not return the target");
  const auto before=session.target();session.udpBacklog();require(session.target()<before&&published.size()==cuts+1,"The pacer backlog did not cut");Sleep(200);
  session.udpPath(false);session.queue(0,8*1024*1024,1);require(published.size()==cuts+2,"The queue guard did not come back");
  session.udpPath(true);session.start("s",{true,20000000,100000000},60,[&](const std::string&,int value){published.push_back(value);});Sleep(200);const auto now=published.size();session.queue(0,8*1024*1024,1);require(published.size()==now+1,"A new stream inherited the UDP path");
  session.stop();require(session.udpFeedback(1000,1,0)==0,"A stopped session answered");++passed;
}
}
int main(){
  try{
    for(const auto& scenario:{exchangeRules,proofRequired,switching,firstIdrStaysOnWss,directInputGates,staleProof,fallback,retry,legEnds,tooLarge,stalledLeg,hostile,keyframeRequests,lossAnswers,bitrate,statistics,exhausted,sessionSignals})scenario();
    std::cout<<"{\"kind\":\"udp-media-path\",\"passed\":"<<passed<<",\"link\":\"simulated\",\"sockets\":false}\n";return 0;
  }catch(const std::exception& error){std::cerr<<"UDP media path check failed: "<<error.what()<<"\n";return 1;}
}
