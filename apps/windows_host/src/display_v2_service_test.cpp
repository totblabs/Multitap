// Display manager v2: the SDK owner (NativeDisplayService) in v2 mode, driven
// through the actual SDK controller over an in-memory driver transport, plus
// the worker->service size RPC end to end. No real driver or display is used.
#include "service.hpp"
#include <iostream>
#ifdef MULTITAP_NATIVE_DISPLAY
#include "../tests/display-v2-world.hpp"
#include <virtual_display/driver/lease_store.h>
using namespace multitap::vdisplay;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
constexpr uint64_t ownedLease=api::kMinOpaqueLeaseId+321,foreignLease=api::kMinOpaqueLeaseId+654;
struct Transport final:api::ControlTransport {
  api::DisplayStore store{2,8};int creates=0,removes=0,releases=0,failCreate=0;std::vector<api::LeaseDisplayRequest> pending;
  bool ioctl(uint32_t code,const void* input,size_t,void* output,size_t,size_t& size,uint32_t& error)override{
    error=0;size=0;const auto now=std::chrono::steady_clock::now();
    if(code==api::kIoctlGetProtocolVersion){*static_cast<api::ProtocolVersion*>(output)={};size=sizeof(api::ProtocolVersion);return true;}
    if(code==api::kIoctlQueryLease){for(const auto& item:pending)require(store.remove_temporary_display(item).error==api::StoreError::None,"Fixture departure failed");pending.clear();*static_cast<api::QueryLeaseResult*>(output)=store.query_lease(static_cast<const api::LeaseRequest*>(input)->lease_id,now);size=sizeof(api::QueryLeaseResult);return true;}
    if(code==api::kIoctlCreateTemporaryDisplayOwned){++creates;if(creates==failCreate){error=5;return false;}const auto& request=*static_cast<const api::CreateTemporaryDisplayOwnedRequest*>(input);auto result=store.create_temporary_display_owned(request,now);if(result.status.error!=api::StoreError::None){error=5;return false;}result.result.target_id=uint32_t(creates);result.result.os_adapter_luid.low_part=44;*static_cast<api::CreateTemporaryDisplayResult*>(output)=result.result;size=sizeof(result.result);return true;}
    if(code==api::kIoctlRemoveTemporaryDisplay){++removes;const auto request=*static_cast<const api::LeaseDisplayRequest*>(input);require(request.lease_id==ownedLease&&request.display_id!=999,"Foreign display removal attempted");require(store.mark_temporary_display_pending_departure(request).error==api::StoreError::None,"Fixture remove failed");pending.push_back(request);return true;}
    if(code==api::kIoctlFeedLease){error=store.feed_lease(*static_cast<const api::LeaseRequest*>(input),now).error==api::StoreError::None?0:5;return !error;}
    if(code==api::kIoctlReleaseLease){++releases;const auto request=*static_cast<const api::LeaseRequest*>(input);require(request.lease_id==ownedLease,"Foreign lease release attempted");error=store.release_lease(request).error==api::StoreError::None?0:5;return !error;}
    throw std::runtime_error("Unexpected driver control");
  }
  void foreign(){api::CreateTemporaryDisplayOwnedRequest request{};request.display.lease_id=foreignLease;request.display.display_id=999;request.display.width=1280;request.display.height=720;request.display.physical_width_mm=600;request.display.physical_height_mm=340;request.display.refresh_rate_millihz=60000;request.display.requested_timeout_ms=15000;std::memcpy(request.display.display_name,"Other product",14);request.owner_capability.bytes.fill(9);require(store.create_temporary_display_owned(request,std::chrono::steady_clock::now()).status.error==api::StoreError::None,"Fixture foreign lease failed");}
  void checkForeign(){const auto display=store.find_temporary_display(999);require(display&&display->lease_id==foreignLease&&!display->pending_departure&&display->width==1280,"Foreign lease changed");}
};
struct Fixture {
  Transport transport;api::ControlClient client{transport};std::unique_ptr<host::NativeDisplayService> service;host::Json config;
  std::optional<std::string> journal,baseline;bool profilePending=false;std::string rejectPrivateCommitPhase;uint64_t recoveryTick=1000;
  // configure: test hooks that must be in place before the first tick.
  Fixture(bool v2,std::optional<host::NativeRuntimeRecord> boot={},bool verify=true,const std::string& instance="ROOT\\DISPLAY\\00a1",const std::function<void(host::NativeDisplayService&)>& configure={}){
    transport.foreign();api::OwnerCapability owner{};owner.bytes[0]=42;std::vector<Output> outputs{{71,1920,1080},{72,1280,720}};
    config=host::JsonParser::parse(R"({"provider":"libvirtualdisplay-1.6.4","enabled":true,"deviceInstanceId":"ROOT\\DISPLAY\\00a1","monitors":1})");
    std::get<host::Json::Object>(config.value).at("deviceInstanceId").value=instance;
    journal=host::nativeRuntimeBytes(boot?*boot:host::NativeRuntimeRecord{instance,"stopped",1,{{"0",1920,1080}}});
    service=std::make_unique<host::NativeDisplayService>([this,owner]{return std::make_unique<Controller>(client,ownedLease,owner,std::chrono::milliseconds(0));},outputs,[this]{return config;},[](const auto&){},
      host::NativeRuntimeJournalAccess{[this]{return journal;},[this](const auto& expected,const std::string& bytes){if(expected!=journal)return false;if(!rejectPrivateCommitPhase.empty()&&host::parseNativeRuntimeRecord(bytes).phase==rejectPrivateCommitPhase)return false;journal=bytes;return true;}},
      host::NativeRuntimeJournalAccess{[this]{return baseline;},[this](const auto& expected,const std::string& bytes){if(expected!=baseline)return false;baseline=bytes;return true;}},
      [this]{return quiescentOwnedLease(client,ownedLease);},[this]{return recoveryTick;},[this]{return profilePending;});
    service->setDisplayManagerV2(v2);
    if(configure)configure(*service);
    service->tick();
    if(verify&&state().get("leaseActive").boolean())require(service->activationReady(DWORD(state().get("generation").number())),"Fixture activation failed");
  }
  ~Fixture(){service.reset();}
  host::Json state(){return host::JsonParser::parse(service->viewportSnapshot());}
  host::NativeRuntimeRecord runtime(){return host::parseNativeRuntimeRecord(*journal);}
};
int sdkCases(){
  using namespace host;int passed=0;
  {Fixture f(true);const auto generation=f.state().get("generation").number();const auto creates=f.transport.creates;
    const auto snapshot=f.service->setOwnedSizeV2("0",1600,900);require(snapshot.has_value(),"v2 SDK size change was refused");
    const auto value=JsonParser::parse(*snapshot);const auto targets=nativeDisplayTargets(value);
    require(targets.size()==1&&targets[0].width==1600&&targets[0].height==900&&value.get("generation").number()>generation&&value.get("activationVerified").boolean()&&!value.get("recoveryRequired").boolean()&&!value.get("configurationPending").boolean(),"v2 SDK change did not publish a verified lease at the new size");
    require(f.runtime().phase=="stable"&&f.runtime().outputs[0].width==1600&&f.transport.creates==creates+1&&f.transport.removes==1,"v2 SDK change did not persist a stable runtime or recreate exactly one target");
    require(nativeActivationPublished(f.state())&&nativeDisplayLeaseKey(f.state())==nativeDisplayLeaseKey(value),"v2 snapshot differs from the published state");
    const auto unchanged=f.service->setOwnedSizeV2("0",1600,900);require(unchanged&&f.transport.creates==creates+1,"Same-size v2 request changed the SDK");
    f.transport.checkForeign();++passed;}
  {Fixture f(true);const auto creates=f.transport.creates;
    for(const auto& [monitor,width,height]:std::array<std::tuple<const char*,DWORD,DWORD>,5>{{{"0",1601,900},{"0",1600,901},{"0",UINT32_MAX-1u,900},{"1",1600,900},{"2",1600,900}}}){
      const auto accepted=f.service->setOwnedSizeV2(monitor,width,height);
      require(!accepted,("Invalid v2 size request was accepted: "+std::string(monitor)+" "+std::to_string(width)+"x"+std::to_string(height)).c_str());
    }
    require(f.transport.creates==creates&&!f.transport.removes,"Invalid v2 request reached the SDK");++passed;}
  // A representable wide fixture mode is not a hardware/capture capability
  // claim. The invalid table above checks actual stride overflow instead.
  {Fixture f(true);const auto changed=f.service->setOwnedSizeV2("0",4000,900);
    require(changed&&nativeDisplayTargets(f.state())[0].width==4000&&f.runtime().phase=="stable","A representable wide SDK fixture mode was refused");
    f.transport.checkForeign();++passed;}
  {Fixture f(false);require(!f.service->setOwnedSizeV2("0",1600,900)&&!f.transport.removes,"v1 mode accepted a v2 SDK request");++passed;}
  // SDK failure: the controller rolls back; no latch, the next request works.
  {Fixture f(true);f.transport.failCreate=f.transport.creates+1;
    require(!f.service->setOwnedSizeV2("0",1600,900),"Failed SDK create was reported as success");
    const auto after=f.state();require(after.get("leaseActive").boolean()&&!after.get("recoveryRequired").boolean()&&nativeDisplayTargets(after)[0].width==1920,"Rolled-back SDK failure latched recovery or lost the original size");
    require(f.service->setOwnedSizeV2("0",1280,720).has_value(),"SDK failure latched later v2 changes");f.transport.checkForeign();++passed;}
  // Sign-in capture must borrow a fresh proof from this exact held controller,
  // not the public cached snapshot or another peer's 'owned' boolean.
  {Fixture f(true);const std::string stream="00000000-0000-0000-0000-000000000001";
    const auto before=f.state();const auto instance=before.get("deviceInstanceId").string();const auto generation=uint32_t(before.get("generation").number());
    const auto creates=f.transport.creates,removes=f.transport.removes,releases=f.transport.releases;
    require(!f.service->proveOwnedStreamLease(stream,instance,generation),"Unreserved stream borrowed the native lease");
    require(f.service->reserveStreamProfile(stream,nativeDisplayLeaseKey(before)),"Owned proof fixture reservation failed");
    const auto proof=f.service->proveOwnedStreamLease(stream,instance,generation);
    require(proof&&proof->stream==stream&&proof->instance==instance&&proof->generation==generation&&
      proof->service==nativeStreamProcessIdentity(GetCurrentProcess())&&proof->targets.size()==1&&proof->targets.front().id=="0", "Actual held SDK controller did not prove its exact reservation");
    require(!f.service->proveOwnedStreamLease("00000000-0000-0000-0000-000000000002",instance,generation)&&
      !f.service->proveOwnedStreamLease(stream,"ROOT\\DISPLAY\\0001",generation)&&
      !f.service->proveOwnedStreamLease(stream,instance,generation+1),"Different stream/device/generation borrowed a proof");
    require(f.service->releaseStreamProfile(stream)&&!f.service->proveOwnedStreamLease(stream,instance,generation),"Released stream retained authority");
    require(f.transport.creates==creates&&f.transport.removes==removes&&f.transport.releases==releases,"Read-only proof created or released a native output");
    f.transport.checkForeign();++passed;}
  {Fixture f(false);const auto before=f.state();const std::string stream="00000000-0000-0000-0000-000000000001";
    require(f.service->reserveStreamProfile(stream,nativeDisplayLeaseKey(before))&&
      !f.service->proveOwnedStreamLease(stream,before.get("deviceInstanceId").string(),uint32_t(before.get("generation").number())),"Legacy display manager admitted the sign-in lease path");
    f.transport.checkForeign();++passed;}
  return passed;
}
int startupCases(){
  using namespace host;int passed=0;
  // v1 stream-profile journals never block the v2 SDK owner.
  {Fixture v1(false,{},false);v1.profilePending=true;v1.service->tick();
    require(v1.state().get("recoveryRequired").boolean(),"Fixture: v1 did not treat pending profile journals as blocking");
    Fixture v2(true,{},false);v2.profilePending=true;v2.service->tick();
    require(v2.state().get("leaseActive").boolean()&&!v2.state().get("recoveryRequired").boolean(),"v1 profile journals blocked the v2 SDK owner");
    require(v2.service->activationReady(DWORD(v2.state().get("generation").number()))&&v2.service->setOwnedSizeV2("0",1600,900).has_value(),"v2 owner was not usable with leftover v1 journals");++passed;}
  // A stale v1 checkpoint binding (uncertain runtime + missing baseline) used to
  // stop the controller forever; v2 starts it once the old lease is quiescent.
  {const NativeRuntimeRecord stale{"ROOT\\DISPLAY\\00a1","changing",5,{{"0",2324,952}},std::string(64,'a')};
    Fixture v1(false,stale,false);for(int i=0;i<3;++i){v1.recoveryTick+=60000;v1.service->tick();}
    require(!v1.state().get("leaseActive").boolean()&&v1.transport.creates==0,"Fixture: v1 accepted a broken checkpoint binding");
    Fixture v2(true,stale,false);for(int i=0;i<3&&!v2.state().get("leaseActive").boolean();++i){v2.recoveryTick+=60000;v2.service->tick();}
    require(v2.state().get("leaseActive").boolean()&&nativeDisplayTargets(v2.state())[0].width==2324,"v2 did not start the controller after a stale checkpoint binding");
    require(v2.service->activationReady(DWORD(v2.state().get("generation").number()))&&v2.runtime().phase=="stable"&&v2.runtime().baselineHash.empty(),"v2 kept the stale checkpoint binding");++passed;}
  return passed;
}
int selfHealCases(){
  using namespace host;int passed=0;
  for(const bool v2:{false,true}){
    Fixture f(v2);f.service->cancelViewportSession("lost-result",true);require(f.state().get("recoveryRequired").boolean(),"Fixture latch was not set");
    const auto generation=f.state().get("generation").number();f.recoveryTick+=60000;f.service->tick();f.service->tick();
    if(!v2){require(f.state().get("recoveryRequired").boolean(),"Fixture: v1 unexpectedly cleared its latch");++passed;continue;}
    const auto healed=f.state();
    require(!healed.get("recoveryRequired").boolean()&&healed.get("generation").number()>generation&&healed.get("leaseActive").boolean(),"v2 did not self-heal a live-lease recovery latch");
    require(f.service->activationReady(DWORD(healed.get("generation").number()))&&nativeActivationPublished(f.state()),"Self-healed v2 lease could not be re-verified");
    require(f.transport.removes==0&&f.transport.releases==0,"Self-heal recreated or released an output");f.transport.checkForeign();++passed;
  }
  return passed;
}
// ------------------------------------------------ owned output count (v2)
host::NativeCountRequest countKey(int count){return {{"10000000-0000-0000-0000-000000000001",std::string(43,'c')},count};}
int countSdkCases(){
  using namespace host;int passed=0;
  {Fixture f(true);const auto before=nativeDisplayTargets(f.state());const auto creates=f.transport.creates,removes=f.transport.removes;
    const auto snapshot=f.service->setOwnedCountV2(2);require(snapshot.has_value(),"v2 SDK count change was refused");
    const auto targets=nativeDisplayTargets(JsonParser::parse(*snapshot));
    require(targets.size()==2&&targets[0].id=="0"&&targets[0].target==before[0].target&&targets[0].width==before[0].width&&targets[1].id=="1"&&targets[1].width==targets[0].width&&targets[1].height==targets[0].height,"v2 count 2 did not add output 1 at output 0's size beside the untouched output 0");
    require(f.transport.creates==creates+1&&f.transport.removes==removes&&f.transport.releases==0,"v2 count 2 did more SDK work than adding one output");
    require(f.runtime().phase=="stable"&&f.runtime().outputs.size()==2&&f.runtime().baselineHash.empty(),"v2 count 2 was not committed in the runtime record");
    const auto state=f.state();require(state.get("countControlAvailable").boolean()&&nativeActivationPublished(state)&&nativeDisplayLeaseKey(state)==nativeDisplayLeaseKey(JsonParser::parse(*snapshot)),"v2 count 2 did not publish a verified, count-controllable lease");
    // The service's existing final acknowledgement (no v1 transaction pending).
    require(f.service->acknowledgeCountOperation(countKey(2),true)&&f.service->acknowledgeCountOperation(countKey(2),false),"v2 count result was not acknowledged");
    require(f.service->setOwnedCountV2(2)&&f.transport.creates==creates+1,"Same-count v2 request changed the SDK");
    const auto down=f.service->setOwnedCountV2(1);
    require(down&&nativeDisplayTargets(JsonParser::parse(*down)).size()==1&&nativeDisplayTargets(f.state())[0].target==before[0].target&&f.transport.removes==removes+1&&f.runtime().outputs.size()==1,"v2 count 1 did not remove only output 1");
    f.transport.checkForeign();++passed;}
  {Fixture f(true);for(const int invalid:{0,3,-1})require(!f.service->setOwnedCountV2(invalid),"Invalid v2 count accepted");
    Fixture v1(false);require(!v1.service->setOwnedCountV2(2)&&nativeDisplayTargets(v1.state()).size()==1&&!v1.transport.removes,"v1 mode accepted a v2 count request");++passed;}
  // SDK failure: the controller rolls back; no latch, the next request works.
  {Fixture f(true);f.transport.failCreate=f.transport.creates+1;
    require(!f.service->setOwnedCountV2(2),"Failed SDK create was reported as success");
    const auto after=f.state();require(after.get("leaseActive").boolean()&&!after.get("recoveryRequired").boolean()&&nativeDisplayTargets(after).size()==1&&f.runtime().phase=="stable"&&f.runtime().outputs.size()==1,"Rolled-back SDK count failure latched recovery or changed the count");
    require(f.service->setOwnedCountV2(2).has_value(),"SDK count failure latched later v2 count changes");f.transport.checkForeign();++passed;}
  // The committed count survives a clean service stop/start: the next streams
  // and the client's displayed count follow it, not the installer default.
  {Fixture f(true);require(f.service->setOwnedCountV2(2).has_value(),"Fixture count failed");f.service->shutdown();const auto stopped=f.runtime();
    require(stopped.phase=="stopped"&&stopped.outputs.size()==2,"Clean stop lost the v2 count");
    Fixture fresh(true,stopped);require(nativeDisplayTargets(fresh.state()).size()==2&&fresh.state().get("countControlAvailable").boolean(),"Restarted service did not keep the v2 count");++passed;}
  return passed;
}
// Worker-side DisplaySessionV2::count end to end: a Windows CCD model that
// follows the real SDK owner's lease, the count/size RPCs dispatched and
// authorized exactly like ServiceBridge::displayControl does.
struct CountHarness {
  Fixture f{true};v2world::World w;v2world::Memory m;std::vector<host::Json> inbound;int operationCount=2,countCalls=0,sizesCalls=0;bool cancelled=false;
  host::DisplayV2Access access(){
    host::DisplayV2Access a;
    a.owned=[this]{return host::nativeDisplayTargets(f.state());};
    a.query=[this]{const auto lease=host::nativeDisplayTargets(f.state());w.sync(lease);return w.query();};
    a.configure=[this](const host::NativeCcdData& ccd,UINT32 flags){return w.configure(ccd,flags);};
    const auto write=[this](const std::string& message){
      const auto reply=host::dispatchDisplayV2(host::JsonParser::parse(message),true,
        [this](const std::string& monitor,DWORD width,DWORD height){return f.service->setOwnedSizeV2(monitor,width,height);},
        [this](int requested)->std::optional<std::string>{++countCalls;if(!host::displayV2CountAuthorized(requested,operationCount,true,cancelled,true,true,true))return {};return f.service->setOwnedCountV2(requested);},
        [this](std::span<const host::DisplayV2Output> outputs){++sizesCalls;std::vector<host::NativeRuntimeOutput> sizes;for(const auto& o:outputs)sizes.push_back({o.id,o.width,o.height});return f.service->setOwnedSizesV2(sizes);});
      require(reply.handled&&!reply.response.empty(),"v2 RPC was not handled");inbound.push_back(host::JsonParser::parse(reply.response));return true;};
    const auto read=[this](DWORD)->std::optional<host::Json>{if(inbound.empty())return {};auto value=inbound.front();inbound.erase(inbound.begin());return value;};
    a.resize=[write,read](const std::string& monitor,DWORD width,DWORD height){return host::displayV2SizeRpc({host::utf8(host::randomName()),monitor,width,height},write,read,{},5000);};
    a.sizes=[write,read](std::span<const host::DisplayV2Output> outputs){return host::displayV2SizesRpc({host::utf8(host::randomName()),{outputs.begin(),outputs.end()}},write,read,{},5000);};
    a.count=[write,read](size_t count){return host::displayV2CountRpc({host::utf8(host::randomName()),int(count)},write,read,{},5000);};
    a.identifyConsole=v2world::identifyFixtureConsole;
    a.store=m.store();a.eligible=[this]{return w.eligible;};a.now=[this]{return w.clock;};a.pause=[this]{w.clock+=50;};
    return a;
  }
};
int countSessionCases(){
  using namespace host;int passed=0;
  // 1->2->1 with the console active, and with it inactive at rest while
  // Windows re-activates it on every SDK target arrival/departure.
  for(const bool inactive:{false,true}){
    CountHarness h;
    if(inactive){h.w.console().active=false;h.w.owned().x=0;h.w.afterCount=[](v2world::World& world){auto& c=world.console();c.active=true;c.x=0;c.y=0;c.width=c.dbWidth;c.height=c.dbHeight;};}
    DisplaySessionV2 session(h.access());
    require(h.w.target().target==nativeDisplayTargets(h.f.state())[0].target,"Fixture: CCD model and SDK lease disagree");
    const auto up=session.count("count-e2e",2,true,h.w.clock+90000);
    require(up.ok&&up.owned.size()==2&&!h.m.bytes&&h.f.runtime().phase=="stable"&&h.f.runtime().outputs.size()==2,"End-to-end 1->2 failed or was not committed by the SDK owner");
    require(h.w.hasSecond()&&h.w.owned().x==0&&h.w.second().active&&h.w.second().x==1920&&h.w.console().active==!inactive&&up.displaysReturned==inactive,"End-to-end 1->2 left a wrong Windows topology");
    // The next dual stream (v1 tracks path) starts from the real lease and this topology.
    const auto targets=nativeDisplayTargets(h.f.state());const auto ccd=h.w.query();const auto baseline=captureNativeTopologyBaseline(ccd,targets,"0");
    require(nativeTopologyMatches(planNativeTopology(ccd,targets,baseline.placements),ccd,ccd)&&h.f.state().get("countControlAvailable").boolean()&&h.f.service->acknowledgeCountOperation(countKey(2),true),"End-to-end 1->2 does not meet the dual-stream start preconditions");
    h.operationCount=1;const auto down=session.count("count-e2e",1,true,h.w.clock+90000);
    require(down.ok&&!h.w.hasSecond()&&h.f.runtime().outputs.size()==1&&h.w.console().active==!inactive&&h.w.owned().x==0&&!h.m.bytes,"End-to-end 2->1 failed");
    h.f.transport.checkForeign();++passed;
  }
  // Persistent foreign change: stage 8 with its code; the previous count goes
  // back through the same RPC and the SDK owner's runtime record follows.
  {CountHarness h;h.w.afterApply=[](v2world::World& world){if(world.hasSecond())world.console().refreshN=50;};
    DisplaySessionV2 session(h.access());const auto failed=session.count("c",2,true,h.w.clock+90000);
    require(!failed.ok&&failed.stage==displayV2Stage::foreign&&failed.foreignDifference==displayV2ForeignCode::refresh&&failed.restore&&failed.restore->outcome==DisplayV2Outcome::restored&&h.countCalls==2,"End-to-end persistent foreign change did not fail with its code and put the count back");
    require(h.f.runtime().phase=="stable"&&h.f.runtime().outputs.size()==1&&nativeDisplayTargets(h.f.state()).size()==1&&!h.w.hasSecond()&&!h.m.bytes,"End-to-end failure left the SDK count or the state file");
    require(h.w.console().refreshN==60&&h.w.console().active&&h.w.console().x==0&&h.w.owned().x==1024,"End-to-end failure did not restore the original configuration");
    h.w.afterApply=nullptr;require(session.count("c",2,true,h.w.clock+90000).ok&&h.f.runtime().outputs.size()==2,"End-to-end count after a failure latched");
    h.f.transport.checkForeign();++passed;}
  // A cancelled operation cannot apply its own count; nothing changes.
  {CountHarness h;h.cancelled=true;DisplaySessionV2 session(h.access());const auto refused=session.count("c",2,true,h.w.clock+90000);
    require(!refused.ok&&refused.stage==displayV2Stage::sdk&&h.f.runtime().outputs.size()==1&&!h.w.hasSecond()&&!h.m.bytes&&h.w.owned().x==1024&&h.w.console().active,"A cancelled count operation changed the SDK or Windows");++passed;}
  return passed;
}
// ------------------------------------------ independent outputs (v2 group)
int sizesSdkCases(){
  using namespace host;int passed=0;
  // Both outputs in one SDK reconfiguration (one generation step).
  {Fixture f(true);require(f.service->setOwnedCountV2(2).has_value(),"Fixture count failed");const auto generation=f.state().get("generation").number();const auto creates=f.transport.creates;
    const std::vector<NativeRuntimeOutput> sizes{{"0",1600,900},{"1",1280,720}};
    const auto snapshot=f.service->setOwnedSizesV2(sizes);require(snapshot.has_value(),"v2 SDK sizes change was refused");
    const auto value=JsonParser::parse(*snapshot);const auto targets=nativeDisplayTargets(value);
    require(targets.size()==2&&targets[0].id=="0"&&targets[0].width==1600&&targets[0].height==900&&targets[1].width==1280&&targets[1].height==720&&value.get("generation").number()==generation+1&&value.get("activationVerified").boolean()&&!value.get("recoveryRequired").boolean()&&!value.get("configurationPending").boolean(),"v2 SDK sizes did not publish both sizes in one generation");
    require(f.runtime().phase=="stable"&&f.runtime().outputs.size()==2&&f.runtime().outputs[0].width==1600&&f.runtime().outputs[1].width==1280&&f.transport.creates>creates,"v2 SDK sizes were not committed");
    const auto created=f.transport.creates;require(f.service->setOwnedSizesV2(sizes)&&f.transport.creates==created,"Same-size v2 sizes request changed the SDK");
    f.transport.checkForeign();++passed;}
  // Refusals: duplicate, unknown or absent output, invalid sizes, empty; v1 mode.
  {Fixture f(true);const auto creates=f.transport.creates;
    for(const auto& bad:std::vector<std::vector<NativeRuntimeOutput>>{{},{{"0",1600,900},{"0",1280,720}},{{"1",1600,900}},{{"2",1600,900}},{{"0",1601,900}},{{"0",1600,178}},{{"0",UINT32_MAX-1u,900}},{{"0",1600,900},{"1",1280,720},{"1",1280,720}}})
      require(!f.service->setOwnedSizesV2(bad),"Invalid v2 sizes request was accepted");
    require(f.transport.creates==creates&&!f.transport.removes,"Invalid v2 sizes request reached the SDK");
    Fixture v1(false);const std::vector<NativeRuntimeOutput> one{{"0",1600,900}};require(!v1.service->setOwnedSizesV2(one)&&!v1.transport.removes,"v1 mode accepted a v2 sizes request");++passed;}
  {Fixture f(true);require(f.service->setOwnedCountV2(2).has_value(),"Fixture count failed");
    const std::vector<NativeRuntimeOutput> sizes{{"0",4000,900},{"1",1280,720}};
    require(f.service->setOwnedSizesV2(sizes).has_value()&&nativeDisplayTargets(f.state())[0].width==4000&&nativeDisplayTargets(f.state())[1].width==1280&&f.runtime().phase=="stable","A representable wide SDK group fixture was refused");
    f.transport.checkForeign();++passed;}
  // SDK failure: the controller rolls back; no latch, the next request works.
  {Fixture f(true);require(f.service->setOwnedCountV2(2).has_value(),"Fixture count failed");f.transport.failCreate=f.transport.creates+1;
    const std::vector<NativeRuntimeOutput> sizes{{"0",1600,900},{"1",1280,720}};
    require(!f.service->setOwnedSizesV2(sizes),"Failed SDK sizes change was reported as success");
    const auto after=f.state();require(after.get("leaseActive").boolean()&&!after.get("recoveryRequired").boolean()&&nativeDisplayTargets(after).size()==2&&nativeDisplayTargets(after)[0].width==1920&&f.runtime().phase=="stable","Rolled-back SDK sizes failure latched recovery or lost the sizes");
    require(f.service->setOwnedSizesV2(sizes).has_value(),"SDK sizes failure latched later changes");f.transport.checkForeign();++passed;}
  return passed;
}
// Worker-side DisplaySessionV2::startGroup end to end over the size RPCs and
// the real SDK owner: count 2 first (the client's separate step), then the
// dual stream, then its stop. Console active at rest, and inactive while
// Windows re-activates it on every target change.
int groupSessionCases(){
  using namespace host;int passed=0;
  const std::vector<DisplayV2Output> group{{"0",1600,900,0,0},{"1",1280,720,1600,0}};
  for(const bool inactive:{false,true}){
    CountHarness h;
    if(inactive){h.w.console().active=false;h.w.owned().x=0;h.w.afterCount=[](v2world::World& world){auto& c=world.console();c.active=true;c.x=0;c.y=0;c.width=c.dbWidth;c.height=c.dbHeight;};}
    {DisplaySessionV2 count(h.access());require(count.count("count-e2e",2,true,h.w.clock+90000).ok&&!h.m.bytes&&h.f.runtime().outputs.size()==2,"Fixture end-to-end count 2 failed");}
    if(inactive)h.w.afterCount=[](v2world::World& world){auto& c=world.console();c.active=true;c.x=-1024;c.y=300;c.width=c.dbWidth;c.height=c.dbHeight;};
    const auto generation=h.f.state().get("generation").number();
    DisplaySessionV2 stream(h.access());const auto started=stream.startGroup("group-e2e",group,true,h.w.clock+90000);
    require(started.ok&&h.m.bytes&&h.sizesCalls==1&&h.f.state().get("generation").number()==generation+1&&h.f.runtime().outputs[0].width==1600&&h.f.runtime().outputs[1].width==1280,"End-to-end group start did not size both outputs in one SDK generation");
    require(h.w.owned().active&&!h.w.owned().x&&!h.w.owned().y&&h.w.owned().width==1600&&h.w.second().active&&h.w.second().x==1600&&!h.w.second().y&&h.w.second().width==1280&&!h.w.console().active&&!v2world::overlapping(h.w),"End-to-end group start left a wrong Windows topology");
    require(stream.restore().outcome==DisplayV2Outcome::restored&&!h.m.bytes&&h.sizesCalls==2&&h.f.runtime().phase=="stable"&&h.f.runtime().outputs[0].width==1920&&h.f.runtime().outputs[1].width==1920,"End-to-end group stop did not restore the SDK sizes");
    require(!h.w.owned().x&&h.w.second().active&&h.w.second().x==1920&&h.w.console().active==!inactive&&(inactive||h.w.console().x==3840),"End-to-end group stop did not restore Windows");
    h.f.transport.checkForeign();++passed;
  }
  return passed;
}
int rpcCases(){
  using namespace host;int passed=0;
  Fixture f(true);std::vector<Json> inbound;
  const auto write=[&](const std::string& message){const auto reply=dispatchDisplayV2(JsonParser::parse(message),true,[&](const std::string& monitor,DWORD width,DWORD height){return f.service->setOwnedSizeV2(monitor,width,height);});require(reply.handled,"v2 size request was not handled");inbound.push_back(JsonParser::parse(reply.response));return true;};
  const auto read=[&](DWORD)->std::optional<Json>{if(inbound.empty())return {};auto value=inbound.front();inbound.erase(inbound.begin());return value;};
  const auto targets=displayV2SizeRpc({utf8(randomName()),"0",2560,1440},write,read,{},5000);
  require(targets&&targets->size()==1&&targets->front().width==2560&&targets->front().height==1440&&nativeDisplayTargets(f.state())[0].width==2560,"Worker->service->SDK size RPC did not change the owned output");
  require(!displayV2SizeRpc({utf8(randomName()),"0",1600,900},[&](const std::string& message){inbound.push_back(JsonParser::parse(dispatchDisplayV2(JsonParser::parse(message),false,[&](const std::string& m,DWORD w,DWORD h){return f.service->setOwnedSizeV2(m,w,h);}).response));return true;},read,{},5000)&&nativeDisplayTargets(f.state())[0].width==2560,"Unauthorized RPC changed the SDK");
  f.transport.checkForeign();++passed;
  return passed;
}
}
int privatePreparationCases(){
  using namespace host;int passed=0;
  const std::string stream="00000000-0000-0000-0000-000000000017";
  for(const bool activated:{false,true}){
    Fixture fixture(true,{},activated,"ROOT\\DISPLAY\\00A3");
    const auto before=fixture.state();const auto instance=before.get("deviceInstanceId").string();
    const auto generation=uint32_t(before.get("generation").number());
    require(fixture.runtime().phase==(activated?"stable":"starting"),"Cold SDK fixture was silently activated");++passed;
    if(!activated){require(!fixture.service->reserveStreamProfile(stream,nativeDisplayLeaseKey(before)),"Ordinary stream admitted cold unverified SDK outputs");++passed;}
    const auto creates=fixture.transport.creates,removes=fixture.transport.removes;
    require(fixture.service->reservePreparation(stream,nativeDisplayLeaseKey(before))&&
      fixture.transport.creates==creates&&fixture.transport.removes==removes,"Private reservation recreated controller outputs");++passed;
    const auto owned=fixture.service->preparationLease(stream,instance,generation);
    require(owned&&owned->phase==(activated?"stable":"starting")&&owned->targets.size()==1&&
      !fixture.state().get("activationVerified").boolean(),"Private SDK proof advertised capture readiness");++passed;
    require(!fixture.service->reservePreparation(stream,nativeDisplayLeaseKey(before))&&
      !fixture.service->reserveStreamProfile("00000000-0000-0000-0000-000000000018",nativeDisplayLeaseKey(before))&&
      !fixture.service->proveOwnedStreamLease(stream,instance,generation),"Private owner was reused as ordinary capture authority");++passed;
    require(!fixture.service->setOwnedSizeV2("0",1600,900)&&!fixture.service->setOwnedCountV2(2)&&
      !fixture.service->activationReady(generation)&&!fixture.service->releaseStreamProfile(stream)&&
      fixture.transport.creates==creates&&fixture.transport.removes==removes,"Ordinary mutator escaped private preparation purpose");++passed;
    require(!fixture.service->preparationLease(stream,instance,generation+1)&&
      !fixture.service->setPreparationOriginalBoundSize(stream,instance,generation+1,1280,720),"Stale private generation reached SDK");++passed;
    const auto resized=fixture.service->setPreparationOriginalBoundSize(stream,instance,generation,1280,720);
    require(resized&&resized->generation>generation&&resized->targets[0].width==1280&&
      fixture.runtime().phase==(activated?"stable":"starting")&&!fixture.state().get("activationVerified").boolean(),
      "Private size promoted starting to stable/readiness");++passed;
    require(fixture.service->refreshPreparationLease(stream)&&
      !fixture.service->refreshPreparationLease("00000000-0000-0000-0000-000000000018"),"Private heartbeat accepted another reservation");++passed;
    fixture.service->tick();require(fixture.runtime().phase==(activated?"stable":"starting")&&
      !fixture.state().get("activationVerified").boolean(),"Service tick promoted a held private preparation");++passed;
    require(fixture.service->releasePreparation(stream)&&!fixture.service->hasStreamProfileReservation()&&
      !fixture.service->preparationLease(stream,instance,resized->generation)&&
      fixture.state().get("activationVerified").boolean()==activated,"Private release changed prior readiness or retained authority");++passed;
    fixture.transport.checkForeign();
  }
  {
    Fixture fixture(true,{},false,"ROOT\\DISPLAY\\00A4");const auto state=fixture.state();
    const auto instance=state.get("deviceInstanceId").string();const auto generation=uint32_t(state.get("generation").number());
    require(fixture.service->reservePreparation(stream,nativeDisplayLeaseKey(state)),"Private release-failure fixture could not reserve");
    unsigned failedWrites=0;
    fixture.service->testSnapshotPublisher([&](const std::string&){++failedWrites;throw std::runtime_error("Injected snapshot publication failure");},true);
    require(!fixture.service->releasePreparation(stream)&&fixture.service->hasStreamProfileReservation()&&
      failedWrites==1&&fixture.service->preparationCleanupLease(stream,instance,generation).has_value()&&
      !fixture.service->preparationLease(stream,instance,generation)&&!fixture.state().get("activationVerified").boolean(),
      "Failed private release lost its actual owner/purpose or advertised readiness");++passed;
    fixture.service->testSnapshotPublisher([](const std::string&){});
    require(fixture.service->releasePreparation(stream)&&!fixture.service->hasStreamProfileReservation()&&fixture.runtime().phase=="starting",
      "Private release retry could not finish the same cold reservation");++passed;
    fixture.transport.checkForeign();
  }
  return passed;
}
int privatePreparationCommitCases(){
  using namespace host;int passed=0;
  const std::string stream="00000000-0000-0000-0000-000000000019",other="00000000-0000-0000-0000-000000000020";
  for(const bool activated:{false,true})for(const bool runtimeFailure:{false,true}){
    Fixture fixture(true,{},activated,"ROOT\\DISPLAY\\00B1");const auto before=fixture.state();
    const auto instance=before.get("deviceInstanceId").string();const auto generation=uint32_t(before.get("generation").number());
    const auto targets=nativeDisplayTargets(before);v2world::World world;world.sync(targets);
    const auto original=displayV2StateBytes({stream,world.query(),targets,{}});v2world::Memory saved;
    require(saved.store().save(original)&&fixture.service->reservePreparation(stream,nativeDisplayLeaseKey(before)),"Private commit fixture could not retain original/reserve");
    const auto phase=activated?"stable":"starting";
    unsigned failedWrites=0;
    fixture.rejectPrivateCommitPhase=runtimeFailure?phase:"";
    fixture.service->testSnapshotPublisher([&](const std::string&){++failedWrites;if(!runtimeFailure)throw std::runtime_error("Injected private snapshot failure");},true);
    const auto creates=fixture.transport.creates,removes=fixture.transport.removes,releases=fixture.transport.releases;
    require(!fixture.service->setPreparationOriginalBoundSize(stream,instance,generation,1280,720),"Injected private commit failure was reported successful");
    const auto after=fixture.state();auto current=uint32_t(after.get("generation").number());const auto actual=nativeDisplayTargets(after);
    require(actual.size()==1&&actual[0].width==1280&&actual[0].height==720&&current>generation&&
      fixture.transport.creates==creates+1&&fixture.transport.removes==removes+1&&fixture.transport.releases==releases&&
      fixture.runtime().phase==(runtimeFailure?"changing":phase)&&failedWrites==(runtimeFailure?0u:1u),"Failure did not follow actual SDK success at the correct commit boundary");++passed;
    require(fixture.service->preparationCleanupLease(stream,instance,current).has_value()&&
      !fixture.service->preparationLease(stream,instance,current)&&!fixture.service->refreshPreparationLease(stream)&&
      !fixture.service->setPreparationOriginalBoundSize(stream,instance,current,1600,900)&&
      !fixture.service->setOwnedSizeV2("0",1600,900)&&!fixture.service->setOwnedCountV2(2)&&
      !fixture.service->activationReady(current)&&!fixture.service->proveOwnedStreamLease(stream,instance,current)&&
      fixture.service->refreshPreparationCleanupLease(stream)&&!fixture.service->releasePreparation(stream)&&
      saved.bytes==std::optional(original)&&fixture.service->hasStreamProfileReservation(),"Private commit failure opened forward capture or lost exact cleanup heartbeat/original");++passed;
    const auto noCreates=fixture.transport.creates,noRemoves=fixture.transport.removes;
    for(const auto& key:std::array<std::tuple<std::string,std::string,uint32_t>,3>{{{other,instance,current},{stream,"ROOT\\DISPLAY\\FFFF",current},{stream,instance,current+1}}}){
      require(!fixture.service->preparationCleanupLease(std::get<0>(key),std::get<1>(key),std::get<2>(key))&&
        !fixture.service->restorePreparationOriginalBoundSize(std::get<0>(key),std::get<1>(key),std::get<2>(key),1920,1080),"Foreign/stale cleanup reached SDK");
    }
    require(!fixture.service->refreshPreparationCleanupLease(other)&&fixture.transport.creates==noCreates&&fixture.transport.removes==noRemoves,"Invalid cleanup proof mutated SDK");++passed;
    const auto durable=fixture.journal;fixture.journal=*durable+" ";
    require(!fixture.service->preparationCleanupLease(stream,instance,current)&&!fixture.service->refreshPreparationCleanupLease(stream)&&
      !fixture.service->restorePreparationOriginalBoundSize(stream,instance,current,1920,1080)&&
      fixture.transport.creates==noCreates&&fixture.transport.removes==noRemoves&&saved.bytes==std::optional(original),"External journal CAS change admitted cleanup or lost original");
    fixture.journal=durable;++passed;
    // Persistent publication/runtime failure may restore the original SDK mode
    // once, but must neither release its obligation nor recreate it on retries.
    require(!fixture.service->restorePreparationOriginalBoundSize(stream,instance,current,targets[0].width,targets[0].height),"Persistent cleanup IO failure reported success");
    current=uint32_t(fixture.state().get("generation").number());const auto restoredCreates=fixture.transport.creates;
    require(nativeDisplayTargets(fixture.state())[0].width==targets[0].width&&
      fixture.service->preparationCleanupLease(stream,instance,current).has_value()&&
      !fixture.service->restorePreparationOriginalBoundSize(stream,instance,current,targets[0].width,targets[0].height)&&
      fixture.transport.creates==restoredCreates&&!fixture.service->releasePreparation(stream)&&
      fixture.service->hasStreamProfileReservation()&&saved.bytes==std::optional(original),"Persistent IO released original or recreated an already restored SDK mode");++passed;
    fixture.rejectPrivateCommitPhase.clear();unsigned healedWrites=0;
    fixture.service->testSnapshotPublisher([&](const std::string&){++healedWrites;},true);
    const auto healed=fixture.service->restorePreparationOriginalBoundSize(stream,instance,current,targets[0].width,targets[0].height);
    require(healed&&healedWrites==1&&fixture.transport.creates==restoredCreates&&fixture.runtime().phase==phase&&
      !fixture.state().get("recoveryRequired").boolean()&&!fixture.state().get("activationVerified").boolean()&&
      !fixture.service->preparationLease(stream,instance,healed->generation)&&
      !fixture.service->setPreparationOriginalBoundSize(stream,instance,healed->generation,1600,900)&&
      fixture.service->preparationCleanupLease(stream,instance,healed->generation).has_value(),"Durable cleanup retry did not heal exact controller or reopened forward preparation");++passed;
    require(fixture.service->releasePreparation(stream)&&!fixture.service->hasStreamProfileReservation()&&
      fixture.runtime().phase==phase&&fixture.state().get("activationVerified").boolean()==activated&&
      saved.bytes==std::optional(original)&&fixture.transport.releases==releases,"Cleanup release promoted cold state, lost baseline, or released/reacquired controller");++passed;
    fixture.transport.checkForeign();
  }
  for(const bool leaseGone:{false,true}){
    Fixture fixture(true,{},false,"ROOT\\DISPLAY\\00B2");const auto before=fixture.state();const auto instance=before.get("deviceInstanceId").string();
    const auto generation=uint32_t(before.get("generation").number());require(fixture.service->reservePreparation(stream,nativeDisplayLeaseKey(before)),"Recovery refusal fixture could not reserve");
    if(leaseGone){api::LeaseRequest request{};request.lease_id=ownedLease;require(fixture.transport.store.release_lease(request).error==api::StoreError::None,"Fixture lease loss failed");}
    else fixture.service->activationRecovery(generation);
    const auto creates=fixture.transport.creates,removes=fixture.transport.removes;
    require(!fixture.service->preparationCleanupLease(stream,instance,generation)&&
      !fixture.service->restorePreparationOriginalBoundSize(stream,instance,generation,1280,720)&&
      !fixture.service->refreshPreparationCleanupLease(stream)&&!fixture.service->releasePreparation(stream)&&
      fixture.transport.creates==creates&&fixture.transport.removes==removes&&fixture.service->hasStreamProfileReservation(),"Unknown recovery/lease loss manufactured cleanup authority");++passed;
    fixture.transport.checkForeign();
  }
  return passed;
}
// ------------------------------------------------ render adapter re-arm
// The SDK owner's one-per-lifetime re-arm (native_display_service.hpp,
// hostAction 282): the fixture controller path applies the render preference
// at every cold start exactly where the production path does, and the bridge
// idle probe and the hardware adapter choice are plain switches here.
struct RearmHarness {
  bool idle=true,hardware=true;std::string status;unsigned prefers=0;std::vector<int> createsAtPrefer;std::unique_ptr<Fixture> f;
  explicit RearmHarness(const char* preference="accepted",bool hardwareFound=true,bool installProbe=true,bool v2=true):hardware(hardwareFound),status(preference){
    f=std::make_unique<Fixture>(v2,std::nullopt,true,"ROOT\\DISPLAY\\00a1",[this,installProbe](host::NativeDisplayService& service){
      service.testRenderPreference([this]{++prefers;createsAtPrefer.push_back(f?f->transport.creates:0);host::NativeRenderPreference p;p.status=status=="accepted"?"accepted":"skipped";p.reason=status=="accepted"?"request_acknowledged":"no_usable_hardware_adapter";if(status=="accepted")p.requested=0x85aeull;return p;},[this]{return hardware;});
      if(installProbe)service.setRenderRearmIdle([this]{return idle;});
    });
  }
};
int rearmCases(){
  using namespace host;int passed=0;
  // Idle display: one orderly stop ("stopped", no recovery), then the next tick's cold
  // start applies the preference before it creates the new output. Once per lifetime.
  for(const bool v2:{true,false}){RearmHarness h("accepted",true,true,v2);auto& f=*h.f;
    require(h.prefers==1&&h.createsAtPrefer==std::vector<int>{0},"Fixture: the cold start did not apply the preference before its output");
    const auto generation=uint32_t(f.state().get("generation").number());const auto releases=f.transport.releases;
    require(f.service->requestRenderRearm(),"Re-arm request was refused although the preference was accepted");
    f.service->tick();
    require(f.transport.releases==releases+1&&f.runtime().phase=="stopped"&&!f.state().get("leaseActive").boolean()&&!f.state().get("recoveryRequired").boolean()&&h.prefers==1,"Re-arm did not stop the controller like an orderly service stop");
    f.service->tick();
    require(h.prefers==2&&h.createsAtPrefer==std::vector<int>({0,1})&&f.transport.creates==2&&f.state().get("leaseActive").boolean()&&f.runtime().phase=="starting"&&f.state().get("generation").number()==generation+1,"Cold start after the re-arm did not apply the preference before creating the new output");
    require(f.service->activationReady(generation+1)&&f.runtime().phase=="stable"&&nativeActivationPublished(f.state()),"Re-armed controller could not be verified");
    require(!f.service->requestRenderRearm(),"A second re-arm was accepted in the same service lifetime");
    f.service->tick();f.service->tick();
    require(f.transport.releases==releases+1&&f.transport.creates==2&&h.prefers==2&&f.state().get("leaseActive").boolean(),"A second re-arm happened");
    f.transport.checkForeign();++passed;}
  // The bridge still holds the display (a stream, console, recovery or unexited
  // worker): nothing happens until it is idle; the request is kept, not dropped.
  {RearmHarness h;h.idle=false;auto& f=*h.f;require(f.service->requestRenderRearm(),"Re-arm request was refused");
    for(int i=0;i<3;++i)f.service->tick();
    require(f.transport.releases==0&&f.state().get("leaseActive").boolean()&&f.runtime().phase=="stable"&&h.prefers==1,"Re-arm interrupted a display the bridge still holds");
    h.idle=true;f.service->tick();require(f.transport.releases==1&&f.runtime().phase=="stopped","Re-arm did not run once the display became idle");
    f.service->tick();require(h.prefers==2&&f.state().get("leaseActive").boolean(),"Cold start after the deferred re-arm failed");f.transport.checkForeign();++passed;}
  // A reservation (the tick's hold path) defers it the same way.
  {RearmHarness h;auto& f=*h.f;const std::string stream="00000000-0000-0000-0000-000000000001";
    require(f.service->reservePreparation(stream,nativeDisplayLeaseKey(f.state())),"Fixture reservation failed");
    require(f.service->requestRenderRearm(),"Re-arm request was refused");
    for(int i=0;i<3;++i)f.service->tick();
    require(f.transport.releases==0&&f.state().get("leaseActive").boolean()&&f.service->hasStreamProfileReservation(),"Re-arm ran under a reservation hold");
    require(f.service->releasePreparation(stream),"Fixture release failed");
    f.service->tick();require(f.transport.releases==1&&f.runtime().phase=="stopped","Re-arm did not run after the hold ended");f.transport.checkForeign();++passed;}
  // Activation recovery pending: waits for the fresh verification, then runs.
  {RearmHarness h;auto& f=*h.f;const auto generation=uint32_t(f.state().get("generation").number());
    f.service->activationRecovery(generation,true);require(f.service->requestRenderRearm(),"Re-arm request was refused");
    f.service->tick();require(f.transport.releases==0&&f.state().get("recoveryRequired").boolean(),"Re-arm ran during activation recovery");
    f.recoveryTick+=60000;f.service->tick();
    require(f.transport.releases==0&&f.state().get("generation").number()==generation+1,"Fixture: the recovery retry did not advance");
    require(f.service->activationReady(generation+1),"Fixture: re-verification failed");
    f.service->tick();require(f.transport.releases==1&&f.runtime().phase=="stopped","Re-arm did not run after recovery completed");f.transport.checkForeign();++passed;}
  // The SDK never accepted a hardware adapter: refused at the request.
  {RearmHarness h("skipped");auto& f=*h.f;require(!f.service->requestRenderRearm(),"Re-arm was accepted without an accepted preference");
    f.service->tick();f.service->tick();require(f.transport.releases==0&&h.prefers==1&&f.state().get("leaseActive").boolean(),"Re-arm ran without an accepted preference");++passed;}
  // No hardware adapter at the idle tick: skipped for good, and it was the one request.
  {RearmHarness h("accepted",false);auto& f=*h.f;require(f.service->requestRenderRearm(),"Re-arm request was refused");
    f.service->tick();f.service->tick();require(f.transport.releases==0&&h.prefers==1&&f.state().get("leaseActive").boolean(),"Re-arm ran without a hardware adapter");
    h.hardware=true;require(!f.service->requestRenderRearm(),"A second request was accepted after a skipped one");f.service->tick();require(f.transport.releases==0,"A skipped re-arm came back");++passed;}
  // No idle probe installed: fail closed, also when one arrives later.
  {RearmHarness h("accepted",true,false);auto& f=*h.f;require(f.service->requestRenderRearm(),"Re-arm request was refused");
    f.service->tick();f.service->tick();require(f.transport.releases==0&&h.prefers==1,"Re-arm ran without an idle probe");
    f.service->setRenderRearmIdle([]{return true;});f.service->tick();require(f.transport.releases==0&&f.state().get("leaseActive").boolean(),"A skipped re-arm ran once a probe arrived");++passed;}
  return passed;
}
// ------------------------------------------------------- console park
// The SDK owner's part of a console opened with nobody at an unlocked
// administrator's session (qemu_console_route.hpp): an orderly stop that is
// held, renewed and ended by the bridge's console hold, then the ordinary cold
// start. The fixture clock is the park's clock.
int consoleParkCases(){
  using namespace host;int passed=0;
  // Park: one orderly stop, nothing recreated while it is held, then one cold start.
  {Fixture f(true);const auto generation=uint32_t(f.state().get("generation").number());const auto releases=f.transport.releases,creates=f.transport.creates;
    require(!f.service->consoleParked(),"Fixture: parked before any console");
    require(f.service->parkForConsole(35000)==NativeConsolePark::parked,"An idle verified display refused the console park");
    require(f.transport.releases==releases+1&&f.runtime().phase=="stopped"&&!f.state().get("leaseActive").boolean()&&!f.state().get("recoveryRequired").boolean()&&f.service->consoleParked(),"The park did not stop the controller like an orderly service stop");
    for(int i=0;i<4;++i){f.recoveryTick+=5000;f.service->tick();}
    require(f.transport.creates==creates&&f.transport.releases==releases+1&&!f.state().get("leaseActive").boolean()&&!f.state().get("recoveryRequired").boolean()&&f.runtime().phase=="stopped","A held park let the outputs come back, or looked like a recovery");
    require(!f.service->setOwnedSizeV2("0",1600,900)&&!f.service->setOwnedCountV2(2)&&!f.service->reservePreparation("00000000-0000-0000-0000-000000000001",""),"A parked display accepted a stream's SDK request");
    // A second request for the same park only extends it.
    require(f.service->parkForConsole(35000)==NativeConsolePark::parked&&f.transport.releases==releases+1,"A repeated park stopped something twice");
    f.service->endConsolePark();require(!f.service->consoleParked()&&!f.service->renewConsolePark(35000),"An ended park could still be renewed");
    f.service->tick();
    require(f.transport.creates==creates+1&&f.state().get("leaseActive").boolean()&&f.runtime().phase=="starting"&&f.state().get("generation").number()==generation+1,"The end of the park was not the ordinary cold start");
    require(f.service->activationReady(generation+1)&&f.runtime().phase=="stable"&&nativeActivationPublished(f.state()),"The display could not be verified after a park");
    require(f.service->setOwnedSizeV2("0",1600,900).has_value(),"A stream's SDK request failed after a park");
    f.transport.checkForeign();++passed;}
  // Renewed in time it stays; not renewed it runs out and the outputs return by themselves.
  {Fixture f(true);const auto creates=f.transport.creates;
    require(f.service->parkForConsole(35000)==NativeConsolePark::parked,"Fixture park failed");
    f.recoveryTick+=34000;f.service->tick();require(f.transport.creates==creates&&f.service->renewConsolePark(35000),"A park inside its time was lost or could not be renewed");
    f.recoveryTick+=34000;f.service->tick();require(f.transport.creates==creates&&f.service->consoleParked(),"A renewed park ran out at its first deadline");
    f.recoveryTick+=1000;require(!f.service->consoleParked()&&!f.service->renewConsolePark(35000),"A park past its time was renewed");
    f.service->tick();require(f.transport.creates==creates+1&&f.state().get("leaseActive").boolean()&&!f.state().get("recoveryRequired").boolean(),"A park nobody renewed kept the outputs away");
    f.transport.checkForeign();++passed;}
  // Fail closed: v1, a reservation, activation recovery or no time leave the display exactly as it is.
  {Fixture f(false);const auto releases=f.transport.releases;require(f.service->parkForConsole(35000)==NativeConsolePark::refused&&f.transport.releases==releases&&f.state().get("leaseActive").boolean(),"The v1 display manager was parked");++passed;}
  {Fixture f(true);const auto releases=f.transport.releases;const std::string stream="00000000-0000-0000-0000-000000000001";
    require(f.service->reservePreparation(stream,nativeDisplayLeaseKey(f.state())),"Fixture reservation failed");
    require(f.service->parkForConsole(35000)==NativeConsolePark::refused&&f.transport.releases==releases&&f.state().get("leaseActive").boolean()&&f.service->hasStreamProfileReservation()&&!f.service->consoleParked(),"A reserved display was parked");
    require(f.service->releasePreparation(stream)&&f.service->parkForConsole(35000)==NativeConsolePark::parked,"The display could not be parked after the reservation ended");++passed;}
  {Fixture f(true);const auto releases=f.transport.releases;f.service->activationRecovery(uint32_t(f.state().get("generation").number()),true);
    require(f.service->parkForConsole(35000)==NativeConsolePark::refused&&f.transport.releases==releases&&!f.service->consoleParked(),"A display in recovery was parked");
    require(f.service->parkForConsole(0)==NativeConsolePark::refused,"A park without a time was accepted");++passed;}
  // The cold start after a park applies the render preference again, before its
  // output, like the re-arm's; a re-arm that was waiting is served by the park.
  {RearmHarness h;auto& f=*h.f;require(f.service->requestRenderRearm()&&f.service->parkForConsole(35000)==NativeConsolePark::parked,"Fixture re-arm request or park failed");
    const auto releases=f.transport.releases;f.service->tick();f.service->tick();require(f.transport.releases==releases&&h.prefers==1&&f.transport.creates==1,"Something ran on a parked display");
    f.service->endConsolePark();f.service->tick();require(f.state().get("leaseActive").boolean()&&h.prefers==2&&h.createsAtPrefer==std::vector<int>({0,1})&&f.transport.creates==2,"The cold start after a park did not apply the render preference before its output");
    require(f.service->activationReady(uint32_t(f.state().get("generation").number())),"Fixture: verification after the park failed");
    f.service->tick();f.service->tick();require(f.transport.releases==releases&&f.transport.creates==2&&h.prefers==2,"The waiting re-arm stopped the display a second time");++passed;}
  return passed;
}
// ------------------------------------------- state file over the worker pipe
// display_v2_state_rpc.hpp: a worker that is not an administrator has the
// service keep the saved display state. The pipe here is a queue with the
// production dispatcher at the other end and the model's memory as the file.
struct StatePipe {
  v2world::World world;v2world::Memory file;bool reader=true,writer=true;unsigned serial=0;
  std::deque<host::Json> replies;std::vector<std::string> sent;std::optional<std::vector<host::NativeDisplayTarget>> held;
  explicit StatePipe(bool extra=false):world(extra){}
  host::DisplayV2StateService service(){host::DisplayV2StateService s;s.store=file.store();s.reader=[this]{return reader;};s.writer=[this]{return writer;};s.owned=[this]{return held?*held:world.targets();};return s;}
  host::DisplayV2Dispatch ask(const std::string& bytes){return host::dispatchDisplayV2State(host::JsonParser::parse(bytes),service());}
  std::string nonce(){char text[33]{};sprintf_s(text,"%032x",++serial);return text;}
  host::DisplayV2Store store(){
    return host::displayV2PipeStore([this](const std::string& message){sent.push_back(message);const auto reply=ask(message);if(!reply.response.empty())replies.push_back(host::JsonParser::parse(reply.response));return true;},
      [this](DWORD)->std::optional<host::Json>{if(replies.empty())return {};auto value=replies.front();replies.pop_front();return value;},{},[]{return DWORD(5000);},[this]{return nonce();},[this]{return world.clock;});
  }
  // The display access of a worker without administrator rights: the model for Windows and the SDK, the pipe for the file.
  host::DisplayV2Access standard(bool console=true){auto access=v2world::worldAccess(world,file,console);access.store=store();return access;}
  host::DisplayV2Access administrator(bool console=true){return v2world::worldAccess(world,file,console);}
  std::string state(const std::string& label="stream-a"){return host::displayV2StateBytes({label,host::displayV2ActiveCcd(world.query()),world.targets(),host::DisplayV2Console{v2world::consoleAdapter,v2world::consoleAdapter,0,v2world::consoleTarget}});}
};
int stateRpcCases(){
  using namespace host;int passed=0;
  const auto request=[](const std::string& nonce,const char* op,const std::string& rest={}){return "{\"type\":\"worker.v2.state\",\"nonce\":"+jsonQuote(nonce)+",\"op\":"+jsonQuote(op)+rest+'}';};
  const auto expected=[](const std::optional<std::string>& digest){return ",\"expected\":"+(digest?jsonQuote(*digest):std::string("null"));};
  const auto answer=[](const DisplayV2Dispatch& reply,const std::string& nonce){require(reply.handled&&!reply.response.empty(),"A well-formed state request got no answer");const auto parsed=parseDisplayV2StateReply(JsonParser::parse(reply.response),nonce);require(parsed.has_value(),"A state answer did not parse");return *parsed;};
  // Accept table: save, load, replace, remove on the one file, bound to what was last seen.
  {StatePipe p;const auto first=p.state(),digest=nativeBaselineDigest(first);
    auto n=p.nonce();auto reply=answer(p.ask(request(n,"load")),n);require(reply.ok&&!reply.sha256&&!reply.state,"An absent state was not reported as absent");
    n=p.nonce();reply=answer(p.ask(request(n,"save",expected({})+",\"state\":"+jsonQuote(first))),n);
    require(reply.ok&&reply.sha256==digest&&!reply.state&&p.file.bytes==first&&p.file.saves==1,"A valid first save was refused or stored other bytes");
    n=p.nonce();reply=answer(p.ask(request(n,"load")),n);
    require(reply.ok&&reply.sha256==digest&&reply.state&&*reply.state!=first,"The saved state did not come back, or came back with its stream label");
    auto shown=parseDisplayV2State(*reply.state);require(shown.session==displayV2StateForeignLabel,"The stream label of the saved state reached a worker");
    shown.session="stream-a";require(displayV2StateBytes(shown)==first,"The state a worker reads differs from the file in more than its label");
    // Replay: the same save again, and a save or remove against an older file.
    n=p.nonce();reply=answer(p.ask(request(n,"save",expected({})+",\"state\":"+jsonQuote(first))),n);require(!reply.ok&&p.file.saves==1,"A repeated first save was accepted over an existing state");
    const auto second=p.state("stream-b"),secondDigest=nativeBaselineDigest(second);
    n=p.nonce();reply=answer(p.ask(request(n,"save",expected(std::string(64,'0'))+",\"state\":"+jsonQuote(second))),n);require(!reply.ok&&p.file.bytes==first,"A save against another file's digest was accepted");
    n=p.nonce();reply=answer(p.ask(request(n,"save",expected(digest)+",\"state\":"+jsonQuote(second))),n);require(reply.ok&&reply.sha256==secondDigest&&p.file.bytes==second,"A save over exactly the state last seen was refused");
    n=p.nonce();reply=answer(p.ask(request(n,"remove",expected(digest))),n);require(!reply.ok&&p.file.bytes==second&&!p.file.removes,"A remove against the replaced state was accepted");
    n=p.nonce();reply=answer(p.ask(request(n,"discard")),n);require(!reply.ok&&p.file.bytes==second&&!p.file.discards,"A readable state was discarded");
    n=p.nonce();reply=answer(p.ask(request(n,"remove",expected(secondDigest))),n);require(reply.ok&&!p.file.bytes&&p.file.removes==1,"A remove of exactly the state last seen was refused");
    n=p.nonce();reply=answer(p.ask(request(n,"remove",expected({}))),n);require(reply.ok&&p.file.removes==1,"Removing an absent state was not a no-op");
    ++passed;}
  // What a save may carry: one canonical display state naming the owned outputs the service holds, or the saved ones.
  {StatePipe p;const auto valid=p.state();const auto saves=[&]{return p.file.saves;};
    const auto refused=[&](const std::string& state,const char* reason){const auto n=p.nonce();const auto reply=answer(p.ask(request(n,"save",expected({})+",\"state\":"+jsonQuote(state))),n);require(!reply.ok&&!p.file.bytes&&!saves(),reason);};
    refused(valid+" ","A state with trailing bytes was stored");
    refused("{}","An empty object was stored as a state");
    refused("C:\\Windows\\System32\\config\\SAM","Arbitrary text was stored as a state");
    refused("{\"schema\":\"multitap-display-v2\",\"version\":2,\"session\":\"x\",\"owned\":[],\"console\":null,\"paths\":\"\",\"modes\":\"\"}","Another schema version was stored");
    {auto other=parseDisplayV2State(valid);other.owned.front().target+=1;refused(displayV2StateBytes(other),"A state naming an output the service does not hold was stored");}
    {auto other=parseDisplayV2State(valid);other.owned.front().width=1600;other.owned.front().height=900;refused(displayV2StateBytes(other),"A state naming another size than the service holds was stored");}
    // The older original is kept across an SDK size change: its owned outputs are the saved ones.
    auto n=p.nonce();require(answer(p.ask(request(n,"save",expected({})+",\"state\":"+jsonQuote(valid))),n).ok,"Fixture save failed");
    p.held=std::vector<NativeDisplayTarget>{{"0",v2world::ownedAdapter,77,1600,900}};
    auto kept=parseDisplayV2State(valid);kept.console.reset();const auto keptBytes=displayV2StateBytes(kept);
    n=p.nonce();require(answer(p.ask(request(n,"save",expected(nativeBaselineDigest(valid))+",\"state\":"+jsonQuote(keptBytes))),n).ok&&p.file.bytes==keptBytes,"Re-saving the kept original after an SDK change was refused");
    ++passed;}
  // Who may ask: nothing without the running operation's console session; reading only while it is locked.
  {StatePipe p;const auto valid=p.state(),digest=nativeBaselineDigest(valid);p.file.bytes=valid;
    p.reader=false;
    for(const auto& body:{request(p.nonce(),"load"),request(p.nonce(),"save",expected(digest)+",\"state\":"+jsonQuote(p.state("other"))),request(p.nonce(),"remove",expected(digest)),request(p.nonce(),"discard")}){
      const auto value=JsonParser::parse(body);const auto reply=answer(p.ask(body),value.get("nonce").string());require(!reply.ok&&!reply.sha256&&!reply.state,"A worker outside the running operation was served");}
    require(p.file.bytes==valid&&!p.file.saves&&!p.file.removes&&!p.file.discards,"A refused worker changed the state");
    p.reader=true;p.writer=false;
    auto n=p.nonce();auto reply=answer(p.ask(request(n,"load")),n);require(reply.ok&&reply.sha256==digest,"A locked session could not read the state it must keep");
    n=p.nonce();require(!answer(p.ask(request(n,"save",expected(digest)+",\"state\":"+jsonQuote(p.state("other")))),n).ok,"A locked session replaced the state");
    n=p.nonce();require(!answer(p.ask(request(n,"remove",expected(digest))),n).ok&&p.file.bytes==valid,"A locked session removed the state");
    ++passed;}
  // A file that is not a state: never returned, only discardable.
  for(const bool unopenable:{false,true}){StatePipe p;if(unopenable)p.file.throwLoad=true;else p.file.bytes="private text that is not a display state";
    auto n=p.nonce();const auto raw=p.ask(request(n,"load"));auto reply=answer(raw,n);
    require(!reply.ok&&!reply.state&&raw.response.find("private text")==std::string::npos,"Bytes of a file that is not a state reached a worker");
    n=p.nonce();require(!answer(p.ask(request(n,"save",expected({})+",\"state\":"+jsonQuote(p.state()))),n).ok&&!p.file.saves,"A save went over an unreadable file without a discard");
    n=p.nonce();require(!answer(p.ask(request(n,"remove",expected({}))),n).ok&&!p.file.removes,"A remove was accepted on an unreadable file");
    n=p.nonce();require(answer(p.ask(request(n,"discard")),n).ok&&p.file.discards==1&&!p.file.bytes,"An unreadable state could not be discarded");
    ++passed;}
  // Shapes that are not one of the four requests get no answer and change nothing.
  {StatePipe p;const auto valid=p.state();p.file.bytes=valid;const auto digest=nativeBaselineDigest(valid);const auto n=p.nonce();
    const std::string big(displayV2StatePipeLimit+1,'a');
    for(const auto& body:{
        request(n,"load",",\"path\":\"C:\\\\ProgramData\\\\Multitap\\\\Host\\\\private\\\\credentials.dpapi\""),
        request(n,"read"),request(n,"load",expected(digest)),request(n,"discard",expected(digest)),
        request(n,"remove"),request(n,"remove",",\"expected\":\"abc\""),request(n,"remove",",\"expected\":7"),request(n,"remove",expected(digest)+",\"state\":"+jsonQuote(valid)),
        request(n,"save",expected(digest)),request(n,"save",",\"state\":"+jsonQuote(valid)),request(n,"save",expected(digest)+",\"state\":7"),request(n,"save",expected(digest)+",\"state\":\"\""),
        request(n,"save",expected(digest)+",\"state\":"+jsonQuote(big)),request(n,"save",expected(digest)+",\"state\":"+jsonQuote(valid)+",\"file\":\"display-v2.state\""),
        request("short","load"),request(std::string(32,'G'),"load"),std::string("{\"type\":\"worker.v2.state\",\"nonce\":")+jsonQuote(n)+",\"op\":7}"}){
      const auto reply=p.ask(body);require(reply.handled&&reply.response.empty(),("A malformed state request was answered: "+body.substr(0,96)).c_str());}
    require(p.file.bytes==valid&&!p.file.saves&&!p.file.removes&&!p.file.discards,"A malformed request changed the state");
    require(!dispatchDisplayV2State(JsonParser::parse("{\"type\":\"worker.v2.display.size\"}"),p.service()).handled,"Another packet was taken for a state request");
    // The largest state the pipe carries still fits one packet in both directions.
    require(displayV2StateRequestBytes({n,DisplayV2StateOp::save,digest,std::string(displayV2StatePipeLimit,'a')}).size()<65536&&displayV2StateReplyBytes(n,{true,digest,std::string(displayV2StatePipeLimit,'a')}).size()<65536,"A state within the limit does not fit a pipe packet");
    ++passed;}
  // A stream by a worker without administrator rights, start to stop: the same
  // Windows result as an administrator's worker, with the file kept by the service.
  {StatePipe standard(true),administrator(true);
    DisplaySessionV2 a(standard.standard()),b(administrator.administrator());
    const auto startedA=a.start("stream-standard",1600,900,true,v2world::deadline(standard.world)),startedB=b.start("stream-standard",1600,900,true,v2world::deadline(administrator.world));
    require(startedA.ok&&startedB.ok&&startedA.consoleRemoved==startedB.consoleRemoved,"A standard user's stream start differs from an administrator's");
    v2world::requireStreaming(standard.world,1600,900,true,"A standard user's stream did not get the stream topology");
    require(standard.file.bytes&&standard.file.bytes==administrator.file.bytes,"The state the service saved for a standard user differs from the administrator's file");
    require(standard.world.applies==administrator.world.applies&&standard.world.sizeCalls==administrator.world.sizeCalls&&standard.world.resizes==administrator.world.resizes,"A standard user's start made other display or SDK calls");
    require(!standard.sent.empty(),"Fixture: the standard worker never asked the service");
    for(const auto& message:standard.sent){
      const auto value=JsonParser::parse(message);
      for(const auto& [name,field]:std::get<Json::Object>(value.value)){(void)field;require(name=="type"||name=="nonce"||name=="op"||name=="expected"||name=="state","A state request carried a field outside its four shapes");}
      require(message.find("ProgramData")==std::string::npos&&message.find("display-v2.state")==std::string::npos&&message.find("private")==std::string::npos,"A state request named a path");
    }
    require(a.resize(1280,720,v2world::deadline(standard.world)).ok&&b.resize(1280,720,v2world::deadline(administrator.world)).ok&&standard.file.bytes==administrator.file.bytes,"A resize changed the saved original");
    const auto restoredA=a.restore(),restoredB=b.restore();
    require(restoredA.outcome==DisplayV2Outcome::restored&&restoredB.outcome==DisplayV2Outcome::restored&&!standard.file.bytes&&standard.file.removes==1,"A standard user's stream stop did not restore and remove the state");
    v2world::requireOriginal(standard.world,true,"A standard user's stream stop did not put Windows back");
    require(standard.world.applies==administrator.world.applies&&standard.world.sizeCalls==administrator.world.sizeCalls,"A standard user's stop made other display or SDK calls");
    ++passed;}
  // The state a stream left when its session ended (the worker died at sign-out):
  // the next signed-in user's worker restores it, administrator or not.
  for(const bool leftByAdministrator:{true,false}){StatePipe p(true);
    {DisplaySessionV2 stream(leftByAdministrator?p.administrator():p.standard());require(stream.start("stream-gone",1600,900,true,v2world::deadline(p.world)).ok&&p.file.bytes,"Fixture stream start failed");}
    const auto left=*p.file.bytes;p.replies.clear();
    DisplaySessionV2 recovery(p.standard(false));const auto restored=recovery.restore();
    require(restored.outcome==DisplayV2Outcome::restored&&restored.consoleExpected&&restored.consoleActive&&!p.file.bytes,"A standard user's sign-in did not restore the state a stream left");
    v2world::requireOriginal(p.world,true,"The recovery by a standard user's worker did not put Windows back");
    require(parseDisplayV2State(left).session=="stream-gone","Fixture: the left state lost its label in the file");
    ++passed;}
  // Locked, or not the running operation: nothing changes and the state stays for the next attempt.
  {StatePipe p(true);{DisplaySessionV2 stream(p.standard());require(stream.start("stream-gone",1600,900,true,v2world::deadline(p.world)).ok,"Fixture stream start failed");}
    const auto left=*p.file.bytes;const auto applies=p.world.applies;
    p.world.eligible=false;p.writer=false;
    {DisplaySessionV2 recovery(p.standard(false));const auto result=recovery.restore();require(result.outcome==DisplayV2Outcome::failed&&result.stage==displayV2Stage::ineligible&&p.file.bytes==left&&p.world.applies==applies,"A locked standard session changed the display or lost the state");}
    p.world.eligible=true;p.writer=true;p.reader=false;
    {DisplaySessionV2 recovery(p.standard(false));const auto result=recovery.restore();require(p.file.bytes==left&&!p.file.discards&&!p.file.removes&&p.world.applies==applies&&result.outcome!=DisplayV2Outcome::restored,"A refused worker discarded or restored the state");}
    {DisplaySessionV2 stream(p.standard());require(!stream.start("stream-new",1280,720,true,v2world::deadline(p.world)).ok&&p.file.bytes==left&&p.world.applies==applies,"A stream started although the service refused its state file");}
    p.reader=true;
    {DisplaySessionV2 recovery(p.standard(false));require(recovery.restore().outcome==DisplayV2Outcome::restored&&!p.file.bytes,"The state could not be restored once the session was unlocked again");}
    v2world::requireOriginal(p.world,true,"The later recovery did not put Windows back");++passed;}
  // An unreadable file never blocks a standard user's stream: the service discards it on request.
  {StatePipe p(true);p.file.bytes="not a state";
    DisplaySessionV2 stream(p.standard());require(stream.start("stream-after-bad",1600,900,true,v2world::deadline(p.world)).ok&&p.file.discards==1&&p.file.bytes&&parseDisplayV2State(*p.file.bytes).session=="stream-after-bad","A standard user's stream could not start over an unreadable state");
    require(stream.restore().outcome==DisplayV2Outcome::restored&&!p.file.bytes,"The stream over a discarded state did not restore");++passed;}
  // Output count 1 -> 2 -> 1 by a worker without administrator rights (the model is also the SDK).
  {StatePipe p;DisplaySessionV2 session(p.standard(false));
    const auto up=session.count("count-standard",2,false,v2world::deadline(p.world));
    require(up.ok&&up.owned.size()==2&&p.world.hasSecond()&&!p.file.bytes&&p.file.saves==1&&p.file.removes==1,"A standard user's count change failed or left its own snapshot behind");
    DisplaySessionV2 back(p.standard(false));const auto down=back.count("count-standard-back",1,false,v2world::deadline(p.world));
    require(down.ok&&down.owned.size()==1&&!p.world.hasSecond()&&!p.file.bytes,"A standard user's count change back failed");++passed;}
  // A reply meant for an earlier state request never answers a later RPC.
  {const std::string old(32,'1'),fresh(32,'2');const auto stale=JsonParser::parse(displayV2StateReplyBytes(old,{true,{},{}}));
    require(displayV2StaleReply(stale,fresh)&&!displayV2StaleReply(stale,old)&&!parseDisplayV2StateReply(stale,fresh)&&parseDisplayV2StateReply(stale,old),"A stale state reply was taken for the current answer");
    require(!parseDisplayV2StateReply(JsonParser::parse("{\"type\":\"worker.v2.state.result\",\"nonce\":"+jsonQuote(old)+",\"ok\":false,\"sha256\":"+jsonQuote(std::string(64,'a'))+",\"state\":null}"),old),"A refusal carrying data was accepted");
    ++passed;}
  // Who has display authority in the worker: an administrator always; anyone else only on the native provider under display manager v2.
  {require(workerDisplayAuthority(true,false,false)&&workerDisplayAuthority(true,true,false)&&workerDisplayAuthority(true,false,true)&&workerDisplayAuthority(true,true,true),"An administrator's worker lost its display authority");
    require(workerDisplayAuthority(false,true,true)&&!workerDisplayAuthority(false,true,false)&&!workerDisplayAuthority(false,false,true)&&!workerDisplayAuthority(false,false,false),"A standard user's worker has authority outside the native provider under display manager v2, or lacks it there");
    ++passed;}
  return passed;
}
int main(){
  try{int passed=0;passed+=stateRpcCases();passed+=sdkCases();passed+=startupCases();passed+=selfHealCases();passed+=rpcCases();passed+=countSdkCases();passed+=countSessionCases();passed+=sizesSdkCases();passed+=groupSessionCases();passed+=privatePreparationCases();passed+=privatePreparationCommitCases();passed+=rearmCases();passed+=consoleParkCases();std::cout<<"Display manager v2 service checks passed: "<<passed<<"\n";return 0;}
  catch(const std::exception& error){std::cerr<<"Display manager v2 service check failed: "<<error.what()<<"\n";return 1;}
}
#else
int main(){std::cerr<<"Display manager v2 service checks require the native display SDK build\n";return 1;}
#endif
