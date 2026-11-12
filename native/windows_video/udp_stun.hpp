#pragma once
#include "udp_direct.hpp"

namespace multitap::udp {
// RFC 8489 Binding on the existing media socket. Only an exact source and
// transaction can supply a candidate; DirectPath must still authenticate it.
class StunBinding {
 public:
  using Request=std::array<uint8_t,20>;
  void server(DirectEndpoint endpoint){for(const auto& row:servers_)if(row.endpoint==endpoint)return;if(servers_.size()<2)servers_.push_back({endpoint});}
  template<class Send> void tick(uint64_t now,Send&& send){
    for(auto& row:servers_){
      if(row.done||row.attempts>=5||now<row.next)continue;
      if(!row.attempts){row.request[1]=1;row.request[4]=0x21;row.request[5]=0x12;row.request[6]=0xa4;row.request[7]=0x42;if(BCryptGenRandom(nullptr,row.request.data()+8,12,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0){row.done=true;continue;}row.started=now;}
      row.next=now+500000;++row.attempts;send(row.endpoint,row.request);
    }
  }
  std::optional<DirectEndpoint> received(const uint8_t* bytes,size_t size,DirectEndpoint from,uint64_t now){
    if(size<20||size>1200||bytes[0]!=1||bytes[1]!=1||bytes[4]!=0x21||bytes[5]!=0x12||bytes[6]!=0xa4||bytes[7]!=0x42)return {};
    const size_t length=(size_t(bytes[2])<<8)|bytes[3];if(length%4||length!=size-20)return {};
    for(auto& row:servers_)if(!row.done&&row.attempts&&row.endpoint==from&&now-row.started<5000000&&std::equal(row.request.begin()+8,row.request.end(),bytes+8)){
      std::optional<DirectEndpoint> mapped;
      for(size_t at=20;at<size;){
        if(size-at<4)return {};const uint16_t type=uint16_t((bytes[at]<<8)|bytes[at+1]);const size_t count=(size_t(bytes[at+2])<<8)|bytes[at+3];at+=4;
        if(count>size-at)return {};
        if(type==0x20){
          if(mapped||count!=8||bytes[at]!=0||bytes[at+1]!=1)return {};
          DirectEndpoint endpoint;endpoint.port=uint16_t(((bytes[at+2]<<8)|bytes[at+3])^0x2112);std::array<uint8_t,4> address{};
          for(size_t n=0;n<4;++n)address[n]=bytes[at+4+n]^bytes[4+n];std::memcpy(&endpoint.address,address.data(),4);
          if(!endpoint.address||!endpoint.port)return {};mapped=endpoint;
        }else if(type<0x8000&&type!=1)return {};
        const size_t padded=(count+3)&~size_t(3);if(padded>size-at)return {};at+=padded;
      }
      if(mapped)row.done=true;return mapped;
    }
    return {};
  }
 private:
  struct Server{DirectEndpoint endpoint;Request request{};uint64_t next=0,started=0;size_t attempts=0;bool done=false;};std::vector<Server> servers_;
};
}
