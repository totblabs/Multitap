#pragma once
// The Host's udp-media-v1 leg (udp_probe.hpp, udp_media_path.hpp): one UDP socket and
// one thread, started only by a grant, so a broker that never grants costs nothing
// here. The leg shares no lock or queue with capture or encode and blocks nothing
// else, so any failure ends the leg and leaves media on the broker channel.
// It runs in the service beside that channel, which owns the media send path.
#include "udp_media_path.hpp"
#include "../../../native/windows_video/udp_stun.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <chrono>
#include <memory>
#include <thread>
#pragma comment(lib,"ws2_32.lib")

namespace host {
// IPv4 first, as the relay's socket is; WSS covers everything else.
inline std::optional<sockaddr_in> udpResolveRelay(const UdpRelayAddress& relay){
  addrinfo hints{},*found=nullptr;hints.ai_family=AF_INET;hints.ai_socktype=SOCK_DGRAM;hints.ai_protocol=IPPROTO_UDP;
  if(getaddrinfo(relay.host.c_str(),nullptr,&hints,&found)||!found)return {};
  std::optional<sockaddr_in> address;
  if(found->ai_addrlen>=sizeof(sockaddr_in)){address.emplace(*reinterpret_cast<const sockaddr_in*>(found->ai_addr));address->sin_port=htons(relay.port);}
  freeaddrinfo(found);return address;
}
struct UdpProbeLegOptions {
  // The grant's outcome (stage, relay index), at most once, on the leg's thread.
  std::function<void(uint32_t,uint32_t)> record;
  // Blocking, on the leg's thread: a stalled resolver holds up this leg only.
  std::function<std::optional<sockaddr_in>(const UdpRelayAddress&)> resolve=udpResolveRelay;
  UdpMediaHooks media;
  // Each path change (hostAction 271), on the leg's thread: it writes a file.
  UdpMediaPath::Record recordPath;
};
struct UdpProbeSocket {
  bool started=false;SOCKET handle=INVALID_SOCKET;WSAEVENT readable=WSA_INVALID_EVENT;
  // A wait's timeout follows the 15.6 ms system tick, which the pacer's 2 ms of credit
  // cannot ride out: it would send a fraction of the target.
  HANDLE pacer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
  ~UdpProbeSocket(){if(pacer)CloseHandle(pacer);if(handle!=INVALID_SOCKET)closesocket(handle);if(readable!=WSA_INVALID_EVENT)WSACloseEvent(readable);if(started)WSACleanup();}
  bool open(){
    WSADATA data{};started=WSAStartup(MAKEWORD(2,2),&data)==0;if(!started)return false;
    // Not inherited: a helper this service starts must not keep the socket open.
    handle=WSASocketW(AF_INET,SOCK_DGRAM,IPPROTO_UDP,nullptr,0,WSA_FLAG_NO_HANDLE_INHERIT);if(handle==INVALID_SOCKET)return false;
    // Windows reports an ICMP answer to a send as a failed read. A relay port
    // that is closed says nothing about this socket.
    BOOL report=FALSE;DWORD used=0;WSAIoctl(handle,SIO_UDP_CONNRESET,&report,sizeof(report),nullptr,0,&used,nullptr,nullptr);
    sockaddr_in any{};any.sin_family=AF_INET;readable=WSACreateEvent();
    return readable!=WSA_INVALID_EVENT&&::bind(handle,reinterpret_cast<const sockaddr*>(&any),sizeof(any))==0&&WSAEventSelect(handle,readable,FD_READ)==0;
  }
};
// Reads that fail for one datagram or one ICMP answer. Each one consumes what
// caused it, so going on cannot spin; any other error ends the leg.
inline bool udpProbeReadTransient(int error){return error==WSAECONNRESET||error==WSAENETRESET||error==WSAEMSGSIZE||error==WSAEHOSTUNREACH||error==WSAENETUNREACH;}
inline uint64_t udpProbeNowMs(){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
inline uint64_t udpProbeNowUs(){return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
inline udpcore::DirectEndpoint udpEndpoint(const sockaddr_in& address){return {address.sin_addr.s_addr,ntohs(address.sin_port)};}
inline sockaddr_in udpSockaddr(udpcore::DirectEndpoint endpoint){sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=endpoint.address;address.sin_port=htons(endpoint.port);return address;}
// A temporary connected UDP socket asks Windows for the route's local address;
// the real, unconnected socket keeps its port and can receive from either path.
inline std::optional<udpcore::DirectEndpoint> udpLocalEndpoint(SOCKET socket,const sockaddr_in& relay){
  sockaddr_in own{},route{};int size=sizeof(own);if(getsockname(socket,reinterpret_cast<sockaddr*>(&own),&size))return {};
  const SOCKET query=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(query==INVALID_SOCKET)return {};
  size=sizeof(route);const bool ok=connect(query,reinterpret_cast<const sockaddr*>(&relay),sizeof(relay))==0&&getsockname(query,reinterpret_cast<sockaddr*>(&route),&size)==0;closesocket(query);
  if(!ok)return {};route.sin_port=own.sin_port;return udpEndpoint(route);
}
inline void udpProbeRun(UdpGrant& grant,const UdpProbeLegOptions& options,HANDLE stop,HANDLE wake,UdpMediaPath& media)noexcept{
  struct Ended{UdpMediaPath& media;~Ended(){media.ended();}}ended{media};
  // An exception out of a thread would end the boot service.
  try{
    UdpLegDatagrams datagrams;const bool built=udpLegDatagrams(grant,datagrams);SecureZeroMemory(grant.ticket.data(),grant.ticket.size());
    const auto report=[&](const std::optional<UdpProbeOutcome>& outcome){if(outcome&&options.record)options.record(outcome->stage,outcome->relay);};
    UdpProbeSocket sock;if(!built||!sock.open()){report(UdpProbeOutcome{udpStageSocket,0});return;}
    std::vector<std::optional<sockaddr_in>> addresses(grant.relays.size());
    struct Lookup{std::atomic<bool> ready{false};std::optional<sockaddr_in> address;};
    std::vector<std::shared_ptr<Lookup>> stunLookups;udpcore::StunBinding stun;
    if(grant.direct)for(const auto& server:grant.stun){auto lookup=std::make_shared<Lookup>();stunLookups.push_back(lookup);try{std::thread([lookup,server]{lookup->address=udpResolveRelay(server);lookup->ready=true;}).detach();}catch(...){lookup->ready=true;}}
    // A full send buffer drops the datagram: UDP sends never wait here.
    const auto sendTo=[&](size_t relay,std::span<const uint8_t> bytes){return sendto(sock.handle,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<const sockaddr*>(&*addresses[relay]),sizeof(sockaddr_in))==int(bytes.size());};
    const auto directSend=[&](udpcore::DirectEndpoint endpoint,std::span<const uint8_t> bytes){
      const auto address=udpSockaddr(endpoint);std::array<uint8_t,udpHeaderBytes+udpcore::DirectPath::packetBytes> wrapped{};
      if(bytes.size()==udpcore::DirectPath::packetBytes&&bytes[0]=='T'&&bytes[1]=='V'&&bytes[2]=='D')for(const auto& relay:addresses)if(relay&&udpEndpoint(*relay)==endpoint){wrapped[0]=udpTypeData;std::copy(grant.id.begin(),grant.id.end(),wrapped.begin()+1);std::copy(bytes.begin(),bytes.end(),wrapped.begin()+udpHeaderBytes);bytes=wrapped;break;}
      return sendto(sock.handle,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==int(bytes.size());
    };
    UdpProbe probe(datagrams,grant.relays.size(),udpProbeNowMs(),
      [&](size_t relay){addresses[relay]=options.resolve(grant.relays[relay]);return addresses[relay].has_value();},sendTo);
    std::array<uint8_t,udpMaxDatagram> buffer{};
    // Bounded per wake so that stop is seen under a flood; FD_READ signals again
    // while datagrams remain.
    const auto drain=[&]{
      for(int reads=0;reads<64;++reads){
        sockaddr_in from{};int size=sizeof(from);
        const int bytes=recvfrom(sock.handle,reinterpret_cast<char*>(buffer.data()),int(buffer.size()),0,reinterpret_cast<sockaddr*>(&from),&size);
        if(bytes==SOCKET_ERROR){const int error=WSAGetLastError();if(error==WSAEWOULDBLOCK)return true;if(!udpProbeReadTransient(error))return false;continue;}
        const std::span<const uint8_t> incoming(buffer.data(),size_t(bytes));const auto stamp=udpProbeNowUs();
        if(const auto mapped=stun.received(incoming.data(),incoming.size(),udpEndpoint(from),stamp)){char text[INET_ADDRSTRLEN]{};in_addr address{};address.s_addr=mapped->address;if(inet_ntop(AF_INET,&address,text,sizeof(text)))media.reflexive(text,mapped->port);continue;}
        if(media.directReceived(incoming,udpEndpoint(from),stamp,directSend))continue;
        if(media.directAccepts(udpEndpoint(from),stamp)){media.received(incoming,stamp,true);continue;}
        for(size_t relay=0;relay<addresses.size();++relay)if(addresses[relay]&&addresses[relay]->sin_addr.s_addr==from.sin_addr.s_addr&&addresses[relay]->sin_port==from.sin_port){
          const std::span<const uint8_t> datagram(buffer.data(),size_t(bytes));probe.received(relay,datagram,udpProbeNowMs());
          if(datagram.size()==udpHeaderBytes+udpcore::DirectPath::packetBytes&&datagram[0]==udpTypeData&&media.directReceived(datagram.subspan(udpHeaderBytes),udpEndpoint(from),stamp,directSend))break;
          if(probe.boundRelay()==relay)media.received(datagram,udpProbeNowUs());
          break;
        }
      }
      return true;
    };
    const HANDLE events[]{stop,sock.readable,wake,sock.pacer};const DWORD count=sock.pacer?4:3;
    bool localReported=false;
    for(;;){
      const auto now=udpProbeNowMs();const auto due=probe.tick(now);
      if(WaitForSingleObject(stop,0)==WAIT_OBJECT_0)return;
      report(probe.takeOutcome());if(probe.failed())return;
      if(const auto relay=probe.boundRelay()){
        for(const auto& lookup:stunLookups)if(lookup->ready&&lookup->address)stun.server(udpEndpoint(*lookup->address));
        stun.tick(udpProbeNowUs(),directSend);
        media.directRelay(udpEndpoint(*addresses[*relay]));
        if(grant.direct&&!localReported){localReported=true;if(const auto local=udpLocalEndpoint(sock.handle,*addresses[*relay])){char text[INET_ADDRSTRLEN]{};in_addr address{};address.s_addr=local->address;if(inet_ntop(AF_INET,&address,text,sizeof(text)))media.local(text,local->port);}}
        if(probe.answered()&&sock.pacer)media.probePassed();
        const auto stamp=udpProbeNowUs();
        media.pump(stamp,[&](std::span<const uint8_t> bytes){if(const auto direct=media.directTarget(stamp))directSend(*direct,bytes);else sendTo(*relay,bytes);});
        media.directTick(stamp,directSend);
      }
      if(const DWORD pace=media.paceMs()){LARGE_INTEGER after{};after.QuadPart=-10000LL*pace;SetWaitableTimer(sock.pacer,&after,0,nullptr,nullptr,FALSE);}
      const DWORD signaled=WaitForMultipleObjects(count,events,FALSE,due?DWORD(*due>now?*due-now:0):INFINITE);
      if(signaled==WAIT_TIMEOUT||signaled==WAIT_OBJECT_0+2||signaled==WAIT_OBJECT_0+3)continue;
      if(signaled!=WAIT_OBJECT_0+1)return;
      WSAResetEvent(sock.readable);
      if(!drain()){probe.socketFailed();report(probe.takeOutcome());return;}
    }
  }catch(...){ }
}
class UdpProbeLeg {
 public:
  // Null when Windows refuses the events or the thread: the probe does not run.
  static std::shared_ptr<UdpProbeLeg> start(UdpGrant grant,UdpProbeLegOptions options)noexcept{
    try{
      auto events=std::make_shared<Events>();if(!events->stop||!events->exited||!events->wake)return nullptr;
      auto media=std::make_shared<UdpMediaPath>(grant.id,std::move(options.media),std::move(options.recordPath),grant.direct);
      std::shared_ptr<UdpProbeLeg> leg(new UdpProbeLeg(events,media));
      // Detached so that nothing ever waits for a stalled resolver. It holds only
      // this shared state, never the Agent, and ends soon after close().
      // One copy, on the heap: std::array moves by copying, and the thread wipes this one
      // as soon as the datagrams are built.
      auto held=std::make_shared<UdpGrant>(std::move(grant));SecureZeroMemory(grant.ticket.data(),grant.ticket.size());
      std::thread([events,media,held,options=std::move(options)]()mutable{udpProbeRun(*held,options,events->stop,events->wake,*media);SetEvent(events->exited);}).detach();
      return leg;
    }catch(...){return nullptr;}
  }
  ~UdpProbeLeg(){close();}
  UdpProbeLeg(const UdpProbeLeg&)=delete;UdpProbeLeg& operator=(const UdpProbeLeg&)=delete;
  // Safe on the broker channel's receive thread. After it, nothing reaches the session.
  void close()noexcept{media_->detach();SetEvent(events_->stop);}
  bool waitClosed(DWORD milliseconds)const noexcept{return WaitForSingleObject(events_->exited,milliseconds)==WAIT_OBJECT_0;}
  // The receive thread's and the pipe reader's way in (udp_media_path.hpp).
  std::optional<std::string> exchange(const Json& message,const std::string& session,int target){
    auto reply=media_->exchange(message,session,target);if(reply)SetEvent(events_->wake);return reply;
  }
  void wssHeard()noexcept{media_->wssHeard(udpProbeNowUs());}
  void pauseInputs(){media_->pauseInputs();}
  void peer(const Json& message){
    if(message.get("protocol").string()!="udp-direct-v1")return;
    const auto* list=std::get_if<Json::Array>(&message.get("candidates").value);if(!list||list->size()>5)return;
    std::vector<udpcore::DirectEndpoint> candidates;
    for(const auto& value:*list){in_addr address{};const auto* port=std::get_if<double>(&value.get("port").value);if(!port||*port<1||*port>65535||std::floor(*port)!=*port||inet_pton(AF_INET,value.get("address").string().c_str(),&address)!=1)return;candidates.push_back({address.s_addr,uint16_t(*port)});}
    media_->peer(std::move(candidates));
  }
  bool outgoing(bool text,std::vector<uint8_t>& bytes,bool recovery=false){
    const bool taken=media_->outgoing(text,bytes,recovery);if(taken)SetEvent(events_->wake);return taken;
  }
  std::pair<uint64_t,uint64_t> takeAudio()noexcept{return media_->takeAudio();}
 private:
  struct Events {
    HANDLE stop=CreateEventW(nullptr,TRUE,FALSE,nullptr),exited=CreateEventW(nullptr,TRUE,FALSE,nullptr),wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    Events()=default;Events(const Events&)=delete;Events& operator=(const Events&)=delete;
    ~Events(){if(stop)CloseHandle(stop);if(exited)CloseHandle(exited);if(wake)CloseHandle(wake);}
  };
  UdpProbeLeg(std::shared_ptr<Events> events,std::shared_ptr<UdpMediaPath> media):events_(std::move(events)),media_(std::move(media)){}
  std::shared_ptr<Events> events_;std::shared_ptr<UdpMediaPath> media_;
};
}
