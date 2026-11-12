#include "signin_restoration.hpp"
#include "../tests/display-v2-world.hpp"
#include <cstdio>
#include <cstdlib>

using namespace host;
using namespace host::signin;
using namespace host::signin::restoration;
namespace {
unsigned checks=0;
void expect(bool value,const char* message){++checks;if(!value){std::fprintf(stderr,"FAILED: %s\n",message);std::exit(1);}}
template<class F>void rejects(F&& f,const char* message){bool caught=false;try{f();}catch(...){caught=true;}expect(caught,message);}
std::string uuid(StreamId value){constexpr char digits[]="0123456789abcdef";std::string out;for(size_t i=0;i<value.size();++i){if(i==4||i==6||i==8||i==10)out+='-';out+=digits[value[i]>>4];out+=digits[value[i]&15];}return out;}
struct Parent final:Authority {
  v2world::World world{true};v2world::Memory memory;
  RestorationAdmission admission;RestorationBinding bound;OriginalProof saved;
  uint32_t generation=9;uint64_t serial=0;
  bool predecessorsExited=true,currentOwner=true,originalAvailable=true,refuseSize=false;
  unsigned sizeRequests=0;
  explicit Parent(bool console=false,bool sdkMismatch=false,bool ownedInitiallyInactive=false){
    admission.service={101,10001};admission.stream.fill(0x24);admission.operation.fill(0x35);admission.installedImageSha256.fill(0x46);
    admission.predecessors={{202,20002},{303,30003}};admission.consoleSession=4;
    if(sdkMismatch){world.owned().width=1280;world.owned().height=720;world.extra().x=2304;}
    if(ownedInitiallyInactive){world.owned().active=false;world.extra().x=1024;}
    DisplaySessionV2 prepare(v2world::worldAccess(world,memory,console));
    const auto modeWidth=sdkMismatch?1600u:1280u,modeHeight=sdkMismatch?900u:720u;
    expect(prepare.start(uuid(admission.stream),modeWidth,modeHeight,console,v2world::deadline(world)).ok,"actual V2 fixture prepared original before stream");
    expect(memory.bytes.has_value(),"fixture has one retained original");
    saved={*memory.bytes,nativeBaselineDigest(*memory.bytes)};admission.originalSha256=saved.sha256;
    expect(prepare.retainForHandoff(saved.sha256,[]{return true;}),"fixture old worker retains original, no recapture");
    admission.lease={"ROOT\\DISPLAY\\0000",generation};admission.issuedAt=world.clock;admission.expiresAt=world.clock+30000;
    bound={admission.service,{404,40004},admission.stream,admission.operation,admission.consoleSession};
  }
  std::optional<LeaseProof> current(const RestorationAdmission&,const RestorationBinding&)const override{
    if(!predecessorsExited||!currentOwner)return{};return LeaseProof{{admission.lease.instance,generation},world.targets()};
  }
  std::optional<OriginalProof> original(const RestorationAdmission&,const RestorationBinding&)const override{return originalAvailable?std::optional<OriginalProof>(saved):std::nullopt;}
  std::optional<LeaseProof> sizeOriginal(const RestorationAdmission&,const RestorationBinding&)override{
    ++sizeRequests;if(refuseSize)return{};
    const auto state=parseDisplayV2State(saved.bytes);uint32_t width=state.owned[0].width,height=state.owned[0].height;
    for(const auto& path:state.ccd.paths)if(displayV2Active(path)&&nativeSameTarget(path,state.owned[0])){const auto& source=nativeCcdSource(state.ccd,path);width=source.width;height=source.height;break;}
    if(!world.resize("0",width,height))return{};++generation;return LeaseProof{{admission.lease.instance,generation},world.targets()};
  }
  Receipt run(){
    auto access=v2world::worldAccess(world,memory);
    return restoreDisplay(admission,bound,std::move(access),[&](const Json& packet){return JsonParser::parse(answer(packet,admission,bound,*this,serial,world.clock));},[&]{return world.clock;});
  }
  LeaseProof lease()const{return{{admission.lease.instance,generation},world.targets()};}
};
std::string bindingBytes(const Parent& p){return jsonObject(JsonParser::parse(initPacket(p.admission,p.bound,99)).get("binding"));}
Json request(const Parent& p,const char* operation,uint64_t serial=1){return JsonParser::parse("{\"type\":\"signin.restore.request-v1\",\"protocol\":\"signin-private-restoration-v1\",\"binding\":"+bindingBytes(p)+",\"serial\":"+jsonQuote(std::to_string(serial))+",\"operation\":"+jsonQuote(operation)+",\"payload\":{}}");}
void actualV2Restore(bool console){
  Parent p(console);const auto original=p.saved.bytes;const int saves=p.memory.saves,removes=p.memory.removes,discards=p.memory.discards;
  const auto receipt=p.run();
  expect(receipt.outcome==Outcome::topologyRestoredOriginalHeld,"actual V2 restore yields distinct held-original topology outcome");
  expect(receipt.underlyingOutcome==DisplayV2Outcome::failed&&receipt.stage==displayV2Stage::state&&!receipt.error,"underlying denied retirement outcome remains verbatim");
  expect(receipt.fallback==(console?1u:0u),"positive console already active preserves underlying fallback1 without faking success");
  expect(p.saved.bytes==original&&p.memory.bytes==original,"restorer retains exact original bytes");
  expect(p.memory.saves==saves&&p.memory.removes==removes&&p.memory.discards==discards,"no new baseline, removal, discard or local store authority");
  expect(p.sizeRequests==1&&p.world.ownedWidth==1920&&p.world.ownedHeight==1080,"one parent-derived original-size request");
  v2world::requireOriginal(p.world,true,"all original foreign and owned modes restored");++checks;
  expect(originalLayoutMatches(original,receipt.observedState),"fresh semantic full-layout proof");
  expect(verifiedReceipt(p.admission,p.bound,p.saved,p.lease(),receipt,p.world.clock),"parent can independently verify receipt against actual current lease/original");
  const auto round=parseReceipt(JsonParser::parse(receiptPacket(receipt)));
  expect(round.binding==p.bound&&round.originalSha256==receipt.originalSha256&&round.fallback==receipt.fallback&&round.observedState==receipt.observedState,"strict receipt codec preserves full proof and underlying partial outcome");
}
void admissionAndCommand(){
  Parent p;expect(validAdmission(p.admission,p.world.clock),"cleanup admission has no operator-live or capture Context flags");
  for(unsigned i=0;i<13;++i){auto a=p.admission;
    switch(i){case 0:a.service.created=0;break;case 1:a.stream={};break;case 2:a.operation={};break;case 3:a.installedImageSha256={};break;
      case 4:a.predecessors.resize(5,a.predecessors[0]);break;case 5:a.predecessors.push_back(a.predecessors[0]);break;case 6:a.predecessors[0]=a.service;break;
      case 7:a.consoleSession=0;break;case 8:a.originalSha256="bad";break;case 9:a.lease.generation=0;break;
      case 10:a.issuedAt=p.world.clock+1;break;case 11:a.expiresAt=a.issuedAt+60001;break;case 12:a.expiresAt=a.issuedAt;break;}
    expect(!validAdmission(a,p.world.clock),"malformed/unknown/expired cleanup obligation refused");
  }
  // A leftover original whose stream worker already ended with its session
  // has no predecessor left to hold; that obligation is valid.
  {auto a=p.admission;a.predecessors.clear();expect(validAdmission(a,p.world.clock),"leftover original without predecessors admitted");}
  const std::wstring pipe=L"\\\\.\\pipe\\MultitapHost.SigninRestore.101."+std::wstring(64,L'a');
  const wchar_t* argv[]{L"Host",L"--signin-restorer-v1",L"--pipe",pipe.c_str(),L"--service-pid",L"101"};
  expect(parseCommand(6,argv).has_value(),"fixed restoration-only command accepted");
  expect(!parseCommand(7,argv)&&!parseCommand(5,argv),"extra/truncated CLI refused before reading arguments");
  const wchar_t* bad[]{L"Host",L"--signin-preparer-v1",L"--pipe",pipe.c_str(),L"--service-pid",L"101"};
  expect(!parseCommand(6,bad),"preparation cannot masquerade as restoration");bad[1]=L"--signin-restorer-v1";bad[5]=L"0101";
  expect(!parseCommand(6,bad),"PID aliases rejected");bad[5]=L"102";expect(!parseCommand(6,bad),"pipe and parent PID must match");
  bad[5]=L"101";const std::wstring wrong=L"\\\\remote\\pipe\\MultitapHost.SigninRestore.101."+std::wstring(64,L'a');bad[3]=wrong.c_str();expect(!parseCommand(6,bad),"remote pipe rejected");
  auto reused=p.bound;reused.worker=p.admission.predecessors[0];expect(!sameBinding(reused,p.admission,reused.worker),"old predecessor lifetime cannot become new restorer");
}
void deniedWorkPreservesOriginal(){
  for(unsigned failure=0;failure<5;++failure){Parent p;const auto original=p.saved.bytes;const int applies=p.world.applies,resizes=p.world.resizes;
    if(failure==0)p.predecessorsExited=false;else if(failure==1)p.originalAvailable=false;else if(failure==2)p.currentOwner=false;else if(failure==3)p.world.eligible=false;else p.admission.originalSha256=std::string(64,'a');
    const auto receipt=p.run();expect(receipt.outcome!=Outcome::topologyRestoredOriginalHeld,"unexited owner/unknown baseline/revoked scope cannot restore");
    expect(p.world.applies==applies&&p.world.resizes==resizes,"denial before SDK/CCD mutation");expect(p.saved.bytes==original&&p.memory.bytes==original,"uncertainty retains exact protected original");
  }
  Parent failed;failed.refuseSize=true;const auto original=failed.saved.bytes;const auto receipt=failed.run();
  expect(receipt.outcome!=Outcome::topologyRestoredOriginalHeld&&failed.saved.bytes==original,"SDK failure is not full recovery or original deletion");
}
void originalSdkMismatch(){
  Parent p(false,true);const auto before=parseDisplayV2State(p.saved.bytes);
  expect(before.owned[0].width==1920,"fixture SDK original nominal width differs from actual original CCD mode");
  const auto receipt=p.run();expect(receipt.outcome==Outcome::topologyRestoredOriginalHeld,"original active CCD mode preferred over stale SDK nominal size");
  expect(p.world.ownedWidth==1280&&p.world.ownedHeight==720&&originalLayoutMatches(p.saved.bytes,receipt.observedState),"parent derives exact original CCD mode with no peer width or height");
}
void semanticNegatives(){
  Parent p(true);const auto receipt=p.run();expect(receipt.outcome==Outcome::topologyRestoredOriginalHeld,"semantic negatives start from real complete fixture");
  for(unsigned failure=0;failure<10;++failure){auto observed=parseDisplayV2State(receipt.observedState);
    auto foreign=std::find_if(observed.ccd.paths.begin(),observed.ccd.paths.end(),[](const auto& path){return displayV2Active(path)&&nativeSameLuid(path.targetInfo.adapterId,v2world::extraAdapter);});
    auto owned=std::find_if(observed.ccd.paths.begin(),observed.ccd.paths.end(),[&](const auto& path){return displayV2Active(path)&&nativeSameTarget(path,observed.owned[0]);});
    expect(foreign!=observed.ccd.paths.end()&&owned!=observed.ccd.paths.end(),"semantic fixture identities are actual parsed paths");
    if(failure==0)foreign->flags&=~DISPLAYCONFIG_PATH_ACTIVE;
    else if(failure==1)observed.ccd.modes[nativeSourceIndex(*foreign)].sourceMode.width+=2;
    else if(failure==2)foreign->targetInfo.refreshRate.Numerator=60;
    else if(failure==3)foreign->targetInfo.rotation=DISPLAYCONFIG_ROTATION_ROTATE90;
    else if(failure==4)observed.ccd.modes[nativeSourceIndex(*foreign)].sourceMode.pixelFormat=DISPLAYCONFIG_PIXELFORMAT_16BPP;
    else if(failure==5)observed.ccd.modes[nativeSourceIndex(*owned)].sourceMode.position.x+=2;
    else if(failure==6)owned->targetInfo.refreshRate.Numerator=75;
    else if(failure==7)owned->targetInfo.rotation=DISPLAYCONFIG_ROTATION_ROTATE180;
    else if(failure==8)observed.ccd.modes[nativeSourceIndex(*owned)].sourceMode.pixelFormat=DISPLAYCONFIG_PIXELFORMAT_16BPP;
    else for(auto& path:observed.ccd.paths)if(nativeSameLuid(path.targetInfo.adapterId,v2world::consoleAdapter))path.flags&=~DISPLAYCONFIG_PATH_ACTIVE;
    observed.ccd=displayV2ActiveCcd(observed.ccd);
    expect(!originalLayoutMatches(p.saved.bytes,displayV2StateBytes(observed)),"missing foreign/console or wrong mode/owned geometry cannot qualify full restore");
  }
  auto repositioned=parseDisplayV2State(receipt.observedState);
  for(auto& path:repositioned.ccd.paths)if(displayV2Active(path)&&nativeSameLuid(path.targetInfo.adapterId,v2world::extraAdapter))repositioned.ccd.modes[nativeSourceIndex(path)].sourceMode.position.x+=2000;
  expect(originalLayoutMatches(p.saved.bytes,displayV2StateBytes(repositioned)),"foreign reposition remains allowed while exact modes and owned geometry are retained");
  for(unsigned failure=0;failure<8;++failure){auto altered=receipt;auto lease=p.lease();
    if(failure==0)altered.error=87;else if(failure==1)altered.stage=displayV2Stage::apply;else if(failure==2)altered.fallback=2;
    else if(failure==3)altered.observedAt=p.world.clock+1;else if(failure==4)altered.observedAt=p.world.clock>2001?p.world.clock-2001:1;
    else if(failure==5)++lease.lease.generation;else if(failure==6)++lease.targets[0].target;else altered.observedState.clear();
    const auto now=failure==4?altered.observedAt+2001:p.world.clock;
    expect(!verifiedReceipt(p.admission,p.bound,p.saved,lease,altered,now),"native error87/stageApply/fallbackfailure/stale/wrong actual lease/absent observation cannot become full success");
  }
}
void inactiveObservationPaths(){
  Parent p;
  p.world.displays.push_back({{99,0},0,99,true,false,0,0,800,600,60,1,DISPLAYCONFIG_ROTATION_IDENTITY,800,600});
  const auto receipt=p.run();
  expect(receipt.outcome==Outcome::topologyRestoredOriginalHeld,"inactive QDC_ALL_PATHS alternatives do not make canonical active observation fail");
  const auto parsed=parseDisplayV2State(receipt.observedState);
  expect(parsed.ccd.paths.size()==3&&p.world.query().paths.size()==4,"numeric observation projects actual active paths only");
  expect(verifiedReceipt(p.admission,p.bound,p.saved,p.lease(),receipt,p.world.clock),"inactive alternatives never become evidence of an extra active foreign display");
  Parent originallyInactive(false,false,true);const auto restored=originallyInactive.run();
  expect(restored.outcome==Outcome::topologyRestoredOriginalHeld&&!originallyInactive.world.owned().active,"saved inactive owned output remains inactive after restoring foreign topology");
  expect(originalLayoutMatches(originallyInactive.saved.bytes,restored.observedState),"full observation supports original owned-inactive state without inventing its active CCD path");
  originallyInactive.world.owned().active=true;originallyInactive.world.owned().x=1824;
  const auto wrong=displayV2StateBytes({uuid(originallyInactive.admission.stream),displayV2ActiveCcd(originallyInactive.world.query()),originallyInactive.world.targets(),{}});
  expect(!originalLayoutMatches(originallyInactive.saved.bytes,wrong),"unexpected active owned output cannot qualify original inactive state");
}
void protocolDenials(){
  Parent p;uint64_t sequence=0;const auto first=JsonParser::parse(answer(request(p,"original.read"),p.admission,p.bound,p,sequence,p.world.clock));
  expect(first.get("ok").boolean()&&first.get("payload").get("bytes").string()==p.saved.bytes,"only exact nonempty sealed original can be read");
  rejects([&]{(void)answer(request(p,"original.read"),p.admission,p.bound,p,sequence,p.world.clock);},"replayed serial rejected");
  for(const auto* op:{"original.save","original.remove","original.discard","size","count","prepare","input","capture","publish"}){
    uint64_t serial=0;const auto denied=JsonParser::parse(answer(request(p,op),p.admission,p.bound,p,serial,p.world.clock));expect(!denied.get("ok").boolean(),"unsupported mutation/input/capture operation denied");
  }
  auto chosen=request(p,"original.size");std::get<Json::Object>(chosen.value)["payload"]=JsonParser::parse("{\"width\":4096,\"height\":2160}");uint64_t serial=0;
  rejects([&]{(void)answer(chosen,p.admission,p.bound,p,serial,p.world.clock);},"peer cannot choose a restoration width or height");
  expect(p.sizeRequests==0,"invalid RPC never invokes parent SDK callback");
  auto incomplete=JsonParser::parse(receiptPacket(Receipt{p.bound}));
  std::get<Json::Object>(incomplete.value)["unexpected"]=Json{true};rejects([&]{(void)parseReceipt(incomplete);},"extra receipt fields refused");
}
void servicePhasePolicy(){
  using Purpose=native::detail::ServiceIdentityPurpose;
  for(const DWORD state:{DWORD(SERVICE_STOPPED),DWORD(SERVICE_START_PENDING),DWORD(SERVICE_STOP_PENDING),DWORD(SERVICE_RUNNING),
      DWORD(SERVICE_CONTINUE_PENDING),DWORD(SERVICE_PAUSE_PENDING),DWORD(SERVICE_PAUSED),DWORD(0),DWORD(UINT32_MAX)}){
    expect(native::detail::serviceIdentityPhaseAllowed(state,Purpose::ordinary)==(state==SERVICE_RUNNING),
      "ordinary capture/preparer identity remains RUNNING-only for every SCM phase");
    expect(native::detail::serviceIdentityPhaseAllowed(state,Purpose::sealedOriginalRestoration)==
      (state==SERVICE_RUNNING||state==SERVICE_STOP_PENDING),"sealed-original identity accepts only RUNNING/STOP_PENDING");
  }
  Parent p;p.predecessorsExited=false;
  const auto receipt=p.run();expect(receipt.outcome!=Outcome::topologyRestoredOriginalHeld&&p.sizeRequests==0,
    "cleanup service phase cannot bypass live predecessor exclusion");
  Parent unknown;unknown.originalAvailable=false;
  const auto denied=unknown.run();expect(denied.outcome!=Outcome::topologyRestoredOriginalHeld&&unknown.sizeRequests==0,
    "cleanup service phase cannot invent sealed original authority");
}
void cleanupDesktopPolicy(){
  Parent parent;const auto& b=parent.bound;const uint64_t now=10000;
  native::ConsoleFacts console{b.consoleSession,WindowsState::locked,"S-1-5-21-1-2-3-1001",now,true,true};
  native::WorkerFacts worker{b.worker,b.consoleSession,true,true,true,true};
  expect(cleanupDesktop(console,b.consoleSession,now)==CleanupDesktop::winlogon&&
    eligibleCleanupWorker(b,console,worker,now),"fresh locked cleanup requires actual Winlogon thread and input desktop");
  auto noUser=console;noUser.windows=WindowsState::noUser;noUser.signedInSid.clear();
  expect(eligibleCleanupWorker(b,noUser,worker,now),"known no-user cleanup uses Winlogon without configured-account flags");
  noUser.signedInSid=console.signedInSid;
  expect(!cleanupDesktop(noUser,b.consoleSession,now),"contradictory no-user observation rejected");
  auto unlocked=console;unlocked.windows=WindowsState::unlocked;
  expect(cleanupDesktop(unlocked,b.consoleSession,now)==CleanupDesktop::defaultDesktop&&
    !eligibleCleanupWorker(b,unlocked,worker,now),"old Winlogon child cannot continue cleanup after unlock");
  auto ordinaryDesktop=worker;ordinaryDesktop.threadWinlogon=ordinaryDesktop.inputWinlogon=false;
  ordinaryDesktop.threadDefault=ordinaryDesktop.inputDefault=true;
  expect(eligibleCleanupWorker(b,unlocked,ordinaryDesktop,now)&&!eligibleCleanupWorker(b,console,ordinaryDesktop,now),
    "fresh Default cleanup child is allowed only on actually unlocked console");
  for(unsigned field=0;field<8;++field){auto bad=ordinaryDesktop;
    switch(field){case 0:++bad.process.pid;break;case 1:++bad.process.created;break;case 2:++bad.session;break;
      case 3:bad.localSystem=false;break;case 4:bad.stationWinSta0=false;break;case 5:bad.threadDefault=false;break;
      case 6:bad.inputDefault=false;break;default:bad.inputWinlogon=true;break;}
    expect(!eligibleCleanupWorker(b,unlocked,bad,now),"wrong child/token/session/thread or secure UAC input desktop refuses cleanup");
  }
  for(unsigned field=0;field<7;++field){auto bad=unlocked;
    switch(field){case 0:bad.valid=false;break;case 1:bad.consoleProtocol=false;break;case 2:++bad.session;break;
      case 3:bad.windows=WindowsState::unknown;break;case 4:bad.observedAt=now+1;break;
      case 5:bad.observedAt=now-1001;break;default:bad.observedAt=0;break;}
    expect(!eligibleCleanupWorker(b,bad,ordinaryDesktop,now),"unknown/remote/stale/future console cannot authorize cleanup desktop");
  }
  expect(eligibleCleanupWorker(b,unlocked,ordinaryDesktop,now+1000)&&
    !eligibleCleanupWorker(b,unlocked,ordinaryDesktop,now+1001),"cleanup observation one-second boundary is exact");
  // Synthetic fully admitted CAPTURE context tests the actual unchanged gate.
  // These fixture flags are never a production diagnostic Context/conversion.
  Context capture;capture.service=b.service;capture.stream=b.stream;capture.lease=parent.admission.lease;
  capture.baseline[0]=1;capture.nonce[0]=2;capture.consoleSession=b.consoleSession;
  capture.workerGeneration=1;capture.mediaEpoch=1;capture.ownedOutputMask=1;capture.configuredSid=console.signedInSid;
  capture.assigned=capture.clientOptIn=capture.clientTransitions=capture.signinRuntimeQualified=true;
  capture.serviceIsLocalSystem=capture.leaseOwned=true;
  const Binding captureBinding{capture.stream,capture.service,b.worker,capture.lease,capture.baseline,capture.nonce,
    capture.consoleSession,capture.workerGeneration,capture.mediaEpoch,Role::signin};
  expect(bool(native::checkWorker(captureBinding,capture,console,worker,now)),"ordinary capture control still requires Winlogon");
  expect(!native::checkWorker(captureBinding,capture,console,ordinaryDesktop,now)&&
    !native::checkWorker(captureBinding,capture,unlocked,ordinaryDesktop,now),"Default cleanup observation never grants ordinary capture/input");
}
}
int main(){
  actualV2Restore(false);actualV2Restore(true);admissionAndCommand();deniedWorkPreservesOriginal();originalSdkMismatch();semanticNegatives();inactiveObservationPaths();protocolDenials();servicePhasePolicy();cleanupDesktopPolicy();
  std::printf("{\"kind\":\"signin-restoration-injected-tests\",\"checks\":%u,\"actualSystemProcesses\":0,\"actualCcdOrSdkMutations\":0,\"runtimeQualified\":false}\n",checks);
}
