// Two independent real UDP outputs, synthetic media and in-memory broker
// controls. No installed service, display, decoder or OS input is exercised.
#include "udp_tracks_session.hpp"
#include "../../windows_client/windows/runner/udp_media_leg.h"
#include <iostream>
using namespace host;
namespace core=multitap::udp;
static void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
static std::string base64(std::span<const uint8_t> bytes){
  DWORD size=0;require(CryptBinaryToStringA(bytes.data(),DWORD(bytes.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&size),"base64 size");
  std::string text(size,'\0');require(CryptBinaryToStringA(bytes.data(),DWORD(bytes.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,text.data(),&size),"base64");text.resize(size);return text;
}
struct Relay {
  UdpProbeSocket socket;uint16_t port=0;UdpGrant hg,cg;std::array<UdpLegDatagrams,2> proof;std::array<sockaddr_in,2> addresses{};
  std::atomic<bool> stopped{false},blockData{false};std::atomic<uint64_t> forwarded{0};std::mutex mutex;std::thread thread;
  explicit Relay(uint8_t seed,bool direct=true){
    require(socket.open(),"relay socket");sockaddr_in own{};int size=sizeof(own);require(!getsockname(socket.handle,reinterpret_cast<sockaddr*>(&own),&size),"relay port");port=ntohs(own.sin_port);
    for(size_t n=0;n<8;++n){hg.id[n]=uint8_t(seed+n);cg.id[n]=uint8_t(seed+11+n);}for(size_t n=0;n<32;++n){hg.ticket[n]=uint8_t(seed+n);cg.ticket[n]=uint8_t(seed+51+n);}
    hg.relays={{"127.0.0.1",port}};cg.relays=hg.relays;hg.direct=cg.direct=direct;
    require(udpLegDatagrams(hg,proof[0])&&udpLegDatagrams(cg,proof[1]),"proofs");thread=std::thread([this]{run();});
  }
  ~Relay(){stopped=true;thread.join();}
  void send(std::span<const uint8_t> bytes,const sockaddr_in& target){sendto(socket.handle,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<const sockaddr*>(&target),sizeof(target));}
  void run(){while(!stopped){std::array<uint8_t,1201> bytes{};sockaddr_in from{};int fromSize=sizeof(from);const int size=recvfrom(socket.handle,reinterpret_cast<char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&from),&fromSize);if(size==SOCKET_ERROR){Sleep(1);continue;}if(size<9)continue;
    for(size_t n=0;n<2;++n){if(!std::equal(proof[n].id.begin(),proof[n].id.end(),bytes.begin()+1))continue;
      if(size==int(udpBindBytes)&&std::equal(proof[n].bind.begin(),proof[n].bind.end(),bytes.begin())){std::lock_guard lock(mutex);addresses[n]=from;send(proof[n].ack,from);}
      else if(bytes[0]==udpTypeData&&!blockData){std::lock_guard lock(mutex);if(udpEndpoint(from)==udpEndpoint(addresses[n])&&addresses[1-n].sin_port){send(std::span<const uint8_t>(bytes.data(),size_t(size)),addresses[1-n]);++forwarded;}}
    }
  }}
  uint16_t peerPort(size_t n){std::lock_guard lock(mutex);return ntohs(addresses[n].sin_port);}
  std::string grant()const{return std::string("{\"type\":\"udp.grant\",\"protocol\":\"udp-media-v1\"")+(hg.direct?",\"directProtocol\":\"udp-direct-v1\"":"")+",\"keys\":true,\"expiresAt\":9007199254740991,\"id\":"+jsonQuote(base64(hg.id))+",\"ticket\":"+jsonQuote(base64(hg.ticket))+",\"relays\":[{\"host\":\"127.0.0.1\",\"port\":"+std::to_string(port)+"}]}";}
};
struct Test {
  std::mutex mutex;std::vector<Json> controls,inputs;std::atomic<bool> key{true};std::atomic<uint64_t> invalid{0};
  UdpTracksSession host{"dual-loopback",{[this](const std::string& text){std::lock_guard lock(mutex);controls.push_back(JsonParser::parse(text));return true;},[this](const Json& input){std::lock_guard lock(mutex);inputs.push_back(input);},[this]{key=true;}},true};
  std::array<core::TrackBinding,2> bindings{{{0,7,11,true},{1,7,12,false}}};
  uint64_t pts=1;uint32_t generation=0;
  std::array<std::unique_ptr<Relay>,2> relays;
  std::array<std::shared_ptr<multitap::UdpPathGate>,2> gates;
  std::array<std::unique_ptr<multitap::UdpMediaLeg>,2> clients;
  std::array<std::atomic<uint64_t>,2> video{},udpVideo{},audio{},wssOffered{},consumed{};
  std::array<bool,2> keyed{},peered{};
  std::array<std::string,2> publicKeys{};
  ~Test(){host.close();for(auto& client:clients)client.reset();for(auto& gate:gates)if(gate)gate->Attach({});}
  std::string scope(size_t n,uint32_t gen=0)const{return "\"sessionId\":\"dual-loopback\",\"track\":"+jsonQuote(std::to_string(n))+",\"topologyEpoch\":"+std::to_string(bindings[n].topology)+",\"epoch\":"+std::to_string(bindings[n].epoch)+",\"generation\":"+std::to_string(gen?gen:generation);}
  void control(const std::string& text){require(host.control(JsonParser::parse(text)),"track control not consumed");}
  void configure(){
    const std::string text="{\"type\":\"stream.tracks.info\",\"sessionId\":\"dual-loopback\",\"topologyEpoch\":"+std::to_string(bindings[0].topology)+",\"audio\":{\"format\":\"s16le\"},\"tracks\":[{\"id\":\"0\",\"epoch\":"+std::to_string(bindings[0].epoch)+",\"primary\":true,\"bitrateTarget\":12000000},{\"id\":\"1\",\"epoch\":"+std::to_string(bindings[1].epoch)+",\"primary\":false,\"bitrateTarget\":12000000}]}";
    std::vector<uint8_t> bytes(text.begin(),text.end());require(!host.outgoing(true,bytes),"configuration bypassed WSS");
  }
  void round(uint32_t gen,bool direct=true){
    for(auto& client:clients)client.reset();for(auto& gate:gates)if(gate)gate->Attach({});
    generation=gen;keyed={};peered={};publicKeys={};key=true;
    for(size_t n=0;n<2;++n){
      relays[n]=std::make_unique<Relay>(uint8_t(gen*41+n*17),direct);gates[n]=std::make_shared<multitap::UdpPathGate>();
      const auto binding=bindings[n];gates[n]->Attach([this,n,binding](const std::vector<uint8_t>& packet,bool watched){
        auto restored=packet;
        // UdpPathGate validates the received epoch and zeroes it before the
        // stable decoder API. The immutable leg binding restores it here.
        if(restored.size()>=4){restored[2]=uint8_t(binding.epoch);restored[3]=uint8_t(binding.epoch>>8);}
        std::vector<uint8_t> tvt;multitap::tracks::Packet parsed;
        if(!core::udpToTrack(binding,restored,tvt)||!multitap::tracks::parse(tvt.data(),tvt.size(),parsed)||parsed.topology!=binding.topology){++invalid;return multitap::MediaPushResult::invalid;}
        if(parsed.kind==1){if(parsed.track!=n||parsed.epoch!=binding.epoch||parsed.payload[parsed.size-1]!=uint8_t(30+n)){++invalid;return multitap::MediaPushResult::invalid;}++video[n];if(watched)++udpVideo[n];}
        else ++audio[n];return multitap::MediaPushResult::accepted;
      });gates[n]->Stream(uint16_t(binding.epoch),gen);gates[n]->Engage(false);
      multitap::UdpMediaLeg::Grant grant;grant.relays={{"127.0.0.1",relays[n]->port}};grant.id=relays[n]->cg.id;grant.ticket=relays[n]->cg.ticket;grant.sessionId="dual-loopback";grant.direct=direct;
      clients[n]=std::make_unique<multitap::UdpMediaLeg>(std::move(grant),gates[n],0);
    }
    control("{\"type\":\"udp.tracks.grant\",\"protocol\":\"udp-tracks-v1\",\"sessionId\":\"dual-loopback\",\"topologyEpoch\":"+std::to_string(bindings[0].topology)+",\"generation\":"+std::to_string(gen)+",\"tracks\":[{\"track\":\"0\",\"epoch\":"+std::to_string(bindings[0].epoch)+",\"primary\":true,\"bitrateTarget\":12000000,\"grant\":"+relays[0]->grant()+"},{\"track\":\"1\",\"epoch\":"+std::to_string(bindings[1].epoch)+",\"primary\":false,\"bitrateTarget\":12000000,\"grant\":"+relays[1]->grant()+"}]}");
  }
  void lease(){control("{\"type\":\"udp.tracks.lease\",\"protocol\":\"udp-tracks-v1\",\"sessionId\":\"dual-loopback\",\"topologyEpoch\":"+std::to_string(bindings[0].topology)+",\"generation\":"+std::to_string(generation)+'}');}
  void tick(bool leased=true){
    for(size_t n=0;n<2;++n)if(clients[n]){
      const auto read=clients[n]->Read();
      if(read.publicKey&&!keyed[n]){publicKeys[n]=base64(*read.publicKey);control("{\"type\":\"udp.tracks.key\",\"protocol\":\"udp-media-v1\","+scope(n)+",\"publicKey\":"+jsonQuote(publicKeys[n])+'}');}
      if(keyed[n]&&!peered[n]&&relays[n]->peerPort(0)&&relays[n]->peerPort(1)){
        control("{\"type\":\"udp.tracks.peer\",\"protocol\":\"udp-direct-v1\","+scope(n)+",\"candidates\":[{\"address\":\"127.0.0.1\",\"port\":"+std::to_string(relays[n]->peerPort(1))+"}]}");
        clients[n]->Peer({{"127.0.0.1",relays[n]->peerPort(0)}});peered[n]=true;
      }
    }
    if(leased)lease();
    std::vector<Json> pending;{std::lock_guard lock(mutex);pending.swap(controls);}
    for(const auto& message:pending)if(message.get("type").string()=="udp.tracks.key"){
      const size_t n=message.get("track").string()=="0"?0:1;
      if(!clients[n]||!udpTrackScopeMatches(message,{std::to_string(n),bindings[n],generation,12000000}))continue;
      core::PublicKey publicKey{};require(udpBase64(message.get("publicKey").string(),publicKey),"Host public key");clients[n]->HostKey(publicKey);keyed[n]=true;
    }
    const bool idr=key.exchange(false);
    for(size_t n=0;n<2;++n){
      std::vector<uint8_t> payload(1000,uint8_t(30+n)),bytes;require(multitap::tracks::pack({1,uint8_t(idr?1:0),uint8_t(n),bindings[n].topology,bindings[n].epoch,pts,payload.data(),payload.size()},bytes),"video pack");
      if(host.outgoing(false,bytes))++consumed[n];else{std::vector<uint8_t> normalized;require(core::trackToUdp(bindings[n],bytes,normalized),"WSS video normalization");++wssOffered[n];gates[n]->OfferWss(normalized,false);}
    }
    std::array<uint8_t,4> sound{1,2,3,4};std::vector<uint8_t> bytes;require(multitap::tracks::pack({2,0,255,bindings[0].topology,0,pts,sound.data(),sound.size()},bytes),"audio pack");
    if(!host.outgoing(false,bytes)){std::vector<uint8_t> normalized;require(core::trackToUdp(bindings[0],bytes,normalized),"WSS audio normalization");gates[0]->OfferWss(normalized,false);}
    ++pts;Sleep(10);
  }
  template<class Predicate> void until(Predicate predicate,const char* why,uint64_t limit=15000){const auto start=GetTickCount64();while(!predicate()){require(GetTickCount64()-start<limit,why);tick();}}
  bool direct(){for(size_t n=0;n<2;++n)if(!clients[n]||clients[n]->Read().directPath!=2||gates[n]->view().active!=1)return false;return true;}
  void wssInput(uint32_t sequence,const std::string& body){control("{\"type\":\"input.direct\",\"protocol\":\"udp-direct-v1\",\"sessionId\":\"dual-loopback\",\"sequence\":"+std::to_string(sequence)+",\"epoch\":"+std::to_string(bindings[0].epoch)+",\"input\":"+body+'}');}
  size_t inputCount(){std::lock_guard lock(mutex);return inputs.size();}
};
int main(){try{
  Test test;test.configure();test.round(1);test.until([&]{return test.direct();},"both outputs did not become direct");
  const std::array<uint64_t,2> before{test.udpVideo[0].load(),test.udpVideo[1].load()},forwarded{test.relays[0]->forwarded.load(),test.relays[1]->forwarded.load()};
  for(auto& relay:test.relays)relay->blockData=true;
  test.until([&]{return test.udpVideo[0]>=before[0]+40&&test.udpVideo[1]>=before[1]+40;},"two direct media outputs stalled after relay disable");
  for(size_t n=0;n<2;++n)require(test.relays[n]->forwarded<=forwarded[n]+2,"relay still forwarded direct media");
  require(test.audio[0]>0&&test.audio[1]==0&&test.invalid==0,"cross-output media or duplicated audio");
  const auto oldKeys=test.publicKeys;
  test.wssInput(1,R"({"type":"input.fence"})");test.tick();
  const std::string move=R"({"type":"input","event":"move","x":0.31,"y":0.47,"track":"1","topologyEpoch":7,"epoch":12})";
  test.clients[1]->Input(2,12,{move.begin(),move.end()});test.until([&]{return test.inputCount()==1;},"direct secondary input did not arrive");
  test.wssInput(2,move);test.wssInput(3,R"({"type":"input","event":"key","vk":65,"scan":30,"down":false,"extended":false,"topologyEpoch":7})");
  test.until([&]{return test.inputCount()==2;},"ordered reliable key release missing");
  // One stopped output cannot stop or steal the other output's UDP path.
  test.clients[1]->Stop();test.clients[1].reset();const auto stillDirect=test.udpVideo[0].load();
  test.until([&]{return test.gates[1]->view().active==0&&test.wssOffered[1]>10&&test.udpVideo[0]>stillDirect+40;},"independent WSS fallback failed");
  require(test.gates[0]->view().active==1,"secondary failure changed primary path");
  test.host.suspend();test.key=true;for(unsigned n=0;n<5;++n)test.tick();require(test.gates[0]->view().active==0&&test.gates[1]->view().active==0,"full WSS fallback failed");
  // A new generation with identical topology/epochs must reject old keys and
  // preserve the session's input sequence. A consumed stale key breaks ECDH.
  test.round(2);for(size_t n=0;n<2;++n)test.control("{\"type\":\"udp.tracks.key\",\"protocol\":\"udp-media-v1\","+test.scope(n,1)+",\"publicKey\":"+jsonQuote(oldKeys[n])+'}');
  test.until([&]{return test.direct();},"regrant consumed an old generation key");
  test.wssInput(2,move);test.wssInput(4,R"({"type":"input","event":"key","vk":66,"scan":48,"down":false,"extended":false,"topologyEpoch":7})");
  test.until([&]{return test.inputCount()==3;},"session dispatcher lost sequence across regrant");
  const auto previousBindings=test.bindings;const auto previousKeys=test.publicKeys;
  test.bindings={{{0,8,21,true},{1,8,22,false}}};test.configure();
  std::array<uint8_t,4> stalePayload{1,2,3,4};std::vector<uint8_t> staleMedia;
  require(multitap::tracks::pack({1,1,1,7,12,test.pts,stalePayload.data(),stalePayload.size()},staleMedia)&&!test.host.outgoing(false,staleMedia),"old topology media entered UDP");
  test.round(3);
  for(size_t n=0;n<2;++n)test.control("{\"type\":\"udp.tracks.key\",\"protocol\":\"udp-media-v1\",\"sessionId\":\"dual-loopback\",\"track\":"+jsonQuote(std::to_string(n))+",\"topologyEpoch\":"+std::to_string(previousBindings[n].topology)+",\"epoch\":"+std::to_string(previousBindings[n].epoch)+",\"generation\":3,\"publicKey\":"+jsonQuote(previousKeys[n])+'}');
  test.until([&]{return test.direct();},"new topology consumed an old epoch key");
  test.wssInput(4,move);test.wssInput(5,R"({"type":"input.reset"})");test.until([&]{return test.inputCount()==4;},"topology change lost sequence or reset");
  for(auto& relay:test.relays)relay->blockData=true;
  // With live P2P but no current broker lease, no fast input may remain active.
  const auto darkAt=GetTickCount64();while(GetTickCount64()-darkAt<2200)test.tick(false);
  const std::string newMove=R"({"type":"input","event":"move","x":0.31,"y":0.47,"track":"1","topologyEpoch":8,"epoch":22})";
  test.clients[1]->Input(6,22,{newMove.begin(),newMove.end()});for(unsigned n=0;n<30;++n)test.tick(false);require(test.inputCount()==4,"expired lease accepted direct input");
  const std::array<uint64_t,2> relayBefore{test.udpVideo[0].load(),test.udpVideo[1].load()};test.round(4,false);
  test.until([&]{return test.gates[0]->view().active==1&&test.gates[1]->view().active==1&&test.udpVideo[0]>relayBefore[0]+10&&test.udpVideo[1]>relayBefore[1]+10;},"relay-only encrypted output negotiation failed");
  for(size_t n=0;n<2;++n)require(test.relays[n]->forwarded>10&&test.clients[n]->Read().directPath!=2,"relay-only output claimed P2P");
  test.host.close();const auto afterClose=test.inputCount();test.wssInput(7,R"({"type":"input.reset"})");Sleep(30);require(test.inputCount()==afterClose,"closed session accepted input");
  require(test.invalid==0,"track binding corruption");
  std::cout<<"{\"kind\":\"udp-tracks-real-legs\",\"outputs\":2,\"directFramesPerOutput\":40,\"relayForwardingDisabled\":true,\"secondaryFallback\":true,\"fullWssFallback\":true,\"regrantGeneration\":4,\"topologyReset\":true,\"relayOnly\":true,\"orderedInputs\":"<<test.inputCount()<<",\"leaseExpiry\":true,\"realInput\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
