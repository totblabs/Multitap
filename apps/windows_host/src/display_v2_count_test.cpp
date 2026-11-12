// Display manager v2: owned output count 1 <-> 2 (docs/display-manager-v2.md,
// "Output count"). Pure fixtures: the in-memory Windows CCD/SDK model
// (tests/display-v2-world.hpp) and an in-memory state file. Nothing here
// queries or changes this PC's displays, drivers or registry.
#include "display_v2_rpc.hpp"
#include "native_track_topology.hpp"
#include "../tests/display-v2-world.hpp"
#include <iostream>
using namespace host;
namespace {
using namespace v2world;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
DisplayV2Access access(World& w,Memory& m,bool console=true){return worldAccess(w,m,console);}

// Field evidence: count requests on the v1 path failed (display_configuration_failed,
// ~2.1 s) while v2 resizes succeeded; the console is inactive at rest and
// Windows re-activates/re-lays displays when the SDK adds or removes a target.
void requireCount(World& w,size_t count,const char* reason){
  const auto& o=w.owned();
  require(o.active&&o.x==0&&o.y==0&&o.width==w.ownedWidth&&o.height==w.ownedHeight&&w.hasSecond()==(count==2)&&!overlapping(w),reason);
  if(count==2){const auto& s=w.second();require(s.active&&s.x==LONG(w.ownedWidth)&&s.y==0&&s.width==w.secondWidth&&s.height==w.secondHeight,reason);}
}
// The next dual stream (still the v1 tracks path) starts from this topology:
// both owned outputs active once at their lease size with the 60 Hz/identity
// owned path shape, output 0 primary at the origin, and a typical client group
// (applyNativeWorkerTracks -> planNativeTrackViewport) fits without moving any
// other display.
void requireTracksReady(World& w,const char* reason){
  const auto targets=w.targets();const auto ccd=w.query();
  NativeTopologyBaseline baseline;
  try{baseline=captureNativeTopologyBaseline(ccd,targets,"0");}catch(...){require(false,reason);}
  require(baseline.placements.size()==targets.size()&&baseline.placements[0].primary&&!baseline.placements[0].left&&!baseline.placements[0].top,reason);
  require(nativeTopologyMatches(planNativeTopology(ccd,targets,baseline.placements),ccd,ccd),reason);
  if(targets.size()!=2)return;
  require(baseline.placements[1].left==LONG(targets[0].width)&&!baseline.placements[1].top&&!baseline.placements[1].primary,reason);
  const std::array<NativeTrackViewport,2> requested{{{"0",1600,900,0,0},{"1",1280,720,1600,0}}};
  NativeTrackViewportPlan group;try{group=planNativeTrackViewport(baseline,requested);}catch(...){require(false,reason);}
  require(group.placements.size()==2&&group.placements[0].primary&&group.placements[1].left==1600&&!group.placements[1].top,reason);
}
// Windows changes another display (the same five shapes as the stream cases).
void changeForeign(int scenario,World& world){auto& e=world.extra();
  if(scenario==0){e.width=1024;e.height=768;e.dbWidth=1024;e.dbHeight=768;}
  if(scenario==1)e.active=false;
  if(scenario==2)e.refreshN=60;
  if(scenario==3)e.rotation=DISPLAYCONFIG_ROTATION_ROTATE90;
  if(scenario==4)e.format=DISPLAYCONFIG_PIXELFORMAT_16BPP;
}
constexpr std::array<uint32_t,5> foreignCodes{displayV2ForeignCode::size,displayV2ForeignCode::count,displayV2ForeignCode::refresh,displayV2ForeignCode::rotation,displayV2ForeignCode::format};
bool extraOriginal(World& w){const auto& e=w.extra();return e.active&&e.width==800&&e.height==600&&e.refreshN==75&&e.refreshD==1&&e.rotation==DISPLAYCONFIG_ROTATION_IDENTITY&&e.format==DISPLAYCONFIG_PIXELFORMAT_32BPP;}
int countCases(){
  int passed=0;
  // Console active before: it stays active with its mode, right of the owned
  // outputs; output 0 becomes primary, output 1 sits to its right.
  for(const bool proof:{true,false}){
    World w;Memory m;DisplaySessionV2 session(access(w,m,proof));
    const auto up=session.count("count-up",2,proof,deadline(w));
    require(up.ok&&up.countAttempted&&!up.stateKept&&!m.bytes&&!session.active()&&w.counts==1&&up.owned.size()==2,"1->2 failed, latched a session or left the state file");
    require(up.consoleIdentified==proof&&up.foreignBefore==1&&!up.displaysReturned&&!up.restore,"1->2 diagnostics are wrong");
    requireCount(w,2,"1->2 did not leave output 0 primary and output 1 to its right");
    require(w.console().active&&w.console().width==1024&&w.console().height==768&&w.console().x==3840&&w.console().y==0,"Console active before 1->2 was not kept with its mode");
    requireTracksReady(w,"Topology after 1->2 does not meet the dual-stream (tracks) path's expectations");
    const auto down=session.count("count-down",1,proof,deadline(w));
    require(down.ok&&!m.bytes&&w.counts==2&&down.owned.size()==1,"2->1 failed or left the state file");
    requireCount(w,1,"2->1 did not leave output 0 alone at the origin");
    require(w.console().active&&w.console().width==1024&&w.console().x==1920,"Console active before 2->1 was not kept with its mode");
    // The next single stream uses the post-count configuration as its baseline.
    DisplaySessionV2 stream(access(w,m));
    require(stream.start("s",1600,900,true,deadline(w)).ok,"Stream after a count change did not start");
    requireStreaming(w,1600,900,true,"Stream after a count change is not streaming");
    require(stream.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Stop after a count change failed");
    requireCount(w,1,"Stop after a count change did not return to the post-count configuration");
    require(w.console().active&&w.console().x==1920,"Stop after a count change lost the console");++passed;
  }
  // Field shape: the console is inactive at rest and Windows re-activates it
  // (at an arbitrary place) whenever the SDK adds or removes an owned target.
  for(const bool proof:{true,false})for(const bool autoActivate:{false,true}){
    World w;w.console().active=false;w.owned().x=0;w.autoActivate=autoActivate;Memory m;DisplaySessionV2 session(access(w,m,proof));
    w.afterCount=[](World& world){auto& c=world.console();c.active=true;c.x=-1024;c.y=200;c.width=c.dbWidth;c.height=c.dbHeight;};
    const auto up=session.count("vm149-count-up",2,proof,deadline(w));
    require(up.ok&&up.displaysReturned&&up.consoleIdentified==proof&&up.foreignBefore==0&&!w.console().active&&!m.bytes,"1->2 failed when Windows re-activated the inactive console");
    requireCount(w,2,"1->2 with a re-activated console did not leave both owned outputs alone");
    requireTracksReady(w,"Topology after 1->2 (console inactive) does not meet the tracks path's expectations");
    const auto down=session.count("vm149-count-down",1,proof,deadline(w));
    require(down.ok&&down.displaysReturned&&!w.console().active&&!m.bytes,"2->1 failed when Windows re-activated the inactive console");
    requireCount(w,1,"2->1 with a re-activated console did not leave output 0 alone");++passed;
  }
  // Windows applies its own layout once more after our apply (re-plan).
  {World w;w.console().active=false;w.owned().x=0;Memory m;DisplaySessionV2 session(access(w,m));int late=0;
    w.afterApply=[&](World& world){if(world.hasSecond()&&!late++){auto& c=world.console();c.active=true;c.x=0;c.y=world.ownedHeight;world.second().x=0;world.second().y=-1080;}};
    const auto up=session.count("late",2,true,deadline(w));
    require(up.ok&&!w.console().active&&late==2,"A late Windows re-layout after the apply was not re-planned");requireCount(w,2,"Late re-layout left a wrong topology");++passed;}
  // Windows moves/re-modes another display during the SDK change: it is put
  // back with its pre-change mode (positions are re-laid), both directions.
  for(int scenario=0;scenario<5;++scenario){
    World w(true);Memory m;DisplaySessionV2 session(access(w,m));
    w.afterCount=[&](World& world){changeForeign(scenario,world);world.console().x=0;world.console().y=900;};
    require(session.count("c",2,true,deadline(w)).ok&&extraOriginal(w)&&w.console().active&&w.console().width==1024,"A foreign change made by the SDK addition was not put back");
    requireCount(w,2,"Corrected 1->2 left a wrong owned topology");
    require(w.console().x==3840&&w.extra().x==3840+1024,"Other displays were not kept contiguous right of the owned outputs");
    require(session.count("c",1,true,deadline(w)).ok&&extraOriginal(w),"A foreign change made by the SDK removal was not put back");
    requireCount(w,1,"Corrected 2->1 left a wrong owned topology");++passed;
  }
  // Windows keeps re-imposing it: fixed foreign code at stage 8, the previous
  // count is put back and the snapshot restored; the next attempt works.
  for(int scenario=0;scenario<5;++scenario){
    World w(true);Memory m;DisplaySessionV2 session(access(w,m));
    w.afterCount=[&](World& world){changeForeign(scenario,world);};
    w.afterApply=[&](World& world){if(world.hasSecond())changeForeign(scenario,world);};
    const auto failed=session.count("c",2,true,deadline(w));
    require(!failed.ok&&failed.stage==displayV2Stage::foreign&&failed.foreignDifference==foreignCodes[size_t(scenario)]&&failed.countAttempted&&failed.restore&&failed.restore->outcome==DisplayV2Outcome::restored,"Persistent foreign change did not fail with its code and restore");
    require(!w.hasSecond()&&w.counts==2&&!m.bytes&&!failed.stateKept&&!session.active(),"Failed 1->2 did not put the previous count back or left the state file");
    requireOriginal(w,true,"Failed 1->2 did not restore the original configuration");
    const auto diagnostic=displayV2CountDiagnostic(failed);
    require(!diagnostic.ok&&diagnostic.stage==displayV2Stage::foreign&&diagnostic.error==foreignCodes[size_t(scenario)]&&diagnostic.restored&&!diagnostic.stateKept&&diagnostic.foreignCount==2,"Count diagnostic lost the stage-8 code");
    w.afterCount=nullptr;w.afterApply=nullptr;
    require(session.count("c",2,true,deadline(w)).ok&&extraOriginal(w),"Count after a persistent foreign failure latched");requireCount(w,2,"Count after a failure is wrong");++passed;
  }
  // 2->1 failing persistently restores both outputs, the second at its saved size.
  {World w(true);Memory m;DisplaySessionV2 session(access(w,m));
    require(session.count("c",2,true,deadline(w)).ok,"Fixture 1->2 failed");
    // Output 1 at another size (e.g. left by a dual stream), laid out validly.
    require(w.resize("1",1280,720).has_value(),"Fixture output 1 resize failed");
    w.second().active=true;w.second().x=1920;w.second().y=0;w.console().x=3200;w.extra().x=4224;
    w.afterCount=[&](World& world){if(!world.hasSecond())changeForeign(2,world);};
    w.afterApply=[&](World& world){if(!world.hasSecond())changeForeign(2,world);};
    const auto failed=session.count("c",1,true,deadline(w));
    require(!failed.ok&&failed.stage==displayV2Stage::foreign&&failed.foreignDifference==displayV2ForeignCode::refresh&&failed.restore&&failed.restore->outcome==DisplayV2Outcome::restored,"Persistent foreign change on 2->1 did not fail with its code and restore");
    w.afterCount=nullptr;w.afterApply=nullptr;
    require(w.hasSecond()&&w.secondWidth==1280&&w.secondHeight==720&&w.second().active&&w.second().x==1920&&w.second().width==1280,"Failed 2->1 did not bring output 1 back at its saved size and place");
    require(w.owned().x==0&&w.console().active&&w.console().x==3200&&extraOriginal(w)&&w.extra().x==4224&&!m.bytes,"Failed 2->1 did not restore the other displays");++passed;}
  // SDK refusal: every attempt fails cleanly with nothing left behind; no latch.
  {World w;Memory m;w.failCount=true;
    for(int attempt=0;attempt<3;++attempt){
      DisplaySessionV2 session(access(w,m));const auto result=session.count("c",2,true,deadline(w));
      require(!result.ok&&result.stage==displayV2Stage::sdk&&result.restore&&result.restore->ok()&&!m.bytes&&!session.active()&&!w.hasSecond(),"SDK count refusal left a state file, a session or an output");
      requireOriginal(w,false,"SDK count refusal changed the displays");
    }
    w.failCount=false;DisplaySessionV2 session(access(w,m));require(session.count("c",2,true,deadline(w)).ok,"Count after SDK refusals latched");requireCount(w,2,"Count after SDK refusals is wrong");++passed;}
  // Locked session before the change: refused without any change.
  {World w;Memory m;w.eligible=false;DisplaySessionV2 session(access(w,m));const auto result=session.count("c",2,true,deadline(w));
    require(!result.ok&&result.stage==displayV2Stage::ineligible&&!m.bytes&&w.applies==0&&w.counts==0&&!result.countAttempted,"Locked count changed something");
    w.eligible=true;require(session.count("c",2,true,deadline(w)).ok,"Count after a locked attempt latched");++passed;}
  // Locked in the middle: the SDK count goes back (no session needed), Windows
  // is not touched while locked, the state stays for automatic recovery.
  {World w;Memory m;DisplaySessionV2 session(access(w,m));
    w.afterCount=[](World& world){if(world.hasSecond())world.eligible=false;};
    const auto applies=w.applies;const auto result=session.count("c",2,true,deadline(w));
    require(!result.ok&&result.stage==displayV2Stage::ineligible&&!w.hasSecond()&&w.applies==applies&&result.restore&&!result.restore->ok()&&result.stateKept&&m.bytes,"Lock during a count change did not put the count back and keep the state");
    w.eligible=true;w.afterCount=nullptr;DisplaySessionV2 later(access(w,m,false));
    require(later.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Unlocked recovery after a locked count change failed");requireOriginal(w,false,"Unlocked recovery did not restore the original");
    require(session.count("c",2,true,deadline(w)).ok,"Count after a locked failure latched");++passed;}
  // Already the requested count: nothing is changed (same as v1).
  {World w;Memory m;DisplaySessionV2 session(access(w,m));const auto result=session.count("c",1,true,deadline(w));
    require(result.ok&&w.counts==0&&w.applies==0&&w.validations==0&&!m.saves&&!result.countAttempted,"No-op count changed the SDK, Windows or the state");++passed;}
  // GDI/capture does not list the owned outputs: stage 7, count and Windows back.
  {World w;Memory m;auto a=access(w,m);size_t seen=0;a.usable=[&](std::span<const NativeDisplayTarget> targets){seen=targets.size();return false;};DisplaySessionV2 session(std::move(a));
    const auto result=session.count("c",2,true,deadline(w));
    require(!result.ok&&result.stage==displayV2Stage::verify&&result.reason=="display_v2_capture_unavailable"&&seen==2&&!w.hasSecond()&&!m.bytes,"Unusable owned outputs were kept");
    requireOriginal(w,false,"Unusable owned outputs did not restore the original");++passed;}
  // An older unrestored original (a stream whose restore failed) stays the
  // original; the ordinary recovery later restores it and keeps the count.
  {World w;Memory m;
    {DisplaySessionV2 first(access(w,m));require(first.start("a",2560,1440,true,deadline(w)).ok,"Fixture start failed");
      w.reject=[](const NativeCcdData&,UINT32){return LONG(ERROR_GEN_FAILURE);};w.consoleReturns=false;
      require(first.restore().outcome==DisplayV2Outcome::failed&&m.bytes,"Fixture restore unexpectedly succeeded");}
    const auto original=*m.bytes;w.reject=nullptr;w.consoleReturns=true;
    DisplaySessionV2 session(access(w,m));const auto up=session.count("c",2,false,deadline(w));
    require(up.ok&&up.stateKept&&m.bytes==std::optional(original),"Count replaced or deleted an older unrestored original");
    requireCount(w,2,"Count with an older original left a wrong topology");
    DisplaySessionV2 recovery(access(w,m,false));require(recovery.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Recovery after the count did not restore the older original");
    require(w.console().active&&w.console().x==0&&w.owned().active&&w.owned().x==1024&&w.owned().width==1920&&w.hasSecond()&&w.second().active&&w.second().x==1024+1920,"Recovery lost the console or the user's new count");++passed;}
  // Worker/service gone in the middle (snapshot written, SDK count already
  // committed, Windows half-done): the service-start recovery restores the
  // other displays, keeps the committed count and never blocks a new change.
  for(const bool autoActivate:{false,true}){
    World w;Memory m;w.autoActivate=autoActivate;
    m.bytes=displayV2StateBytes({"count-crashed",displayV2ActiveCcd(w.query()),w.targets(),std::nullopt});
    require(w.setCount(2).has_value(),"Fixture SDK count failed");
    w.serviceRestart();
    DisplaySessionV2 recovery(access(w,m,false));
    require(recovery.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Recovery after an interrupted count change failed");
    require(w.console().active&&w.console().x==0&&w.owned().active&&w.owned().x==1024&&w.hasSecond()&&w.second().active&&w.second().x==1024+1920&&!overlapping(w),"Recovery after an interrupted count change lost a display or the committed count");
    DisplaySessionV2 next(access(w,m));require(next.count("c",1,true,deadline(w)).ok&&!m.bytes,"Count after recovery latched");requireCount(w,1,"Count after recovery is wrong");++passed;}
  // Invalid count requests change nothing.
  for(const size_t invalid:{size_t(0),size_t(3)}){World w;Memory m;DisplaySessionV2 session(access(w,m));const auto result=session.count("c",invalid,true,deadline(w));
    require(!result.ok&&w.counts==0&&w.applies==0&&!m.bytes,"Invalid count request changed something");++passed;}
  return passed;
}
// Count 2 -> the v2 dual-output stream (DisplaySessionV2::startGroup) -> stop
// returns to the post-count configuration, with the console active at rest and
// with it inactive while Windows re-activates it on every target change.
int groupAfterCountCases(){
  int passed=0;
  const std::vector<DisplayV2Output> group{{"0",1600,900,0,0},{"1",1280,720,1600,0}};
  for(const bool inactive:{false,true}){
    World w;if(inactive){w.console().active=false;w.owned().x=0;w.afterCount=[](World& world){auto& c=world.console();c.active=true;c.x=-1024;c.y=200;c.width=c.dbWidth;c.height=c.dbHeight;};}
    Memory m;DisplaySessionV2 count(access(w,m));
    require(count.count("count",2,true,deadline(w)).ok&&!m.bytes,"Fixture count 2 failed");requireCount(w,2,"Fixture count 2 left a wrong topology");
    if(inactive)w.afterResize=[](World& world){auto& c=world.console();c.active=true;c.x=0;c.y=1080;c.width=c.dbWidth;c.height=c.dbHeight;};
    DisplaySessionV2 stream(access(w,m));const auto started=stream.startGroup("group-after-count",group,true,deadline(w));
    require(started.ok&&m.bytes&&!w.console().active,"Dual stream after a count change did not start");
    const auto& o=w.owned();const auto& s=w.second();
    require(o.active&&!o.x&&!o.y&&o.width==1600&&s.active&&s.x==1600&&!s.y&&s.width==1280&&s.height==720&&!overlapping(w),"Dual stream after a count change left a wrong layout");
    require(stream.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Dual stream stop after a count change failed");
    requireCount(w,2,"Dual stream stop did not return to the post-count topology");
    require(w.console().active==!inactive&&(inactive||(w.console().x==3840&&w.console().width==1024)),"Dual stream stop lost or invented the console");
    w.afterResize=nullptr;require(count.count("count",1,true,deadline(w)).ok&&!m.bytes,"Count 1 after a dual stream failed");requireCount(w,1,"Count 1 after a dual stream is wrong");++passed;
  }
  // An older unrestored original (a single-output stream whose restore failed,
  // from before the count) stays the original through count 2 and a dual
  // stream; the stop restores it and keeps output 1 (unknown to it) active.
  {World w;Memory m;
    {DisplaySessionV2 first(access(w,m));require(first.start("a",2560,1440,true,deadline(w)).ok,"Fixture start failed");
      w.reject=[](const NativeCcdData&,UINT32){return LONG(ERROR_GEN_FAILURE);};w.consoleReturns=false;
      require(first.restore().outcome==DisplayV2Outcome::failed&&m.bytes,"Fixture restore unexpectedly succeeded");}
    const auto original=*m.bytes;w.reject=nullptr;w.consoleReturns=true;
    {DisplaySessionV2 count(access(w,m));require(count.count("c",2,false,deadline(w)).ok&&m.bytes==std::optional(original),"Count replaced an older unrestored original");}
    DisplaySessionV2 stream(access(w,m));require(stream.startGroup("g",group,true,deadline(w)).ok&&m.bytes==std::optional(original),"Dual stream replaced an older unrestored original");
    require(stream.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Dual stream stop did not restore the older original");
    require(w.console().active&&!w.console().x&&w.owned().active&&w.owned().x==1024&&w.owned().width==1920&&w.hasSecond()&&w.second().active&&w.second().x==1024+1920&&!overlapping(w),"Restoring the older original lost the console or output 1");
    DisplaySessionV2 next(access(w,m));require(next.startGroup("g",group,true,deadline(w)).ok&&next.restore().ok(),"Dual stream after the older original was restored failed");++passed;}
  return passed;
}
int countPlanCases(){
  int passed=0;
  // Plan shape: owned 0 at the origin, owned 1 right of it, baseline displays
  // (console included) with their baseline modes slid right; an inactive
  // baseline display is not supplied even when Windows activated it.
  {World w(true);w.setCount(2);w.second().active=false;
    const auto before=w.query();
    auto& c=w.console();c.width=640;c.height=480;     // Windows re-moded the console meanwhile
    const auto plan=planDisplayV2Count(w.query(),before,w.targets(),w.targets());
    std::map<std::pair<DWORD,UINT32>,DISPLAYCONFIG_SOURCE_MODE> got;for(const auto& p:plan.paths)got[{p.targetInfo.adapterId.LowPart,p.targetInfo.id}]=nativeCcdSource(plan,p);
    require(plan.paths.size()==4&&same(plan.paths[0].targetInfo.adapterId,ownedAdapter)&&plan.paths[0].targetInfo.id==w.ownedTarget&&plan.paths[1].targetInfo.id==w.secondTarget,"Count plan path order is wrong");
    const auto& o=got[{ownedAdapter.LowPart,w.ownedTarget}];const auto& s=got[{ownedAdapter.LowPart,w.secondTarget}];const auto& cs=got[{consoleAdapter.LowPart,consoleTarget}];const auto& es=got[{extraAdapter.LowPart,extraTarget}];
    require(!o.position.x&&!o.position.y&&o.width==1920&&s.position.x==1920&&!s.position.y&&s.width==1920,"Count plan did not lay out the owned outputs from the origin");
    require(cs.width==1024&&cs.height==768&&cs.position.x==3840&&es.position.x==3840+1024&&es.width==800,"Count plan did not re-supply baseline modes right of the owned outputs");++passed;}
  {World w;w.console().active=false;w.owned().x=0;const auto before=w.query();w.setCount(2);w.console().active=true;w.console().x=1920;
    const auto plan=planDisplayV2Count(w.query(),before,w.targets(),w.targets());
    require(plan.paths.size()==2&&std::none_of(plan.paths.begin(),plan.paths.end(),[](const auto& p){return same(p.targetInfo.adapterId,consoleAdapter);}),"Count plan re-supplied a console that was inactive before");++passed;}
  // Satisfaction rejects a departed owned generation still active, a
  // non-primary output 0, output 1 elsewhere, a wrong size and overlap; other
  // displays may sit anywhere that does not overlap.
  {World w;w.setCount(2);w.second().active=true;w.second().x=1920;w.owned().x=0;w.console().x=3840;
    const auto ours=w.targets();std::string why;
    require(displayV2CountSatisfied(w.query(),w.targets(),ours,&why),"Satisfied fixture rejected");
    w.console().x=-1024;w.console().y=-300;require(displayV2CountSatisfied(w.query(),w.targets(),ours,&why),"A console Windows placed elsewhere was rejected");
    const std::vector<NativeDisplayTarget> single{w.target()};
    require(!displayV2CountSatisfied(w.query(),single,ours,&why)&&why=="display_v2_owned_departure_pending","Departed owned output still active was accepted");
    w.owned().x=10;require(!displayV2CountSatisfied(w.query(),w.targets(),ours,&why)&&why=="display_v2_owned_not_primary","Non-primary output 0 was accepted");w.owned().x=0;
    w.second().x=0;w.second().y=1080;require(!displayV2CountSatisfied(w.query(),w.targets(),ours,&why)&&why=="display_v2_owned_not_placed","Output 1 away from the right of output 0 was accepted");w.second().x=1920;w.second().y=0;
    w.console().x=3000;w.console().y=0;require(!displayV2CountSatisfied(w.query(),w.targets(),ours,&why)&&why=="display_v2_overlap","A display overlapping an owned output was accepted");w.console().x=3840;
    w.second().active=false;require(!displayV2CountSatisfied(w.query(),w.targets(),ours,&why)&&why=="display_v2_owned_inactive","Inactive output 1 was accepted");w.second().active=true;
    w.second().width=1280;require(!displayV2CountSatisfied(w.query(),w.targets(),ours,&why)&&why=="display_v2_owned_size_mismatch","Wrong-sized output 1 was accepted");++passed;}
  return passed;
}
int countRpcCases(){
  int passed=0;
  const std::string two=R"({"schema":1,"provider":"libvirtualdisplay-1.6.4","leaseActive":true,"targets":[{"id":"0","adapterLow":44,"adapterHigh":0,"target":1,"width":1920,"height":1080},{"id":"1","adapterLow":44,"adapterHigh":0,"target":2,"width":1920,"height":1080}]})";
  const std::string one=R"({"schema":1,"provider":"libvirtualdisplay-1.6.4","leaseActive":true,"targets":[{"id":"0","adapterLow":44,"adapterHigh":0,"target":1,"width":1920,"height":1080}]})";
  const DisplayV2CountRequest request{std::string(32,'c'),2};
  {const auto parsed=parseDisplayV2CountRequest(JsonParser::parse(displayV2CountRequestBytes(request)));require(parsed&&parsed->nonce==request.nonce&&parsed->count==2,"Count request did not round-trip");++passed;}
  for(const auto* bad:{R"({"type":"worker.v2.display.count","nonce":"cccccccccccccccccccccccccccccccc","count":3})",R"({"type":"worker.v2.display.count","nonce":"cccccccccccccccccccccccccccccccc","count":0})",R"({"type":"worker.v2.display.count","nonce":"cccccccccccccccccccccccccccccccc","count":1.5})",R"({"type":"worker.v2.display.count","nonce":"short","count":2})",R"({"type":"worker.v2.display.count","nonce":"cccccccccccccccccccccccccccccccc","count":2,"extra":1})"}){
    require(!parseDisplayV2CountRequest(JsonParser::parse(bad)),"Malformed count request accepted");++passed;
  }
  int calls=0;const auto count=[&](int value)->std::optional<std::string>{++calls;return value==2?two:one;};
  const auto resize=[](const std::string&,DWORD,DWORD)->std::optional<std::string>{return {};};
  // The stream and recovery pipes pass no count function: refused, never run.
  {const auto refused=dispatchDisplayV2(JsonParser::parse(displayV2CountRequestBytes(request)),true,resize);require(refused.handled&&!JsonParser::parse(refused.response).get("ok").boolean()&&calls==0,"Count request on a pipe without count authority reached the SDK");++passed;}
  {const auto denied=dispatchDisplayV2(JsonParser::parse(displayV2CountRequestBytes(request)),false,resize,count);require(denied.handled&&!JsonParser::parse(denied.response).get("ok").boolean()&&calls==0,"Unauthorized count request reached the SDK");++passed;}
  // Worker side through an in-memory pipe.
  std::vector<Json> inbound;
  const auto read=[&](DWORD)->std::optional<Json>{if(inbound.empty())return {};auto value=inbound.front();inbound.erase(inbound.begin());return value;};
  const auto write=[&](const std::string& message){inbound.push_back(JsonParser::parse(dispatchDisplayV2(JsonParser::parse(message),true,resize,count).response));return true;};
  {const auto targets=displayV2CountRpc(request,write,read,{},5000);require(targets&&targets->size()==2&&calls==1,"Worker count RPC did not return both owned targets");++passed;}
  {const auto targets=displayV2CountRpc({std::string(32,'d'),1},write,read,{},5000);require(targets&&targets->size()==1&&targets->front().id=="0","Worker count RPC did not return the single owned target");++passed;}
  {const auto targets=displayV2CountRpc(request,[&](const std::string&){inbound.push_back(JsonParser::parse(displayV2CountReplyBytes(request.nonce,one)));return true;},read,{},5000);require(!targets,"A snapshot with the wrong count was accepted");++passed;}
  {const auto targets=displayV2CountRpc(request,[&](const std::string&){inbound.push_back(JsonParser::parse(displayV2CountReplyBytes(std::string(32,'e'),two)));return true;},read,{},5000);require(!targets,"A reply for another request was accepted");++passed;}
  {const auto targets=displayV2CountRpc(request,[&](const std::string&){inbound.push_back(JsonParser::parse(displayV2CountReplyBytes(request.nonce,std::nullopt)));return true;},read,{},5000);require(!targets,"A refused SDK count change was accepted");++passed;}
  {inbound.clear();const auto targets=displayV2CountRpc(request,[](const std::string&){return true;},read,{},5000);require(!targets,"A missing count reply was accepted");++passed;}
  {const auto targets=displayV2CountRpc({std::string(32,'f'),3},write,read,{},5000);require(!targets,"An invalid count was sent");++passed;}
  // Authorization: the operation's own count only while it is current; the
  // previous count (restore after a failure) also after a cancel/lock/expiry.
  require(displayV2CountAuthorized(2,2,true,false,true,true,true),"Current forward count refused");
  require(!displayV2CountAuthorized(2,2,false,false,true,true,true)&&!displayV2CountAuthorized(2,2,true,true,true,true,true)&&!displayV2CountAuthorized(2,2,true,false,false,true,true)&&!displayV2CountAuthorized(2,2,true,false,true,false,true)&&!displayV2CountAuthorized(2,2,true,false,true,true,false),"Forward count allowed for a stale/cancelled/locked operation");
  require(displayV2CountAuthorized(1,2,true,true,false,false,false)&&!displayV2CountAuthorized(1,2,false,false,true,true,true)&&!displayV2CountAuthorized(3,2,true,false,true,true,true),"Count put-back authorization is wrong");++passed;
  // Diagnostics round-trip through worker.display.result.
  {DisplayV2Result result;result.stage=displayV2Stage::foreign;result.foreignDifference=displayV2ForeignCode::count;result.foreignBefore=1;result.displaysReturned=true;result.restore=DisplayV2RestoreResult{DisplayV2Outcome::restored};
    const auto value=displayV2CountDiagnostic(result);
    const auto packet=JsonParser::parse(R"({"type":"worker.display.result","ok":false,"nativeStage":)"+std::to_string(value.stage)+",\"nativeError\":"+std::to_string(value.error)+displayV2CountDiagnosticFields(value)+'}');
    const auto parsed=parseDisplayV2CountDiagnostic(packet);
    require(parsed&&!parsed->ok&&parsed->stage==8&&parsed->error==1&&parsed->foreignCount==1&&parsed->displaysReturned&&parsed->restored&&!parsed->stateKept&&!parsed->consoleIdentified,"Count diagnostic did not round-trip");
    auto bad=packet;std::get<Json::Object>(bad.value)["ok"].value=true;require(!parseDisplayV2CountDiagnostic(bad),"An ok result with a failure stage was accepted");
    auto extra=packet;std::get<Json::Object>(std::get<Json::Object>(extra.value)["v2"].value)["text"].value=std::string("free text");require(!parseDisplayV2CountDiagnostic(extra),"Free text in the count diagnostic was accepted");++passed;}
  return passed;
}
}
int main(){
  try{
    int passed=0;
    passed+=countCases();
    passed+=groupAfterCountCases();
    passed+=countPlanCases();
    passed+=countRpcCases();
    std::cout<<"Display manager v2 count checks passed: "<<passed<<"\n";return 0;
  }catch(const std::exception& error){std::cerr<<"Display manager v2 count check failed: "<<error.what()<<"\n";return 1;}
}