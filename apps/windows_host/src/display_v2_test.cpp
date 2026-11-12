// Display manager v2 regression (docs/display-manager-v2.md). Pure fixtures:
// an in-memory Windows CCD/SDK model (tests/display-v2-world.hpp) and an
// in-memory or temporary state file. Nothing here queries or changes this
// PC's displays, drivers or registry.
#include "display_v2_rpc.hpp"
#include "display_v2_tracks.hpp"
#include "legacy_display_identity.hpp"
#include "stream_prepare_diagnostics.hpp"
#include "../tests/display-v2-world.hpp"
#include <iostream>
#include <source_location>
using namespace host;
namespace {
using namespace v2world;
void require(bool value,const char* reason,const std::source_location where=std::source_location::current()){
  if(!value)throw std::runtime_error(std::string(reason)+" (line "+std::to_string(where.line())+")");
}
std::string jsonObject(const Json& value){
  if(const auto* array=std::get_if<Json::Array>(&value.value)){std::string out="[";for(const auto& child:*array){if(out.size()>1)out+=',';out+=jsonObject(child);}return out+']';}
  if(const auto* object=std::get_if<Json::Object>(&value.value)){std::string out="{";for(const auto& [key,child]:*object){if(out.size()>1)out+=',';out+=jsonQuote(key)+':'+jsonObject(child);}return out+'}';}
  if(const auto* text=std::get_if<std::string>(&value.value))return jsonQuote(*text);
  if(const auto* number=std::get_if<double>(&value.value)){std::ostringstream out;out<<std::setprecision(16)<<*number;return out.str();}
  if(const auto* flag=std::get_if<bool>(&value.value))return *flag?"true":"false";
  return "null";
}
DisplayV2Access access(World& w,Memory& m,bool console=true){return worldAccess(w,m,console);}

int preparedHandoffCases(){
  int passed=0;
  {World w(true);Memory m;auto a=access(w,m);a.hidden=[](LUID){return true;};
    DisplaySessionV2 old(std::move(a));require(old.start("same-logical-stream",1280,720,true,deadline(w)).ok,"Handoff fixture initial prepare failed");
    const auto original=m.bytes;require(original.has_value(),"Handoff fixture has no original");const auto sha=nativeBaselineDigest(*original);
    const auto saves=m.saves,removes=m.removes,applies=w.applies,resizes=w.resizes;
    require(old.retainForHandoff(sha,[]{return true;})&&!old.active(),"Exact old owner could not retain baseline for successor");
    auto nextAccess=access(w,m);nextAccess.hidden=[](LUID){return true;};DisplaySessionV2 next(std::move(nextAccess));int confirmed=0;
    require(next.attachPrepared("same-logical-stream",sha,{"0",1280,720,0,0},[&]{++confirmed;return true;}).ok&&next.active()&&confirmed==1,"Successor could not attach exact prepared output");
    require(m.bytes==original&&m.saves==saves&&m.removes==removes&&w.applies==applies&&w.resizes==resizes,"Worker handoff recaptured or modified original topology/SDK");
    require(next.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Final successor did not restore the one original");
    requireOriginal(w,true,"Final handoff restore lost original foreign displays");++passed;}
  for(unsigned failure=0;failure<7;++failure){
    World w;Memory m;DisplaySessionV2 first(access(w,m));require(first.start("sealed",1280,720,true,deadline(w)).ok,"Negative handoff fixture failed");
    if(failure==6){require(w.setCount(2).has_value(),"Second owned output fixture failed");w.second().active=false;}
    const auto original=m.bytes;const auto sha=nativeBaselineDigest(*original);const auto saves=m.saves,removes=m.removes,applies=w.applies,resizes=w.resizes;
    auto expected=DisplayV2Output{"0",1280,720,0,0};auto wanted=sha;bool accepted=true;DisplaySessionV2::Confirm confirm=[]{return true;};
    if(failure==0)wanted[0]=wanted[0]=='0'?'1':'0';
    if(failure==1)expected.width=1600;
    if(failure==2)confirm={};
    if(failure==3)confirm=[]{return false;};
    if(failure==4)w.console().active=true;
    if(failure==5)confirm=[&]{m.bytes=*original+" ";return true;};
    DisplaySessionV2 next(access(w,m));accepted=next.attachPrepared("sealed",wanted,expected,confirm).ok;
    require(!accepted&&!next.active()&&m.saves==saves&&m.removes==removes&&w.applies==applies&&w.resizes==resizes,"Unbound/unconfirmed handoff changed displays or became active");
    if(failure!=5)require(m.bytes==original,"Failed attach replaced the protected baseline");++passed;
  }
  {World w;Memory m;DisplaySessionV2 old(access(w,m));require(old.start("retain-denial",1280,720,true,deadline(w)).ok,"Retain denial fixture failed");
    const auto sha=nativeBaselineDigest(*m.bytes);
    require(!old.retainForHandoff(sha,{})&&!old.retainForHandoff(sha,[]{return false;})&&old.active(),"Absent private service grant bypassed ordinary restoration");
    require(old.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Denied handoff skipped ordinary final restore");++passed;}
  return passed;
}

int stoppingServiceCleanupCases(){
  int cases=0;
  for(const bool cleanupPath:{false,true}){
    World w;Memory m;bool stopping=false;int cleanupReads=0;
    auto a=access(w,m);const auto running=a.owned;
    a.owned=[&]{if(stopping)throw std::runtime_error("SCM is STOP_PENDING");return running();};
    if(cleanupPath)a.restoreOwned=[&]{++cleanupReads;return running();};
    DisplaySessionV2 session(std::move(a));
    require(session.start("scm-stop",1280,720,true,deadline(w)).ok,"SCM stop fixture did not start");
    stopping=true;
    const auto result=session.restore();
    if(cleanupPath){
      require(result.outcome==DisplayV2Outcome::restored&&!m.bytes&&cleanupReads==1,"Held lease was not restored while SCM was stopping");
      requireOriginal(w,false,"SCM cleanup did not restore the original console and owned output");
    }else require(result.outcome==DisplayV2Outcome::failed&&m.bytes,"Released RUNNING-only lookup did not reproduce the stopped-service restore failure");
    ++cases;
  }
  {World w;Memory m;auto a=access(w,m);int cleanupReads=0;
    a.owned=[]{throw std::runtime_error("SCM is STOP_PENDING");return std::vector<NativeDisplayTarget>{};};
    a.restoreOwned=[&]{++cleanupReads;return w.targets();};
    DisplaySessionV2 session(std::move(a));const auto applies=w.applies;
    require(!session.start("no-new-stream-during-stop",1280,720,true,deadline(w)).ok&&cleanupReads==0&&w.applies==applies&&!m.bytes,"Cleanup observation admitted a new stream while stopping");++cases;
  }
  {World w;Memory m;auto a=access(w,m);bool invalid=false;
    a.restoreOwned=[&]{if(invalid)throw std::runtime_error("held lease replaced");return w.targets();};
    DisplaySessionV2 session(std::move(a));require(session.start("replaced-cleanup",1280,720,true,deadline(w)).ok,"Replaced cleanup fixture did not start");
    invalid=true;const auto applies=w.applies;
    require(session.restore().outcome==DisplayV2Outcome::failed&&m.bytes&&w.applies==applies,"Invalid cleanup proof changed displays or lost the restore state");++cases;
  }
  return cases;
}

int sdkCcdMismatchCases(){
  // SDK target metadata can lag Windows' actual source mode. The saved CCD
  // source, not the stale SDK size, is the desktop the user must get back.
  for(const bool previousState:{false,true}){
  World w;Memory m;
  w.console().width=w.console().dbWidth=1280;w.console().height=w.console().dbHeight=720;
  w.owned().x=1280;w.ownedWidth=1280;w.ownedHeight=720;
  if(previousState){
    m.bytes=displayV2StateBytes({"older-original",displayV2ActiveCcd(w.query()),w.targets(),DisplayV2Console{consoleAdapter,consoleAdapter,0,consoleTarget}});
    w.ownedWidth=w.owned().width=2560;w.ownedHeight=w.owned().height=1440;w.owned().x=0;w.console().active=false;
  }
  DisplaySessionV2 session(access(w,m));
  const std::array<DisplayV2Output,1> group{{{"0",1280,720,0,0}}};
  require(session.startGroup("sdk-ccd-mismatch",group,true,deadline(w)).ok,"Mismatched-source fixture could not start");
  const auto saved=parseDisplayV2State(*m.bytes);
  const auto path=std::find_if(saved.ccd.paths.begin(),saved.ccd.paths.end(),[&](const auto& p){return nativeSameTarget(p,saved.owned.front());});
  require(path!=saved.ccd.paths.end()&&nativeCcdSource(saved.ccd,*path).width==1920&&nativeCcdSource(saved.ccd,*path).height==1080,"The actual original CCD mode was not captured");
  const auto stopped=session.restore();
  std::cout<<"{\"kind\":\"display-v2-sdk-ccd-mismatch\",\"savedSdkWidth\":"<<saved.owned.front().width<<",\"savedCcdWidth\":1920,\"restoredOutcome\":"<<int(stopped.outcome)<<",\"actualWidth\":"<<w.owned().width<<",\"actualHeight\":"<<w.owned().height<<",\"actualLeft\":"<<w.owned().x<<",\"actualTop\":"<<w.owned().y<<",\"osDisplayChanges\":false}"<<std::endl;
  require(stopped.outcome==DisplayV2Outcome::restored&&!m.bytes,"Mismatched-source fixture did not finish restore");
  require(w.owned().width==1920&&w.owned().height==1080&&w.owned().x==1280&&w.owned().y==0,"Restore lost the original CCD mode or primary relationship when SDK size differed");
  require(w.console().active&&w.console().width==1280&&w.console().height==720&&w.console().x==0,"Mode correction changed the original foreign console");
  }
  {World w;Memory m;w.console().active=false;w.consoleReturns=false;w.owned().x=0;w.ownedWidth=1280;w.ownedHeight=720;
    DisplaySessionV2 session(access(w,m,false));
    require(session.start("sdk-ccd-failed-resize",1280,720,false,deadline(w)).ok,"Failed-resize fixture could not start");
    w.failResize=true;const auto failed=session.restore();
    require(failed.outcome==DisplayV2Outcome::failed&&m.bytes,"Wrong SDK size was reported restored after original CCD-size recovery failed");
    w.failResize=false;require(session.restore().outcome==DisplayV2Outcome::restored&&!m.bytes&&w.owned().width==1920&&w.owned().height==1080,"Retry could not restore the original CCD size");
  }
  return 3;
}

int primaryRelayoutCase(){
  World w;Memory m;DisplaySessionV2 session(access(w,m));
  const std::array<DisplayV2Output,1> group{{{"0",1280,720,0,0}}};
  require(session.startGroup("primary-relayout",group,true,deadline(w)).ok,"Primary-relayout fixture could not start");
  w.afterApply=[](World& value){value.owned().x=0;value.owned().y=0;value.console().x=LONG(value.owned().width);value.console().y=0;};
  const auto result=session.restore();
  const bool changed=w.owned().x==0&&w.owned().y==0;
  std::cout<<"{\"kind\":\"display-v2-primary-relayout\",\"savedOwnedPrimary\":false,\"actualOwnedPrimary\":"<<(changed?"true":"false")<<",\"restoreOutcome\":"<<int(result.outcome)<<",\"stateKept\":"<<(m.bytes?"true":"false")<<",\"osDisplayChanges\":false}"<<std::endl;
  require(result.outcome!=DisplayV2Outcome::restored||!changed,"A changed primary display was reported as exact restored configuration");
  require(result.outcome==DisplayV2Outcome::consoleOnly&&!m.bytes,"A failed owned-position verification changed the existing console fallback contract");
  {World another;Memory state;DisplaySessionV2 noProof(access(another,state,false));
    require(noProof.start("owned-position-no-proof",1280,720,false,deadline(another)).ok,"Owned-position fixture could not start");
    another.afterApply=[](World& value){value.owned().x=0;value.console().x=LONG(value.owned().width);};
    require(noProof.restore().outcome==DisplayV2Outcome::failed&&state.bytes,"Misplaced owned output deleted the recovery state without a verified console fallback");
    another.afterApply=nullptr;
    require(noProof.restore().outcome==DisplayV2Outcome::restored&&!state.bytes&&another.owned().x==1024,"A later recovery did not restore owned placement");
  }
  {World another(true);Memory state;DisplaySessionV2 movable(access(another,state));
    require(movable.start("foreign-placement-flexible",1280,720,true,deadline(another)).ok,"Foreign-placement fixture could not start");
    another.afterApply=[](World& value){value.extra().x+=500;value.extra().y+=90;};
    require(movable.restore().outcome==DisplayV2Outcome::restored&&!state.bytes&&another.owned().x==1024,"Owned-position verification accidentally pinned foreign geometry");
  }
  return 3;
}

int happyPathCases(){
  int passed=0;
  {World w;Memory m;DisplaySessionV2 session(access(w,m));
    const auto started=session.start("00000000-0000-0000-0000-000000000001",2560,1440,true,deadline(w));
    require(started.ok&&started.consoleRemoved&&m.bytes&&session.active(),"v2 start did not snapshot, resize and deactivate the identified console");
    requireStreaming(w,2560,1440,true,"v2 start left the owned output non-primary, wrong-sized or the console active");
    const auto saved=parseDisplayV2State(*m.bytes);require(saved.console&&saved.owned.size()==1&&saved.owned[0].width==1920&&displayV2ConsoleActive(saved.ccd,*saved.console),"Snapshot did not record the original console/owned configuration");++passed;
    // SDK replaces the only active output; Windows brings the console back.
    const auto resized=session.resize(1600,900,deadline(w));
    require(resized.ok&&resized.consoleReturned,"Resize did not re-deactivate the returning console");
    requireStreaming(w,1600,900,true,"Resize did not leave the owned output primary at the requested size");++passed;
    w.autoActivate=true;w.consoleReturns=false;
    require(session.resize(1280,720,deadline(w)).ok,"Resize after Windows auto-activated the new output failed");
    requireStreaming(w,1280,720,true,"Auto-activated output was not moved to primary at the requested size");++passed;
    const auto bytes=*m.bytes;require(m.saves==1&&bytes==*m.bytes,"Resize rewrote the original snapshot");
    w.autoActivate=false;w.consoleReturns=true;
    const auto stopped=session.restore();
    require(stopped.outcome==DisplayV2Outcome::restored&&!m.bytes&&!session.active(),"Stop did not restore and delete the state file");
    require(stopped.consoleExpected&&stopped.consoleActive,"Successful restore reported the pre-restore inactive console observation");++passed;
    requireOriginal(w,false,"Stop did not put the original console/owned configuration back");++passed;
    require(session.restore().outcome==DisplayV2Outcome::nothing,"Second restore was not a no-op");++passed;
  }
  {World w;Memory m;DisplaySessionV2 session(access(w,m));const auto resizes=w.resizes;
    require(session.start("s",1920,1080,true,deadline(w)).ok&&w.resizes==resizes,"Current-size start called the SDK");
    requireStreaming(w,1920,1080,true,"Current-size start did not make the owned output primary");
    require(session.restore().ok(),"Current-size stop failed");requireOriginal(w,false,"Current-size stop did not restore");++passed;
  }
  return passed;
}
int positionToleranceCases(){
  int passed=0;
  // No console proof: the console is an ordinary other display and must stay
  // active, but Windows may move it (and the extra display) on every resize.
  {World w(true);Memory m;DisplaySessionV2 session(access(w,m,false));
    require(session.start("s",2560,1440,false,deadline(w)).ok,"Start without console proof failed");
    requireStreaming(w,2560,1440,false,"Start without proof deactivated a display");
    require(w.extra().active&&w.extra().x==2560+1024&&w.console().x==2560,"Other displays were not kept contiguous to the right of the owned primary");++passed;
    w.afterResize=[](World& world){world.console().x=0;world.console().y=0;world.extra().x=1024;world.extra().y=0;};
    w.afterApply=[](World& world){world.console().y+=8;};
    const auto resized=session.resize(1600,900,deadline(w));
    require(resized.ok&&w.console().active&&w.console().width==1024&&w.extra().active&&w.extra().width==800&&w.extra().refreshN==75,"Windows re-positioning the console on resize failed the stream");
    require(w.owned().x==0&&w.owned().y==0&&w.owned().width==1600,"Owned output was not primary at the new size");++passed;
    w.afterResize=nullptr;w.afterApply=nullptr;
    require(session.restore().outcome==DisplayV2Outcome::restored,"Restore after re-positioned resize failed");requireOriginal(w,true,"Positions/modes were not restored");++passed;
  }
  // A field shape: initial 1580x952 -> 1264x760 with the console kept
  // (no proof). Windows' own re-layout is not modeled: on the SDK target
  // recreation it snaps the console to x=0 and parks the new owned output at an
  // arbitrary spot; after our apply it moves the console and the extra output
  // again (y changed, above/left of the origin). Identity/active/mode/refresh/
  // format/rotation are unchanged, so every case must succeed.
  {const std::array<std::pair<POINT,POINT>,4> moves{{{{1264,333},{-800,-600}},{{0,760},{1264+1024,0}},{{-1024,-768},{1264,100}},{{2000,-17},{-800,760}}}};
    for(const auto& move:moves){
      const POINT console=move.first,extra=move.second;
      World w(true);w.ownedWidth=1580;w.ownedHeight=952;w.owned().width=1580;w.owned().height=952;w.owned().dbWidth=1580;w.owned().dbHeight=952;w.extra().x=1024+1580;
      Memory m;DisplaySessionV2 session(access(w,m,false));
      w.autoActivate=true;
      w.afterResize=[](World& world){world.console().x=0;world.console().y=0;world.extra().x=1024;world.extra().y=0;auto& o=world.owned();o.x=1824;o.y=137;};
      w.afterApply=[console,extra](World& world){world.console().x=console.x;world.console().y=console.y;world.extra().x=extra.x;world.extra().y=extra.y;};
      const auto started=session.start("vm149-shape",1264,760,false,deadline(w));
      require(started.ok&&w.console().active&&w.console().x==console.x&&w.console().y==console.y&&w.extra().active&&w.extra().x==extra.x,"Initial shrink failed when Windows moved the console/extra output to an arbitrary position");
      requireStreaming(w,1264,760,false,"Initial shrink did not leave the owned output primary at the requested size");
      w.afterResize=[](World& world){world.console().x=-1024;world.console().y=300;};
      w.afterApply=[](World& world){world.console().x=LONG(world.ownedWidth)+77;world.console().y=450;world.extra().x=-800;world.extra().y=1080;};
      const auto resized=session.resize(1920,1080,deadline(w));
      require(resized.ok&&w.console().active&&w.console().width==1024&&w.console().height==768&&w.extra().width==800&&w.extra().refreshN==75,"Resize failed when Windows moved the console to an arbitrary position");
      w.afterResize=nullptr;w.afterApply=nullptr;w.autoActivate=false;
      require(session.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Restore after arbitrary re-layout failed");
      const auto& c=w.console();const auto& o=w.owned();
      require(c.active&&c.x==0&&c.y==0&&o.active&&o.x==1024&&o.width==1580&&o.height==952&&w.extra().active&&w.extra().x==1024+1580&&w.extra().width==800,"Restore did not put the original positions/modes back");++passed;
    }
  }
  // With console proof: Windows brings the console back at an arbitrary
  // position after the target recreation; v2 deactivates it again.
  {World w;Memory m;DisplaySessionV2 session(access(w,m));
    require(session.start("s",1580,952,true,deadline(w)).ok,"Proof start failed");
    w.consoleReturns=false;w.afterResize=[](World& world){auto& c=world.console();c.active=true;c.x=-1024;c.y=412;};
    const auto resized=session.resize(1264,760,deadline(w));
    require(resized.ok&&resized.consoleReturned&&!w.console().active,"Console returning at an arbitrary position was not deactivated again");
    requireStreaming(w,1264,760,true,"Owned output not primary after console return");
    w.afterResize=nullptr;w.consoleReturns=true;require(session.restore().ok(),"Restore failed");requireOriginal(w,false,"Original not restored");++passed;
  }
  // With console proof, the extra display moves but must keep its mode.
  {World w(true);Memory m;DisplaySessionV2 session(access(w,m));
    require(session.start("s",1600,900,true,deadline(w)).ok,"Start with extra display failed");
    require(!w.console().active&&w.extra().active&&w.extra().x==1600&&w.extra().refreshN==75,"Extra display was not kept active next to the owned primary");
    w.afterResize=[](World& world){world.extra().x=0;};
    require(session.resize(2560,1080,deadline(w)).ok&&w.extra().x==2560,"Moved extra display failed the resize");
    w.afterResize=nullptr;require(session.restore().ok(),"Restore failed");requireOriginal(w,true,"Extra display not restored");++passed;
  }
  return passed;
}
int foreignChangeCases(){
  int passed=0;
  const auto change=[](int scenario,World& world){auto& e=world.extra();
    if(scenario==0){e.width=1024;e.height=768;e.dbWidth=1024;e.dbHeight=768;}   // mode size
    if(scenario==1)e.active=false;                                            // active state
    if(scenario==2){e.refreshN=60;}                                           // refresh
    if(scenario==3)e.rotation=DISPLAYCONFIG_ROTATION_ROTATE90;                // rotation
    if(scenario==4)e.format=DISPLAYCONFIG_PIXELFORMAT_16BPP;                  // pixel format
  };
  constexpr std::array<uint32_t,5> codes{displayV2ForeignCode::size,displayV2ForeignCode::count,displayV2ForeignCode::refresh,displayV2ForeignCode::rotation,displayV2ForeignCode::format};
  for(int scenario=0;scenario<5;++scenario){
    // Windows changes another display while the SDK recreates the owned target:
    // the plan supplies that display's pre-change modes again and succeeds.
    {World w(true);Memory m;DisplaySessionV2 session(access(w,m));
      require(session.start("s",2560,1440,true,deadline(w)).ok,"Fixture start failed");
      w.afterResize=[&](World& world){change(scenario,world);};
      require(session.resize(1600,900,deadline(w)).ok,"A foreign change made by the SDK recreation was not put back");
      const auto& e=w.extra();require(e.active&&e.width==800&&e.height==600&&e.refreshN==75&&e.rotation==DISPLAYCONFIG_ROTATION_IDENTITY&&e.format==DISPLAYCONFIG_PIXELFORMAT_32BPP,"Other display not back at its pre-change mode");
      requireStreaming(w,1600,900,true,"Owned output not primary after the correction");
      w.afterResize=nullptr;require(session.restore().ok(),"Restore failed");requireOriginal(w,true,"Original not restored");++passed;}
    // Windows keeps overriding it while streaming: fail with the fixed code
    // for what differs, then restore.
    {World w(true);Memory m;DisplaySessionV2 session(access(w,m));
      require(session.start("s",2560,1440,true,deadline(w)).ok,"Fixture start failed");
      w.afterResize=[&](World& world){change(scenario,world);};
      w.afterApply=[&](World& world){if(world.ownedWidth!=1920)change(scenario,world);};
      const auto resized=session.resize(1600,900,deadline(w));
      require(!resized.ok&&resized.stage==displayV2Stage::foreign&&resized.foreignDifference==codes[size_t(scenario)]&&resized.restore&&resized.restore->outcome==DisplayV2Outcome::restored,"Persistent foreign display change did not fail with its code and restore");
      require(!m.bytes&&!session.active(),"Failed resize left the state file or an active session");
      requireOriginal(w,true,"Failed resize did not restore the original configuration");++passed;}
  }
  return passed;
}
// Field case (action 210 stage 8): the console was already
// inactive before the stream, so it could not be identified at start. When the
// SDK recreates the only active output, Windows activates the console. v2 puts
// it back to inactive: identified now with proof, or simply not re-supplied
// without proof because it was not active before the change.
int inactiveConsoleCases(){
  int passed=0;
  for(const bool proof:{true,false}){
    World w;w.console().active=false;w.owned().x=0;Memory m;DisplaySessionV2 session(access(w,m,proof));
    const auto started=session.start("vm149-inactive-console",1264,760,proof,deadline(w));
    require(started.ok&&started.foreignBefore==0&&started.consoleIdentified==proof&&!w.console().active,"Start failed when Windows re-activated an inactive console");
    requireStreaming(w,1264,760,true,"Start did not leave the owned output alone at the requested size");
    for(const auto size:{std::pair<DWORD,DWORD>{1580,952},{2388,1190}}){
      const auto resized=session.resize(size.first,size.second,deadline(w));
      require(resized.ok&&!w.console().active,"Resize failed when Windows re-activated an inactive console");
      requireStreaming(w,size.first,size.second,true,"Resize did not leave the owned output alone at the requested size");
    }
    const auto original=*m.bytes;const auto applies=w.applies,resizes=w.resizes;
    const auto restored=session.restore();
    if(proof){
      require(restored.outcome==DisplayV2Outcome::restored&&!m.bytes,"Verified inactive console restore failed");
      require(!w.console().active&&w.owned().active&&w.owned().x==0&&w.owned().width==1920&&w.owned().height==1080,"Restore did not return to the pre-stream (console inactive) configuration");
    }else{
      // An SDK resize activated a target absent from the original active
      // snapshot. Without positive proof, do not deactivate it or claim a
      // completed restore. The one preceding owned resize is not undone.
      require(restored.outcome==DisplayV2Outcome::failed&&restored.stage==displayV2Stage::foreign&&m.bytes==std::optional(original),"An unverified newly active target was accepted or its state was dropped");
      require(w.console().active&&w.applies==applies&&w.resizes==resizes+1,"An unverified newly active target was deactivated during restore");
    }
    ++passed;
  }
  return passed;
}
int crashRecoveryCases(){
  int passed=0;
  for(const bool autoActivate:{false,true}){
    World w;Memory m;
    {DisplaySessionV2 crashed(access(w,m));require(crashed.start("a",2560,1440,true,deadline(w)).ok,"Fixture start failed");}
    // Process gone without restore; the service restarts and re-creates the
    // owned output at its committed (streaming) size.
    w.autoActivate=autoActivate;w.serviceRestart();
    require(m.bytes.has_value(),"Crash fixture lost the state file");
    DisplaySessionV2 recovery(access(w,m,false));const auto restored=recovery.restore();
    require(restored.outcome==DisplayV2Outcome::restored&&!m.bytes,"Service-start recovery did not restore and delete the leftover state");
    requireOriginal(w,false,"Service-start recovery did not bring back the original configuration");
    w.autoActivate=false;
    DisplaySessionV2 next(access(w,m));require(next.start("b",1600,900,true,deadline(w)).ok,"Stream after recovery did not start");
    requireStreaming(w,1600,900,true,"Stream after recovery is not streaming");
    require(next.restore().ok(),"Stop after recovery failed");requireOriginal(w,false,"Stop after recovery did not restore");++passed;
  }
  return passed;
}
int restoreVerificationCases(){
  int passed=0;
  // An accepted CCD apply can leave every target active while the SDK refused
  // the original owned size. No console fallback: keep the state for a retry.
  {World w;Memory m;w.console().active=false;w.consoleReturns=false;w.owned().x=0;
    DisplaySessionV2 session(access(w,m));
    require(session.start("s",2560,1440,true,deadline(w)).ok,"Owned-size restore fixture start failed");
    const auto original=*m.bytes;w.failResize=true;
    const auto partial=session.restore();
    require(partial.outcome==DisplayV2Outcome::failed&&partial.stage==displayV2Stage::verify&&m.bytes==std::optional(original),"Active owned target at stream size was reported as fully restored or lost its saved state");
    require(w.owned().active&&w.owned().width==2560&&!w.console().active,"Refused SDK restore invented a mode or activated an originally inactive console");
    w.failResize=false;DisplaySessionV2 later(access(w,m,false));
    require(later.restore().outcome==DisplayV2Outcome::restored&&!m.bytes&&w.owned().width==1920&&w.owned().height==1080,"Later accepted SDK size restore did not complete normally");++passed;
  }
  // A safe console return is still allowed, but it is explicitly partial.
  {World w;Memory m;DisplaySessionV2 session(access(w,m));
    require(session.start("s",2560,1440,true,deadline(w)).ok,"Partial console restore fixture start failed");
    w.failResize=true;const auto partial=session.restore();
    require(partial.outcome==DisplayV2Outcome::consoleOnly&&partial.stage==displayV2Stage::verify&&partial.fallback==1&&partial.consoleActive&&!m.bytes,"Console-only fallback was mislabeled full restore or changed its state policy");
    require(w.owned().width==2560,"SDK refusal was not retained by the fixture");++passed;
  }
  // Modes, refresh, rotation and format are preserved; active flags alone are
  // insufficient when Windows accepts a plan but changes one of those fields.
  for(int scenario=0;scenario<4;++scenario){World w(true);Memory m;DisplaySessionV2 session(access(w,m));
    require(session.start("s",2560,1440,true,deadline(w)).ok,"Foreign-mode restore fixture start failed");
    w.afterApply=[scenario](World& value){auto& extra=value.extra();
      if(scenario==0)extra.width+=2;
      else if(scenario==1)extra.refreshN=60;
      else if(scenario==2)extra.rotation=DISPLAYCONFIG_ROTATION_ROTATE90;
      else extra.format=DISPLAYCONFIG_PIXELFORMAT_16BPP;
    };
    const auto partial=session.restore();
    require(partial.outcome==DisplayV2Outcome::consoleOnly&&partial.stage==displayV2Stage::verify&&partial.consoleActive,"Active foreign target with a changed mode was reported as fully restored");++passed;
  }
  // Foreign position changes remain permitted; this is no geometry latch.
  {World w(true);Memory m;DisplaySessionV2 session(access(w,m));
    require(session.start("s",2560,1440,true,deadline(w)).ok,"Foreign-position restore fixture start failed");
    w.afterApply=[](World& value){value.extra().x+=20000;};
    require(session.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Foreign repositioning was incorrectly required for restore acceptance");++passed;
  }
  // Refresh fractions are compared as rates, not raw numerator/denominator.
  {World w(true);Memory m;DisplaySessionV2 session(access(w,m));
    require(session.start("s",2560,1440,true,deadline(w)).ok,"Refresh-equivalence restore fixture start failed");
    w.afterApply=[](World& value){value.extra().refreshN=150;value.extra().refreshD=2;};
    require(session.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Equivalent foreign refresh rate was rejected");++passed;
  }
  return passed;
}
int restoreFailureCases(){
  int passed=0;
  // Exact restore impossible -> the identified console is re-activated.
  {World w;Memory m;DisplaySessionV2 session(access(w,m));
    require(session.start("s",2560,1440,true,deadline(w)).ok,"Fixture start failed");
    w.consoleReturns=false;w.autoActivate=true;
    w.reject=[](const NativeCcdData& plan,UINT32 flags)->LONG{
      if(flags&SDC_TOPOLOGY_SUPPLIED)return LONG(ERROR_SUCCESS);
      for(const auto& p:plan.paths)if(same(p.targetInfo.adapterId,ownedAdapter)&&nativeCcdSource(plan,p).position.x!=0)return LONG(ERROR_GEN_FAILURE);
      return LONG(ERROR_SUCCESS);};
    const auto restored=session.restore();
    require(restored.outcome==DisplayV2Outcome::consoleOnly&&!m.bytes,"Failed exact restore did not fall back to the console");
    require(restored.consoleExpected&&restored.consoleActive&&restored.fallback==1,"Console fallback diagnostics were wrong");
    require(w.console().active&&w.owned().active&&!overlapping(w),"Console fallback left the VM without its console");++passed;
  }
  // Even the supplied console path fails -> topology-only console activation.
  {World w;Memory m;DisplaySessionV2 session(access(w,m));
    require(session.start("s",2560,1440,true,deadline(w)).ok,"Fixture start failed");
    w.consoleReturns=false;w.autoActivate=true;
    w.reject=[](const NativeCcdData&,UINT32 flags)->LONG{return (flags&SDC_TOPOLOGY_SUPPLIED)?LONG(ERROR_SUCCESS):LONG(ERROR_GEN_FAILURE);};
    const auto restored=session.restore();
    require(restored.outcome==DisplayV2Outcome::consoleOnly&&w.topologyApplies==1&&w.console().active&&!m.bytes,"Topology-only console fallback was not used");++passed;
  }
  // Nothing works -> failed, state kept; the next attempt restores (no latch).
  {World w;Memory m;DisplaySessionV2 session(access(w,m));
    require(session.start("s",2560,1440,true,deadline(w)).ok,"Fixture start failed");
    w.consoleReturns=false;w.autoActivate=true;w.reject=[](const NativeCcdData&,UINT32){return LONG(ERROR_GEN_FAILURE);};
    const auto failed=session.restore();require(failed.outcome==DisplayV2Outcome::failed&&m.bytes,"Total restore failure deleted the state file");
    // The fixed diagnostics say what did not come back: the console was
    // expected, is still off and its fallback failed.
    require(failed.stage==displayV2Stage::apply&&failed.expected>0&&failed.consoleExpected&&!failed.consoleActive&&failed.fallback==2,"Restore failure diagnostics were wrong");
    w.reject=nullptr;DisplaySessionV2 later(access(w,m,false));
    require(later.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Later recovery attempt did not restore");requireOriginal(w,false,"Later recovery did not restore the original");++passed;
  }
  // A saved console with a new adapter LUID needs fresh positive identity.
  // One stale adapter and one unnamed adapter alone cannot prove that this
  // is the old QEMU console: leave Windows/SDK unchanged and keep the state.
  {World w;Memory m;DisplaySessionV2 session(access(w,m));
    require(session.start("s",2560,1440,true,deadline(w)).ok,"Fixture start failed");
    w.restartConsoleAdapter(LUID{77,0});
    const auto original=*m.bytes;const auto applies=w.applies,validations=w.validations,resizes=w.resizes,sizeCalls=w.sizeCalls;
    DisplaySessionV2 later(access(w,m,false));const auto refused=later.restore();
    require(refused.outcome==DisplayV2Outcome::failed&&refused.stage==displayV2Stage::foreign&&m.bytes==std::optional(original)&&refused.stale==1&&!refused.remapped,"A stale adapter was guessed or its unresolved state was dropped");
    require(w.applies==applies&&w.validations==validations&&w.resizes==resizes&&w.sizeCalls==sizeCalls&&!w.console().active&&w.owned().width==2560,"Unverified stale recovery changed Windows or SDK output size");++passed;
  }
  // Two adapters changed: ambiguous, nothing is guessed; the state stays.
  {World w(true);Memory m;DisplaySessionV2 session(access(w,m));
    require(session.start("s",2560,1440,true,deadline(w)).ok,"Fixture start failed");
    w.restartConsoleAdapter(LUID{77,0});for(auto& d:w.displays)if(same(d.adapter,extraAdapter))d.adapter=LUID{78,0};
    DisplaySessionV2 later(access(w,m,false));const auto failed=later.restore();
    require(failed.outcome==DisplayV2Outcome::failed&&m.bytes&&failed.stale==2&&!failed.remapped,"Ambiguous stale adapters were guessed or the state was dropped");++passed;
  }
  // Locked/ineligible session: restore is refused without changes, then works.
  {World w;Memory m;DisplaySessionV2 session(access(w,m));
    require(session.start("s",2560,1440,true,deadline(w)).ok,"Fixture start failed");
    w.eligible=false;const auto applies=w.applies;
    require(session.restore().outcome==DisplayV2Outcome::failed&&m.bytes&&w.applies==applies,"Ineligible restore changed Windows or dropped the state");
    w.eligible=true;DisplaySessionV2 later(access(w,m,false));require(later.restore().ok()&&!m.bytes,"Unlocked recovery did not restore");requireOriginal(w,false,"Unlocked recovery did not restore");++passed;
  }
  return passed;
}
int noLatchCases(){
  int passed=0;
  // SDK refusal: every attempt fails cleanly, nothing is left behind.
  {World w;Memory m;w.failResize=true;
    for(int attempt=0;attempt<3;++attempt){
      DisplaySessionV2 session(access(w,m));const auto result=session.start("s",2560,1440,true,deadline(w));
      require(!result.ok&&result.stage==displayV2Stage::sdk&&result.restore&&result.restore->ok()&&!m.bytes&&!session.active(),"SDK refusal left a state file or active session");
      requireOriginal(w,false,"SDK refusal changed the displays");
    }
    w.failResize=false;DisplaySessionV2 session(access(w,m));require(session.start("s",2560,1440,true,deadline(w)).ok,"Start after SDK refusals latched");
    require(session.restore().ok(),"Stop failed");++passed;
  }
  // Windows refuses the streaming layout: fail + restore, then succeed.
  {World w;Memory m;w.reject=[](const NativeCcdData& plan,UINT32 flags)->LONG{if(flags&SDC_TOPOLOGY_SUPPLIED)return LONG(ERROR_SUCCESS);for(const auto& p:plan.paths)if(same(p.targetInfo.adapterId,ownedAdapter)&&nativeCcdSource(plan,p).width==2560)return LONG(ERROR_GEN_FAILURE);return LONG(ERROR_SUCCESS);};
    for(int attempt=0;attempt<3;++attempt){
      DisplaySessionV2 session(access(w,m));const auto result=session.start("s",2560,1440,true,deadline(w));
      require(!result.ok&&result.stage==displayV2Stage::apply&&result.error==ERROR_GEN_FAILURE&&result.restore&&result.restore->outcome==DisplayV2Outcome::restored&&!m.bytes,"Refused streaming layout did not fail cleanly");
      requireOriginal(w,false,"Refused streaming layout was not restored");
    }
    w.reject=nullptr;DisplaySessionV2 session(access(w,m));require(session.start("s",2560,1440,true,deadline(w)).ok,"Start after refused layouts latched");require(session.restore().ok(),"Stop failed");++passed;
  }
  // A leftover state (failed restore) never blocks the next start; the older
  // snapshot stays the original and is what the next stop restores.
  {World w;Memory m;
    {DisplaySessionV2 first(access(w,m));require(first.start("a",2560,1440,true,deadline(w)).ok,"First start failed");
      w.reject=[](const NativeCcdData&,UINT32){return LONG(ERROR_GEN_FAILURE);};w.consoleReturns=false;
      require(first.restore().outcome==DisplayV2Outcome::failed&&m.bytes,"Fixture restore unexpectedly succeeded");}
    const auto original=*m.bytes;w.reject=nullptr;
    DisplaySessionV2 second(access(w,m));require(second.start("b",1280,720,true,deadline(w)).ok&&m.bytes==std::optional(original),"Leftover state blocked a start or was replaced by the streaming topology");
    requireStreaming(w,1280,720,true,"Second start did not stream");
    w.consoleReturns=true;require(second.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Second stop did not restore");requireOriginal(w,false,"Second stop did not restore the true original");++passed;
  }
  // A leftover state from an earlier boot (every adapter it names is gone:
  // LUIDs last one boot) can never be restored. The next start replaces it with
  // this boot's original, which its stop restores. Were it kept, the stop
  // would fail, recovery would discard it and the console would stay off.
  {World w;Memory m;
    m.bytes=displayV2StateBytes({"before-restart",displayV2ActiveCcd(w.query()),w.targets(),DisplayV2Console{consoleAdapter,consoleAdapter,0,consoleTarget}});
    w.restartConsoleAdapter(LUID{77,0});
    auto a=access(w,m);a.hidden=[](LUID){return true;};DisplaySessionV2 session(a);
    require(session.start("s",2560,1440,true,deadline(w)).ok&&!w.console().active,"Start after a restart failed or kept the console on");
    const auto saved=parseDisplayV2State(*m.bytes);
    require(saved.session=="s"&&displayV2TargetActive(saved.ccd,LUID{77,0},consoleTarget),"A state from an earlier boot was kept instead of this boot's original");
    require(session.restore().outcome==DisplayV2Outcome::restored&&!m.bytes&&w.console().active&&!w.console().x,"Stop did not bring this boot's console back");++passed;
  }
  // One named adapter gone, another present: not an earlier boot; the state stays.
  {World w(true);Memory m;
    m.bytes=displayV2StateBytes({"same-boot",displayV2ActiveCcd(w.query()),w.targets(),DisplayV2Console{consoleAdapter,consoleAdapter,0,consoleTarget}});const auto original=*m.bytes;
    require(std::erase_if(w.displays,[](const Display& d){return same(d.adapter,extraAdapter);})==1,"Fixture lost the extra display");
    DisplaySessionV2 session(access(w,m));
    require(session.start("s",2560,1440,true,deadline(w)).ok&&m.bytes==std::optional(original),"A partly unresolved state was replaced at start");++passed;
  }
  // Unreadable/corrupt state never blocks a start.
  for(const bool unreadable:{true,false}){
    World w;Memory m;if(unreadable)m.throwLoad=true;else m.bytes=std::string("{\"schema\":\"something else\"}");
    DisplaySessionV2 session(access(w,m));require(session.start("s",2560,1440,true,deadline(w)).ok&&m.discards==1&&m.bytes,"Corrupt/unreadable state blocked a start");
    require(parseDisplayV2State(*m.bytes).owned[0].width==1920,"Fresh snapshot was not taken after discarding corrupt state");
    require(session.restore().ok(),"Stop failed");requireOriginal(w,false,"Stop after corrupt state did not restore");++passed;
  }
  // Ineligible (locked) session: start refused without any change.
  {World w;Memory m;w.eligible=false;DisplaySessionV2 session(access(w,m));const auto result=session.start("s",2560,1440,true,deadline(w));
    require(!result.ok&&result.stage==displayV2Stage::ineligible&&!m.bytes&&w.applies==0&&w.resizes==0,"Ineligible start changed something");w.eligible=true;
    require(session.start("s",2560,1440,true,deadline(w)).ok,"Start after ineligible attempt latched");require(session.restore().ok(),"Stop failed");++passed;}
  // State that cannot be saved: start refused without any change.
  {World w;Memory m;m.failSave=true;DisplaySessionV2 session(access(w,m));const auto result=session.start("s",2560,1440,true,deadline(w));
    require(!result.ok&&result.stage==displayV2Stage::state&&w.applies==0&&w.resizes==0,"Start without a restore point changed displays");m.failSave=false;
    require(session.start("s",2560,1440,true,deadline(w)).ok,"Start after a save failure latched");require(session.restore().ok(),"Stop failed");++passed;}
  return passed;
}
int codecCases(){
  int passed=0;World w(true);
  const auto console=DisplayV2Console{consoleAdapter,consoleAdapter,0,consoleTarget};
  for(const bool withConsole:{true,false}){
    DisplayV2State state{"00000000-0000-0000-0000-000000000001",displayV2ActiveCcd(w.query()),{w.target()},withConsole?std::optional(console):std::nullopt};
    const auto bytes=displayV2StateBytes(state);const auto parsed=parseDisplayV2State(bytes);
    require(displayV2StateBytes(parsed)==bytes&&parsed.ccd.paths.size()==3&&parsed.console.has_value()==withConsole&&std::memcmp(parsed.ccd.paths.data(),state.ccd.paths.data(),state.ccd.paths.size()*sizeof(DISPLAYCONFIG_PATH_INFO))==0,"State codec did not round-trip exactly");++passed;
  }
  DisplayV2State state{"s",displayV2ActiveCcd(w.query()),{w.target()},console};const auto bytes=displayV2StateBytes(state);
  for(int scenario=0;scenario<8;++scenario){
    auto value=JsonParser::parse(bytes);auto& object=std::get<Json::Object>(value.value);
    switch(scenario){
      case 0:object["schema"].value=std::string("multitap-display-v1");break;
      case 1:object["version"].value=2.0;break;
      case 2:object["paths"].value=object["paths"].string().substr(1);break;
      case 3:object["paths"].value=std::string("zz")+object["paths"].string().substr(2);break;
      case 4:object["session"].value=std::string("bad session");break;
      case 5:std::get<Json::Object>(std::get<Json::Array>(object["owned"].value)[0].value)["id"].value=std::string("2");break;
      case 6:object["console"].value=true;break;
      case 7:object["paths"].value=std::string();break;
    }
    bool rejected=false;try{(void)parseDisplayV2State(jsonObject(value));}catch(...){rejected=true;}require(rejected,"Malformed v2 state was accepted");++passed;
  }
  {bool rejected=false;try{(void)parseDisplayV2State(bytes+" ");}catch(...){rejected=true;}require(rejected,"Non-canonical v2 state was accepted");++passed;}
  // An inactive path inside the saved topology is rejected.
  {auto bad=state;bad.ccd.paths[0].flags&=~DISPLAYCONFIG_PATH_ACTIVE;bool rejected=false;try{(void)displayV2StateBytes(bad);}catch(...){rejected=true;}require(rejected,"Inactive saved path was encoded");++passed;}
  return passed;
}
World dualWorld(bool extra,bool inactive);
int planCases(){
  int passed=0;
  // Stacked foreign displays keep their vertical arrangement; gaps close.
  {World w(true);w.extra().x=0;w.extra().y=768;   // extra below the console
    const auto plan=planDisplayV2Streaming(w.query(),w.query(),w.target(),std::vector<NativeDisplayTarget>{w.target()},std::nullopt);
    LONG consoleX=-1,consoleY=-1,extraX=-1,extraY=-1,ownedX=-1;
    for(const auto& p:plan.paths){const auto& s=nativeCcdSource(plan,p);if(same(p.targetInfo.adapterId,consoleAdapter)){consoleX=s.position.x;consoleY=s.position.y;}if(same(p.targetInfo.adapterId,extraAdapter)){extraX=s.position.x;extraY=s.position.y;}if(same(p.targetInfo.adapterId,ownedAdapter))ownedX=s.position.x;}
    require(ownedX==0&&consoleX==1920&&consoleY==0&&extraX==1920&&extraY==768,"Streaming plan broke the stacked layout of other displays");++passed;
  }
  {World w;bool removed=false;const auto console=DisplayV2Console{consoleAdapter,consoleAdapter,0,consoleTarget};
    const auto plan=planDisplayV2Streaming(w.query(),w.query(),w.target(),std::vector<NativeDisplayTarget>{w.target()},console,&removed);
    require(removed&&plan.paths.size()==1&&same(plan.paths[0].targetInfo.adapterId,ownedAdapter),"Streaming plan kept the identified console");++passed;
  }
  // An owned output added after the snapshot (count change) is kept active.
  {World w;const DisplayV2State saved{"s",displayV2ActiveCcd(w.query()),{w.target()},std::nullopt};
    w.displays.push_back({ownedAdapter,1,50,true,false,0,0,1280,720,60,1,DISPLAYCONFIG_ROTATION_IDENTITY,1280,720});
    const std::vector<NativeDisplayTarget> current{w.target(),{"1",ownedAdapter,50,1280,720}};std::vector<DisplayV2Expected> expected;
    const auto plan=planDisplayV2Restore(saved,w.query(),current,&expected);bool second=false;
    for(const auto& p:plan.paths)if(same(p.targetInfo.adapterId,ownedAdapter)&&p.targetInfo.id==50){const auto& s=nativeCcdSource(plan,p);second=s.position.x==1024+1920&&s.width==1280;}
    require(second&&expected.size()==3,"Restore dropped an owned output added after the snapshot");++passed;
  }
  // A retired Multitap legacy output is neither saved, restored nor "foreign".
  {const LUID legacy{61,0};const DisplayV2Retired retired=[legacy](LUID value){return same(value,legacy);};
    World w;w.displays.push_back({legacy,2,5,true,true,3000,0,1920,1080,60,1,DISPLAYCONFIG_ROTATION_IDENTITY,1920,1080});
    const auto withLegacy=displayV2ActiveCcd(w.query());
    require(withLegacy.paths.size()==3,"Fixture did not save the legacy output");
    const auto without=displayV2ActiveCcd(w.query(),retired);bool kept=false;
    for(const auto& p:without.paths)if(same(p.targetInfo.adapterId,legacy))kept=true;
    require(!kept&&without.paths.size()==2,"Snapshot saved a retired legacy output");++passed;
    // An older state that still names the legacy output, whose device is now gone.
    const DisplayV2State saved{"s",withLegacy,{w.target()},std::nullopt};
    w.displays.pop_back();
    bool refused=false;try{planDisplayV2Restore(saved,w.query(),std::vector<NativeDisplayTarget>{w.target()});}catch(const DisplayV2RestoreIdentityError&){refused=true;}
    require(refused,"Fixture: a vanished legacy adapter no longer blocks the restore");
    require(preflightDisplayV2Restore(saved,w.query(),std::vector<NativeDisplayTarget>{w.target()},retired).resolved,"A vanished retired adapter blocked the restore");++passed;
    std::vector<DisplayV2Expected> expected;
    const auto plan=planDisplayV2Restore(saved,w.query(),std::vector<NativeDisplayTarget>{w.target()},&expected,retired);bool revived=false;
    for(const auto& p:plan.paths)if(same(p.targetInfo.adapterId,legacy))revived=true;
    require(!revived&&plan.paths.size()==2&&expected.size()==2,"Restore planned a retired legacy output");++passed;
    // Still present and active (not yet disabled): restore does not keep it.
    w.displays.push_back({legacy,2,5,true,true,3000,0,1920,1080,60,1,DISPLAYCONFIG_ROTATION_IDENTITY,1920,1080});
    const auto live=planDisplayV2Restore(saved,w.query(),std::vector<NativeDisplayTarget>{w.target()},nullptr,retired);bool liveKept=false;
    for(const auto& p:live.paths)if(same(p.targetInfo.adapterId,legacy))liveKept=true;
    require(!liveKept,"Restore kept a live retired legacy output");++passed;
  }
  // Positive identification of the retired legacy adapter (never by name alone).
  {const auto device=[](std::wstring instance,std::wstring enumerator=L"ROOT",std::vector<std::wstring> hardware={L"ROOT\\MTTVDD"},bool present=true,GUID klass=nativeConsoleDisplayClass){
      NativeConsoleAdapterIdentity value;value.adapter={61,0};value.instanceId=std::move(instance);value.enumerator=std::move(enumerator);value.hardwareIds=std::move(hardware);value.classGuid=klass;value.present=present;value.started=true;return value;};
    const std::wstring owner=L"ROOT\\MULTITAPVIRTUALDISPLAY\\0000",ours=owner;
    require(legacyDisplayDevice(device(ours),owner),"The installed legacy device was not identified");++passed;
    require(legacyDisplayDevice(device(L"root\\multitapvirtualdisplay\\0000",L"root",{L"Root\\MttVDD"}),owner),"Identification must ignore letter case (Windows reports mixed case)");++passed;
    require(!legacyDisplayDevice(device(ours),L""),"Legacy accepted without the installer's owner marker");++passed;
    require(!legacyDisplayDevice(device(ours),L"ROOT\\MULTITAPVIRTUALDISPLAY\\0001"),"Legacy accepted with another device's owner marker");++passed;
    // Another product's MttVDD install: right vendor ID, not our instance.
    require(!legacyDisplayDevice(device(L"ROOT\\DISPLAY\\0001"),L"ROOT\\DISPLAY\\0001"),"Another product's MttVDD device was retired");++passed;
    require(!legacyDisplayDevice(device(L"ROOT\\MTTVDD\\0000"),L"ROOT\\MTTVDD\\0000"),"A vendor-named MttVDD instance was retired");++passed;
    // The current native output and the QEMU console are never legacy.
    require(!legacyDisplayDevice(device(L"ROOT\\DISPLAY\\0000",L"ROOT",{L"ROOT\\MULTITAPVIRTUALDISPLAY2"}),L"ROOT\\DISPLAY\\0000"),"The current native device was retired");++passed;
    require(!legacyDisplayDevice(device(L"PCI\\VEN_1234&DEV_1111&SUBSYS_11001AF4&REV_02\\3&11583659&0&08",L"PCI",{L"PCI\\VEN_1234&DEV_1111"}),owner),"The QEMU console was retired");++passed;
    require(!legacyDisplayDevice(device(L"ROOT\\MULTITAPVIRTUALDISPLAY\\00000"),L"ROOT\\MULTITAPVIRTUALDISPLAY\\00000")&&!legacyDisplayDevice(device(L"ROOT\\MULTITAPVIRTUALDISPLAY\\0000X"),L"ROOT\\MULTITAPVIRTUALDISPLAY\\0000X"),"A malformed instance was retired");++passed;
    require(!legacyDisplayDevice(device(ours,L"ROOT",{L"ROOT\\OTHER"}),owner),"A device without the vendor hardware ID was retired");++passed;
    require(!legacyDisplayDevice(device(ours,L"PCI"),owner),"A device from another enumerator was retired");++passed;
    require(!legacyDisplayDevice(device(ours,L"ROOT",{L"ROOT\\MTTVDD"},false),owner)&&!legacyDisplayDevice(device(ours,L"ROOT",{L"ROOT\\MTTVDD"},true,GUID{}),owner),"An absent or non-display device was retired");++passed;
    // The resolver adapts it to the session hook.
    const auto resolve=[&](LUID adapter)->std::optional<NativeConsoleAdapterIdentity>{if(same(adapter,LUID{61,0}))return device(ours);if(same(adapter,LUID{8,0}))throw std::runtime_error("unreadable");return {};};
    const auto retired=legacyRetiredResolver(resolve,[&]{return owner;});
    require(retired(LUID{61,0})&&!retired(LUID{62,0})&&!retired(LUID{8,0}),"Resolver retired the wrong adapter");++passed;
    require(!legacyRetiredResolver(resolve,[]{return std::wstring();})(LUID{61,0})&&!legacyRetiredResolver({},[&]{return owner;})(LUID{61,0}),"Resolver retired without proof");++passed;
    // On a Host still using the legacy provider this device is the live output.
    require(legacyRetirementPermitted(true,true,false),"Retirement was refused when every gate held");++passed;
    for(const auto& gate:std::initializer_list<std::array<bool,3>>{{false,true,false},{true,false,false},{true,true,true},{false,false,true}})
      require(!legacyRetirementPermitted(gate[0],gate[1],gate[2]),"Retirement was permitted past a closed gate");
    ++passed;
    const auto parse=[](const char* text){return JsonParser::parse(text);};
    require(nativeProviderConfirmed(parse(R"({"enabled":true,"provider":"libvirtualdisplay-1.6.4"})"),true),"The native provider was not confirmed");++passed;
    require(!nativeProviderConfirmed(parse(R"({"enabled":true,"provider":"libvirtualdisplay-1.6.4"})"),false),"Confirmed without a marker file");++passed;
    require(!nativeProviderConfirmed(parse(R"({"enabled":false,"provider":"libvirtualdisplay-1.6.4"})"),true),"Confirmed while disabled");++passed;
    require(!nativeProviderConfirmed(parse(R"({"provider":"libvirtualdisplay-1.6.4"})"),true)&&!nativeProviderConfirmed(parse(R"({"enabled":"true","provider":"libvirtualdisplay-1.6.4"})"),true),"An uncertain marker confirmed the provider");++passed;
    require(!nativeProviderConfirmed(parse(R"({"enabled":true})"),true)&&!nativeProviderConfirmed(parse(R"({"enabled":true,"provider":"legacy-mtt"})"),true)&&!nativeProviderConfirmed(parse(R"({"enabled":true,"provider":"future"})"),true)&&!nativeProviderConfirmed(Json{},true),"An unknown provider was confirmed");++passed;
    // The service's own view: the gate above must be able to open in session 0.
    const auto lease=parse(R"({"leaseActive":true,"activationVerified":true,"configurationPending":false,"recoveryRequired":false})");
    require(nativeLeaseHealthy(lease,true),"A healthy lease was refused");++passed;
    require(!nativeLeaseHealthy(lease,false),"A device that is not started counted");++passed;
    for(const char* closed:{R"({"activationVerified":true})",R"({"leaseActive":true})",R"({"leaseActive":true,"activationVerified":true,"configurationPending":true})",R"({"leaseActive":true,"activationVerified":true,"recoveryRequired":true})"})
      require(!nativeLeaseHealthy(parse(closed),true),"An unhealthy lease counted");
    ++passed;
  }
  // The session passes the hook to its snapshot, restore and console fallback.
  {const LUID legacy{61,0};const Display old{legacy,2,5,true,true,2944,0,1920,1080,60,1,DISPLAYCONFIG_ROTATION_IDENTITY,1920,1080};
    const auto hooked=[legacy](World& w,Memory& m){auto a=access(w,m);a.retired=[legacy](LUID value){return same(value,legacy);};return a;};
    // By adapter: the SDK re-creates owned targets at the end of the list.
    const auto gone=[legacy](World& w){require(std::erase_if(w.displays,[&](const Display& d){return same(d.adapter,legacy);})==1,"Fixture lost the legacy display");};
    {World w;w.displays.push_back(old);Memory m;DisplaySessionV2 session(hooked(w,m));
      require(session.start("s",2560,1440,true,deadline(w)).ok,"Legacy session fixture start failed");
      bool saved=false;for(const auto& p:parseDisplayV2State(*m.bytes).ccd.paths)if(same(p.targetInfo.adapterId,legacy))saved=true;
      require(!saved,"Session snapshot saved a retired legacy output");++passed;
      gone(w);
      require(session.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"A vanished retired adapter blocked the session restore");++passed;
    }
    // A state saved before this rule still names the adapter.
    {World w;w.displays.push_back(old);Memory m;DisplaySessionV2 first(access(w,m));
      require(first.start("s",2560,1440,true,deadline(w)).ok,"Older legacy state fixture start failed");
      gone(w);
      DisplaySessionV2 later(hooked(w,m));
      require(later.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"A vanished retired adapter blocked restoring an older state");++passed;
    }
    {World w;w.displays.push_back(old);Memory m;DisplaySessionV2 first(access(w,m));
      require(first.start("s",2560,1440,true,deadline(w)).ok,"Fallback legacy fixture start failed");
      gone(w);w.failResize=true;
      DisplaySessionV2 later(hooked(w,m));const auto partial=later.restore();
      require(partial.outcome==DisplayV2Outcome::consoleOnly&&partial.fallback==1&&!m.bytes,"A retired adapter blocked the console fallback");++passed;
    }
  }
  // A stream shows its virtual outputs and nothing else; the rest comes back after it.
  {const auto onlyOurs=[](World& w,Memory& m){auto a=access(w,m);a.hidden=[](LUID){return true;};return a;};
    const auto lit=[](const World& w,bool ours){return size_t(std::count_if(w.displays.begin(),w.displays.end(),[&](const Display& d){return d.active&&same(d.adapter,ownedAdapter)==ours;}));};
    {World w(true);Memory m;DisplaySessionV2 session(onlyOurs(w,m));const auto others=lit(w,false);
      require(others==2,"Fixture lost its other displays");
      require(session.start("s",2560,1440,true,deadline(w)).ok&&!lit(w,false)&&lit(w,true)==1,"One client monitor did not mean one display");++passed;
      require(session.resize(1920,1080,deadline(w)).ok&&!lit(w,false),"A resize brought another display back");++passed;
      require(session.restore().outcome==DisplayV2Outcome::restored&&lit(w,false)==others&&!m.bytes,"The other displays did not come back after the stream");++passed;
    }
    {World w=dualWorld(true,false);Memory m;DisplaySessionV2 session(onlyOurs(w,m));const auto others=lit(w,false);
      const std::vector<DisplayV2Output> outputs{{"0",1920,1080,0,0},{"1",1920,1080,1920,0}};
      require(session.startGroup("s",outputs,true,deadline(w)).ok&&!lit(w,false)&&lit(w,true)==2,"Two client monitors did not mean two displays");++passed;
      require(session.restore().outcome==DisplayV2Outcome::restored&&lit(w,false)==others,"The other displays did not come back after two outputs");++passed;
    }
    {World w=dualWorld(true,false);Memory m;DisplaySessionV2 session(onlyOurs(w,m));
      require(session.start("s",1920,1080,true,deadline(w)).ok&&!lit(w,false)&&lit(w,true)==1,"A second virtual output stayed on for one client monitor");++passed;
    }
    // No console proof (the broker's answer when the VM's Proxmox display is not
    // standard VGA, or Proxmox could not be read): nothing is identified as
    // the console, and still every other display, the console included, is
    // off for the stream and back as it was after it.
    {const auto unproved=[](World& w,Memory& m){auto a=access(w,m,false);a.hidden=[](LUID){return true;};return a;};
      {World w(true);Memory m;DisplaySessionV2 session(unproved(w,m));const auto others=lit(w,false);
        const auto started=session.start("s",2560,1440,false,deadline(w));
        require(started.ok&&!started.consoleProof&&!started.consoleIdentified&&!lit(w,false)&&lit(w,true)==1,"A stream without a console proof kept another display on");++passed;
        require(session.resize(1920,1080,deadline(w)).ok&&!lit(w,false),"A resize without a console proof brought another display back");++passed;
        require(session.restore().outcome==DisplayV2Outcome::restored&&lit(w,false)==others&&!m.bytes,"The other displays did not come back after a stream without a console proof");
        requireOriginal(w,true,"A stream without a console proof did not restore the original layout");++passed;
      }
      // The owned output already primary at the requested size: only the
      // hidden displays still on make the start apply its plan.
      {World w(true);w.owned().x=0;w.console().x=1920;w.extra().x=2944;Memory m;DisplaySessionV2 session(unproved(w,m));const auto others=lit(w,false);
        require(others==2,"Fixture lost its other displays");
        const auto started=session.start("s",1920,1080,false,deadline(w));
        require(started.ok&&!lit(w,false)&&lit(w,true)==1,"A ready owned output without a console proof kept the console on");++passed;
        require(session.restore().outcome==DisplayV2Outcome::restored&&lit(w,false)==others&&!m.bytes,"The other displays did not come back after a ready owned output");++passed;
      }
      {World w=dualWorld(true,false);Memory m;DisplaySessionV2 session(unproved(w,m));const auto others=lit(w,false);
        const std::vector<DisplayV2Output> outputs{{"0",1920,1080,0,0},{"1",1920,1080,1920,0}};
        const auto started=session.startGroup("s",outputs,false,deadline(w));
        require(started.ok&&!started.consoleProof&&!lit(w,false)&&lit(w,true)==2,"Two outputs without a console proof kept another display on");++passed;
        require(session.restore().outcome==DisplayV2Outcome::restored&&lit(w,false)==others&&!m.bytes,"The other displays did not come back after two outputs without a console proof");++passed;
      }
    }
    // Without the hook other displays stay, which the plans above rely on.
    {World w(true);Memory m;DisplaySessionV2 session(access(w,m));
      require(session.start("s",2560,1440,true,deadline(w)).ok&&lit(w,false)>0,"Another display went off without the rule");++passed;
    }
  }
  // A saved adapter that stayed gone: only the caller's give-up discards the state.
  for(const int giveUp:{1,0}){
    World w(true);Memory m;DisplaySessionV2 first(access(w,m));
    require(first.start("s",2560,1440,true,deadline(w)).ok,"Stale state fixture start failed");
    require(std::erase_if(w.displays,[](const Display& d){return same(d.adapter,extraAdapter);})==1,"Fixture lost the extra display");
    auto a=access(w,m);a.discardStale=[giveUp]{return giveUp==1;};DisplaySessionV2 later(a);const auto result=later.restore();
    if(giveUp)require(result.outcome==DisplayV2Outcome::nothing&&result.stale==1&&!m.bytes&&m.discards==1,"A stale state was not discarded after giving up");
    else require(result.outcome==DisplayV2Outcome::failed&&result.stale==1&&m.bytes&&m.discards==0,"A stale state was discarded before giving up");
    ++passed;
  }
  {World w(true);Memory m;DisplaySessionV2 first(access(w,m));
    require(first.start("s",2560,1440,true,deadline(w)).ok,"Stale state fixture start failed");
    auto a=access(w,m);a.discardStale=[]{return true;};DisplaySessionV2 later(a);
    require(later.restore().outcome==DisplayV2Outcome::restored&&m.discards==0,"A resolvable state was not restored");++passed;}
  // Unresolved with every adapter still present: it may resolve later, so it stays.
  {World w(true);Memory m;DisplaySessionV2 first(access(w,m));
    require(first.start("s",2560,1440,true,deadline(w)).ok,"Stale state fixture start failed");
    w.displays.push_back({extraAdapter,3,77,true,true,4000,0,800,600,60,1,DISPLAYCONFIG_ROTATION_IDENTITY,800,600});
    auto a=access(w,m);a.discardStale=[]{return true;};DisplaySessionV2 later(a);const auto result=later.restore();
    require(result.outcome==DisplayV2Outcome::failed&&result.stale==0&&m.bytes&&m.discards==0,"A state with its adapters present was discarded");++passed;}
  return passed;
}
int rpcCases(){
  int passed=0;
  const std::string snapshot=R"({"schema":1,"provider":"libvirtualdisplay-1.6.4","leaseActive":true,"targets":[{"id":"0","adapterLow":44,"adapterHigh":0,"target":2,"width":1600,"height":900}]})";
  const DisplayV2SizeRequest request{std::string(32,'a'),"0",1600,900};
  {const auto parsed=parseDisplayV2SizeRequest(JsonParser::parse(displayV2SizeRequestBytes(request)));require(parsed&&parsed->nonce==request.nonce&&parsed->width==1600&&parsed->height==900,"Size request did not round-trip");++passed;}
  for(const auto* bad:{R"({"type":"worker.v2.display.size","nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","monitor":"0","width":1601,"height":900})",R"({"type":"worker.v2.display.size","nonce":"short","monitor":"0","width":1600,"height":900})",R"({"type":"worker.v2.display.size","nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","monitor":"2","width":1600,"height":900})",R"({"type":"worker.v2.display.size","nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","monitor":"0","width":1600,"height":900,"extra":1})"}){
    require(!parseDisplayV2SizeRequest(JsonParser::parse(bad)),"Malformed size request accepted");++passed;
  }
  int calls=0;const auto resize=[&](const std::string& monitor,DWORD width,DWORD height)->std::optional<std::string>{++calls;require(monitor=="0"&&width==1600&&height==900,"Dispatch altered the request");return snapshot;};
  {const auto ignored=dispatchDisplayV2(JsonParser::parse(R"({"type":"worker.native.resize.prepare"})"),true,resize);require(!ignored.handled&&calls==0,"Foreign packet was handled as v2");++passed;}
  {const auto denied=dispatchDisplayV2(JsonParser::parse(displayV2SizeRequestBytes(request)),false,resize);require(denied.handled&&calls==0&&!JsonParser::parse(denied.response).get("ok").boolean(),"Unauthorized size request reached the SDK");++passed;}
  // Worker side through an in-memory pipe, with an interleaved clipboard packet.
  std::vector<Json> inbound;int interleaved=0;
  const auto write=[&](const std::string& message){const auto reply=dispatchDisplayV2(JsonParser::parse(message),true,resize);inbound.push_back(JsonParser::parse(R"({"type":"clip.packet"})"));inbound.push_back(JsonParser::parse(reply.response));return true;};
  const auto read=[&](DWORD)->std::optional<Json>{if(inbound.empty())return {};auto value=inbound.front();inbound.erase(inbound.begin());return value;};
  const auto interleave=[&](const Json& value){if(value.get("type").string()=="clip.packet"){++interleaved;return true;}return false;};
  {const auto targets=displayV2SizeRpc(request,write,read,interleave,5000);require(targets&&targets->size()==1&&targets->front().width==1600&&targets->front().target==2&&interleaved==1,"Worker size RPC did not return the new owned target");++passed;}
  {auto other=request;other.width=1280;other.height=720;const auto targets=displayV2SizeRpc(other,[&](const std::string&){inbound.push_back(JsonParser::parse(displayV2SizeReplyBytes(other.nonce,snapshot)));return true;},read,interleave,5000);require(!targets,"Snapshot with a different size was accepted");++passed;}
  {const auto targets=displayV2SizeRpc(request,[&](const std::string&){inbound.push_back(JsonParser::parse(displayV2SizeReplyBytes(std::string(32,'b'),snapshot)));return true;},read,interleave,5000);require(!targets,"Reply for another request was accepted");++passed;}
  {const auto targets=displayV2SizeRpc(request,[&](const std::string&){inbound.push_back(JsonParser::parse(displayV2SizeReplyBytes(request.nonce,std::nullopt)));return true;},read,interleave,5000);require(!targets,"Refused SDK change was accepted");++passed;}
  {inbound.clear();const auto targets=displayV2SizeRpc(request,[](const std::string&){return true;},read,interleave,5000);require(!targets,"Missing reply was accepted");++passed;}
  return passed;
}
// ------------------------------------------------ independent outputs (group)
// Dual output at rest: the user's count 2 (DisplaySessionV2::count) from the
// default world: owned "0" at the origin, "1" right of it, the console (when
// active) and the extra display right of both with their modes.
World dualWorld(bool extra=false,bool inactive=false){
  World w(extra);if(inactive){w.console().active=false;w.owned().x=0;if(extra)w.extra().x=1920;}
  Memory m;{DisplaySessionV2 count(worldAccess(w,m));require(count.count("count",2,true,deadline(w)).ok&&!m.bytes,"Fixture count 2 failed");}
  return w;
}
struct Placed {DWORD width=0,height=0;LONG x=0,y=0;};
void requireGroup(World& w,Placed zero,Placed one,bool consoleOff,const char* reason){
  const auto& o=w.owned();require(o.active&&o.x==0&&o.y==0&&o.width==zero.width&&o.height==zero.height&&w.ownedWidth==zero.width&&w.ownedHeight==zero.height,reason);
  const auto& s=w.second();require(w.hasSecond()&&s.active&&s.x==one.x&&s.y==one.y&&s.width==one.width&&s.height==one.height&&w.secondWidth==one.width&&w.secondHeight==one.height,reason);
  require(w.console().active==!consoleOff&&!overlapping(w),reason);
}
bool extraOriginal(World& w){const auto& e=w.extra();return e.active&&e.width==800&&e.height==600&&e.refreshN==75&&e.refreshD==1&&e.rotation==DISPLAYCONFIG_ROTATION_IDENTITY&&e.format==DISPLAYCONFIG_PIXELFORMAT_32BPP;}
// The post-count configuration every group stop must put back.
void requireDual(World& w,bool consoleActive,bool extra,const char* reason){
  requireGroup(w,{1920,1080,0,0},{1920,1080,1920,0},!consoleActive,reason);
  if(consoleActive){const auto& c=w.console();require(c.x==3840&&c.y==0&&c.width==1024&&c.height==768,reason);}
  if(extra)require(extraOriginal(w)&&w.extra().x==(consoleActive?3840+1024:3840),reason);
}
const std::vector<DisplayV2Output> rightGroup{{"0",1600,900,0,0},{"1",1280,720,1600,0}};
int groupCases(){
  int passed=0;
  // Console active at rest (with proof): one SDK call sizes both outputs, "0"
  // primary, "1" right, console off, the extra display right of the group.
  {World w=dualWorld(true);Memory m;DisplaySessionV2 session(access(w,m));const auto calls=w.sizeCalls,resizes=w.resizes;
    const auto started=session.startGroup("g",rightGroup,true,deadline(w));
    require(started.ok&&started.consoleRemoved&&started.consoleIdentified&&started.foreignBefore==1&&m.bytes&&session.active()&&w.sizeCalls==calls+1&&w.resizes==resizes,"Group start did not size both outputs in one SDK call and deactivate the console");
    requireGroup(w,{1600,900,0,0},{1280,720,1600,0},true,"Group start left a wrong layout");
    require(extraOriginal(w)&&w.extra().x==2880&&w.extra().y==0,"Other display not kept right of the group with its mode");
    const auto saved=parseDisplayV2State(*m.bytes);require(saved.owned.size()==2&&saved.owned[1].width==1920&&saved.console&&displayV2ConsoleActive(saved.ccd,*saved.console),"Group snapshot did not record both owned outputs and the console");++passed;
    const auto resized=session.resizeGroup(std::vector<DisplayV2Output>{{"0",1920,1080,0,0},{"1",1280,1024,1920,0}},deadline(w));
    require(resized.ok&&w.sizeCalls==calls+2&&m.saves==1&&!resized.consoleReturned,"Group resize failed or rewrote the snapshot");
    requireGroup(w,{1920,1080,0,0},{1280,1024,1920,0},true,"Group resize left a wrong layout");
    require(extraOriginal(w)&&w.extra().x==3200,"Other display lost its mode on a group resize");++passed;
    // Only output 1 changes: one per-output SDK resize.
    require(session.resizeGroup(std::vector<DisplayV2Output>{{"0",1920,1080,0,0},{"1",1600,900,1920,0}},deadline(w)).ok&&w.resizes==resizes+1&&w.sizeCalls==calls+2,"A one-output group change did not use one SDK resize");
    requireGroup(w,{1920,1080,0,0},{1600,900,1920,0},true,"One-output group change left a wrong layout");
    const auto stopped=session.restore();require(stopped.outcome==DisplayV2Outcome::restored&&!m.bytes&&!session.active(),"Group stop did not restore and delete the state file");
    requireDual(w,true,true,"Group stop did not put the post-count configuration back");++passed;}
  // "1" left, below, above, offset; client coordinates are relative ("0" is
  // moved to the origin) and the request order does not matter.
  {const std::array<std::pair<POINT,POINT>,5> layouts{{{{0,0},{-1280,0}},{{0,0},{0,900}},{{0,0},{0,-720}},{{0,0},{1600,100}},{{500,200},{2100,200}}}};
    for(const auto& [zero,one]:layouts){
      World w=dualWorld(true);Memory m;DisplaySessionV2 session(access(w,m));
      const std::vector<DisplayV2Output> group{{"1",1280,720,one.x,one.y},{"0",1600,900,zero.x,zero.y}};
      require(session.startGroup("g",group,true,deadline(w)).ok,"Group start failed for an arrangement");
      const LONG x=one.x-zero.x,y=one.y-zero.y;requireGroup(w,{1600,900,0,0},{1280,720,x,y},true,"Output 1 not at its relative place");
      const LONG reach=std::max<LONG>(1600,x+1280),top=x+1280>1600?y:0;
      require(extraOriginal(w)&&w.extra().x==reach&&w.extra().y==top,"Other display not slid right of the group, top-aligned with its right-most output");
      require(session.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Arrangement restore failed");requireDual(w,true,true,"Arrangement stop did not restore");++passed;
    }}
  // Refusals without any change: overlapping or invalid outputs, a group that
  // does not name exactly the owned outputs (the count is a separate step).
  {World w=dualWorld();Memory m;DisplaySessionV2 session(access(w,m));const auto applies=w.applies,calls=w.sizeCalls,resizes=w.resizes;
    const auto overlap=session.startGroup("g",std::vector<DisplayV2Output>{{"0",1600,900,0,0},{"1",1280,720,800,0}},true,deadline(w));
    require(!overlap.ok&&overlap.stage==displayV2Stage::ineligible&&overlap.reason=="display_v2_group_invalid"&&!overlap.restore,"Overlapping group was not refused before any change");
    const auto partial=session.startGroup("g",std::vector<DisplayV2Output>{{"0",1600,900,0,0}},true,deadline(w));
    require(!partial.ok&&partial.stage==displayV2Stage::ineligible&&partial.reason=="display_v2_group_mismatch"&&!partial.restore,"Group without output 1 was not refused before any change");
    for(const auto& bad:std::vector<std::vector<DisplayV2Output>>{{{"1",1600,900,0,0}},{{"0",1601,900,0,0},{"1",1280,720,1601,0}},{{"0",1600,900,0,0},{"0",1280,720,1600,0}}})
      require(!session.startGroup("g",bad,true,deadline(w)).ok,"Invalid group accepted");
    require(!m.bytes&&!m.saves&&w.applies==applies&&w.sizeCalls==calls&&w.resizes==resizes&&!session.active(),"A refused group changed the SDK, Windows or the state");
    requireDual(w,true,false,"A refused group changed the displays");++passed;}
  {World w;Memory m;DisplaySessionV2 session(access(w,m));
    const auto two=session.startGroup("g",rightGroup,true,deadline(w));require(!two.ok&&two.reason=="display_v2_group_mismatch"&&!m.bytes&&w.applies==0&&w.resizes==0&&w.sizeCalls==0,"A two-output group on one owned output changed something");
    // A one-output group is the single-output start.
    require(session.startGroup("g",std::vector<DisplayV2Output>{{"0",1600,900,0,0}},true,deadline(w)).ok,"One-output group failed");requireStreaming(w,1600,900,true,"One-output group is not streaming");
    require(session.restore().outcome==DisplayV2Outcome::restored,"One-output group restore failed");requireOriginal(w,false,"One-output group stop did not restore");++passed;}
  // The console inactive at rest; Windows re-activates and moves it on
  // every SDK target recreation. With proof it is identified then; without, it
  // is simply not re-supplied. Start, resize ("1" now left) and stop.
  for(const bool proof:{true,false}){
    World w=dualWorld(false,true);Memory m;DisplaySessionV2 session(access(w,m,proof));
    w.afterResize=[](World& world){auto& c=world.console();c.active=true;c.x=-1024;c.y=200;c.width=c.dbWidth;c.height=c.dbHeight;};
    const auto started=session.startGroup("vm149-group",rightGroup,proof,deadline(w));
    require(started.ok&&!w.console().active&&started.consoleIdentified==proof&&started.foreignBefore==0&&!started.consoleRemoved,"Group start failed when Windows re-activated the inactive console");
    requireGroup(w,{1600,900,0,0},{1280,720,1600,0},true,"Group start with a re-activated console left a wrong layout");
    const auto resized=session.resizeGroup(std::vector<DisplayV2Output>{{"0",1280,720,0,0},{"1",1600,900,-1600,0}},deadline(w));
    require(resized.ok&&!w.console().active&&resized.consoleReturned==proof,"Group resize failed when Windows re-activated the console");
    requireGroup(w,{1280,720,0,0},{1600,900,-1600,0},true,"Group resize with a re-activated console left a wrong layout");
    const auto original=*m.bytes;const auto applies=w.applies,sizeCalls=w.sizeCalls;
    const auto restored=session.restore();
    if(proof){
      require(restored.outcome==DisplayV2Outcome::restored&&!m.bytes,"Verified group restore failed when Windows re-activated the console");
      requireDual(w,false,false,"Group stop did not return to the console-inactive post-count configuration");
    }else{
      require(restored.outcome==DisplayV2Outcome::failed&&restored.stage==displayV2Stage::foreign&&m.bytes==std::optional(original),"An unverified newly active group target was accepted or its state was dropped");
      require(w.console().active&&w.applies==applies&&w.sizeCalls==sizeCalls+1,"Group restore deactivated an unverified target or hid preceding SDK work");
    }
    ++passed;
  }
  // Windows changes another display while the SDK recreates the targets: its
  // baseline modes are re-supplied. Persistently: stage 8 with the fixed
  // code, the saved configuration back, and the next start works (no latch).
  {World w=dualWorld(true);Memory m;DisplaySessionV2 session(access(w,m));
    w.afterResize=[](World& world){world.extra().refreshN=60;world.extra().x=0;world.extra().y=900;};
    require(session.startGroup("g",rightGroup,true,deadline(w)).ok&&extraOriginal(w)&&w.extra().x==2880,"A foreign change made by the SDK recreation was not put back");
    w.afterResize=nullptr;require(session.restore().ok(),"Restore failed");requireDual(w,true,true,"Restore after a corrected foreign change failed");++passed;}
  {World w=dualWorld(true);Memory m;DisplaySessionV2 session(access(w,m));
    w.afterResize=[](World& world){world.extra().refreshN=60;};w.afterApply=[](World& world){if(world.ownedWidth==1600)world.extra().refreshN=60;};
    const auto failed=session.startGroup("g",rightGroup,true,deadline(w));
    require(!failed.ok&&failed.stage==displayV2Stage::foreign&&failed.foreignDifference==displayV2ForeignCode::refresh&&failed.restore&&failed.restore->outcome==DisplayV2Outcome::restored&&!m.bytes&&!session.active(),"Persistent foreign change did not fail with its code and restore");
    requireDual(w,true,true,"Failed group start did not restore the post-count configuration");
    w.afterResize=nullptr;w.afterApply=nullptr;require(session.startGroup("g",rightGroup,true,deadline(w)).ok,"Group start after a foreign failure latched");
    require(session.restore().ok(),"Restore failed");++passed;}
  // SDK refusal: clean failure, nothing left, no latch. Without the
  // multi-size call both outputs are sized one by one (and restored so).
  {World w=dualWorld();Memory m;w.failSizes=true;
    for(int attempt=0;attempt<2;++attempt){DisplaySessionV2 session(access(w,m));const auto result=session.startGroup("g",rightGroup,true,deadline(w));
      require(!result.ok&&result.stage==displayV2Stage::sdk&&result.restore&&result.restore->ok()&&!m.bytes&&!session.active(),"SDK refusal left a state file or an active session");
      requireDual(w,true,false,"SDK refusal changed the displays");}
    w.failSizes=false;auto a=access(w,m);a.sizes=nullptr;DisplaySessionV2 session(std::move(a));const auto resizes=w.resizes,calls=w.sizeCalls;
    require(session.startGroup("g",rightGroup,true,deadline(w)).ok&&w.resizes==resizes+2&&w.sizeCalls==calls,"Per-output SDK fallback did not size both outputs");
    requireGroup(w,{1600,900,0,0},{1280,720,1600,0},true,"Per-output fallback left a wrong layout");
    require(session.restore().outcome==DisplayV2Outcome::restored&&w.resizes==resizes+4,"Per-output restore failed");requireDual(w,true,false,"Per-output restore did not restore");++passed;}
  // Windows re-lays the group once more after our apply: re-planned.
  {World w=dualWorld();Memory m;DisplaySessionV2 session(access(w,m));int late=0;
    w.afterApply=[&](World& world){if(world.ownedWidth==1600&&!late++){world.second().x=0;world.second().y=900;}};
    require(session.startGroup("g",rightGroup,true,deadline(w)).ok&&late==2,"A late Windows re-layout of the group was not re-planned");
    requireGroup(w,{1600,900,0,0},{1280,720,1600,0},true,"Late re-layout left a wrong group");w.afterApply=nullptr;require(session.restore().ok(),"Restore failed");++passed;}
  // Crash mid-stream: the service restarts with new targets at the streaming
  // sizes; the service-start recovery restores both outputs (one SDK call)
  // to their saved size/place and the console; the next start works.
  for(const bool autoActivate:{false,true}){
    World w=dualWorld(true);Memory m;
    {DisplaySessionV2 crashed(access(w,m));require(crashed.startGroup("g",rightGroup,true,deadline(w)).ok,"Fixture group start failed");}
    w.autoActivate=autoActivate;w.serviceRestart();const auto calls=w.sizeCalls;
    DisplaySessionV2 recovery(access(w,m,false));const auto restored=recovery.restore();
    require(restored.outcome==DisplayV2Outcome::restored&&!m.bytes&&w.sizeCalls==calls+1,"Recovery of a group stream did not restore both outputs in one SDK call");
    requireDual(w,true,true,"Recovery did not restore both outputs and the console");
    w.autoActivate=false;DisplaySessionV2 next(access(w,m));require(next.startGroup("g",rightGroup,true,deadline(w)).ok,"Group start after recovery failed");require(next.restore().ok(),"Restore failed");++passed;}
  // Locked: refused without any change, then works.
  {World w=dualWorld();Memory m;w.eligible=false;DisplaySessionV2 session(access(w,m));const auto applies=w.applies,calls=w.sizeCalls;const auto result=session.startGroup("g",rightGroup,true,deadline(w));
    require(!result.ok&&result.stage==displayV2Stage::ineligible&&!m.bytes&&w.applies==applies&&w.sizeCalls==calls,"Locked group start changed something");
    w.eligible=true;require(session.startGroup("g",rightGroup,true,deadline(w)).ok&&session.restore().ok(),"Group start after a locked attempt latched");++passed;}
  // Resize of an inactive or mismatching group restores (the stream ends).
  {World w=dualWorld();Memory m;DisplaySessionV2 session(access(w,m));
    require(!session.resizeGroup(rightGroup,deadline(w)).ok,"Resize without a started group succeeded");
    require(session.startGroup("g",rightGroup,true,deadline(w)).ok,"Fixture group start failed");
    const auto wrong=session.resizeGroup(std::vector<DisplayV2Output>{{"0",1600,900,0,0}},deadline(w));
    require(!wrong.ok&&wrong.stage==displayV2Stage::ineligible&&wrong.restore&&wrong.restore->outcome==DisplayV2Outcome::restored&&!m.bytes,"A mismatching group resize did not restore");
    requireDual(w,true,false,"A mismatching group resize did not put the configuration back");++passed;}
  return passed;
}
int groupPlanCases(){
  int passed=0;
  // Canonical layout: ordered by id, "0" at the origin; refusals.
  {const auto group=displayV2GroupLayout(std::vector<DisplayV2Output>{{"1",1280,720,2100,150},{"0",1600,900,500,200}});
    require(group.size()==2&&group[0].id=="0"&&!group[0].left&&!group[0].top&&group[1].id=="1"&&group[1].left==1600&&group[1].top==-50,"Group layout is not canonical");++passed;
    for(const auto& bad:std::vector<std::vector<DisplayV2Output>>{{},{{"1",1600,900,0,0}},{{"0",1600,900,0,0},{"0",1280,720,1600,0}},{{"0",1601,900,0,0}},{{"0",1600,178,0,0}},{{"0",1600,900,0,0},{"1",1280,720,1599,899}},{{"0",1600,900,0,0},{"2",1280,720,1600,0}},{{"0",1600,900,0,0},{"1",1280,720,INT32_MAX,0}},{{"0",1600,900,0,0},{"1",1280,720,1600,0},{"1",1280,720,1600,0}}}){
      bool refused=false;try{(void)displayV2GroupLayout(bad);}catch(...){refused=true;}require(refused,"Invalid group layout accepted");++passed;
    }
    // A representable offset is not a driver/capture capability claim. The
    // old 32000 rejection was a removed product cap, not an overflow boundary.
    const auto wide=displayV2GroupLayout(std::vector<DisplayV2Output>{{"0",1600,900,0,0},{"1",1280,720,32000,0}});
    require(wide.size()==2&&wide[1].left==32000,"A representable group offset was restricted");++passed;
  }
  // Plan: owned first ("0", "1"), the identified console omitted, the other
  // display with its baseline mode right of the group, top-aligned with "1".
  {World w=dualWorld(true);const auto before=w.query();const auto console=DisplayV2Console{consoleAdapter,consoleAdapter,0,consoleTarget};
    w.extra().refreshN=60;w.extra().x=0;   // Windows changed it meanwhile
    const auto group=displayV2GroupLayout(std::vector<DisplayV2Output>{{"0",1920,1080,0,0},{"1",1920,1080,1920,300}});bool removed=false;
    const auto plan=planDisplayV2Group(w.query(),before,group,w.targets(),w.targets(),console,&removed);
    require(removed&&plan.paths.size()==3&&same(plan.paths[0].targetInfo.adapterId,ownedAdapter)&&plan.paths[0].targetInfo.id==w.ownedTarget&&plan.paths[1].targetInfo.id==w.secondTarget&&same(plan.paths[2].targetInfo.adapterId,extraAdapter),"Group plan path set/order is wrong or kept the console");
    const auto& o=nativeCcdSource(plan,plan.paths[0]);const auto& s=nativeCcdSource(plan,plan.paths[1]);const auto& e=nativeCcdSource(plan,plan.paths[2]);
    require(!o.position.x&&!o.position.y&&s.position.x==1920&&s.position.y==300&&e.position.x==3840&&e.position.y==300&&e.width==800&&nativeCcdTarget(plan,plan.paths[2]).targetVideoSignalInfo.vSyncFreq.Numerator==75,"Group plan geometry or baseline modes are wrong");++passed;}
  // A console inactive in the baseline is not supplied even when Windows
  // activated it (no proof, so it is not identified).
  {World w=dualWorld(false,true);const auto before=w.query();w.console().active=true;w.console().x=3840;
    const auto plan=planDisplayV2Group(w.query(),before,displayV2GroupLayout(std::vector<DisplayV2Output>{{"0",1920,1080,0,0},{"1",1920,1080,-1920,0}}),w.targets(),w.targets(),std::nullopt);
    require(plan.paths.size()==2&&std::none_of(plan.paths.begin(),plan.paths.end(),[](const auto& p){return same(p.targetInfo.adapterId,consoleAdapter);}),"Group plan re-supplied a console that was inactive before");++passed;
    // A group whose sizes the SDK has not applied cannot be planned.
    bool refused=false;try{(void)planDisplayV2Group(w.query(),before,displayV2GroupLayout(rightGroup),w.targets(),w.targets(),std::nullopt);}catch(...){refused=true;}require(refused,"A group at sizes the SDK has not applied was planned");++passed;}
  // Satisfaction: the post-count layout satisfies its own group; rejections.
  {World w=dualWorld();const auto group=displayV2GroupLayout(std::vector<DisplayV2Output>{{"0",1920,1080,0,0},{"1",1920,1080,1920,0}});
    const auto targets=w.targets();const auto console=DisplayV2Console{consoleAdapter,consoleAdapter,0,consoleTarget};std::string why;
    require(displayV2GroupSatisfied(w.query(),group,targets,targets,std::nullopt,&why),"Satisfied group rejected");
    require(!displayV2GroupSatisfied(w.query(),group,targets,targets,console,&why)&&why=="display_v2_console_active","Active identified console accepted");
    w.console().x=-1024;w.console().y=-300;require(displayV2GroupSatisfied(w.query(),group,targets,targets,std::nullopt,&why),"A display Windows placed elsewhere was rejected");
    w.second().x=0;w.second().y=1080;require(!displayV2GroupSatisfied(w.query(),group,targets,targets,std::nullopt,&why)&&why=="display_v2_owned_not_placed","Misplaced output 1 accepted");w.second().x=1920;w.second().y=0;
    w.owned().x=-10;require(!displayV2GroupSatisfied(w.query(),group,targets,targets,std::nullopt,&why)&&why=="display_v2_owned_not_primary","Non-primary output 0 accepted");w.owned().x=0;
    w.second().width=1280;require(!displayV2GroupSatisfied(w.query(),group,targets,targets,std::nullopt,&why)&&why=="display_v2_owned_size_mismatch","Wrong-sized output 1 accepted");w.second().width=1920;
    w.second().active=false;require(!displayV2GroupSatisfied(w.query(),group,targets,targets,std::nullopt,&why)&&why=="display_v2_owned_inactive","Inactive output 1 accepted");w.second().active=true;
    w.console().x=3000;w.console().y=0;require(!displayV2GroupSatisfied(w.query(),group,targets,targets,std::nullopt,&why)&&why=="display_v2_overlap","Overlap accepted");w.console().x=3840;
    w.displays.push_back({ownedAdapter,2,77,true,true,5000,0,1280,720,60,1,DISPLAYCONFIG_ROTATION_IDENTITY,1280,720});auto ours=targets;ours.push_back({"1",ownedAdapter,77,1280,720});
    require(!displayV2GroupSatisfied(w.query(),group,targets,ours,std::nullopt,&why)&&why=="display_v2_owned_departure_pending","Departed owned generation still active accepted");++passed;}
  // Restore from a two-output state: both owned outputs at their saved place
  // under their current target ids, retaining the original source sizes even
  // while the SDK's current sizes still belong to the stream.
  {World w=dualWorld(true);const DisplayV2State saved{"s",displayV2ActiveCcd(w.query()),w.targets(),std::nullopt};
    w.setSizes(rightGroup);std::vector<DisplayV2Expected> expected;
    const auto plan=planDisplayV2Restore(saved,w.query(),w.targets(),&expected);int owned=0;
    for(const auto& p:plan.paths)if(same(p.targetInfo.adapterId,ownedAdapter)){const auto& s=nativeCcdSource(plan,p);if(p.targetInfo.id==w.ownedTarget&&!s.position.x&&s.width==1920&&s.height==1080)++owned;if(p.targetInfo.id==w.secondTarget&&s.position.x==1920&&s.width==1920&&s.height==1080)++owned;}
    require(owned==2&&expected.size()==4,"Restore of a two-output state lost an owned output");++passed;}
  return passed;
}
int groupRpcCases(){
  int passed=0;
  const std::string snapshot=R"({"schema":1,"provider":"libvirtualdisplay-1.6.4","leaseActive":true,"targets":[{"id":"0","adapterLow":44,"adapterHigh":0,"target":3,"width":1600,"height":900},{"id":"1","adapterLow":44,"adapterHigh":0,"target":4,"width":1280,"height":720}]})";
  const DisplayV2SizesRequest request{std::string(32,'a'),{{"0",1600,900},{"1",1280,720}}};
  {const auto parsed=parseDisplayV2SizesRequest(JsonParser::parse(displayV2SizesRequestBytes(request)));require(parsed&&parsed->nonce==request.nonce&&parsed->outputs.size()==2&&parsed->outputs[1].id=="1"&&parsed->outputs[1].width==1280&&!parsed->outputs[1].left,"Sizes request did not round-trip");++passed;}
  for(const auto* bad:{R"({"type":"worker.v2.display.sizes","nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","outputs":[]})",R"({"type":"worker.v2.display.sizes","nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","outputs":[{"id":"0","width":1601,"height":900}]})",
      R"({"type":"worker.v2.display.sizes","nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","outputs":[{"id":"0","width":1600,"height":900},{"id":"0","width":1280,"height":720}]})",R"({"type":"worker.v2.display.sizes","nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","outputs":[{"id":"2","width":1600,"height":900}]})",
      R"({"type":"worker.v2.display.sizes","nonce":"short","outputs":[{"id":"0","width":1600,"height":900}]})",R"({"type":"worker.v2.display.sizes","nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","outputs":[{"id":"0","width":1600,"height":900,"x":0}]})",
      R"({"type":"worker.v2.display.sizes","nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","outputs":[{"id":"0","width":1600,"height":900}],"extra":1})",R"({"type":"worker.v2.display.sizes","nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","outputs":[{"id":"0","width":1600,"height":900},{"id":"1","width":1280,"height":720},{"id":"1","width":1280,"height":720}]})"}){
    require(!parseDisplayV2SizesRequest(JsonParser::parse(bad)),"Malformed sizes request accepted");++passed;
  }
  int calls=0;const DisplayV2SizesCall sizes=[&](std::span<const DisplayV2Output> outputs)->std::optional<std::string>{++calls;require(outputs.size()==2&&outputs[0].width==1600&&outputs[1].height==720,"Dispatch altered the sizes");return snapshot;};
  const auto resize=[](const std::string&,DWORD,DWORD)->std::optional<std::string>{return {};};
  {const auto refused=dispatchDisplayV2(JsonParser::parse(displayV2SizesRequestBytes(request)),true,resize);require(refused.handled&&!JsonParser::parse(refused.response).get("ok").boolean()&&calls==0,"Sizes request on a pipe without sizes authority reached the SDK");++passed;}
  {const auto denied=dispatchDisplayV2(JsonParser::parse(displayV2SizesRequestBytes(request)),false,resize,{},sizes);require(denied.handled&&!JsonParser::parse(denied.response).get("ok").boolean()&&calls==0,"Unauthorized sizes request reached the SDK");++passed;}
  std::vector<Json> inbound;int interleaved=0;
  const auto read=[&](DWORD)->std::optional<Json>{if(inbound.empty())return {};auto value=inbound.front();inbound.erase(inbound.begin());return value;};
  const auto write=[&](const std::string& message){inbound.push_back(JsonParser::parse(R"({"type":"clip.packet"})"));inbound.push_back(JsonParser::parse(dispatchDisplayV2(JsonParser::parse(message),true,resize,{},sizes).response));return true;};
  const auto interleave=[&](const Json& value){if(value.get("type").string()=="clip.packet"){++interleaved;return true;}return false;};
  {const auto targets=displayV2SizesRpc(request,write,read,interleave,5000);require(targets&&targets->size()==2&&targets->front().target==3&&targets->back().width==1280&&calls==1&&interleaved==1,"Worker sizes RPC did not return both owned targets");++passed;}
  {auto other=request;other.outputs[1].width=1920;const auto targets=displayV2SizesRpc(other,[&](const std::string&){inbound.push_back(JsonParser::parse(displayV2SizesReplyBytes(other.nonce,snapshot)));return true;},read,interleave,5000);require(!targets,"A snapshot without the requested size was accepted");++passed;}
  {const auto targets=displayV2SizesRpc(request,[&](const std::string&){inbound.push_back(JsonParser::parse(displayV2SizesReplyBytes(std::string(32,'b'),snapshot)));return true;},read,interleave,5000);require(!targets,"A reply for another request was accepted");++passed;}
  {const auto targets=displayV2SizesRpc(request,[&](const std::string&){inbound.push_back(JsonParser::parse(displayV2SizesReplyBytes(request.nonce,std::nullopt)));return true;},read,interleave,5000);require(!targets,"A refused SDK sizes change was accepted");++passed;}
  {inbound.clear();const auto targets=displayV2SizesRpc(request,[](const std::string&){return true;},read,interleave,5000);require(!targets,"A missing sizes reply was accepted");++passed;}
  {const auto targets=displayV2SizesRpc({std::string(32,'c'),{{"0",1601,900}}},write,read,interleave,5000);require(!targets&&calls==1,"An invalid sizes request was sent");++passed;}
  return passed;
}
// Private worker <-> service packets of the v2 tracks path: fixed fields only.
int trackPacketCases(){
  int passed=0;
  const auto request=nativeTrackResizeRequest(JsonParser::parse(R"({"sessionId":"00000000-0000-0000-0000-000000000001","requestId":"tracks-qa","resizeId":"00000000-0000-0000-0000-000000000007","topologyEpoch":2,"fps":60,"tracks":[{"id":"1","width":1280,"height":1024,"x":-1280,"y":100,"epoch":2},{"id":"0","width":1600,"height":900,"x":0,"y":0,"epoch":1}]})"));
  {const auto outputs=displayV2TrackOutputs(request);const auto group=displayV2GroupLayout(outputs);
    require(outputs.size()==2&&group[0].id=="0"&&group[0].width==1600&&group[1].left==-1280&&group[1].top==100&&group[1].height==1024,"Track outputs lost a size or place");++passed;}
  {const auto current=JsonParser::parse(R"({"type":"session.start","sessionId":"00000000-0000-0000-0000-000000000001","mode":"stream","mediaProtocol":"tracks-v1","monitor":"all","maxWidth":1280,"maxHeight":720,"fps":30,"topologyEpoch":1,"tracks":[]})");
    const auto next=displayV2TrackCommand(current,request);
    require(next.get("requestId").string()=="tracks-qa"&&next.get("topologyEpoch").number()==2&&next.get("fps").number()==60&&next.get("maxWidth").number()==1600&&next.get("maxHeight").number()==1024&&nativeSameTrackResize(nativeTrackResizeRequest(next),request)&&next.get("monitor").string()=="all","Next track command is wrong");++passed;}
  const std::string display=R"({"installed":true,"displays":[]})";const std::string session="00000000-0000-0000-0000-000000000001";
  DisplayV2Result okResult;okResult.ok=true;okResult.consoleRemoved=okResult.consoleProof=okResult.consoleIdentified=true;okResult.foreignBefore=3;
  {const auto parsed=parseDisplayV2TracksStarted(JsonParser::parse(displayV2TracksStartedBytes(session,okResult,true,true,display)),session);
    require(parsed&&parsed->ok&&!parsed->stage&&!parsed->error&&parsed->consoleRemoved&&parsed->consoleProof&&parsed->consoleIdentified&&parsed->foreignCount==3&&!parsed->restoreAttempted&&parsed->desktopAvailable&&parsed->display.installed,"Successful group start did not round-trip");++passed;}
  DisplayV2Result failed;failed.stage=displayV2Stage::foreign;failed.foreignDifference=displayV2ForeignCode::refresh;failed.error=5;failed.restore=DisplayV2RestoreResult{DisplayV2Outcome::failed,displayV2Stage::verify};
  {const auto parsed=parseDisplayV2TracksStarted(JsonParser::parse(displayV2TracksStartedBytes(session,failed,false,false,display)),session);
    require(parsed&&!parsed->ok&&parsed->stage==8&&parsed->error==displayV2ForeignCode::refresh&&parsed->restoreAttempted&&!parsed->restored&&parsed->restoreStage==displayV2Stage::verify,"Failed group start lost its fixed codes");++passed;}
  {DisplayV2Result internal;const auto parsed=parseDisplayV2TracksStarted(JsonParser::parse(displayV2TracksStartedBytes(session,internal,false,true,display)),session);require(parsed&&parsed->stage==displayV2Stage::internal&&!parsed->restoreAttempted,"A failure without a stage was not reported as internal");++passed;}
  const auto packet=JsonParser::parse(displayV2TracksStartedBytes(session,failed,false,false,display));
  for(int variant=0;variant<11;++variant){auto wrong=packet;auto& fields=std::get<Json::Object>(wrong.value);switch(variant){
    case 0:streamField(wrong,"reason",std::string("free text"));break;case 1:fields.erase("restoreStage");break;case 2:streamField(wrong,"type",std::string("worker.v2.started"));break;
    case 3:streamField(wrong,"sessionId",std::string("00000000-0000-0000-0000-000000000002"));break;case 4:fields["ok"].value=true;break;case 5:streamField(wrong,"nativeStage",0);break;
    case 6:streamField(wrong,"nativeStage",10);break;case 7:streamField(wrong,"restored",1);break;case 8:fields["restoreAttempted"].value=false;fields["restored"].value=true;break;
    case 9:streamField(wrong,"foreignCount",65);break;case 10:streamField(wrong,"display",std::string("text"));break;}
    require(!parseDisplayV2TracksStarted(wrong,session),"Malformed group start result accepted");++passed;
  }
  require(!parseDisplayV2TracksStarted(packet,""),"Group start result accepted without a session");++passed;
  // The resize result carries the single-output v2 diagnostic fields.
  {DisplayV2Result resized;resized.stage=displayV2Stage::apply;resized.error=ERROR_GEN_FAILURE;resized.consoleReturned=true;
    const auto value=JsonParser::parse(displayV2TrackResizeResultBytes(request,resized,false,true,display));const auto diagnostic=streamViewportDiagnostic(value);
    require(diagnostic&&diagnostic->nativeStage==displayV2Stage::apply&&diagnostic->nativeError==ERROR_GEN_FAILURE&&diagnostic->consoleReturned&&!value.get("ok").boolean()&&nativeSameTrackResize(nativeTrackResizeRequest(value),request),"Group resize result lost its correlation or fixed codes");
    const auto good=JsonParser::parse(displayV2TrackResizeResultBytes(request,okResult,true,true,display));const auto clean=streamViewportDiagnostic(good);
    require(clean&&!clean->nativeStage&&!clean->nativeError&&good.get("ok").boolean(),"Successful group resize result carried a failure code");++passed;}
  return passed;
}
// ------------------------------------------------ rapid display changes
// Field case: rapid window size /
// fullscreen / 모든 모니터 changes ended streams as display_change_failed. The
// worker's SDK size RPC read whatever the service wrote next; Session0
// forwards the client's lossless opt-in (stream.lossless) for every lt1- ping
// (about once a second) and a track tunnel forwards pings, so a resize whose
// RPC wait overlapped one failed at stage 4. This model drives the real
// DisplaySessionV2 over the real RPC functions and an in-memory pipe into
// which the "service" writes session traffic before each reply.
std::string leaseJson(const std::vector<NativeDisplayTarget>& targets){
  std::string out=R"({"schema":1,"provider":"libvirtualdisplay-1.6.4","leaseActive":true,"targets":[)";
  for(size_t i=0;i<targets.size();++i){const auto& t=targets[i];if(i)out+=',';out+="{\"id\":"+jsonQuote(t.id)+",\"adapterLow\":"+std::to_string(t.adapter.LowPart)+",\"adapterHigh\":"+std::to_string(t.adapter.HighPart)+",\"target\":"+std::to_string(t.target)+",\"width\":"+std::to_string(t.width)+",\"height\":"+std::to_string(t.height)+'}';}
  return out+"]}";
}
struct Pipe {
  std::deque<Json> inbound;int rpcs=0,injected=0;uint64_t nonce=0;
  // Session traffic the service writes before the reply to RPC number `rpc`.
  std::function<std::vector<std::string>(int rpc)> traffic;
  std::string nextNonce(){char text[33]{};std::snprintf(text,sizeof(text),"%032llx",static_cast<unsigned long long>(++nonce));return text;}
  void before(){++rpcs;if(traffic)for(const auto& packet:traffic(rpcs)){inbound.push_back(JsonParser::parse(packet));++injected;}}
  std::optional<Json> read(){if(inbound.empty())return {};auto value=std::move(inbound.front());inbound.pop_front();return value;}
};
// The worker's interleave (service.hpp v2Interleave): `fixed` keeps session
// traffic in the backlog; one that does not keep it fails on it.
std::function<bool(const Json&)> workerInterleave(WorkerPipeBacklog& backlog,bool fixed){
  return [&backlog,fixed](const Json& value){
    const auto type=value.get("type").string();
    if(type=="clip.packet"||type=="input.reset"||type=="input"||type=="session.stop"||type=="session.ended")return true;
    return fixed&&backlog.defer(value);
  };
}
DisplayV2Access pipedAccess(World& w,Memory& m,Pipe& p,std::function<bool(const Json&)> interleave,bool console=true){
  auto a=worldAccess(w,m,console);
  const auto read=[&p](DWORD)->std::optional<Json>{return p.read();};
  a.resize=[&w,&p,interleave,read](const std::string& id,DWORD width,DWORD height){
    return displayV2SizeRpc({p.nextNonce(),id,width,height},[&w,&p](const std::string& message){
      const auto reply=dispatchDisplayV2(JsonParser::parse(message),true,[&w](const std::string& monitor,DWORD wd,DWORD ht)->std::optional<std::string>{const auto t=w.resize(monitor,wd,ht);if(!t)return {};return leaseJson(*t);});
      p.before();p.inbound.push_back(JsonParser::parse(reply.response));return true;},read,interleave,5000,[&w]{return w.clock;});
  };
  a.sizes=[&w,&p,interleave,read](std::span<const DisplayV2Output> outputs){
    return displayV2SizesRpc({p.nextNonce(),{outputs.begin(),outputs.end()}},[&w,&p](const std::string& message){
      const auto reply=dispatchDisplayV2(JsonParser::parse(message),true,[](const std::string&,DWORD,DWORD)->std::optional<std::string>{return {};},{},
        [&w](std::span<const DisplayV2Output> o)->std::optional<std::string>{const auto t=w.setSizes(o);if(!t)return {};return leaseJson(*t);});
      p.before();p.inbound.push_back(JsonParser::parse(reply.response));return true;},read,interleave,5000,[&w]{return w.clock;});
  };
  return a;
}
// What the client really sends during a change: an lt1- ping each second,
// which Session0 answers itself and turns into stream.lossless for the worker
// (single output); a track tunnel forwards the ping itself and ready ACKs.
std::vector<std::string> singleTraffic(int rpc){if(rpc%3==2)return {};return {R"({"type":"stream.lossless","protocol":"tiles-v1"})"};}
std::vector<std::string> trackTraffic(int rpc){
  std::vector<std::string> out{R"({"type":"ping","sessionId":"00000000-0000-0000-0000-000000000001","id":"lt1-)"+std::to_string(rpc)+"\"}"};
  if(rpc%2)out.push_back(R"({"type":"clip.packet","sessionId":"00000000-0000-0000-0000-000000000001","payload":{}})");
  return out;
}
// Window drag sizes, fullscreen on one 1080p/1440p monitor and a window
// spanning both local monitors (3840x1080), the largest single output (4K).
const std::vector<std::pair<DWORD,DWORD>> rapidSizes{{1580,952},{1920,1080},{1264,760},{3840,1080},{1600,900},{2560,1440},{1346,812},{1920,1080},{3840,2160},{1548,916},{1920,1032},{1280,720}};
// Windows' own reactions to a recreated owned target, varied per change.
void windowsReacts(World& w,int change){
  w.afterResize=nullptr;w.afterApply=nullptr;w.autoActivate=false;w.consoleReturns=true;
  switch(change%5){
    case 1:w.autoActivate=true;w.consoleReturns=false;break;                                                      // new output auto-activated at the right
    case 2:w.afterResize=[](World& world){auto& c=world.console();c.active=true;c.x=-1024;c.y=300;c.width=c.dbWidth;c.height=c.dbHeight;};break;   // console back at an odd place
    case 3:{auto late=std::make_shared<int>(0);w.afterApply=[late](World& world){if(!(*late)++){world.owned().x=1024;world.owned().y=0;}};}break; // one late re-layout after our apply
    case 4:w.afterResize=[](World& world){world.console().x=0;world.console().y=0;};break;
  }
}
int rapidChangeCases(){
  int passed=0;
  // Pre-fix behaviour: the released interleave fails a resize whenever session
  // traffic lands in its SDK RPC wait (the field's display_change_failed).
  {World w;Memory m;Pipe p;p.traffic=[](int){return std::vector<std::string>{R"({"type":"stream.lossless","protocol":"tiles-v1"})"};};WorkerPipeBacklog backlog;DisplaySessionV2 session(pipedAccess(w,m,p,workerInterleave(backlog,false)));
    require(session.start("s",1920,1080,true,deadline(w)).ok,"Fixture start failed");
    const auto failed=session.resize(1580,952,deadline(w));
    require(!failed.ok&&failed.stage==displayV2Stage::sdk&&failed.reason=="display_v2_sdk_resize_failed"&&!session.active(),"The released interleave did not reproduce the field failure");++passed;}
  // 200 rapid single-output changes with session traffic in every other RPC
  // wait, Windows re-laying displays in five different ways and GDI lagging
  // behind CCD every seventh change: every change succeeds, the original
  // snapshot is never replaced, the kept packets are handled in order, and
  // the stop restores the original configuration.
  {World w(true);Memory m;Pipe p;p.traffic=singleTraffic;WorkerPipeBacklog backlog;DisplaySessionV2 session(pipedAccess(w,m,p,workerInterleave(backlog,true)));
    require(session.start("rapid",1920,1080,true,deadline(w)).ok,"Rapid fixture start failed");const auto original=*m.bytes;
    int failures=0,confirmRounds=0,handled=0;
    for(int change=0;change<200;++change){
      windowsReacts(w,change);const auto size=rapidSizes[size_t(change)%rapidSizes.size()];const DWORD width=size.first,height=size.second;
      int calls=0;const auto confirm=[&]{++calls;if(change%7==3&&calls==1)return false;const auto& o=w.owned();return o.active&&!o.x&&!o.y&&o.width==width&&o.height==height;};
      const auto resized=session.resize(width,height,deadline(w),confirm);
      if(!resized.ok){++failures;break;}
      confirmRounds+=int(resized.confirmRetries);
      requireStreaming(w,width,height,true,"A rapid change did not leave the owned output primary at the requested size");
      require(extraOriginal(w),"A rapid change changed another display's mode");
      while(const auto kept=backlog.next()){require(kept->get("type").string()=="stream.lossless","A kept packet changed type");++handled;}
    }
    require(!failures&&session.active()&&m.bytes==std::optional(original),"Rapid single-output changes failed or replaced the original snapshot");
    require(p.injected>0&&handled>0&&backlog.deferred>=uint32_t(handled)&&confirmRounds>=28,"The stress did not exercise kept packets and confirmation re-plans");
    w.afterResize=nullptr;w.afterApply=nullptr;w.autoActivate=false;w.consoleReturns=true;
    require(session.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Stop after rapid changes did not restore");requireOriginal(w,true,"Stop after rapid changes did not restore the original");++passed;}
  // The same stress through independent outputs (모든 모니터): track pings
  // and clipboard packets in the sizes RPC wait, 200 group changes.
  {World w=dualWorld(true);Memory m;Pipe p;p.traffic=trackTraffic;WorkerPipeBacklog backlog;DisplaySessionV2 session(pipedAccess(w,m,p,workerInterleave(backlog,true)));
    const std::vector<std::vector<DisplayV2Output>> groups{{{"0",1920,1080,0,0},{"1",1920,1080,1920,0}},{{"0",1600,900,0,0},{"1",1280,720,1600,0}},{{"0",1920,1080,0,0},{"1",1280,1024,-1280,0}},{{"0",2560,1440,0,0},{"1",1920,1080,2560,180}},{{"0",1280,720,0,0},{"1",1600,900,0,720}}};
    require(session.startGroup("rapid-group",groups[0],true,deadline(w)).ok,"Rapid group fixture start failed");
    int failures=0,pings=0;
    for(int change=1;change<=200;++change){
      const auto& group=groups[size_t(change)%groups.size()];
      if(change%4==2)w.afterResize=[](World& world){auto& c=world.console();c.active=true;c.x=-1024;c.y=200;c.width=c.dbWidth;c.height=c.dbHeight;};else w.afterResize=nullptr;
      int calls=0;const auto confirm=[&]{return ++calls>1||change%9!=4;};
      const auto resized=session.resizeGroup(group,deadline(w),confirm);
      if(!resized.ok){++failures;break;}
      const auto& zero=group[0];const auto& one=group[1];
      requireGroup(w,{zero.width,zero.height,0,0},{one.width,one.height,one.left,one.top},true,"A rapid group change left a wrong layout");
      require(extraOriginal(w),"A rapid group change changed another display's mode");
      while(const auto kept=backlog.next()){require(kept->get("type").string()=="ping","A kept track packet changed type");++pings;}
    }
    require(!failures&&pings>0,"Rapid group changes failed or kept no pings");
    w.afterResize=nullptr;require(session.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Stop after rapid group changes did not restore");requireDual(w,true,true,"Stop after rapid group changes did not restore");++passed;}
  // Rapid mode switches: window -> 모든 모니터 -> window ... Each switch ends
  // one stream (restore), changes the owned output count (the client's
  // separate step) and starts the next stream, with resizes in between.
  {World w(true);Memory m;Pipe p;WorkerPipeBacklog backlog;int changes=0;
    for(int cycle=0;cycle<25;++cycle){
      p.traffic=singleTraffic;
      {DisplaySessionV2 single(pipedAccess(w,m,p,workerInterleave(backlog,true)));
        require(single.start("mode-single",rapidSizes[size_t(cycle)%rapidSizes.size()].first,rapidSizes[size_t(cycle)%rapidSizes.size()].second,true,deadline(w)).ok,"Single start in a mode switch failed");++changes;
        for(int i=1;i<=3;++i){const auto size=rapidSizes[size_t(cycle+i)%rapidSizes.size()];windowsReacts(w,cycle+i);require(single.resize(size.first,size.second,deadline(w)).ok,"Resize in a mode switch failed");requireStreaming(w,size.first,size.second,true,"Resize in a mode switch left a wrong layout");++changes;}
        w.afterResize=nullptr;w.afterApply=nullptr;w.autoActivate=false;w.consoleReturns=true;
        require(single.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Single stop in a mode switch did not restore");}
      {DisplaySessionV2 count(worldAccess(w,m));require(count.count("count",2,true,deadline(w)).ok,"Count 2 in a mode switch failed");++changes;}
      p.traffic=trackTraffic;
      {DisplaySessionV2 group(pipedAccess(w,m,p,workerInterleave(backlog,true)));
        require(group.startGroup("mode-group",std::vector<DisplayV2Output>{{"0",1920,1080,0,0},{"1",1920,1080,1920,0}},true,deadline(w)).ok,"Group start in a mode switch failed");++changes;
        require(group.resizeGroup(std::vector<DisplayV2Output>{{"0",1600,900,0,0},{"1",1920,1080,1600,0}},deadline(w)).ok,"Group resize in a mode switch failed");++changes;
        require(group.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Group stop in a mode switch did not restore");}
      {DisplaySessionV2 count(worldAccess(w,m));require(count.count("count",1,true,deadline(w)).ok,"Count 1 in a mode switch failed");++changes;}
      while(backlog.next()){}
    }
    // A count change is persistent (owned "0" primary, the others right of it).
    require(changes>=200&&!m.bytes&&!w.hasSecond(),"Mode-switch stress was too short or left a state file / a second output");
    const auto& o=w.owned();const auto& c=w.console();
    require(o.active&&!o.x&&!o.y&&o.width==1920&&o.height==1080&&c.active&&c.width==1024&&c.height==768&&extraOriginal(w)&&!overlapping(w),"Mode switches did not end in the post-count configuration");++passed;}
  return passed;
}
int interleaveCases(){
  int passed=0;
  // Session traffic is kept in order; the opt-in is coalesced; anything that
  // is not session traffic still ends the RPC; a reply to an earlier request
  // is skipped instead of failing this one.
  {WorkerPipeBacklog backlog;
    for(const auto* packet:{R"({"type":"stream.lossless","protocol":"tiles-v1"})",R"({"type":"ping","id":"lt1-1"})",R"({"type":"stream.lossless","protocol":"tiles-v1"})",R"({"type":"stream.tracks.ready","topologyEpoch":2,"tracks":[]})",R"({"type":"stream.bitrate","bitrate":4000})"})
      require(backlog.defer(JsonParser::parse(packet)),"Session traffic was not kept");
    for(const auto* packet:{R"({"type":"worker.native.resize.reply"})",R"({"type":"session.start"})",R"({"type":"worker.v2.restore"})",R"({})"})
      require(!backlog.defer(JsonParser::parse(packet)),"A non-session packet was kept");
    {WorkerPipeBacklog scale;require(scale.defer(JsonParser::parse(R"({"type":"display.scale","sessionId":"s","scale":200})"))&&scale.next()->get("type").string()=="display.scale","A display-scale request during an RPC was not kept");}
    std::vector<std::string> order;while(const auto kept=backlog.next())order.push_back(kept->get("type").string());
    require(order==std::vector<std::string>{"stream.lossless","ping","stream.tracks.ready","stream.bitrate"}&&backlog.deferred==5,"Kept packets lost their order or the opt-in was not coalesced");
    for(size_t i=0;i<WorkerPipeBacklog::limit;++i)require(backlog.defer(JsonParser::parse(R"({"type":"ping","id":"x"})")),"Backlog refused below its limit");
    require(!backlog.defer(JsonParser::parse(R"({"type":"ping","id":"x"})")),"Backlog grew past its limit");++passed;}
  // The opt-in is a state: a pause (게임 마우스) that arrives during an RPC
  // after an opt-in replaces it in place, and an opt-in after it wins again.
  {WorkerPipeBacklog backlog;
    for(const auto* packet:{R"({"type":"stream.lossless","protocol":"tiles-v1"})",R"({"type":"ping","id":"lt0-2"})",R"({"type":"stream.lossless","protocol":"tiles-v1","on":false})"})require(backlog.defer(JsonParser::parse(packet)),"Session traffic was not kept");
    {auto copy=backlog;const auto first=copy.next();require(first&&first->get("type").string()=="stream.lossless"&&std::holds_alternative<bool>(first->get("on").value)&&!first->get("on").boolean()&&copy.next()->get("type").string()=="ping"&&!copy.next(),"A tiles pause during an RPC lost to the earlier opt-in");}
    require(backlog.defer(JsonParser::parse(R"({"type":"stream.lossless","protocol":"tiles-v1"})")),"Session traffic was not kept");
    const auto first=backlog.next();require(first&&first->get("type").string()=="stream.lossless"&&std::holds_alternative<std::nullptr_t>(first->get("on").value)&&backlog.items.size()==1,"A later opt-in did not replace the pause");++passed;}
  const std::string snapshot=R"({"schema":1,"provider":"libvirtualdisplay-1.6.4","leaseActive":true,"targets":[{"id":"0","adapterLow":44,"adapterHigh":0,"target":2,"width":1600,"height":900}]})";
  const DisplayV2SizeRequest request{std::string(32,'a'),"0",1600,900};
  for(const bool fixed:{false,true}){
    std::deque<Json> inbound;WorkerPipeBacklog backlog;const auto interleave=workerInterleave(backlog,fixed);
    const auto read=[&](DWORD)->std::optional<Json>{if(inbound.empty())return {};auto value=inbound.front();inbound.pop_front();return value;};
    const auto targets=displayV2SizeRpc(request,[&](const std::string&){
      inbound.push_back(JsonParser::parse(R"({"type":"stream.lossless","protocol":"tiles-v1"})"));
      inbound.push_back(JsonParser::parse(displayV2SizeReplyBytes(std::string(32,'9'),snapshot)));   // reply of an earlier, timed-out request
      inbound.push_back(JsonParser::parse(displayV2SizeReplyBytes(request.nonce,snapshot)));return true;},read,interleave,5000);
    require(fixed?targets.has_value()&&backlog.items.size()==1:!targets.has_value(),"Session traffic in the RPC wait was not kept (or the released behaviour changed)");++passed;
  }
  {std::deque<Json> inbound;WorkerPipeBacklog backlog;const auto read=[&](DWORD)->std::optional<Json>{if(inbound.empty())return {};auto value=inbound.front();inbound.pop_front();return value;};
    const auto targets=displayV2SizeRpc(request,[&](const std::string&){inbound.push_back(JsonParser::parse(R"({"type":"worker.native.resize.reply"})"));inbound.push_back(JsonParser::parse(displayV2SizeReplyBytes(request.nonce,snapshot)));return true;},read,workerInterleave(backlog,true),5000);
    require(!targets,"A foreign protocol packet in the RPC wait was accepted");++passed;}
  // Sizes and count RPCs skip stale replies the same way.
  {std::deque<Json> inbound;WorkerPipeBacklog backlog;const auto read=[&](DWORD)->std::optional<Json>{if(inbound.empty())return {};auto value=inbound.front();inbound.pop_front();return value;};
    const std::string two=R"({"schema":1,"provider":"libvirtualdisplay-1.6.4","leaseActive":true,"targets":[{"id":"0","adapterLow":44,"adapterHigh":0,"target":3,"width":1600,"height":900},{"id":"1","adapterLow":44,"adapterHigh":0,"target":4,"width":1280,"height":720}]})";
    const DisplayV2SizesRequest sizes{std::string(32,'b'),{{"0",1600,900},{"1",1280,720}}};
    require(displayV2SizesRpc(sizes,[&](const std::string&){inbound.push_back(JsonParser::parse(R"({"type":"ping","id":"lt1-4"})"));inbound.push_back(JsonParser::parse(displayV2SizeReplyBytes(std::string(32,'c'),snapshot)));inbound.push_back(JsonParser::parse(displayV2SizesReplyBytes(sizes.nonce,two)));return true;},read,workerInterleave(backlog,true),5000).has_value(),"Sizes RPC did not skip a stale reply or keep a ping");
    const DisplayV2CountRequest count{std::string(32,'d'),2};
    require(displayV2CountRpc(count,[&](const std::string&){inbound.push_back(JsonParser::parse(displayV2CountReplyBytes(std::string(32,'e'),two)));inbound.push_back(JsonParser::parse(displayV2CountReplyBytes(count.nonce,two)));return true;},read,workerInterleave(backlog,true),5000).has_value(),"Count RPC did not skip a stale reply");++passed;}
  return passed;
}
int retryCases(){
  int passed=0;
  // A refused or lost SDK reply is asked again (the service's size change is
  // idempotent); the change succeeds and says so in its diagnostics.
  {World w;Memory m;auto a=access(w,m);int calls=0;
    a.resize=[&w,&calls](const std::string& id,DWORD width,DWORD height)->std::optional<std::vector<NativeDisplayTarget>>{if(++calls==1){w.resize(id,width,height);return {};}return w.resize(id,width,height);};
    DisplaySessionV2 session(std::move(a));require(session.start("s",1920,1080,true,deadline(w)).ok,"Fixture start failed");
    const auto resized=session.resize(1600,900,deadline(w));
    require(resized.ok&&resized.sdkRetries==1&&calls==2,"A lost SDK reply was not asked again");requireStreaming(w,1600,900,true,"SDK retry left a wrong layout");
    require(session.restore().ok(),"Restore failed");requireOriginal(w,false,"Restore after SDK retry failed");++passed;}
  // GDI lags behind CCD once: re-planned and confirmed again; a confirmation
  // that never comes fails at verify with its fixed reason and restores.
  {World w;Memory m;DisplaySessionV2 session(access(w,m));require(session.start("s",1920,1080,true,deadline(w)).ok,"Fixture start failed");
    int calls=0;const auto lagging=session.resize(1600,900,deadline(w),[&]{return ++calls>1;});
    require(lagging.ok&&lagging.confirmRetries==1&&calls==2,"A lagging capture proof was not re-planned and confirmed again");
    // Windows moves the owned output after the CCD settled: the next round re-applies.
    int late=0;const auto moved=session.resize(1280,720,deadline(w),[&]{if(!late++){w.owned().x=1024;return false;}return w.owned().x==0;});
    require(moved.ok&&moved.confirmRetries==1&&w.owned().x==0,"A late re-layout after verification was not re-planned");
    const auto never=session.resize(1920,1080,deadline(w),[]{return false;});
    require(!never.ok&&never.stage==displayV2Stage::verify&&never.reason=="display_v2_capture_unavailable"&&never.confirmRetries==displayV2ConfirmRounds-1&&never.restore&&never.restore->ok()&&!m.bytes,"A capture proof that never comes did not fail cleanly");
    requireOriginal(w,false,"A failed confirmation did not restore");++passed;}
  // Windows keeps re-laying the displays for a while after the previous
  // change (rapid changes): more re-plans than the old three succeed.
  {World w;Memory m;DisplaySessionV2 session(access(w,m));require(session.start("s",1920,1080,true,deadline(w)).ok,"Fixture start failed");
    int moves=0;w.afterApply=[&](World& world){if(moves++<3){world.owned().x=1024;world.owned().y=0;}};
    require(session.resize(1600,900,deadline(w)).ok&&moves>=4,"Four late re-layouts in a row failed the change");w.afterApply=nullptr;
    require(session.restore().ok(),"Restore failed");++passed;}
  return passed;
}
// Last resort for independent outputs: a group the worker cannot apply is not
// restored to the pre-stream configuration at once; the stream's previous
// group is put back in place (the restored-group path) and the session goes on.
int groupFallbackCases(){
  int passed=0;
  {World w=dualWorld(true);Memory m;DisplaySessionV2 session(access(w,m));
    require(session.startGroup("g",rightGroup,true,deadline(w)).ok,"Fixture group start failed");const auto original=*m.bytes;
    w.reject=[](const NativeCcdData& plan,UINT32 flags)->LONG{if(flags&SDC_TOPOLOGY_SUPPLIED)return LONG(ERROR_SUCCESS);for(const auto& p:plan.paths)if(same(p.targetInfo.adapterId,ownedAdapter)&&nativeCcdSource(plan,p).width==2560)return LONG(ERROR_GEN_FAILURE);return LONG(ERROR_SUCCESS);};
    const std::vector<DisplayV2Output> wanted{{"0",2560,1440,0,0},{"1",1280,720,2560,0}};
    const auto failed=session.resizeGroup(wanted,deadline(w),{},true);
    require(!failed.ok&&!failed.restore&&session.active()&&m.bytes==std::optional(original),"A kept failure restored or dropped the session");
    const auto back=session.resizeGroup(rightGroup,deadline(w));
    require(back.ok&&session.active(),"The previous group could not be put back");requireGroup(w,{1600,900,0,0},{1280,720,1600,0},true,"The previous group is not back in place");
    w.reject=nullptr;require(session.resizeGroup(std::vector<DisplayV2Output>{{"0",1920,1080,0,0},{"1",1280,720,1920,0}},deadline(w)).ok,"A change after the fallback latched");
    require(session.restore().outcome==DisplayV2Outcome::restored&&!m.bytes,"Stop after the fallback did not restore");requireDual(w,true,true,"Stop after the fallback did not restore the original");++passed;}
  return passed;
}
int configCases(){
  int passed=0;const auto parse=[](const char* text){return JsonParser::parse(text);};
  require(displayManagerV2FromConfig({},{}),"v2 is not the default");++passed;
  require(!displayManagerV2FromConfig(parse(R"({"displayManager":"v1"})"),{}),"settings v1 ignored");++passed;
  require(displayManagerV2FromConfig(parse(R"({"displayManager":"v2"})"),parse(R"({"displayManager":"v1"})")),"settings did not take precedence");++passed;
  require(!displayManagerV2FromConfig(parse(R"({"desktopId":"d"})"),parse(R"({"displayManager":"v1"})")),"native-display.json v1 ignored");++passed;
  require(!displayManagerV2FromConfig(parse(R"({"displayManager":"v3"})"),parse(R"({"displayManager":"v1"})"))&&displayManagerV2FromConfig(parse(R"({"displayManager":7})"),{}),"Unknown values were not skipped");++passed;
  return passed;
}
int fileCases(){
  int passed=0;
  const auto root=std::filesystem::current_path()/L".artifacts"/(L"display-v2-qa-"+temporarySuffix());std::filesystem::create_directories(root);
  struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}}cleanup{root};
  const auto trusted=[root](const std::filesystem::path& value){const auto full=std::filesystem::absolute(value).lexically_normal();const DWORD attributes=GetFileAttributesW(full.c_str());const auto relative=full.lexically_relative(root);return attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT)&&(full==root||(!relative.empty()&&*relative.begin()!=L".."));};
  World w;DisplayV2State state{"s",displayV2ActiveCcd(w.query()),{w.target()},DisplayV2Console{consoleAdapter,consoleAdapter,0,consoleTarget}};
  {auto store=displayV2FileStore(root/L"display-v2.state",trusted);
    require(!store.load()&&store.save(displayV2StateBytes(state))&&store.load()==std::optional(displayV2StateBytes(state))&&displayV2StatePresent(root/L"display-v2.state"),"File store save/load failed");
    require(store.remove()&&!store.load()&&!displayV2StatePresent(root/L"display-v2.state")&&store.remove(),"File store remove failed or was not idempotent");
    require(store.save("garbage"),"Fixture save failed");store.discard();require(!store.load()&&std::filesystem::exists(root/L"display-v2.state.bad"),"Discard did not move the bad state aside");++passed;}
  // v1 journal retirement: archived, state created from the v1 profile.
  {const auto directory=root/L"private";std::filesystem::create_directories(directory);
    for(const auto* name:displayV1JournalFiles){std::ofstream file(directory/name,std::ios::binary);file<<"{\"v1\":true}";}
    auto store=displayV2FileStore(directory/L"display-v2.state",trusted);int extracted=0;
    const auto result=migrateDisplayV1Leftovers(directory,store,[&](const std::string& bytes){++extracted;require(bytes=="{\"v1\":true}","Wrong v1 profile bytes");return std::optional(state);},trusted,L"20261025-000000-abcdef01");
    require(result.found&&result.archived&&result.stateCreated&&extracted==1&&store.load()==std::optional(displayV2StateBytes(state)),"v1 profile was not migrated into v2 state");
    for(const auto* name:displayV1JournalFiles)require(!std::filesystem::exists(directory/name)&&std::filesystem::exists(directory/L"display-v1-archive"/L"20261025-000000-abcdef01"/name),"v1 journal was not archived");
    require(!migrateDisplayV1Leftovers(directory,store,{},trusted,L"20261025-000001-abcdef01").found,"Migration repeated");++passed;
    // Unparseable profile: archived, no state, never blocks.
    (void)store.remove();{std::ofstream file(directory/displayV1JournalFiles[0],std::ios::binary);file<<"not json";}
    const auto second=migrateDisplayV1Leftovers(directory,store,[](const std::string&)->std::optional<DisplayV2State>{throw std::runtime_error("bad v1");},trusted,L"20261025-000002-abcdef01");
    require(second.found&&second.archived&&!second.stateCreated&&!store.load(),"Unparseable v1 profile blocked retirement");++passed;
  }
  return passed;
}
}
// Resize stage timing (tuning backlog #1): the SDK call and Windows' settling
// are timed apart, and a stream's faster poll finds a recreated target sooner.
// World time only: the SDK call takes 120 ms, Windows lists the new target
// 130 ms after it. A pass is control-logic timing, not a measured real resize.
int stageTimingCases(){
  int passed=0;
  const auto run=[](std::optional<uint64_t> pollMs){
    World w;Memory m;auto a=access(w,m);if(pollMs)a.poll=[&w,step=*pollMs]{w.clock+=step;};
    DisplaySessionV2 session(std::move(a));
    require(session.start("s",1920,1080,true,deadline(w)).ok,"Timing fixture start failed");
    w.sdkMs=120;w.arrivalMs=130;const auto began=w.clock;
    const auto resized=session.resize(1600,900,deadline(w));
    require(resized.ok,"Timing fixture resize failed");requireStreaming(w,1600,900,true,"Timing fixture left the owned output wrong");
    return std::tuple{resized.sdkMs,resized.settleMs,w.clock-began};
  };
  const auto [slowSdk,slowSettle,slowTotal]=run(std::nullopt);
  require(slowSdk==120,"The SDK size call was not timed apart");
  // 50 ms reads see the target at the first read at or after 130 ms: 150 ms.
  require(slowSettle==150&&slowTotal==slowSdk+slowSettle,"Windows settling was not timed after the SDK call");++passed;
  const auto [fastSdk,fastSettle,fastTotal]=run(displayV2StreamPollMs);
  require(fastSdk==120&&fastSettle==135&&fastTotal==fastSdk+fastSettle,"The stream poll did not find the recreated target sooner");++passed;
  // An unchanged size makes no SDK call and settles at once.
  {World w;Memory m;DisplaySessionV2 session(access(w,m));require(session.start("s",1920,1080,true,deadline(w)).ok,"Fixture start failed");
    const auto same=session.resize(1920,1080,deadline(w));require(same.ok&&same.sdkMs==0&&same.settleMs==0,"An unchanged size reported SDK or settling time");++passed;}
  return passed;
}
int main(int argc,char** argv){
  try{
    if(argc==2&&std::string(argv[1])=="--sdk-ccd-mismatch"){sdkCcdMismatchCases();return 0;}
    if(argc==2&&std::string(argv[1])=="--primary-relayout"){primaryRelayoutCase();return 0;}
    int passed=0;
    passed+=preparedHandoffCases();
    passed+=stoppingServiceCleanupCases();
    passed+=primaryRelayoutCase();
    passed+=sdkCcdMismatchCases();
    passed+=stageTimingCases();
    passed+=happyPathCases();
    passed+=positionToleranceCases();
    passed+=foreignChangeCases();
    passed+=inactiveConsoleCases();
    passed+=crashRecoveryCases();
    passed+=restoreFailureCases();
    passed+=restoreVerificationCases();
    passed+=noLatchCases();
    passed+=codecCases();
    passed+=planCases();
    passed+=rpcCases();
    passed+=groupCases();
    passed+=groupPlanCases();
    passed+=groupRpcCases();
    passed+=trackPacketCases();
    passed+=interleaveCases();
    passed+=retryCases();
    passed+=groupFallbackCases();
    passed+=rapidChangeCases();
    passed+=configCases();
    passed+=fileCases();
    std::cout<<"Display manager v2 checks passed: "<<passed<<"\n";return 0;
  }catch(const std::exception& error){std::cerr<<"Display manager v2 check failed: "<<error.what()<<"\n";return 1;}
}
