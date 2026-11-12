#include "provider.hpp"
#include <iostream>
using namespace multitap::vdisplay;
constexpr uint64_t ownedLease=api::kMinOpaqueLeaseId+1234;
void require(bool condition){if(!condition)throw std::runtime_error("ToTB bridge protocol regression");}
struct Transport final:api::ControlTransport {
  int creates=0,releases=0,feeds=0;bool occupied=false,failSecond=false,failRelease=false;
  std::array<api::CreateTemporaryDisplayOwnedRequest,2> requests{};
  bool ioctl(uint32_t code,const void* input,size_t,void* output,size_t,size_t& received,uint32_t& error)override{
    error=0;received=0;
    if(code==api::kIoctlGetProtocolVersion){*static_cast<api::ProtocolVersion*>(output)={};received=sizeof(api::ProtocolVersion);return true;}
    if(code==api::kIoctlQueryLease){auto& value=*static_cast<api::QueryLeaseResult*>(output);value={};value.lease_id=static_cast<const api::LeaseRequest*>(input)->lease_id;value.lease_exists=occupied;received=sizeof(value);return true;}
    if(code==api::kIoctlCreateTemporaryDisplayOwned){const auto& r=*static_cast<const api::CreateTemporaryDisplayOwnedRequest*>(input);requests[creates++]=r;if(failSecond&&creates==2){error=5;return false;}auto& value=*static_cast<api::CreateTemporaryDisplayResult*>(output);value={};value.lease_id=r.display.lease_id;value.display_id=r.display.display_id;value.target_id=uint32_t(creates);value.effective_timeout_ms=15000;received=sizeof(value);return true;}
    if(code==api::kIoctlReleaseLease){++releases;require(static_cast<const api::LeaseRequest*>(input)->lease_id==ownedLease);return !failRelease;}
    if(code==api::kIoctlFeedLease){++feeds;require(static_cast<const api::LeaseRequest*>(input)->lease_id==ownedLease);return true;}
    throw std::runtime_error("Unexpected global/permanent driver operation");
  }
};
int main(){try{
  const std::array<Output,2> targets{{{501,1600,900},{502,1920,1080}}};api::OwnerCapability owner{};owner.bytes.fill(7);
  for(int scenario=0;scenario<6;++scenario){Transport t;api::ControlClient client(t);Session session(client);
    t.occupied=scenario==1;t.failSecond=scenario==2||scenario==3;t.failRelease=scenario==3;
    auto request=targets;if(scenario==4)request[1].identity=request[0].identity;if(scenario==5)request[1].width=9000;
    const bool started=session.start(ownedLease,owner,request);
    if(scenario==0){require(started&&session.outputs().size()==2&&session.heartbeat());require(std::string(t.requests[0].display.display_name)=="Multitap 1");require(t.requests[1].display.width==1920);require(session.stop()&&t.releases==1&&t.feeds==1);}
    else {require(!started&&session.outputs().empty());if(scenario==1||scenario>=4)require(t.creates==0&&t.releases==0);if(scenario==2||scenario==3)require(t.releases==1);if(scenario==3){require(session.cleanupPending());t.failRelease=false;require(session.stop());}}
  }
  {Transport t;api::ControlClient client(t);Session session(client);require(!session.start(api::kMinOpaqueLeaseId-1,owner,targets)&&!t.creates&&!t.releases);}
  std::cout<<"{\"passed\":7,\"usesActualUpstreamControlClient\":true,\"transport\":\"memory fixture\",\"osDriverCalls\":0,\"certificateImports\":0,\"productionProviderChanged\":false}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
