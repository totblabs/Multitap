// The real udp-media-v1 leg (socket, thread, HMAC, event loop) against a small local
// peer that plays the relay on 127.0.0.1. Loopback only: this says nothing about a
// real network, a NAT or the actual relay.
#include "udp_probe_leg.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
using namespace host;
namespace {
int passed=0;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
using Bytes=std::vector<uint8_t>;
struct Winsock{Winsock(){WSADATA data{};require(!WSAStartup(MAKEWORD(2,2),&data),"Winsock unavailable");}~Winsock(){WSACleanup();}};
sockaddr_in loopback(uint16_t port){sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);return address;}
// A UDP socket on 127.0.0.1 with bounded receives; it answers only when told to.
struct Peer {
  SOCKET handle=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);uint16_t port=0;sockaddr_in leg{};
  Peer(){require(handle!=INVALID_SOCKET,"Peer socket failed");const auto any=loopback(0);require(!::bind(handle,reinterpret_cast<const sockaddr*>(&any),sizeof(any)),"Peer bind failed");sockaddr_in bound{};int size=sizeof(bound);require(!getsockname(handle,reinterpret_cast<sockaddr*>(&bound),&size),"Peer name failed");port=ntohs(bound.sin_port);
    int buffer=1<<20;setsockopt(handle,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer));BOOL report=FALSE;DWORD used=0;WSAIoctl(handle,SIO_UDP_CONNRESET,&report,sizeof(report),nullptr,0,&used,nullptr,nullptr);}
  ~Peer(){closesocket(handle);}
  Peer(const Peer&)=delete;Peer& operator=(const Peer&)=delete;
  std::optional<Bytes> receive(DWORD milliseconds){
    DWORD timeout=milliseconds?milliseconds:1;setsockopt(handle,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    Bytes bytes(1500);sockaddr_in from{};int size=sizeof(from);const int got=recvfrom(handle,reinterpret_cast<char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&from),&size);
    if(got==SOCKET_ERROR)return {};leg=from;bytes.resize(size_t(got));return bytes;
  }
  void send(std::span<const uint8_t> bytes,const sockaddr_in& to){sendto(handle,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<const sockaddr*>(&to),sizeof(to));}
  void reply(std::span<const uint8_t> bytes){send(bytes,leg);}
};
struct Record {
  std::mutex mutex;std::vector<std::pair<uint32_t,uint32_t>> rows;
  void add(uint32_t stage,uint32_t relay){std::lock_guard lock(mutex);rows.push_back({stage,relay});}
  std::vector<std::pair<uint32_t,uint32_t>> read(){std::lock_guard lock(mutex);return rows;}
  bool wait(size_t count,uint64_t milliseconds){const auto end=udpProbeNowMs()+milliseconds;while(udpProbeNowMs()<end){if(read().size()>=count)return true;Sleep(5);}return read().size()>=count;}
};
UdpGrant grantFor(std::vector<UdpRelayAddress> relays){
  UdpGrant grant;grant.relays=std::move(relays);for(size_t i=0;i<grant.id.size();++i)grant.id[i]=uint8_t(0xa0+i);for(size_t i=0;i<grant.ticket.size();++i)grant.ticket[i]=uint8_t(i);return grant;
}
UdpLegDatagrams datagramsOf(const UdpGrant& grant){UdpLegDatagrams out;require(udpLegDatagrams(grant,out),"HMAC failed");return out;}
Bytes ping(uint32_t sequence,uint8_t kind=udpPing){
  Bytes bytes(udpProbeBytes,0);bytes[0]=udpTypeData;for(size_t i=1;i<9;++i)bytes[i]=uint8_t(0x10+i);bytes[9]=kind;
  for(int i=0;i<4;++i)bytes[13+i]=uint8_t(sequence>>(24-8*i));bytes[24]=0x5a;return bytes;
}
std::shared_ptr<UdpProbeLeg> startLeg(const UdpGrant& grant,Record& record,std::function<std::optional<sockaddr_in>(const UdpRelayAddress&)> resolve=udpResolveRelay){
  auto leg=UdpProbeLeg::start(grant,{[&record](uint32_t stage,uint32_t relay){record.add(stage,relay);},std::move(resolve)});require(bool(leg),"The leg did not start");return leg;
}
// BIND, then a valid ACK, as the relay would.
void bindLeg(Peer& relay,const UdpLegDatagrams& expected,Record& record){
  const auto bind=relay.receive(2000);require(bind&&std::equal(bind->begin(),bind->end(),expected.bind.begin(),expected.bind.end()),"No correct BIND reached the relay");
  relay.reply(expected.ack);require(record.wait(1,2000),"The ACK was not recorded");
}
void bound(){
  Winsock winsock;Peer relay;Record record;const auto grant=grantFor({{"127.0.0.1",relay.port}});const auto expected=datagramsOf(grant);
  auto leg=startLeg(grant,record);
  const auto first=relay.receive(2000);require(first&&std::equal(first->begin(),first->end(),expected.bind.begin(),expected.bind.end()),"The first datagram was not the BIND");++passed;
  // Nothing short of the ACK moves it: a PING, a forged ACK and noise are all
  // ignored and the BINDs keep coming.
  relay.reply(ping(1));auto forged=Bytes(expected.ack.begin(),expected.ack.end());forged[30]^=1;relay.reply(forged);relay.reply(std::span<const uint8_t>(expected.bind));const uint8_t noise[]{9,9,9};relay.reply(noise);
  auto next=relay.receive(1000);require(next&&(*next)[0]==udpTypeBind,"Something unproven answered, or the BINDs stopped");require(record.read().empty(),"An unproven datagram bound the leg");++passed;
  relay.reply(expected.ack);require(record.wait(1,2000)&&record.read()[0]==std::pair<uint32_t,uint32_t>{udpStageBound,0},"A valid ACK was not recorded as stage 1, relay 0");++passed;
  // Any BIND still in flight clears, then every PING gets its PONG.
  while(relay.receive(300)){}
  for(uint32_t sequence=1;sequence<=5;++sequence){auto request=ping(sequence);relay.reply(request);const auto pong=relay.receive(2000);
    require(pong&&pong->size()==udpProbeBytes,"A valid PING got no PONG");require((*pong)[0]==udpTypeData&&std::equal(grant.id.begin(),grant.id.end(),pong->begin()+1),"The PONG does not carry this leg's id");
    request[9]=udpPong;require(std::equal(pong->begin()+9,pong->end(),request.begin()+9),"The PONG payload is not the PING's with [0]=2");}++passed;
  // Malformed PINGs get nothing; the next good one still does.
  {auto reservedSet=ping(6);reservedSet[12]=1;relay.reply(reservedSet);auto cut=ping(7);cut.pop_back();relay.reply(cut);auto type=ping(8);type[0]=udpTypeBind;relay.reply(type);relay.reply(ping(9,udpPong));
    const std::vector<uint8_t> oversize(1200,3);relay.reply(oversize);
    const Bytes tooLong(1400,3);relay.reply(tooLong);
    require(!relay.receive(400),"A malformed datagram was answered");relay.reply(ping(10));const auto pong=relay.receive(2000);require(pong&&(*pong)[16]==10,"The leg stopped answering after bad datagrams");++passed;}
  // Another source, even with a perfect PING or ACK, is a stranger.
  {Peer stranger;stranger.send(ping(11),relay.leg);stranger.send(expected.ack,relay.leg);require(!stranger.receive(300)&&!relay.receive(100),"A PING from another source was answered");require(record.read().size()==1,"A second ACK made a second record");++passed;}
  // close() ends the thread and the socket; the leg answers nothing afterwards.
  const auto target=relay.leg;leg->close();require(leg->waitClosed(2000),"The leg did not end on close");relay.send(ping(12),target);require(!relay.receive(300),"A closed leg answered");require(record.read().size()==1,"Closing added a record");++passed;
}
void rateCap(){
  Winsock winsock;Peer relay;Record record;const auto grant=grantFor({{"127.0.0.1",relay.port}});auto leg=startLeg(grant,record);bindLeg(relay,datagramsOf(grant),record);while(relay.receive(300)){}
  const auto began=udpProbeNowMs();for(uint32_t sequence=0;sequence<300;++sequence){relay.reply(ping(sequence));if(sequence%25==24)Sleep(1);}
  int pongs=0;while(relay.receive(250))++pongs;const auto elapsed=udpProbeNowMs()-began;
  // Past the second the cap may rightly pass more; rerun rather than believe it.
  if(elapsed<1000){require(pongs<=100,"Over 100 PONGs within a second");require(pongs>=50,"The burst was starved, or the socket lost it");++passed;}else std::cout<<"{\"rateCapBurstSkipped\":"<<elapsed<<"}\n";
  Sleep(1100);relay.reply(ping(1000));require(relay.receive(2000).has_value(),"The cap did not recover");++passed;
  leg->close();require(leg->waitClosed(2000),"The leg did not end");
}
void failover(){
  // The first relay is a closed port, so Windows may report ICMP on the socket. That
  // must not read as a socket failure, and the second relay still gets its turn.
  Winsock winsock;std::optional<Peer> closed;closed.emplace();const uint16_t dead=closed->port;closed.reset();
  Peer live;Record record;const auto grant=grantFor({{"127.0.0.1",dead},{"localhost",live.port}});const auto expected=datagramsOf(grant);
  const auto began=udpProbeNowMs();auto leg=startLeg(grant,record);
  const auto bind=live.receive(3000);const auto waited=udpProbeNowMs()-began;
  require(bind&&(*bind)[0]==udpTypeBind,"The second relay never got a BIND");require(waited>=500,"The second relay was tried before the first relay's 600 ms");require(record.read().empty(),"A closed port ended the probe");++passed;
  live.reply(expected.ack);require(record.wait(1,2000)&&record.read()[0]==std::pair<uint32_t,uint32_t>{udpStageBound,1},"The relay that answered was not kept as relay 1");++passed;
  while(live.receive(300)){}
  live.reply(ping(1));require(live.receive(2000).has_value(),"The kept relay got no PONG");++passed;
  leg->close();require(leg->waitClosed(2000),"The leg did not end");
}
void lateAck(){
  // An ACK from relay 0 after the schedule moved on to relay 1 still counts.
  Winsock winsock;Peer first,second;Record record;const auto grant=grantFor({{"127.0.0.1",first.port},{"127.0.0.1",second.port}});const auto expected=datagramsOf(grant);
  auto leg=startLeg(grant,record);require(first.receive(2000).has_value()&&second.receive(3000).has_value(),"Both relays should have seen a BIND");
  first.reply(expected.ack);require(record.wait(1,2000)&&record.read()[0]==std::pair<uint32_t,uint32_t>{udpStageBound,0},"An ACK from the earlier relay was lost");++passed;
  leg->close();require(leg->waitClosed(2000),"The leg did not end");
}
void timeout(){
  Winsock winsock;Peer relay;Record record;const auto grant=grantFor({{"127.0.0.1",relay.port}});
  const auto began=udpProbeNowMs();auto leg=startLeg(grant,record);int binds=0;
  while(!record.wait(1,1)){if(relay.receive(20))++binds;require(udpProbeNowMs()-began<9000,"The probe never gave up");}
  const auto elapsed=udpProbeNowMs()-began;require(record.read()[0]==std::pair<uint32_t,uint32_t>{udpStageTimeout,0},"Silence did not record a BIND timeout");require(elapsed>=4900&&elapsed<=7000,"The BIND timeout was not about 5 s");require(binds>=15&&binds<=25,"Not about 25 BINDs in 5 s");++passed;
  require(leg->waitClosed(2000),"A timed out leg kept its thread");require(record.read().size()==1,"The timeout was recorded twice");++passed;
}
void noName(){
  Winsock winsock;Record record;const auto grant=grantFor({{"first.invalid",1000},{"second.invalid",1001}});std::atomic<int> lookups{0};std::atomic<DWORD> thread{0};
  auto leg=startLeg(grant,record,[&](const UdpRelayAddress&)->std::optional<sockaddr_in>{++lookups;thread=GetCurrentThreadId();return {};});
  require(record.wait(1,3000)&&record.read()[0]==std::pair<uint32_t,uint32_t>{udpStageName,1},"Failed lookups were not recorded as stage 3");require(lookups==2,"Lookups were not once per relay");require(thread!=GetCurrentThreadId(),"The lookup ran on the caller's thread");require(leg->waitClosed(2000),"A failed leg kept its thread");++passed;
}
void stalledResolver(){
  // The contract's point: close() never waits, even for a resolver that hangs.
  Winsock winsock;Record record;HANDLE release=CreateEventW(nullptr,TRUE,FALSE,nullptr);HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);require(release&&entered,"Test events failed");
  const auto grant=grantFor({{"stalled.invalid",1000}});
  auto leg=startLeg(grant,record,[&](const UdpRelayAddress&)->std::optional<sockaddr_in>{SetEvent(entered);WaitForSingleObject(release,INFINITE);return {};});
  require(WaitForSingleObject(entered,3000)==WAIT_OBJECT_0,"The lookup never started");
  const auto began=udpProbeNowMs();leg->close();const auto closing=udpProbeNowMs()-began;
  require(closing<50,"close() waited for the resolver");require(!leg->waitClosed(100),"The stalled thread cannot have ended yet");leg.reset();SetEvent(release);
  Sleep(300);require(record.read().empty(),"A leg stopped during its lookup recorded a stage");++passed;CloseHandle(release);CloseHandle(entered);
}
void independent(){
  // A new grant's leg works while the old one is told to stop; neither shares state.
  Winsock winsock;Peer relayA,relayB;Record recordA,recordB;auto grantA=grantFor({{"127.0.0.1",relayA.port}});auto grantB=grantFor({{"127.0.0.1",relayB.port}});grantB.id[0]=0x77;grantB.ticket[0]=0x77;
  auto first=startLeg(grantA,recordA);bindLeg(relayA,datagramsOf(grantA),recordA);auto second=startLeg(grantB,recordB);first->close();require(first->waitClosed(2000),"The replaced leg did not end");bindLeg(relayB,datagramsOf(grantB),recordB);
  require(recordB.read()[0].first==udpStageBound&&recordA.read().size()==1,"The new leg did not bind on its own");while(relayB.receive(300)){}
  relayB.reply(ping(1));const auto pong=relayB.receive(2000);require(pong&&(*pong)[1]==0x77,"The new leg did not answer as itself");++passed;second->close();require(second->waitClosed(2000),"The leg did not end");
}
void droppedWithoutClose(){
  Winsock winsock;Peer relay;Record record;const auto grant=grantFor({{"127.0.0.1",relay.port}});auto leg=startLeg(grant,record);bindLeg(relay,datagramsOf(grant),record);while(relay.receive(300)){}
  const auto target=relay.leg;leg.reset();Sleep(200);relay.send(ping(1),target);require(!relay.receive(300),"A leg nobody holds kept answering");++passed;
}
}
// --leg <grant file> <ms>: the real leg on a grant from elsewhere, to run it by hand
// against the actual relay. Prints each recorded outcome.
int legFromFile(const wchar_t* path,DWORD milliseconds){
  std::ifstream file(path,std::ios::binary);const std::string text{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
  const auto grant=parseUdpGrant(JsonParser::parse(text),text.size());if(!grant){std::cerr<<"Malformed grant\n";return 2;}
  Record record;const auto leg=startLeg(*grant,record);Sleep(milliseconds);leg->close();require(leg->waitClosed(2000),"The leg did not end");
  const auto rows=record.read();for(const auto& [stage,relay]:rows)std::cout<<"{\"stage\":"<<stage<<",\"relay\":"<<relay<<"}\n";return rows.empty()?1:0;
}
int wmain(int argc,wchar_t** argv){
  try{
    if(argc==4&&std::wstring_view(argv[1])==L"--leg")return legFromFile(argv[2],DWORD(std::wcstoul(argv[3],nullptr,10)));
    for(const auto& scenario:{bound,rateCap,failover,lateAck,timeout,noName,stalledResolver,independent,droppedWithoutClose})scenario();
    std::cout<<"{\"kind\":\"udp-probe-loopback\",\"passed\":"<<passed<<",\"network\":\"127.0.0.1 only\"}\n";return 0;
  }catch(const std::exception& error){std::cerr<<"UDP probe loopback check failed: "<<error.what()<<"\n";return 1;}
}
