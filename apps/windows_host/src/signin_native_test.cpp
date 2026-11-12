#include "signin_capture_dxgi.hpp"
#include <iostream>
#include <stdexcept>

using namespace host::signin;
using namespace host::signin::native;
namespace {
unsigned checks=0;
void expect(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
Status success(){return {Outcome::ready,Step::none,0};}
Context context(){
  Context c;c.stream[0]=1;c.service={100,1000};c.serviceIsLocalSystem=true;
  c.lease={"ROOT\\DISPLAY\\0000",7};c.leaseOwned=true;c.baseline[0]=2;
  c.consoleSession=1;c.workerGeneration=3;c.mediaEpoch=5;c.ownedOutputMask=1;
  c.configuredSid="S-1-5-21-1-2-3-1001";c.nonce[0]=3;
  c.assigned=c.clientOptIn=c.clientTransitions=c.signinRuntimeQualified=true;return c;
}
Binding binding(const Context& c){return {c.stream,c.service,{200,2000},c.lease,c.baseline,c.nonce,c.consoleSession,c.workerGeneration,c.mediaEpoch,Role::signin};}
ConsoleFacts locked(const Context& c){return {c.consoleSession,WindowsState::locked,c.configuredSid,10000,true,true};}
WorkerFacts worker(const Binding& b){return {b.worker,b.consoleSession,true,true,true,true};}
struct FakeCapture final:CaptureAccess {
  unsigned acquired=0,readCount=0,released=0;std::string calls;
  Status acquireResult=success(),readResult=success(),releaseResult=success();
  bool throwRead=false,badShape=false;
  Status acquire(uint32_t)override{++acquired;calls+='A';return acquireResult;}
  Status read(BgraFrame& frame)override{++readCount;calls+='R';if(throwRead)throw std::runtime_error("Synthetic readback exception");
    frame.width=16;frame.height=16;frame.pixels.assign(badShape?3:1024,74);return readResult;}
  Status release()noexcept override{++released;calls+='F';return releaseResult;}
};
void nativeFacts(){
  const auto c=context();const auto b=binding(c);const auto console=locked(c);const auto child=worker(b);
  expect(bool(checkConsole(c,console,10000)),"Locked configured console should be eligible");
  expect(bool(checkWorker(b,c,console,child,10000)),"Exact synthetic native binding should pass");
  auto noUser=console;noUser.windows=WindowsState::noUser;noUser.signedInSid.clear();
  expect(bool(checkConsole(c,noUser,10000)),"Proven empty no-user console should be eligible");
  noUser.signedInSid=c.configuredSid;expect(!checkConsole(c,noUser,10000),"No-user with a SID is contradictory");
  auto uac=console;uac.windows=WindowsState::unlocked;
  expect(checkWorker(b,c,uac,child,10000).outcome==Outcome::handoffRequired,"Unlocked UAC must not gain SYSTEM control");
  auto unknown=console;unknown.windows=WindowsState::unknown;expect(!checkConsole(c,unknown,10000),"Unknown session is not no-user");
  auto changed=console;changed.session=2;expect(!checkConsole(c,changed,10000),"Another console must be denied");
  changed=console;changed.consoleProtocol=false;expect(!checkConsole(c,changed,10000),"Remote protocol must be denied");
  changed=console;changed.valid=false;expect(!checkConsole(c,changed,10000),"Missing native facts must be denied");
  changed=console;changed.signedInSid="S-1-5-21-9-9-9-1001";expect(!checkConsole(c,changed,10000),"Another signed-in account must be denied");
  expect(bool(checkConsole(c,console,11000)),"Declared one-second scope boundary must remain inclusive");
  expect(!checkConsole(c,console,11001),"Stale scope must be denied");
  expect(!checkConsole(c,console,9999),"Future scope must be denied");
  for(unsigned flag=0;flag<11;++flag){auto bad=c;
    switch(flag){case 0:bad.assigned=false;break;case 1:bad.clientOptIn=false;break;case 2:bad.clientTransitions=false;break;case 3:bad.signinRuntimeQualified=false;break;case 4:bad.serviceIsLocalSystem=false;break;case 5:bad.ownedOutputMask=3;break;case 6:bad.mediaEpoch=0;break;case 7:bad.workerGeneration=0;break;case 8:bad.nonce={};break;case 9:bad.baseline={};break;default:bad.leaseOwned=false;break;}
    expect(!checkConsole(bad,console,10000),"Missing authority/negotiation/composite scope must be denied");
  }
  for(const auto* system:{"S-1-5-18","S-1-5-19","S-1-5-20"}){auto bad=c;bad.configuredSid=system;auto fake=console;fake.signedInSid=system;expect(!checkConsole(bad,fake,10000),"A service identity cannot be the configured Windows user");}
  for(unsigned field=0;field<14;++field){auto bad=b;
    switch(field){case 0:++bad.stream[0];break;case 1:++bad.service.pid;break;case 2:++bad.service.created;break;case 3:++bad.worker.pid;break;case 4:++bad.worker.created;break;case 5:++bad.lease.generation;break;case 6:bad.lease.instance="ROOT\\DISPLAY\\0001";break;case 7:++bad.baseline[0];break;case 8:++bad.nonce[0];break;case 9:++bad.consoleSession;break;case 10:++bad.generation;break;case 11:++bad.mediaEpoch;break;case 12:bad.role=Role::user;break;default:bad.worker=bad.service;break;}
    expect(!checkWorker(bad,c,console,child,10000),"Stale/rebound worker binding must be denied");
  }
  for(unsigned field=0;field<5;++field){auto bad=child;
    switch(field){case 0:bad.localSystem=false;break;case 1:bad.session=2;break;case 2:bad.stationWinSta0=false;break;case 3:bad.threadWinlogon=false;break;default:bad.inputWinlogon=false;break;}
    expect(!checkWorker(b,c,console,bad,10000),"Wrong token/session/input desktop must be denied");
  }
}
void frames(){
  expect(validFrameShape(1920,1080,7680,size_t(7680)*1080),"Bounded BGRA shape should pass");
  expect(validFrameShape(16,16,128,128*15+64),"Padded rows need only actual final-row bytes");
  expect(!validFrameShape(16,16,63,1024),"A short row pitch must be denied");
  expect(!validFrameShape(16,16,64,1023),"Truncated frame must be denied");
  expect(!validFrameShape(UINT32_MAX,16,UINT32_MAX,1024),"Overflow width must be denied");
  expect(!validFrameShape(8192,8192,32768,size_t(32768)*8192),"Frame memory ceiling must be enforced");
  BgraFrame frame;unsigned observed=0;
  FakeCapture normal;
  expect(bool(acquireValidatedFrame(normal,[&]{++observed;return success();},frame,16)),"A synthetic frame with two valid observations should pass");
  expect(normal.calls=="ARF"&&observed==2&&frame.pixels.size()==1024,"Successful frame must read and release exactly once");
  FakeCapture before;
  expect(!acquireValidatedFrame(before,[]{return Status{Outcome::refused,Step::lease,0};},frame,16)&&before.acquired==0&&frame.pixels.empty(),"Invalid scope must prevent acquisition");
  FakeCapture timeout;timeout.acquireResult={Outcome::noFrame,Step::acquire,1};
  expect(acquireValidatedFrame(timeout,success,frame,16).outcome==Outcome::noFrame&&timeout.readCount==0&&timeout.released==0,"Timeout is not a held frame");
  FakeCapture lost;lost.acquireResult={Outcome::handoffRequired,Step::acquire,1};
  expect(acquireValidatedFrame(lost,success,frame,16).outcome==Outcome::handoffRequired&&lost.released==0,"Access-lost must request handoff without random rebind");
  FakeCapture readFailure;readFailure.readResult={Outcome::failed,Step::readback,1};
  expect(!acquireValidatedFrame(readFailure,success,frame,16)&&readFailure.released==1&&frame.pixels.empty(),"Readback failure must release and discard");
  FakeCapture malformed;malformed.badShape=true;
  expect(!acquireValidatedFrame(malformed,success,frame,16)&&malformed.released==1&&frame.pixels.empty(),"Malformed frame must release and discard");
  FakeCapture switched;observed=0;
  expect(acquireValidatedFrame(switched,[&]{return ++observed==1?success():Status{Outcome::handoffRequired,Step::desktop,0};},frame,16).outcome==Outcome::handoffRequired&&switched.released==1&&frame.pixels.empty(),"Desktop switch after readback must discard pixels and release");
  FakeCapture throwRead;throwRead.throwRead=true;
  expect(!acquireValidatedFrame(throwRead,success,frame,16)&&throwRead.released==1,"Exception after acquisition must release");
  FakeCapture releaseFailure;releaseFailure.releaseResult={Outcome::handoffRequired,Step::release,1};
  expect(acquireValidatedFrame(releaseFailure,success,frame,16).outcome==Outcome::handoffRequired&&releaseFailure.released==1&&frame.pixels.empty(),"Failed release may not publish a frame");
  FakeCapture excessive;
  expect(!acquireValidatedFrame(excessive,success,frame,51)&&excessive.acquired==0,"An unbounded acquisition wait must be refused");
  FakeCapture missing;
  expect(!acquireValidatedFrame(missing,{},frame,16)&&missing.acquired==0,"Missing native scope callback must fail closed");
}
}
int main(){try{
  nativeFacts();frames();
  std::cout<<"{\"kind\":\"signin-native-stage2-injected-contract-tests\",\"checks\":"<<checks
    <<",\"productionAdaptersLinked\":true,\"productionAdaptersInvoked\":false,\"actualTokenOrDesktopOperations\":0,\"processesSpawned\":0,\"gpuDevicesCreated\":0,\"captureCalls\":0,\"inputCalls\":0,\"runtimeQualified\":false}\n";
  return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
