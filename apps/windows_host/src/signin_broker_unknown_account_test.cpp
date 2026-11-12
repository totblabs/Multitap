// Pure/injected facts through real state/channel/native admission functions.
// No observed token, desktop, WTS query, pixel, input or launched process.
#include "signin_worker_channel.hpp"
#include <iostream>
#include <stdexcept>
using host::Json;
using host::JsonParser;
using namespace host::signin;
namespace {
unsigned checks=0;
void expect(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
Context context(){
  Context c;c.stream[0]=1;c.service={101,1001};c.serviceIsLocalSystem=true;
  c.lease={"ROOT\\DISPLAY\\0000",7};c.leaseOwned=true;c.baseline[0]=2;c.nonce[0]=3;
  c.consoleSession=3;c.assigned=c.clientOptIn=c.clientTransitions=c.signinRuntimeQualified=true;
  c.unknownAccountAllowed=true;return c;
}
Scope noUser(uint64_t generation=1){return {3,generation,1000,WindowsState::noUser,Desktop::winlogon,{}};}
Binding bound(const State& s,Process worker){const auto& c=s.context();return {c.stream,c.service,worker,c.lease,c.baseline,c.nonce,c.consoleSession,c.workerGeneration,c.mediaEpoch,s.role()};}
void admissionAndWire(){
  const auto c=context();const auto scope=noUser();
  State state;expect(bool(state.begin(c,Role::signin,scope,1000)),"explicit empty-account noUser admitted");
  State ordinary;expect(!ordinary.begin(c,Role::user,scope,1000),"empty ordinary user refused");
  auto locked=scope;locked.windows=WindowsState::locked;locked.signedInSid="S-1-5-21-1-2-3-1001";
  State refused;expect(!refused.begin(c,Role::signin,locked,1000),"unknown account never authorizes locked known account");
  for(unsigned n=0;n<6;++n){auto bad=c;auto view=scope;
    if(n==0)bad.unknownAccountAllowed=false;
    else if(n==1)bad.configuredSid="S-1-5-21-1-2-3-1001";
    else if(n==2)view.signedInSid="S-1-5-18";
    else if(n==3)view.desktop=Desktop::normal;
    else if(n==4)view.observedAt=1;
    else view.windows=WindowsState::unknown;
    State denied;expect(!denied.begin(bad,Role::signin,view,3000),"contradictory/stale/normal/unknown context refused");
  }
  const Binding b{c.stream,c.service,{202,2002},c.lease,c.baseline,c.nonce,c.consoleSession,c.workerGeneration,c.mediaEpoch,Role::signin};
  native::OwnedTarget target{1,0,0,1,1280,720,0,0,1};
  channel::Init init{c,b,target,99};const auto wire=channel::initPacket(init);
  expect(wire.find("\"unknownAccountAllowed\":true")!=std::string::npos,"explicit bool is carried on authenticated private channel");
  expect(channel::parseInit(JsonParser::parse(wire)).context.unknownAccountAllowed,"explicit unknown context survives strict parser");
  for(unsigned n=0;n<3;++n){auto bad=JsonParser::parse(wire);auto& object=std::get<Json::Object>(bad.value);
    if(n==0)object.erase("unknownAccountAllowed");
    else if(n==1)object["unknownAccountAllowed"].value=1.0;
    else object["configuredSid"].value=std::string("S-1-5-18");
    bool denied=false;try{(void)channel::parseInit(bad);}catch(...){denied=true;}
    expect(denied,"empty SID requires exact explicit true, never coercion/SYSTEM");
  }
  native::ConsoleFacts console{3,WindowsState::noUser,{},1000,true,true};
  expect(bool(native::checkConsole(c,console,1000)),"real native admission allows proven noUser");
  console.windows=WindowsState::locked;console.signedInSid=locked.signedInSid;
  expect(!native::checkConsole(c,console,1000),"native gate independently refuses unknown-account locked console");
  console.windows=WindowsState::unlocked;
  expect(native::checkConsole(c,console,1000).outcome==native::Outcome::handoffRequired,"unlock requires ordinary handoff, not SYSTEM Default");
}
void exactExitBeforeFreshKnownUser(){
  State state;auto c=context();auto live=noUser();const Process old{202,2002},next{303,3003};
  expect(bool(state.begin(c,Role::signin,live,1000)),"unknown sign-in starts");
  expect(bool(state.registerLaunch({old,true,true})),"exact sign-in child registered");
  const auto oldBinding=bound(state,old);
  expect(bool(state.workerReady({oldBinding,live,Token::system,"S-1-5-18",true},1000)),"SYSTEM binding only on Winlogon");
  expect(bool(state.firstFrame({oldBinding,live,1,1,true,true,true},1000)),"symbolic qualified first-frame contract");
  expect(bool(state.decoderReady({oldBinding,1,1,true})),"symbolic decoder contract activates");
  auto unlocked=live;unlocked.generation=2;unlocked.windows=WindowsState::unlocked;
  unlocked.desktop=Desktop::normal;unlocked.signedInSid="S-1-5-21-1-2-3-1001";
  Nonce fresh{};fresh[0]=4;
  auto stale=unlocked;stale.observedAt=1;
  expect(!state.handoff(Role::user,stale,fresh,3000),"stale WTS scope cannot adopt SID");
  auto system=unlocked;system.signedInSid="S-1-5-18";
  expect(!state.handoff(Role::user,system,fresh,1000),"SYSTEM never becomes ordinary user identity");
  expect(bool(state.handoff(Role::user,unlocked,fresh,1000)),"fresh native unlocked SID selected for ordinary phase");
  expect(!state.context().unknownAccountAllowed&&state.context().configuredSid==unlocked.signedInSid,
      "unknown privilege retired and exact observed SID sealed");
  expect(state.context().workerGeneration==2&&state.context().mediaEpoch==2,"new generation and epoch precede ordinary stream");
  expect(!state.inputAllowed({oldBinding},unlocked,1000),"old secure binding cannot send input");
  expect(!state.registerLaunch({next,true,true}),"new worker cannot launch while old child alive");
  expect(!state.workerExited({{old.pid,old.created+1},true,true}),"PID without exact birth cannot retire old worker");
  expect(!state.workerExited({old,true,false}),"wait acknowledgment without actual exit cannot retire old worker");
  expect(bool(state.workerExited({old,true,true})),"exact physical-exit proof frees next launch");
  expect(bool(state.registerLaunch({next,true,true})),"new ordinary worker starts after exact exit");
  const auto nextBinding=bound(state,next);
  expect(!state.workerReady({nextBinding,unlocked,Token::system,"S-1-5-18",true},1000),"SYSTEM Default cannot satisfy ordinary binding");
  expect(bool(state.workerReady({nextBinding,unlocked,Token::user,unlocked.signedInSid,true},1000)),"exact fresh user token binds ordinary worker");
}
}
int main(){try{admissionAndWire();exactExitBeforeFreshKnownUser();std::cout<<checks<<" unknown-account checks passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
