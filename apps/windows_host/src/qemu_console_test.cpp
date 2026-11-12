#include "qemu_console.hpp"
#include "qemu_console_protocol.hpp"
#include "qemu_console_route.hpp"
#include "native_display_activation.hpp"
#include "../tests/display-v2-world.hpp"
#include <iostream>
namespace {
using namespace host;using namespace v2world;
unsigned checks=0;
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);++checks;}
const std::string uuid="01234567-89ab-cdef-8123-456789abcdef";
NativeConsoleAdapterIdentity fixtureDevice(LUID adapter,bool qemu){
  auto result=NativeConsoleAdapterIdentity{adapter,L"\\\\?\\PCI#VEN_1234&DEV_1111#4&ABCD&0&0010#{GUID}",L"PCI\\VEN_1234&DEV_1111&SUBSYS_11001AF4&REV_02\\4&ABCD&0&0010",L"PCI",
    {L"PCI\\VEN_1234&DEV_1111&SUBSYS_11001AF4&REV_02",L"PCI\\VEN_1234&DEV_1111&CC_030000"},nativeConsoleDisplayClass,true,true};
  if(!qemu){result.instanceId=L"PCI\\VEN_10DE&DEV_0001\\1";result.hardwareIds={L"PCI\\VEN_10DE&DEV_0001"};}return result;
}
struct Fixture {
  World world{true};Memory memory;bool lease=true,remote=false;std::string current=uuid;int loads=0;
  std::function<void()> onLease;std::function<void()> onResolve;
  Fixture(){save();}
  void save(){memory.bytes=displayV2StateBytes({"saved",displayV2ActiveCcd(world.query()),world.targets(),DisplayV2Console{consoleAdapter,consoleAdapter,0,consoleTarget}});}
  void suppress(){world.console().active=false;world.owned().x=0;world.extra().x=1920;}
  QemuConsoleAccess access(){
    auto display=worldAccess(world,memory);auto load=display.store.load;display.store.load=[this,load]{++loads;return load();};
    return {std::move(display),{uuid,"std"},[this]{return current;},[this]{return remote;},[this](LUID adapter)->std::optional<NativeConsoleAdapterIdentity>{
      if(onResolve)onResolve();return fixtureDevice(adapter,same(adapter,world.consoleLuid));
    },[this]{if(onLease)onLease();return lease;},world.clock+15000};
  }
  QemuConsoleResult run(){return prepareQemuConsoleOutput(access());}
  void unchanged(const std::optional<std::string>& before){require(world.applies==0&&world.resizes==0&&world.counts==0&&world.sizeCalls==0,"Rejected preparation changed displays/SDK");require(memory.bytes==before&&memory.discards==0&&memory.removes==0,"Rejected preparation changed saved evidence");}
};
template<class Change>void rejected(Change change,const char* reason){Fixture f;f.suppress();change(f);const auto before=f.memory.bytes;const auto r=f.run();require(!r.ok&&!r.attempted,"Unsafe preparation accepted/attempted");require(r.reason==reason,"Wrong fixed preparation failure");f.unchanged(before);}
// Only the QEMU output is active: no owned output, no foreign display.
bool consoleOnly(World& w){return w.console().active&&!w.console().x&&!w.console().y&&std::count_if(w.displays.begin(),w.displays.end(),[](const Display& d){return d.active;})==1;}
// The console's end: the service's ordinary v2 recovery (worker.v2.restore,
// DisplaySessionV2::restore with no stream console) over the same state store.
DisplayV2RestoreResult consoleEnd(Fixture& f){DisplaySessionV2 recovery(worldAccess(f.world,f.memory,false));return recovery.restore();}
// The extra display (a vGPU head such as an NVIDIA GRID P40-1Q) with its fixture modes, active at `x`.
bool foreignBack(World& w,LONG x){const auto& e=w.extra();return e.active&&e.x==x&&!e.y&&e.width==800&&e.height==600&&e.refreshN==75&&e.refreshD==1&&e.format==DISPLAYCONFIG_PIXELFORMAT_32BPP;}
void cases(){
  {Fixture f;f.world.owned().active=false;f.world.extra().active=false;f.memory.throwLoad=true;const auto before=f.memory.bytes;const auto r=f.run();require(r.ok&&!r.attempted&&r.reason=="qemu_console_active","Already isolated primary QEMU not accepted as a no-op");require(f.loads==0,"Verified isolated QEMU read recovery file");f.unchanged(before);}
  // QEMU alone at the origin but a foreign display still on beside it is not the console: it is switched off.
  {Fixture f;f.world.owned().active=false;const auto r=f.run();require(r.ok&&r.attempted&&r.reason=="qemu_console_restored"&&consoleOnly(f.world),"A foreign display beside an otherwise isolated QEMU was accepted as the console");}
  {Fixture f;f.suppress();const auto old=f.memory.bytes;const auto r=f.run();require(r.ok&&r.attempted&&r.reason=="qemu_console_restored","Inactive saved QEMU not restored");require(consoleOnly(f.world),"Successful result lacked primary QEMU or kept an owned/foreign output");require(f.memory.bytes==old&&f.memory.removes==0,"The console retired the pending restore point its end restores");require(f.world.resizes==0&&f.world.counts==0&&f.world.sizeCalls==0,"Console preparation changed the SDK lease");
    // That restore point is the stream's original: the console's end puts it back, foreign display included.
    const auto end=consoleEnd(f);require(end.outcome==DisplayV2Outcome::restored&&!f.memory.bytes,"The console's end did not restore and retire its restore point");requireOriginal(f.world,true,"The console's end did not bring back the original topology");}
  {Fixture f;f.world.console().x=1920;f.world.owned().x=0;f.memory.bytes.reset();const auto r=f.run();require(r.ok&&r.attempted&&consoleOnly(f.world),"An active secondary QEMU remained a side display");require(f.memory.saves==1&&f.memory.bytes&&f.memory.removes==0,"Active QEMU normalization lacked a protected restore point");
    const auto end=consoleEnd(f);require(end.ok()&&!f.memory.bytes&&f.world.console().active&&f.world.console().x==1920&&f.world.owned().active&&!f.world.owned().x&&foreignBack(f.world,2944),"The console's end did not restore the secondary-QEMU topology");}
  {Fixture f;const auto r=f.run();require(r.ok&&r.attempted&&consoleOnly(f.world),"Active primary QEMU with an owned output was treated as a no-op");}
  {Fixture f;f.world.setCount(2);f.save();const auto r=f.run();require(r.ok&&consoleOnly(f.world),"Dual owned outputs stayed active in console mode");}
  {Fixture f;f.world.owned().active=false;f.world.extra().x=0;const auto r=f.run();require(r.ok&&r.attempted&&consoleOnly(f.world),"A foreign clone/overlap at the primary origin was treated as an isolated no-op");}
  // Field case: the VM's vGPU output is on, the console is off, no restore point exists. Only the QEMU
  // output is active afterwards; the restore point holds the vGPU output, and the console's end brings it back.
  {Fixture f;f.suppress();f.memory.bytes.reset();const auto r=f.run();
    require(r.ok&&r.attempted&&r.reason=="qemu_console_restored"&&consoleOnly(f.world)&&!f.world.extra().active,"The NVIDIA-like display stayed active with the console");
    require(f.memory.saves==1&&f.memory.removes==0&&f.memory.bytes,"The console's restore point was not kept");
    const auto point=parseDisplayV2State(*f.memory.bytes);
    require(point.session=="qemu-console"&&point.console&&same(point.console->targetAdapter,consoleAdapter)&&point.console->target==consoleTarget&&!displayV2ConsoleActive(point.ccd,*point.console)&&
      displayV2TargetActive(point.ccd,extraAdapter,extraTarget)&&displayV2TargetActive(point.ccd,ownedAdapter,f.world.ownedTarget),"The restore point lost the foreign display, the owned output or the console's name");
    const auto end=consoleEnd(f);
    require(end.outcome==DisplayV2Outcome::restored&&!f.memory.bytes,"The console's end did not restore its restore point");
    require(foreignBack(f.world,1920)&&f.world.owned().active&&!f.world.owned().x&&!f.world.console().active&&!overlapping(f.world),"The console's end did not bring the foreign display and the owned output back as they were");}
  // The default world (console, owned output, foreign display, all on) without a restore point: the same round trip.
  {Fixture f;f.memory.bytes.reset();const auto r=f.run();require(r.ok&&consoleOnly(f.world)&&f.memory.bytes,"Console-only from a three-display topology failed");
    const auto end=consoleEnd(f);require(end.outcome==DisplayV2Outcome::restored&&!f.memory.bytes,"The console's end left its restore point");requireOriginal(f.world,true,"The console's end did not bring back console, owned output and foreign display");}
  // The console is opened again before its end was restored and is still alone: nothing changes, the restore point stays.
  {Fixture f;f.suppress();f.memory.bytes.reset();require(f.run().ok,"First console failed");const auto point=f.memory.bytes;const auto applies=f.world.applies;
    const auto r=f.run();require(r.ok&&!r.attempted&&r.reason=="qemu_console_active"&&f.memory.bytes==point&&f.world.applies==applies,"A second console open changed the pending restore point or the displays");
    require(consoleEnd(f).outcome==DisplayV2Outcome::restored&&foreignBack(f.world,1920),"The pending restore point did not survive a second console open");}
  // A pending restore point whose foreign display has another mode or is off now: the console proceeds and its
  // end puts the saved one back, as the ordinary recovery would have.
  for(const bool off:{false,true}){Fixture f;f.suppress();if(off)f.world.extra().active=false;else{f.world.extra().width+=100;f.world.extra().dbWidth+=100;}const auto old=f.memory.bytes;
    const auto r=f.run();require(r.ok&&consoleOnly(f.world)&&f.memory.bytes==old,"A pending restore point with other foreign modes blocked the console or was replaced");
    require(consoleEnd(f).outcome==DisplayV2Outcome::restored&&f.world.extra().active&&f.world.extra().width==800,"The console's end did not restore the saved foreign display");}
  // A pending restore point that had the console off (a stream's original on a VM whose console was off):
  // the console is activated with Windows' modes, and its end switches it off again.
  {Fixture f;f.suppress();f.save();const auto r=f.run();require(r.ok&&consoleOnly(f.world)&&f.world.topologyApplies==1,"A restore point with the console off blocked the console");
    require(consoleEnd(f).outcome==DisplayV2Outcome::restored&&!f.world.console().active&&f.world.owned().active&&foreignBack(f.world,1920),"The console's end did not return to the console-off original");}
  {Fixture f;f.memory.bytes.reset();f.world.reject=[](const auto&,UINT32){return LONG(ERROR_GEN_FAILURE);};const auto r=f.run();require(!r.ok&&!r.attempted&&f.memory.saves==1&&f.memory.removes==1&&!f.memory.bytes,"Validation-only failure did not safely retire its newly created rollback snapshot");}
  {Fixture f;const auto old=f.memory.bytes;f.world.reject=[](const auto&,UINT32){return LONG(ERROR_GEN_FAILURE);};const auto r=f.run();require(!r.ok&&!r.attempted&&f.memory.bytes==old&&f.memory.removes==0,"Validation-only failure deleted a pre-existing recovery record");}
  {Fixture f;f.suppress();const auto original=f.world.query();const auto old=f.memory.bytes;auto access=f.access();const auto configure=access.display.configure;int applies=0;access.display.configure=[&](const auto& plan,UINT32 flags){const auto result=configure(plan,flags);return (flags&SDC_APPLY)&&++applies==1?LONG(ERROR_GEN_FAILURE):result;};const auto r=prepareQemuConsoleOutput(std::move(access));require(!r.ok&&r.attempted&&qemuConsoleRollbackMatches(original,f.world.query()),"A partially applied failure was not restored to its original topology");require(f.memory.bytes==old,"Partial failure discarded protected recovery evidence");
    // The apply had switched the foreign display off; the rollback brings it back with its modes and place.
    require(foreignBack(f.world,1920)&&f.world.owned().active&&!f.world.owned().x&&!f.world.console().active,"The rollback did not bring the foreign display back exactly");}
  {Fixture f;f.suppress();f.world.afterApply=[](auto& w){w.owned().active=true;};const auto r=f.run();require(!r.ok&&r.attempted&&f.memory.bytes.has_value(),"SDK/Windows reactivation of an owned output passed console verification");}
  // Windows accepted the apply but a foreign display came back on (here over the QEMU origin): not the console.
  // The reason says so, and the rollback restores the topology from before, foreign display included.
  for(const bool created:{false,true}){Fixture f;f.suppress();if(created)f.memory.bytes.reset();const auto original=f.world.query();const auto old=f.memory.bytes;bool once=false;
    f.world.afterApply=[&](auto& w){if(once)return;once=true;w.extra().active=true;w.extra().x=0;};const auto r=f.run();
    require(!r.ok&&r.attempted&&r.reason=="qemu_console_foreign_changed","Windows put a foreign output back on but preparation claimed the console alone");
    require(qemuConsoleRollbackMatches(original,f.world.query())&&foreignBack(f.world,1920),"A foreign display that came back on was not rolled back exactly");
    require(created?(!f.memory.bytes&&f.memory.removes==1):(f.memory.bytes==old&&f.memory.removes==0),"A rolled-back console kept its own restore point or lost the pending one");}
  // A foreign display changes while the console is being prepared (after the first read, before any apply):
  // nothing is applied, nothing is saved or removed.
  for(unsigned change=0;change<3;++change){Fixture f;f.suppress();f.memory.bytes.reset();auto access=f.access();const auto query=access.display.query;int reads=0;
    access.display.query=[&]{if(++reads==2){auto& e=f.world.extra();if(change==0)e.refreshN=120;else if(change==1)e.width+=100;else e.active=false;}return query();};
    const auto r=prepareQemuConsoleOutput(std::move(access));require(!r.ok&&!r.attempted&&r.reason=="qemu_console_foreign_changed","A foreign display that changed during preparation did not abort it");f.unchanged(std::nullopt);}
  // ... or between validation and apply.
  {Fixture f;f.suppress();auto access=f.access();const auto configure=access.display.configure;access.display.configure=[&](const NativeCcdData& plan,UINT32 flags){const auto code=configure(plan,flags);if(flags&SDC_VALIDATE)f.world.extra().refreshN=120;return code;};
    const auto before=f.memory.bytes;const auto r=prepareQemuConsoleOutput(std::move(access));require(!r.ok&&!r.attempted&&r.reason=="qemu_console_foreign_changed","A foreign change after validation still applied");f.unchanged(before);}
  {Fixture f;f.memory.bytes.reset();f.world.afterApply=[&](auto&){f.lease=false;};const auto r=f.run();require(!r.ok&&r.attempted&&r.reason=="qemu_console_cancelled"&&f.memory.bytes.has_value(),"Lost lease after apply was reported successful or its rollback proof was removed");}
  {Fixture baseline;baseline.suppress();auto access=baseline.access();const auto query=access.display.query;unsigned finalQuery=0;access.display.query=[&]{++finalQuery;return query();};require(prepareQemuConsoleOutput(std::move(access)).ok&&finalQuery>2,"Final-snapshot regression baseline failed");
    for(const bool moveConsole:{false,true}){Fixture f;f.suppress();const auto old=f.memory.bytes;auto fault=f.access();const auto fresh=fault.display.query;unsigned calls=0;bool injected=false;
      fault.display.query=[&]{if(++calls==finalQuery){injected=true;if(moveConsole)f.world.console().x=100;else f.world.owned().active=true;}return fresh();};
      const auto result=prepareQemuConsoleOutput(std::move(fault));require(injected&&!result.ok&&result.attempted&&f.memory.bytes==old,"The final guard observed a secondary QEMU/reactivated owned output but still committed success");}}
  // A stream that starts after the console, before its end was restored (the restore point is still there),
  // keeps that restore point as its original: its stop brings back the topology from before the console.
  for(const bool dual:{false,true}){Fixture f;if(dual){f.world.setCount(2);f.save();}const auto console=f.run();require(console.ok&&consoleOnly(f.world)&&f.memory.bytes,"Console setup retained an active owned/foreign output before stream round-trip");
    DisplaySessionV2 stream(worldAccess(f.world,f.memory));DisplayV2Result started;
    if(dual){const std::array<DisplayV2Output,2> outputs{{{"0",1600,900,0,0},{"1",1280,720,1600,0}}};started=stream.startGroup("after-console",outputs,true,f.world.clock+20000);}
    else started=stream.start("after-console",1600,900,true,f.world.clock+20000);
    require(started.ok&&f.world.owned().active&&!f.world.owned().x&&!f.world.console().active&&(!dual||f.world.second().active),"Explicit single/dual stream could not re-prepare owned outputs after console-only mode");
    const auto restored=stream.restore();require(restored.outcome==DisplayV2Outcome::restored&&!f.memory.bytes&&(!dual||!f.world.second().active),"Ordinary v2 stream stop did not restore the pending pre-console original");
    requireOriginal(f.world,true,"A stream after the console did not return to the topology from before the console");}
  // Field case: the identified console was inactive and no saved state
  // named it. A restore point of the current topology is stored, the console is
  // activated with modes Windows chooses and verified alone at the origin.
  {Fixture f;f.suppress();f.memory.bytes.reset();const auto r=f.run();
    require(r.ok&&r.attempted&&r.reason=="qemu_console_restored","An inactive console without a saved state was not activated");
    require(consoleOnly(f.world)&&f.world.console().width==1024&&f.world.topologyApplies==1,"Activation lacked a primary QEMU with Windows' modes or kept an owned/foreign output");
    require(f.memory.saves==1&&f.memory.removes==0&&f.memory.bytes,"Activation lacked a restore point for the console's end");
    require(f.world.resizes==0&&f.world.counts==0&&f.world.sizeCalls==0,"Activation changed the SDK lease");}
  // Windows put the activated console off the origin: the supplied plan then
  // places it alone at the origin with the modes Windows chose.
  {Fixture f;f.suppress();f.memory.bytes.reset();bool moved=false;
    f.world.afterApply=[&](auto& w){if(moved)return;moved=true;w.console().x=800;};
    const auto r=f.run();
    require(r.ok&&r.reason=="qemu_console_restored"&&f.world.topologyApplies==1&&f.world.applies==2&&consoleOnly(f.world),"An activated console off the origin was not placed there alone");}
  // Windows did not keep the activated console: the restore point is put back,
  // then retired, and the reason says the console stayed inactive.
  {Fixture f;f.suppress();f.memory.bytes.reset();const auto original=f.world.query();bool dropped=false;
    f.world.afterApply=[&](auto& w){if(dropped)return;dropped=true;w.console().active=false;};
    const auto r=f.run();
    require(!r.ok&&r.attempted&&r.reason=="qemu_console_inactive","An activation Windows did not keep passed or lost its reason");
    require(f.world.owned().active&&!f.world.owned().x&&!f.world.console().active&&foreignBack(f.world,1920)&&qemuConsoleRollbackMatches(original,f.world.query()),"A failed activation did not restore the saved topology");
    require(f.memory.saves==1&&f.memory.removes==1&&!f.memory.bytes,"A restored activation kept its restore point");}
  // Windows refuses the activation: nothing changed, nothing left behind.
  {Fixture f;f.suppress();f.memory.bytes.reset();f.world.reject=[](const auto&,UINT32 flags){return (flags&SDC_TOPOLOGY_SUPPLIED)?LONG(ERROR_GEN_FAILURE):LONG(ERROR_SUCCESS);};
    const auto r=f.run();require(!r.ok&&!r.attempted&&r.reason=="qemu_console_unavailable"&&f.world.applies==0&&!f.world.console().active&&f.world.owned().active&&f.memory.saves==1&&f.memory.removes==1&&!f.memory.bytes,"A refused activation changed displays or left its restore point");}
  // A saved state from an earlier boot (every adapter it names is gone: LUIDs
  // last one boot) can never be restored; a restore point of this boot replaces it.
  for(const bool inactive:{true,false}){Fixture f;if(inactive)f.suppress();
    require(std::erase_if(f.world.displays,[](const Display& d){return same(d.adapter,extraAdapter);})==1,"Fixture lost the extra display");f.world.restartConsoleAdapter({17,0});
    const auto r=f.run();
    require(r.ok&&r.attempted&&r.reason=="qemu_console_restored"&&consoleOnly(f.world),"A state from an earlier boot blocked the console");
    require(f.memory.saves==1&&f.memory.removes==0&&f.memory.bytes&&f.world.topologyApplies==(inactive?1:0),"The earlier-boot state was not replaced by a restore point of this boot");
    require(!displayV2StateFromEarlierBoot(parseDisplayV2State(*f.memory.bytes),f.world.query(),f.world.targets())&&consoleEnd(f).outcome==DisplayV2Outcome::restored&&f.world.owned().active,"The restore point of this boot was not restored at the console's end");}
  // No restore point can be stored: the only remaining state_missing.
  rejected([](auto& f){f.memory.bytes.reset();f.memory.failSave=true;},"qemu_console_state_missing");
  rejected([](auto& f){f.memory.bytes="invalid";},"qemu_console_state_invalid");
  rejected([](auto& f){f.memory.throwLoad=true;},"qemu_console_state_invalid");
  rejected([](auto& f){f.current="fedcba98-7654-4321-8123-456789abcdef";},"qemu_console_identity_changed");
  rejected([](auto& f){f.remote=true;},"qemu_console_identity_changed");
  rejected([](auto& f){f.lease=false;},"qemu_console_cancelled");
  rejected([](auto& f){f.world.eligible=false;},"qemu_console_cancelled");
  rejected([](auto& f){f.world.restartConsoleAdapter({17,0});},"qemu_console_state_invalid");
  rejected([](auto& f){auto state=parseDisplayV2State(*f.memory.bytes);state.console->target++;f.memory.bytes=displayV2StateBytes(state);},"qemu_console_state_invalid");
  // A pending restore point that does not name the console cannot be restored at the console's end.
  rejected([](auto& f){auto state=parseDisplayV2State(*f.memory.bytes);state.console.reset();f.memory.bytes=displayV2StateBytes(state);},"qemu_console_state_invalid");
  {Fixture f;f.suppress();auto access=f.access();access.deadline=f.world.clock;const auto before=f.memory.bytes;const auto r=prepareQemuConsoleOutput(std::move(access));require(!r.ok&&r.reason=="qemu_console_cancelled","Expired operation allowed");f.unchanged(before);}
  {Fixture f;f.suppress();auto access=f.access();int reads=0;const auto query=access.display.query;access.display.query=[&]{if(++reads==3)f.lease=false;return query();};const auto before=f.memory.bytes;const auto r=prepareQemuConsoleOutput(std::move(access));require(!r.ok&&!r.attempted&&r.reason=="qemu_console_cancelled","Lease changed before apply was accepted");f.unchanged(before);}
  {Fixture f;f.suppress();auto access=f.access();const auto configure=access.display.configure;access.display.configure=[&](const NativeCcdData& plan,UINT32 flags){const auto code=configure(plan,flags);if(flags&SDC_VALIDATE)f.lease=false;return code;};const auto before=f.memory.bytes;const auto r=prepareQemuConsoleOutput(std::move(access));require(!r.ok&&!r.attempted,"Cancellation after validation still applied");f.unchanged(before);}
  {Fixture f;f.suppress();f.world.afterApply=[](auto& w){w.console().active=false;};const auto r=f.run();require(!r.ok&&r.attempted&&f.memory.bytes.has_value(),"A successful apply without an active output passed or erased evidence");}
  {Fixture f;f.suppress();f.world.reject=[](const auto&,UINT32){return LONG(ERROR_GEN_FAILURE);};const auto r=f.run();require(!r.ok&&!r.attempted&&f.memory.bytes.has_value(),"Failed validation passed or erased evidence");}
  {Fixture f;f.suppress();f.world.afterApply=[&](auto&){f.memory.bytes="external-change";};const auto r=f.run();require(!r.ok&&f.memory.bytes==std::optional<std::string>("external-change"),"Concurrent saved-state change was erased");}
  {Fixture f;f.suppress();f.world.afterApply=[](auto& w){w.extra().active=true;w.extra().x=1024;w.extra().refreshN=120;};const auto r=f.run();require(!r.ok&&r.reason=="qemu_console_foreign_changed"&&f.memory.bytes.has_value(),"A foreign display back on with another mode was hidden by restore success");}
  {Fixture f;f.suppress();f.world.afterApply=[](auto& w){w.clock+=16000;};const auto r=f.run();require(!r.ok&&r.attempted&&r.reason=="qemu_console_cancelled"&&f.memory.bytes.has_value(),"Late completion was reported as on-time success or erased recovery evidence");}
  {Fixture f;f.suppress();auto access=f.access();const auto resolver=access.resolver;int calls=0;access.resolver=[&](LUID adapter)->std::optional<NativeConsoleAdapterIdentity>{auto device=resolver(adapter);if(++calls>=5&&device&&same(adapter,consoleAdapter))device->instanceId=L"PCI\\VEN_1234&DEV_1111\\REPLACED";return device;};const auto before=f.memory.bytes;const auto r=prepareQemuConsoleOutput(std::move(access));require(!r.ok&&!r.attempted&&r.reason=="qemu_console_identity_changed","Replacement device at the same target was accepted");f.unchanged(before);}
}
void protocolCases(){
  const DisplayOperationKey key{"89abcdef-0123-4567-8123-456789abcdef",std::string(43,'a')};
  const auto packet=[&](const std::string& model,const std::string& machine,bool worker=false){return JsonParser::parse("{\"type\":"+jsonQuote(worker?"worker.qemu-console.prepare":"host.qemu-console.prepare")+",\"protocol\":\"qemu-console-v2\","+displayOperationFields(key)+",\"machineUuid\":"+jsonQuote(machine)+",\"model\":"+jsonQuote(model)+(worker?",\"leaseKey\":\"held\",\"workerDeadlineMs\":5000":"")+'}');};
  const auto rejectedProof=[](Json request,bool worker=false){bool rejected=false;try{(void)qemuConsoleProof(request,worker);}catch(...){rejected=true;}require(rejected,"Untrusted QEMU proof accepted");};
  require(qemuConsoleProof(packet("std",uuid)).machineUuid==uuid,"Valid Host model proof rejected");
  require(qemuConsoleProof(packet("std","01234567-89AB-CDEF-8123-456789ABCDEF")).machineUuid==uuid,"Machine UUID was not canonicalized");
  require(qemuConsoleProof(packet("std",uuid,true),true).model=="std","Valid worker proof rejected");
  rejectedProof(packet("std",uuid,true));rejectedProof(packet("std",uuid),true);
  rejectedProof(packet("qxl",uuid));rejectedProof(packet("std",""));
  rejectedProof(packet("std","00000000-0000-0000-0000-000000000000"));
  rejectedProof(packet("std","ffffffff-ffff-ffff-ffff-ffffffffffff"));
  {auto request=packet("std",uuid);std::get<Json::Object>(request.value).emplace("extra",Json{true});rejectedProof(std::move(request));}
  {auto request=packet("std",uuid);std::get<Json::Object>(request.value)["nonce"]=Json{std::string(43,'!')};rejectedProof(std::move(request));}
  {auto request=packet("std",uuid);std::get<Json::Object>(request.value)["operationId"]=Json{std::string("not-an-operation")};rejectedProof(std::move(request));}
  {auto request=packet("std",uuid);std::get<Json::Object>(request.value)["protocol"]=Json{std::string("other")};rejectedProof(std::move(request));}
  {auto request=packet("std",uuid);std::get<Json::Object>(request.value)["type"]=Json{std::string("worker.qemu-console.prepare")};rejectedProof(std::move(request));}
  {const auto reply=JsonParser::parse(qemuConsoleResultPacket(key,false,true,false,"arbitrary sensitive diagnostic"));
    require(key.matches(reply)&&reply.get("type").string()=="host.qemu-console.result"&&reply.get("protocol").string()==qemuConsoleProtocol,"Result correlation/protocol lost");
    require(!reply.get("ok").boolean()&&reply.get("attempted").boolean()&&!reply.get("workerQuiescent").boolean(),"Quiet-false result was relabeled complete");
    require(reply.get("reason").string()=="qemu_console_unavailable","Arbitrary diagnostic leaked into protocol");}
  {const auto reply=JsonParser::parse(qemuConsoleResultPacket(key,true,false,true,"qemu_console_active"));require(reply.get("ok").boolean()&&!reply.get("attempted").boolean()&&reply.get("workerQuiescent").boolean()&&reply.get("reason").string()=="qemu_console_active","No-op active result was relabeled a mutation");}
  {const auto renew=JsonParser::parse("{\"type\":\"host.qemu-console.renew\",\"protocol\":\"qemu-console-v2\","+displayOperationFields(key)+",\"sequence\":1}");require(qemuConsoleRenewSequence(renew)==1,"Exact renewal sequence was rejected");const auto ack=JsonParser::parse(qemuConsoleHeldPacket(key,1));require(key.matches(ack)&&ack.get("sequence").number()==1&&ack.get("leaseMs").number()==30000,"Renewal acknowledgement lost its fixed lease/key binding");}
  {QemuConsoleHold hold;require(!hold.held(),"A new operation started with a display hold");hold.begin(1000);require(hold.held()&&hold.sequence()==0&&hold.deadline()==31000,"Prepared hold did not start its bounded monotonic lease");require(!hold.renew(2,2000)&&hold.deadline()==31000,"Skipped renewal sequence extended the hold");require(hold.renew(1,2000)&&hold.deadline()==32000,"Exact next renewal did not extend the lease");require(!hold.renew(1,3000)&&hold.deadline()==32000,"Duplicate renewal extended the hold");require(!hold.expire(31999)&&hold.held()&&hold.deadline()==32000,"Status observation shortened or extended a hold");require(hold.expire(32000)&&!hold.held()&&!hold.renew(2,32001),"Expired hold was revived");hold.begin(40000);hold.release();require(!hold.held()&&!hold.renew(1,40001),"Cancelled hold was revived");}
  {require(qemuConsoleHoldIdentityCurrent(true,true,true,false),"A Windows lock without a session/lease change invalidated non-mutating hold authority");for(unsigned fault=0;fault<4;++fault)require(!qemuConsoleHoldIdentityCurrent(fault!=0,fault!=1,fault!=2,fault==3),"Changed session, machine, lease or pending configuration retained hold authority");}
  {NativeActivationGate gate;const std::string leaseKey="session:exact-sdk-lease";require(gate.begin(leaseKey),"Activation gate fixture could not reserve the exact lease");gate.observed(leaseKey,true,true,true);require(gate.readinessAllowed(leaseKey),"Verified activation did not open readiness");require(!gate.rearmAfterConsole("another-lease")&&!gate.begin(leaseKey),"A foreign key cleared the successful activation key");require(gate.rearmAfterConsole(leaseKey)&&gate.begin(leaseKey),"Quiet successful console release could not rearm the same owned lease");require(!gate.rearmAfterConsole(leaseKey)&&!gate.readinessAllowed(leaseKey),"Uncertain activation was reset by console release");}
  {require(qemuConsoleActivationRearmAllowed(true,true,true,true,false),"Exact quiet console release could not rearm activation");for(unsigned fault=0;fault<5;++fault)require(!qemuConsoleActivationRearmAllowed(fault!=0,fault!=1,fault!=2,fault!=3,fault==4),"Failed/unquiescent/foreign lease/session or running probe rearmed activation");}
}
// qemu_console_route.hpp: which way a console request goes for each Windows
// session state. The facts are what ServiceBridge::qemuConsoleFacts reads.
void routeCases(){
  const auto route=[](bool provider,bool console,bool unlocked,bool elevated){return qemuConsoleRoute({provider,console,unlocked,elevated});};
  // An administrator signed in at the unlocked console: the worker, exactly as before.
  require(route(true,true,true,true)==QemuConsoleRoute::worker,"An unlocked administrator console session lost its worker");
  // Signed out / the sign-in screen / switching users: no active console session.
  require(route(true,false,false,false)==QemuConsoleRoute::park,"A signed-out console was refused");
  // Locked (Win+L): the session is the console's, not unlocked.
  require(route(true,true,false,false)==QemuConsoleRoute::park,"A locked console was refused");
  // A standard user signed in: unlocked console session, no administrator worker.
  require(route(true,true,true,false)==QemuConsoleRoute::park,"A standard user's console went to a worker that cannot act");
  // Signed in over RDP only: the active session is not the console's, whatever its user is.
  require(route(true,false,true,true)==QemuConsoleRoute::park&&route(true,false,false,true)==QemuConsoleRoute::park,"An RDP-only sign-in was refused");
  // No native provider / display manager v1: nothing to park, the old answer.
  for(unsigned state=0;state<8;++state)require(route(false,(state&1)!=0,(state&2)!=0,(state&4)!=0)==QemuConsoleRoute::refuse,"A Host without the native provider was routed");
  // Exactly one state reaches the worker.
  unsigned workers=0;for(unsigned state=0;state<8;++state)if(route(true,(state&1)!=0,(state&2)!=0,(state&4)!=0)==QemuConsoleRoute::worker)++workers;
  require(workers==1,"More than the unlocked administrator console reached the worker");
  require(qemuConsoleParkCause({true,false,false,false})==1&&qemuConsoleParkCause({true,true,false,false})==2&&qemuConsoleParkCause({true,true,true,false})==3,"Park cause codes moved");
  // An administrator's unlocked console session under a secure desktop (a UAC prompt): its worker cannot change
  // the display until the prompt is answered, and the console is opened to answer it. Parked, with its own cause.
  require(qemuConsoleRoute({true,true,true,true,false})==QemuConsoleRoute::park&&qemuConsoleParkCause({true,true,true,true,false})==4,"An administrator's console under a secure desktop went to a worker that cannot act");
  require(qemuConsoleRoute({true,true,true,true,true})==QemuConsoleRoute::worker&&qemuConsoleRoute({false,true,true,true,false})==QemuConsoleRoute::refuse,"The secure desktop changed another route");
  require(qemuConsoleParkCause({true,true,true,false,false})==3&&qemuConsoleParkCause({true,true,false,false,false})==2,"A secure desktop hid the cause that comes first");
  // A parked hold follows the machine and the park, not a Windows session or a lease.
  require(qemuConsoleParkIdentityCurrent(true,true)&&!qemuConsoleParkIdentityCurrent(false,true)&&!qemuConsoleParkIdentityCurrent(true,false),"Parked hold identity is not machine + park");
  require(qemuConsoleParkMarginMillis>0&&qemuConsoleHoldMillis==30000,"The park no longer outlasts the hold it serves");
}
}
int main(){try{cases();protocolCases();routeCases();std::cout<<checks<<" QEMU console preparation checks passed\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
