#include "runtime.hpp"
#include <virtual_display/driver/ioctl_dispatcher.h>
#include <iostream>
#include <map>
#include <source_location>
using namespace multitap::vdisplay;
namespace {
int checks=0;
void require(bool ok,const std::source_location where=std::source_location::current()){if(!ok)throw std::runtime_error("lease recovery check at "+std::to_string(where.line()));++checks;}
constexpr uint64_t oldLease=api::kMinOpaqueLeaseId+101,newLease=api::kMinOpaqueLeaseId+202,foreignLease=api::kMinOpaqueLeaseId+303;
api::OwnerCapability capability(uint8_t byte=42){api::OwnerCapability owner{};owner.bytes.fill(byte);return owner;}
struct Backend:api::DisplayDriverBackend {
  int arrives=0,departs=0,permanent=0,render=0;std::map<uint64_t,api::DisplayDescriptor> live;
  api::BackendDisplayResult arrive_temporary_display(const api::DisplayDescriptor& d)override{live[d.display_id]=d;return {api::BackendError::None,{44,-1},uint32_t(++arrives)};}
  api::BackendError depart_temporary_display(uint64_t id,uint64_t generation)override{++departs;const auto it=live.find(id);if(it!=live.end()&&(!generation||it->second.generation==generation))live.erase(it);return api::BackendError::None;}
  api::BackendError set_permanent_display_count(const api::PermanentDisplayCountRequest&)override{++permanent;return api::BackendError::None;}
  api::BackendError set_render_adapter(const api::SetRenderAdapterRequest&)override{++render;return api::BackendError::None;}
};
struct Transport:api::ControlTransport {
  Backend backend;api::DriverController controller{api::DisplayStore{2,8},backend};api::IoctlDispatcher dispatcher{controller};
  std::chrono::steady_clock::time_point now{std::chrono::milliseconds(1000)};
  int reclaims=0,creates=0,removes=0,releases=0,feeds=0,queries=0;bool lostAck=false,failAfterReclaim=false,expireOnReclaim=false,unsupported=false;
  std::function<void(api::QueryDisplayStateResult&)> corruptState;std::function<void()> afterReclaim;
  bool ioctl(uint32_t code,const void* input,size_t inputSize,void* output,size_t capacity,size_t& size,uint32_t& error)override{
    size=0;error=0;
    if(code==api::kIoctlCreateTemporaryDisplayOwned)++creates;
    if(code==api::kIoctlRemoveTemporaryDisplay)++removes;
    if(code==api::kIoctlReleaseLease)++releases;
    if(code==api::kIoctlFeedLease)++feeds;
    if(code==api::kIoctlQueryLease||code==api::kIoctlQueryDisplayState){++queries;if(failAfterReclaim&&reclaims){error=5;return false;}}
    if(code==api::kIoctlReclaimTemporaryDisplay){++reclaims;if(unsupported){error=1;return false;}if(expireOnReclaim)now+=std::chrono::seconds(16);}
    const auto result=dispatcher.dispatch(code,input,inputSize,output,capacity,now);size=result.bytes_returned;error=result.status==api::IoctlStatus::Success?0:5;
    if(!error&&code==api::kIoctlQueryDisplayState&&corruptState)corruptState(*static_cast<api::QueryDisplayStateResult*>(output));
    if(code==api::kIoctlReclaimTemporaryDisplay){if(afterReclaim)afterReclaim();if(lostAck){error=123;size=0;return false;}}
    return !error;
  }
};
struct Fixture {
  Transport wire;api::ControlClient client{wire};LiveLeaseRecoveryPlan plan{oldLease,newLease,{{71,1920,1080},{72,1280,720}}, {}};api::DisplayStateEntry foreign{};
  Fixture(bool portrait=false){
    if(portrait){
      plan.outputs.front().width=1080;plan.outputs.front().height=1920;
      // Use the actual product creation path, then drop this controller as a
      // crashed owner would: Session destruction alone does not release a lease.
      Session interrupted(client);require(interrupted.start(oldLease,capability(),plan.outputs));
      const auto targets=interrupted.outputs();plan.targets.assign(targets.begin(),targets.end());
    }else for(const auto& output:plan.outputs){api::CreateTemporaryDisplayOwnedRequest request{};request.owner_capability=capability();auto& d=request.display;d.lease_id=oldLease;d.display_id=output.identity;d.width=output.width;d.height=output.height;d.physical_width_mm=600;d.physical_height_mm=340;d.refresh_rate_millihz=60000;d.requested_timeout_ms=15000;
      std::memcpy(d.display_name,"Multitap Test",sizeof("Multitap Test"));const auto made=client.create_temporary_display_owned(request);require(made.ok());plan.targets.push_back(made.value);}
    api::CreateTemporaryDisplayOwnedRequest request{};request.owner_capability=capability(17);auto& d=request.display;d.lease_id=foreignLease;d.display_id=999;d.width=1024;d.height=768;d.physical_width_mm=500;d.physical_height_mm=300;d.refresh_rate_millihz=75000;d.requested_timeout_ms=15000;std::memcpy(d.display_name,"Foreign Test",13);require(client.create_temporary_display_owned(request).ok());
    const auto state=client.query_display_state();require(state.ok());for(size_t i=0;i<state.value.entry_count;++i)if(state.value.entries[i].display_id==999)foreign=state.value.entries[i];
  }
  void preserved(){const auto state=client.query_display_state();require(state.ok());bool found=false;for(size_t i=0;i<state.value.entry_count;++i)if(state.value.entries[i].display_id==999){found=true;require(std::memcmp(&foreign,&state.value.entries[i],sizeof(foreign))==0);}require(found&&wire.backend.arrives==3&&!wire.backend.departs&&!wire.backend.permanent&&!wire.backend.render&&wire.creates==3&&!wire.removes&&!wire.releases);}
  LiveLeaseRecoveryResult recover(const api::OwnerCapability& owner=capability()){return reconcileLiveLease(client,plan,owner,[]{return true;},[]{return true;});}
};
void transfers(){
  {Fixture f;
    require(validLiveLeaseRecoveryPlan(f.plan));
    require(inspectLiveLeaseRecovery(f.client,f.plan)==LiveLeaseLocation::previous);
    const auto result=f.recover();
    require(result.location==LiveLeaseLocation::replacement&&result.attempted&&result.acknowledged&&f.wire.reclaims==1);f.preserved();
    const auto again=f.recover();require(again.location==LiveLeaseLocation::replacement&&!again.attempted&&f.wire.reclaims==1);f.preserved();
    api::LeaseRequest old{};old.lease_id=oldLease;old.requested_timeout_ms=15000;require(!f.client.feed_lease(old).ok());api::LeaseDisplayRequest stale{};stale.lease_id=oldLease;stale.display_id=71;require(!f.client.remove_temporary_display(stale).ok());require(f.wire.backend.live.size()==3);
  }
  {Fixture f;f.wire.lostAck=true;const auto result=f.recover();require(result.location==LiveLeaseLocation::replacement&&result.attempted&&!result.acknowledged);f.wire.lostAck=false;require(f.recover().location==LiveLeaseLocation::replacement&&f.wire.reclaims==1);f.preserved();}
  {Fixture f;f.wire.failAfterReclaim=true;const auto result=f.recover();require(result.location==LiveLeaseLocation::unknown&&result.attempted);f.wire.failAfterReclaim=false;require(f.recover().location==LiveLeaseLocation::replacement&&f.wire.reclaims==1);f.preserved();}
  {Fixture f;const auto result=f.recover(capability(99));require(result.location==LiveLeaseLocation::previous&&result.attempted&&!result.acknowledged);f.preserved();}
  {Fixture f;f.wire.unsupported=true;const auto result=f.recover();require(result.location==LiveLeaseLocation::previous&&result.attempted&&!result.acknowledged);f.preserved();}
  {Fixture f;f.wire.expireOnReclaim=true;const auto result=f.recover();require(result.location==LiveLeaseLocation::unknown&&result.attempted&&!result.acknowledged&&f.wire.backend.departs==0);f.preserved();}
  {Fixture f;f.wire.now+=std::chrono::seconds(16);f.wire.controller.reap_expired(f.wire.now);const auto before=f.wire.backend.departs;require(f.recover().location==LiveLeaseLocation::absent&&f.wire.reclaims==0&&f.wire.backend.departs==before&&f.wire.creates==3);}
}
void refusals(){
  for(const auto& mutate:std::vector<std::function<void(LiveLeaseRecoveryPlan&)>>{
      [](auto& p){p.replacement=p.previous;},[](auto& p){p.replacement=1;},[](auto& p){p.outputs.clear();},[](auto& p){p.outputs[0].width=1919;},
      [](auto& p){p.outputs[0].height=180;},[](auto& p){p.targets.pop_back();},[](auto& p){p.targets[1].display_id=71;},
      [](auto& p){p.targets[1].connector_index=p.targets[0].connector_index;},[](auto& p){p.targets[0].os_adapter_luid={};},[](auto& p){p.targets[0].lease_id=newLease;}}){
    Fixture f;mutate(f.plan);require(f.recover().location==LiveLeaseLocation::unknown&&f.wire.reclaims==0);f.preserved();}
  for(const auto& corrupt:std::vector<std::function<void(api::QueryDisplayStateResult&)>>{
      [](auto& s){s.entry_count++;},[](auto& s){s.reserved=1;},[](auto& s){s.entries[0].kind=99;},[](auto& s){s.entries[0].width+=2;},
      [](auto& s){s.entries[0].height+=2;},[](auto& s){s.entries[0].connector_index++;},[](auto& s){s.entries[0].lease_id=foreignLease;},
      [](auto& s){s.entries[0].refresh_rate_millihz=30000;},[](auto& s){s.entries[0].physical_width_mm=1;},[](auto& s){s.entries[0].display_id=999;},
      [](auto& s){s.entries[1]=s.entries[0];},[](auto& s){s.temporary_display_count++;}}){
    Fixture f;f.wire.corruptState=corrupt;require(f.recover().location==LiveLeaseLocation::unknown&&f.wire.reclaims==0);f.wire.corruptState={};f.preserved();}
  {Fixture f;require(reconcileLiveLease(f.client,f.plan,capability(),[]{return false;},[]{return true;}).location==LiveLeaseLocation::unknown&&f.wire.reclaims==0);require(reconcileLiveLease(f.client,f.plan,capability(),[]{return true;},[]{return false;}).location==LiveLeaseLocation::unknown&&f.wire.reclaims==0);require(reconcileLiveLease(f.client,f.plan,capability(),{},{}).location==LiveLeaseLocation::unknown);f.preserved();}
  {Fixture f;int calls=0;const auto result=reconcileLiveLease(f.client,f.plan,capability(),[&]{return ++calls==1;},[]{return true;});require(result.location==LiveLeaseLocation::unknown&&!result.attempted);f.preserved();}
  {Fixture f;bool authorized=true;f.wire.afterReclaim=[&]{authorized=false;};const auto result=reconcileLiveLease(f.client,f.plan,capability(),[&]{return authorized;},[]{return true;});require(result.location==LiveLeaseLocation::unknown&&result.attempted);f.preserved();}
}
void adoption(){
  {Fixture f;Controller resumed(f.client,newLease,capability());require(!resumed.adoptReclaimed(f.plan,[]{return true;},[]{return true;})&&!resumed.state().leaseActive);f.preserved();}
  {Fixture f;require(f.recover().location==LiveLeaseLocation::replacement);
    api::LeaseRequest timeout{};timeout.lease_id=newLease;timeout.requested_timeout_ms=7000;require(f.client.feed_lease(timeout).ok());
    {Controller resumed(f.client,newLease,capability());require(resumed.adoptReclaimed(f.plan,[]{return true;},[]{return true;}));const auto state=resumed.state();require(state.leaseActive&&!state.recoveryRequired&&state.targets.size()==2);
      require(state.targets[0].effective_timeout_ms==7000&&state.targets[1].effective_timeout_ms==7000);
      for(size_t i=0;i<2;++i)require(state.targets[i].target_id==f.plan.targets[i].target_id&&state.targets[i].os_adapter_luid==f.plan.targets[i].os_adapter_luid&&state.targets[i].connector_index==f.plan.targets[i].connector_index&&state.targets[i].lease_id==newLease);
      require(resumed.heartbeat()&&resumed.leaseCurrent());f.preserved();require(!resumed.adoptReclaimed(f.plan,[]{return true;},[]{return true;}));}
    require(f.wire.releases==1&&f.wire.backend.live.size()==1&&f.wire.backend.live.contains(999));
  }
  {Fixture f;require(f.recover().location==LiveLeaseLocation::replacement);{Controller wrong(f.client,oldLease,capability());require(!wrong.adoptReclaimed(f.plan,[]{return true;},[]{return true;}));}f.preserved();}
  {Fixture f;require(f.recover().location==LiveLeaseLocation::replacement);{Controller denied(f.client,newLease,capability());require(!denied.adoptReclaimed(f.plan,[]{return true;},[]{return false;}));}f.preserved();}
}
void portraitRecovery(){
  {Fixture f(true);
    const auto before=f.client.query_display_state();require(before.ok());bool portrait=false;
    for(size_t i=0;i<before.value.entry_count;++i)if(before.value.entries[i].display_id==71){
      portrait=true;require(before.value.entries[i].physical_width_mm==340&&before.value.entries[i].physical_height_mm==600);
    }
    require(portrait);
    require(inspectLiveLeaseRecovery(f.client,f.plan)==LiveLeaseLocation::previous);
    const auto recovered=f.recover();require(recovered.location==LiveLeaseLocation::replacement&&recovered.attempted&&recovered.acknowledged&&f.wire.reclaims==1);
    {Controller resumed(f.client,newLease,capability());require(resumed.adoptReclaimed(f.plan,[]{return true;},[]{return true;}));require(resumed.heartbeat()&&resumed.leaseCurrent());f.preserved();}
    require(f.wire.releases==1&&f.wire.backend.live.size()==1&&f.wire.backend.live.contains(999));
  }
  for(const auto& corrupt:std::vector<std::function<void(api::DisplayStateEntry&)>>{
      [](auto& entry){entry.physical_width_mm=600;entry.physical_height_mm=340;},
      [](auto& entry){++entry.physical_width_mm;},[](auto& entry){++entry.physical_height_mm;}}){
    Fixture f(true);f.wire.corruptState=[&](auto& state){for(size_t i=0;i<state.entry_count;++i)if(state.entries[i].display_id==71)corrupt(state.entries[i]);};
    const auto result=f.recover();require(result.location==LiveLeaseLocation::unknown&&!result.attempted&&f.wire.reclaims==0);
    f.wire.corruptState={};f.preserved();
  }
  {Fixture f(true);const auto result=f.recover(capability(99));require(result.location==LiveLeaseLocation::previous&&result.attempted&&!result.acknowledged);f.preserved();}
}
}
int main(){try{transfers();refusals();adoption();portraitRecovery();std::cout<<"Live lease recovery: "<<checks<<" checks passed; actual upstream client/dispatcher/controller/store, memory transport/backend. No Windows driver or display changes.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
