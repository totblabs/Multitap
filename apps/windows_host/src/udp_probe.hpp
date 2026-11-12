#pragma once
// udp-media-v1 probe, the Host's rules (docs/remote-session-protocol.md,
// "UDP media"; framing as server/src/media-udp.mjs). No socket, thread or
// clock of their own: udp_probe_leg.hpp runs them. The ticket is a secret and goes
// nowhere but into the HMAC; only a stage and a relay index ever leave this code.
#include "support.hpp"
#include <bcrypt.h>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#pragma comment(lib,"bcrypt.lib")

namespace host {
inline constexpr char udpMediaProtocol[]="udp-media-v1";
// hostAction 270, once per grant: this stage, with the relay index as `error`.
inline constexpr uint32_t udpProbeAction=270,udpStageBound=1,udpStageSocket=2,udpStageName=3,udpStageTimeout=4;
inline constexpr size_t udpIdBytes=8,udpTicketBytes=32,udpHeaderBytes=1+udpIdBytes,udpBindBytes=udpHeaderBytes+32,udpProbeBytes=udpHeaderBytes+16,udpMaxDatagram=1200;
inline constexpr uint8_t udpTypeBind=1,udpTypeAck=2,udpTypeData=3,udpPing=1,udpPong=2;
// Eight relays is what one pass of 600 ms each fits into the 5 s.
inline constexpr size_t udpGrantMaxBytes=2048,udpMaxRelays=8,udpMaxHostLength=253;
inline constexpr uint64_t udpBindEveryMs=200,udpBindsPerRelay=3,udpBindTotalMs=5000,udpPongWindowMs=1000;
using UdpLegId=std::array<uint8_t,udpIdBytes>;
using UdpLegBytes=std::array<uint8_t,udpBindBytes>;
struct UdpRelayAddress {std::string host;uint16_t port=0;};
struct UdpGrant {std::vector<UdpRelayAddress> relays;UdpLegId id{};std::array<uint8_t,udpTicketBytes> ticket{};bool direct=false;std::vector<UdpRelayAddress> stun;};

// Standard padded base64 of exactly N bytes. The URL alphabet, whitespace, missing
// padding and stray trailing bits all fail.
template<size_t N> inline bool udpBase64(const std::string& text,std::array<uint8_t,N>& out){
  const size_t groups=(N+2)/3,padding=groups*3-N;if(text.size()!=groups*4)return false;
  uint32_t bits=0;unsigned held=0;size_t written=0;
  for(size_t at=0;at<text.size();++at){
    const char c=text[at];if(at>=text.size()-padding){if(c!='=')return false;continue;}
    const int value=c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:c>='0'&&c<='9'?c-'0'+52:c=='+'?62:c=='/'?63:-1;if(value<0)return false;
    bits=(bits<<6)|uint32_t(value);held+=6;
    if(held>=8){held-=8;if(written==N)return false;out[written++]=uint8_t(bits>>held);bits&=(1u<<held)-1;}
  }
  return written==N&&!bits;
}
// Nothing about a grant is repaired: any deviation means no leg at all. expiresAt
// is checked for shape only, because the relay decides expiry and a VM clock may be
// off. A sessionId, if the broker adds one, is the caller's check.
inline std::optional<UdpGrant> parseUdpGrant(const Json& message,size_t messageBytes){
  const auto whole=[](double value,double minimum,double maximum){return value>=minimum&&value<=maximum&&std::floor(value)==value;};
  const auto* relays=std::get_if<Json::Array>(&message.get("relays").value);const auto* expires=std::get_if<double>(&message.get("expiresAt").value);
  if(messageBytes>udpGrantMaxBytes||message.get("type").string()!="udp.grant"||message.get("protocol").string()!=udpMediaProtocol||!relays||relays->empty()||relays->size()>udpMaxRelays||!expires||!whole(*expires,1,9007199254740991.0))return {};
  UdpGrant grant;if(!udpBase64(message.get("id").string(),grant.id)||!udpBase64(message.get("ticket").string(),grant.ticket))return {};
  grant.direct=message.get("directProtocol").string()=="udp-direct-v1";
  for(const auto& relay:*relays){
    const auto* name=std::get_if<std::string>(&relay.get("host").value);const auto* port=std::get_if<double>(&relay.get("port").value);
    if(!name||name->empty()||name->size()>udpMaxHostLength||name->find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-")!=std::string::npos||!port||!whole(*port,1,65535))return {};
    grant.relays.push_back({*name,uint16_t(*port)});
  }
  if(grant.direct)if(const auto* servers=std::get_if<Json::Array>(&message.get("stun").value)){
    if(servers->size()>2)return {};
    for(const auto& server:*servers){const auto name=server.get("host").string();const auto* port=std::get_if<double>(&server.get("port").value);if(name.empty()||name.size()>udpMaxHostLength||name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-")!=std::string::npos||!port||!whole(*port,1,65535))return {};grant.stun.push_back({name,uint16_t(*port)});}
  }
  return grant;
}
inline bool udpProof(const UdpGrant& grant,std::string_view label,uint8_t* proof)noexcept{
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
  const bool ok=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_ALG_HANDLE_HMAC_FLAG)>=0&&
    BCryptCreateHash(algorithm,&hash,nullptr,0,const_cast<PUCHAR>(grant.ticket.data()),ULONG(grant.ticket.size()),0)>=0&&
    BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(label.data())),ULONG(label.size()),0)>=0&&
    BCryptHashData(hash,const_cast<PUCHAR>(grant.id.data()),ULONG(grant.id.size()),0)>=0&&
    BCryptFinishHash(hash,proof,32,0)>=0;
  if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
  return ok;
}
// What the ticket is for: the BIND to send and the ACK to expect. Neither is secret.
struct UdpLegDatagrams {UdpLegId id{};UdpLegBytes bind{},ack{};};
inline bool udpLegDatagrams(const UdpGrant& grant,UdpLegDatagrams& out)noexcept{
  out.id=grant.id;out.bind[0]=udpTypeBind;out.ack[0]=udpTypeAck;
  for(size_t i=0;i<udpIdBytes;++i)out.bind[1+i]=out.ack[1+i]=grant.id[i];
  return udpProof(grant,"multitap-udp-bind-v1",out.bind.data()+udpHeaderBytes)&&udpProof(grant,"multitap-udp-ack-v1",out.ack.data()+udpHeaderBytes);
}
// Every byte is read whichever one differs, so the time taken says nothing about
// how close a forged ACK came. The length of a datagram is no secret.
inline bool udpSameBytes(std::span<const uint8_t> a,std::span<const uint8_t> b)noexcept{
  if(a.size()!=b.size())return false;
  volatile uint8_t difference=0;for(size_t i=0;i<a.size();++i)difference=uint8_t(difference|(a[i]^b[i]));
  return difference==0;
}
// A valid PING's PONG: the payload unchanged except its first byte. The header takes
// this leg's id, not the sender's: the relay routes by the id of whoever sends.
inline std::optional<std::array<uint8_t,udpProbeBytes>> udpProbePong(std::span<const uint8_t> datagram,const UdpLegId& own)noexcept{
  if(datagram.size()!=udpProbeBytes||datagram[0]!=udpTypeData||datagram[udpHeaderBytes]!=udpPing||datagram[udpHeaderBytes+1]||datagram[udpHeaderBytes+2]||datagram[udpHeaderBytes+3])return {};
  std::array<uint8_t,udpProbeBytes> pong{};for(size_t i=0;i<pong.size();++i)pong[i]=datagram[i];
  for(size_t i=0;i<udpIdBytes;++i)pong[1+i]=own[i];
  pong[udpHeaderBytes]=udpPong;return pong;
}
// At most 100 PONGs in any second, not merely per calendar second: the 100th send
// before this one must be a whole window old.
class UdpPongLimit {
 public:
  bool allow(uint64_t now)noexcept{
    auto& oldest=sent_[next_];if(used_==sent_.size()&&now-oldest<udpPongWindowMs)return false;
    oldest=now;next_=(next_+1)%sent_.size();if(used_<sent_.size())++used_;return true;
  }
 private:
  std::array<uint64_t,100> sent_{};size_t next_=0,used_=0;
};
// BIND every 200 ms for 600 ms per relay, then the next. The list repeats until the
// 5 s are up: a relay already pinned to an address answers that address again, so a
// lost ACK is recovered by coming back to it.
class UdpBindSchedule {
 public:
  UdpBindSchedule(size_t relays,uint64_t start)noexcept:relays_(relays),start_(start){}
  bool expired(uint64_t now)const noexcept{return now-start_>=udpBindTotalMs;}
  // The relay whose BIND is due. A late wake skips the sends it missed.
  std::optional<size_t> due(uint64_t now)noexcept{
    const auto slot=(now-start_)/udpBindEveryMs;if(expired(now)||slot<next_)return {};
    next_=slot+1;return size_t((slot/udpBindsPerRelay)%relays_);
  }
  uint64_t wake()const noexcept{return start_+std::min(next_*udpBindEveryMs,udpBindTotalMs);}
 private:
  size_t relays_;uint64_t start_,next_=0;
};
struct UdpProbeOutcome {uint32_t stage=0,relay=0;};
// One grant from BIND to PONG. `resolve` makes a relay's address ready for `send`
// and runs at most once per relay; the caller maps a datagram's source to a relay
// it resolved and drops every other source, so nothing here can answer a stranger.
class UdpProbe {
 public:
  using Resolve=std::function<bool(size_t)>;
  using Send=std::function<bool(size_t,std::span<const uint8_t>)>;
  UdpProbe(const UdpLegDatagrams& datagrams,size_t relays,uint64_t now,Resolve resolve,Send send):datagrams_(datagrams),schedule_(relays,now),names_(relays,Name::unknown),resolve_(std::move(resolve)),send_(std::move(send)){}
  // Sends what is due. Returns when to call again; nothing once a datagram is all
  // that can change anything.
  std::optional<uint64_t> tick(uint64_t now){
    if(outcome_)return {};
    if(schedule_.expired(now)){finish(sent_?udpStageTimeout:udpStageSocket,tried_);return {};}
    if(const auto relay=schedule_.due(now)){
      auto& name=names_[*relay];if(name==Name::unknown)name=resolve_(*relay)?Name::found:Name::missing;
      if(name==Name::found){tried_=*relay;if(send_(*relay,datagrams_.bind))sent_=true;}
      else if(std::all_of(names_.begin(),names_.end(),[](Name each){return each==Name::missing;})){finish(udpStageName,*relay);return {};}
    }
    return schedule_.wake();
  }
  void received(size_t relay,std::span<const uint8_t> datagram,uint64_t now){
    if(!bound_){if(!outcome_&&udpSameBytes(datagram,datagrams_.ack)){bound_=relay;finish(udpStageBound,relay);}return;}
    if(relay!=*bound_)return;
    if(const auto pong=udpProbePong(datagram,datagrams_.id);pong&&limit_.allow(now)&&send_(relay,*pong))answered_=true;
  }
  void socketFailed()noexcept{if(!outcome_)finish(udpStageSocket,tried_);}
  std::optional<size_t> boundRelay()const noexcept{return bound_;}
  // A PING came through and went back: the probe passed as far as this leg can tell.
  bool answered()const noexcept{return answered_;}
  // Once, however often it is asked: hostAction 270 is one record per grant.
  std::optional<UdpProbeOutcome> takeOutcome()noexcept{if(!outcome_||taken_)return {};taken_=true;return outcome_;}
  bool failed()const noexcept{return outcome_&&!bound_;}
 private:
  enum class Name:uint8_t{unknown,found,missing};
  void finish(uint32_t stage,size_t relay)noexcept{outcome_=UdpProbeOutcome{stage,uint32_t(relay)};}
  UdpLegDatagrams datagrams_;UdpBindSchedule schedule_;std::vector<Name> names_;Resolve resolve_;Send send_;UdpPongLimit limit_;
  std::optional<UdpProbeOutcome> outcome_;std::optional<size_t> bound_;size_t tried_=0;bool sent_=false,taken_=false,answered_=false;
};
}
