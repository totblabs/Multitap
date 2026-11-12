// Real Host and Windows client legs on isolated UDP sockets. Synthetic media
// goes to a fake decoder; no display, input, service or installed app is touched.
#include "udp_probe_leg.hpp"
#include "../../windows_client/windows/runner/udp_media_leg.h"
#include <iostream>
using namespace host;
namespace core=multitap::udp;
static void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
static sockaddr_in loopback(uint16_t port){sockaddr_in out{};out.sin_family=AF_INET;out.sin_addr.s_addr=htonl(INADDR_LOOPBACK);out.sin_port=htons(port);return out;}
static std::string base64(std::span<const uint8_t> bytes){DWORD size=0;require(CryptBinaryToStringA(bytes.data(),DWORD(bytes.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&size),"base64 size");std::string text(size,'\0');require(CryptBinaryToStringA(bytes.data(),DWORD(bytes.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,text.data(),&size),"base64");text.resize(size);return text;}
struct Relay {
  UdpProbeSocket socket;uint16_t port=0;UdpGrant hg,cg;std::array<UdpLegDatagrams,2> proof;std::array<sockaddr_in,2> addresses{};
  std::atomic<bool> stopped{false},blockData{false};std::atomic<uint64_t> forwarded{0};std::mutex mutex;std::thread thread;
  Relay(){
    require(socket.open(),"relay socket");sockaddr_in own{};int size=sizeof(own);require(!getsockname(socket.handle,reinterpret_cast<sockaddr*>(&own),&size),"relay port");port=ntohs(own.sin_port);
    for(size_t n=0;n<8;++n){hg.id[n]=uint8_t(n+1);cg.id[n]=uint8_t(n+11);}for(size_t n=0;n<32;++n){hg.ticket[n]=uint8_t(n+1);cg.ticket[n]=uint8_t(n+51);}
    hg.relays={{"127.0.0.1",port}};cg.relays=hg.relays;hg.direct=cg.direct=true;
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
};
static std::vector<uint8_t> video(uint64_t pts,bool key){std::vector<uint8_t> bytes(1012,7);bytes[0]=1;bytes[1]=key?1:0;bytes[2]=1;bytes[3]=0;for(size_t n=0;n<8;++n)bytes[4+n]=uint8_t(pts>>(8*n));return bytes;}
int main(){try{
  Relay relay;std::atomic<bool> key{true};std::atomic<uint64_t> delivered{0},inputReceived{0};
  UdpProbeLegOptions options;options.media.keyFrame=[&]{key=true;};options.media.path=[](bool){return 12000000;};
  options.media.input=[&](uint32_t sequence,uint16_t epoch,const std::string& body){if(sequence==1&&epoch==1&&body.find("\"event\":\"key\"")!=std::string::npos)++inputReceived;};
  const auto host=UdpProbeLeg::start(relay.hg,std::move(options));require(bool(host),"host leg");
  struct Close{std::shared_ptr<UdpProbeLeg> leg;~Close(){leg->close();leg->waitClosed(1000);}}close{host};
  const auto gate=std::make_shared<multitap::UdpPathGate>();gate->Attach([&](const std::vector<uint8_t>&,bool){++delivered;return multitap::MediaPushResult::accepted;});gate->Stream(1,1);gate->Engage(true);
  multitap::UdpMediaLeg::Grant grant;grant.relays={{"127.0.0.1",relay.port}};grant.id=relay.cg.id;grant.ticket=relay.cg.ticket;grant.sessionId="direct-loopback";grant.direct=true;
  multitap::UdpMediaLeg client(std::move(grant),gate,0);
  bool keyed=false,peered=false,blocked=false;uint64_t pts=1,after=0,forwarded=0;const auto start=GetTickCount64();
  for(;;){
    const auto read=client.Read();
    if(read.publicKey&&!keyed){const auto message=JsonParser::parse("{\"protocol\":\"udp-media-v1\",\"publicKey\":\""+base64(*read.publicKey)+"\"}");if(const auto reply=host->exchange(message,"direct-loopback",12000000)){core::PublicKey publicKey{};require(udpBase64(JsonParser::parse(*reply).get("publicKey").string(),publicKey),"host public key");client.HostKey(publicKey);keyed=true;}}
    if(keyed&&!peered&&relay.peerPort(0)&&relay.peerPort(1)){host->peer(JsonParser::parse("{\"protocol\":\"udp-direct-v1\",\"candidates\":[{\"address\":\"127.0.0.1\",\"port\":"+std::to_string(relay.peerPort(1))+"}]}"));client.Peer({{"127.0.0.1",relay.peerPort(0)}});peered=true;}
    host->wssHeard();auto bytes=video(pts++,key.exchange(false));if(!host->outgoing(false,bytes))gate->OfferWss(bytes,false);
    if(read.directPath==2&&gate->view().active==1&&!blocked){blocked=true;relay.blockData=true;after=delivered.load();forwarded=relay.forwarded.load();const std::string body=R"({"type":"input","event":"key","vk":65,"scan":30,"down":true,"extended":false})";client.Input(1,1,{body.begin(),body.end()});}
    if(blocked&&delivered.load()>=after+40){require(relay.forwarded.load()<=forwarded+2,"media still crossed the relay");require(read.directPath==2,"direct path was lost");require(inputReceived==1,"encrypted direct input did not arrive exactly once");std::cout<<"{\"kind\":\"udp-direct-real-legs\",\"directFrames\":"<<delivered.load()-after<<",\"directInputPackets\":"<<inputReceived.load()<<",\"relayForwardingDisabled\":true,\"directRttMs\":"<<read.directRttMs<<",\"relayRttMs\":"<<read.relayRttMs<<"}\n";break;}
    require(GetTickCount64()-start<15000,"direct media timeout");Sleep(10);
  }
  return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
