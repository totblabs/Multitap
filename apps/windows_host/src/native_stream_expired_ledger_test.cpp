#define main upstreamExpiredConsolePlannerMain
#include "native_stream_expired_console_test.cpp"
#undef main
#include "native_stream_expired_ledger.hpp"

#ifdef MULTITAP_NATIVE_DISPLAY
namespace {
using namespace host;
using PrivateKind=NativeStreamExpiredConsolePrivateKind;
struct LedgerFixture:Fixture {
  NativeStreamProfileRecord profile;NativeStreamLaunchReceipt launch;
  NativeRuntimeRecord runtime;NativeDisplayIdentitySnapshot identity;
  NativeStreamViewportCommit receipt;
  std::vector<NativeStreamExpiredConsolePrivateSnapshot> snapshots;
  LedgerFixture(int count=1,bool extra=false):Fixture(count,extra) {
    profile.key={"00000000-0000-0000-0000-000000000149",std::string(64,'a'),context.ownedInstance,2};
    profile.phase=NativeStreamProfilePhase::active;profile.revision=2;
    profile.entry=profile.current={101,7,context.profile.targets};
    profile.scope=context.scope;profile.console=context.console;profile.profile=context.profile;
    profile.profileSha256=nativeBaselineDigest(nativeStreamProfilePayloadBytes(profile.profile));
    launch.key=profile.key;launch.service={101,11};launch.worker={102,12};launch.scope=profile.scope;launch.scope.foreign.clear();launch.entry=profile.entry;launch.lease=0x6000000000000071ull;
    std::memcpy(identity.value.magic,"TOTBVDD1",8);identity.value.lease=launch.lease;identity.value.displays[0]=1000;identity.value.displays[1]=1001;identity.value.owner.bytes.fill(42);
    for(int i=0;i<count;++i)launch.outputs.push_back({identity.value.displays[i],uint32_t(i)});
    profile.launchSha256=nativeBaselineDigest(nativeStreamLaunchBytes(launch));
    runtime={profile.key.instance,"stable",7,{},""};for(const auto& target:profile.current.targets)runtime.outputs.push_back({target.id,target.width,target.height});
    refresh();
  }
  void refresh() {
    snapshots={{PrivateKind::runtime,nativeRuntimeBytes(runtime)},{PrivateKind::identity,sealNativeDisplayIdentity(identity.value)}};
    if(!receipt.profileBeforeBytes.empty())snapshots.push_back({PrivateKind::viewport,nativeStreamViewportCommitBytes(receipt)});
  }
  void resize(bool commit,bool rollback=false) {
    profile.resize=NativeStreamProfileResize{std::string(64,'c'),{},++profile.resizeSequence};++profile.revision;
    receipt.request={profile.key.stream,"viewport-1","00000000-0000-0000-0000-000000000150","0",1600,900,60,1};
    profile.resize->requestSha256=nativeBaselineDigest(nativeResizeFields(receipt.request));
    receipt.instance=profile.key.instance;receipt.profileOperation=profile.key.operation;receipt.resizeOperation=profile.resize->operation;
    receipt.profileBeforeBytes=nativeStreamProfileRecordBytes(profile);receipt.profileBeforeSha256=nativeBaselineDigest(receipt.profileBeforeBytes);
    receipt.baselineSha256=std::string(64,'e');receipt.before=profile.current;receipt.after=profile.current;++receipt.after.generation;
    for(auto& target:receipt.after.targets)target.target+=100;
    receipt.rollback=rollback;if(!rollback){receipt.after.targets[0].width=1600;receipt.after.targets[0].height=900;}
    receipt.scope=profile.scope;receipt.scope.foreign.clear();++receipt.scope.tick;receipt.lease=launch.lease;receipt.outputs=launch.outputs;
    runtime.generation=receipt.after.generation;runtime.outputs.clear();for(const auto& target:receipt.after.targets)runtime.outputs.push_back({target.id,target.width,target.height});receipt.runtimeSha256=nativeBaselineDigest(nativeRuntimeBytes(runtime));
    if(commit){profile.current=receipt.after;profile.resize.reset();++profile.revision;}
    refresh();
  }
  void interruptNative(uint32_t advance=1,bool recovery=false) {
    profile.resize=NativeStreamProfileResize{std::string(64,'e'),std::string(64,'f'),++profile.resizeSequence};++profile.revision;
    NativeCcdData current;LONG x=0;for(size_t i=0;i<profile.current.targets.size();++i){const auto& target=profile.current.targets[i];add(current,target.adapter,UINT32(i),target.target,target.width,target.height,x,0);x+=LONG(target.width);}
    auto baselineScope=profile.scope;baselineScope.foreign.clear();++baselineScope.tick;
    NativeRecoveryBaseline baseline{profile.key.instance,profile.current.generation,captureNativeTopologyBaseline(current,profile.current.targets,"0"),baselineScope};
    const auto bytes=nativeRecoveryBaselineBytes(baseline);
    runtime.phase=recovery?"recovery":"changing";runtime.generation=profile.current.generation+advance;runtime.baselineHash=nativeBaselineDigest(bytes);
    refresh();snapshots.push_back({PrivateKind::baseline,bytes});
  }
  std::optional<NativeStreamExpiredConsoleLedger> resolve(){return resolveNativeStreamExpiredLedger(profile,launch,snapshots);}
};
void ledgerCases() {
  for(const auto count:{1,2}) {
    LedgerFixture f(count);const auto value=f.resolve();require(value&&value->kind==NativeStreamExpiredConsoleLedgerKind::launch&&value->leases.size()==1&&value->leases[0].outputs.size()==size_t(count),"Exact launch private ledger rejected");
    for(const bool commit:{false,true})for(const bool rollback:{false,true}) {
      LedgerFixture resized(count);resized.resize(commit,rollback);const auto accepted=resized.resolve();
      require(accepted&&accepted->kind==(commit?NativeStreamExpiredConsoleLedgerKind::committedViewport:NativeStreamExpiredConsoleLedgerKind::pendingViewport)&&sameNativeStreamLease(accepted->current,resized.receipt.after),"Exact completed/pending native resize ledger rejected");
      require(accepted->launchSha256==resized.profile.launchSha256&&accepted->profileSha256==nativeBaselineDigest(nativeStreamProfileRecordBytes(resized.profile)),"Private ledger omitted source hashes");
      resized.profile.phase=NativeStreamProfilePhase::recovery;resized.profile.stopRequested=true;++resized.profile.revision;
      require(resized.resolve().has_value(),"Recovery lifecycle lost exact receipt authority");
    }
  }
  for(int scenario=0;scenario<23;++scenario) {
    LedgerFixture f(2);f.resize(true);
    switch(scenario) {
      case 0:f.snapshots.erase(f.snapshots.begin());break;
      case 1:f.snapshots.erase(f.snapshots.begin()+1);break;
      case 2:f.snapshots.push_back(f.snapshots[0]);break;
      case 3:f.snapshots[1].bytes[0]^=1;break;
      case 4:f.identity.value.displays[0]+=10;f.refresh();break;
      case 5:f.identity.value.lease++;f.refresh();break;
      case 6:f.runtime.generation++;f.refresh();break;
      case 7:f.runtime.outputs[0].width+=2;f.refresh();break;
      case 8:f.runtime.phase="recovery";f.refresh();break;
      case 9:f.runtime.instance="ROOT\\DISPLAY\\00b2";f.refresh();break;
      case 10:f.snapshots.pop_back();break;
      case 11:f.receipt.runtimeSha256=std::string(64,'f');f.refresh();break;
      case 12:f.profile.current.targets[0].target++;break;
      case 13:f.profile.current.generation++;break;
      case 14:f.profile.resizeSequence++;break;
      case 15:f.profile.revision=2;break;
      case 16:f.profile.scope.logon=std::string(64,'f');break;
      case 17:f.profile.launchSha256=std::string(64,'f');break;
      case 18:f.launch.outputs[0].display++;break;
      case 19:f.snapshots.push_back({PrivateKind::reclaim,"{}"});break;
      case 20:f.profile.phase=NativeStreamProfilePhase::restored;f.profile.stopRequested=true;break;
      case 21:f.profile.scope.foreign[0].monitorPath="different-console-monitor";break;
      case 22:f.snapshots[2].bytes+=" ";break;
    }
    require(!f.resolve(),"Unverified private lease ledger admitted");
  }
  for(int scenario=0;scenario<5;++scenario) {
    LedgerFixture f;f.resize(false);
    if(scenario==0)f.profile.resize->operation=std::string(64,'f');
    if(scenario==1)f.profile.resize->requestSha256=std::string(64,'f');
    if(scenario==2)f.profile.current.targets[0].width+=2;
    if(scenario==3)f.profile.revision=2;
    if(scenario==4)f.profile.resize->sequence=++f.profile.resizeSequence;
    require(!f.resolve(),"Unrelated pending resize accepted private commit");
  }
  for(const bool committed:{false,true}) {
    LedgerFixture f;f.resize(committed);f.profile.stopRequested=true;++f.profile.revision;
    require(f.resolve().has_value(),"Crash immediately after requestStop lost receipt recovery");
    f.profile.phase=NativeStreamProfilePhase::recovery;f.profile.revision+=20;
    require(f.resolve().has_value(),"Repeated verified recovery attempt lost immutable receipt authority");
  }
  for(const auto count:{1,2})for(const bool previouslyResized:{false,true})for(const auto advance:{0u,1u,2u})for(const bool recovery:{false,true}) {
    LedgerFixture f(count);if(previouslyResized)f.resize(true);f.interruptNative(advance,recovery);
    const auto result=f.resolve();require(result&&result->kind==NativeStreamExpiredConsoleLedgerKind::pendingNative&&sameNativeStreamLease(result->current,f.profile.current),"Interrupted native resize lost explicit predecessor-only expiry ledger");
  }
  for(int scenario=0;scenario<9;++scenario) {
    LedgerFixture f(2);f.resize(true);f.interruptNative();
    switch(scenario) {
      case 0:f.snapshots.pop_back();break;
      case 1:f.runtime.baselineHash=std::string(64,'a');f.snapshots[0].bytes=nativeRuntimeBytes(f.runtime);break;
      case 2:f.runtime.generation=f.profile.current.generation+3;f.snapshots[0].bytes=nativeRuntimeBytes(f.runtime);break;
      case 3:f.runtime.generation=f.profile.current.generation-1;f.snapshots[0].bytes=nativeRuntimeBytes(f.runtime);break;
      case 4:f.runtime.outputs[0].width+=2;f.snapshots[0].bytes=nativeRuntimeBytes(f.runtime);break;
      case 5:f.profile.resize.reset();break;
      case 6:{auto baseline=parseNativeRecoveryBaseline(f.snapshots.back().bytes);baseline.scope->logon=std::string(64,'d');f.snapshots.back().bytes=nativeRecoveryBaselineBytes(baseline);f.runtime.baselineHash=nativeBaselineDigest(f.snapshots.back().bytes);f.snapshots[0].bytes=nativeRuntimeBytes(f.runtime);break;}
      case 7:{auto baseline=parseNativeRecoveryBaseline(f.snapshots.back().bytes);baseline.generation++;f.snapshots.back().bytes=nativeRecoveryBaselineBytes(baseline);f.runtime.baselineHash=nativeBaselineDigest(f.snapshots.back().bytes);f.snapshots[0].bytes=nativeRuntimeBytes(f.runtime);break;}
      case 8:f.profile.current.targets[0].target++;break;
    }
    require(!f.resolve(),"Unproven interrupted native resize admitted expiry ledger");
  }
  // viewportFailed + latched/stopped runtime: an unconfirmed
  // reservation (no SDK mutation), profile quarantined, no CCD checkpoint, and
  // runtime "changing"/"recovery"/"stopped" at exactly the reserved generation.
  for(const auto count:{1,2})for(const bool previouslyResized:{false,true})for(const char* phase:{"changing","recovery","stopped"}) {
    LedgerFixture f(count);if(previouslyResized)f.resize(true);
    f.profile.resize=NativeStreamProfileResize{std::string(64,'e'),std::string(64,'f'),++f.profile.resizeSequence};++f.profile.revision;
    f.profile.phase=NativeStreamProfilePhase::recovery;f.profile.stopRequested=true;++f.profile.revision;
    f.runtime.phase=phase;f.refresh();
    const auto result=f.resolve();
    require(result&&result->kind==NativeStreamExpiredConsoleLedgerKind::pendingReservation&&sameNativeStreamLease(result->current,f.profile.current)&&result->pendingOperation==std::string(64,'e')&&result->leases.size()==1&&result->leases[0].lease==f.launch.lease,"never-forwarded reservation with latched runtime stayed unresolvable");
    for(int fault=0;fault<5;++fault) {
      if(fault==2&&std::string(phase)=="stopped")continue; // the runtime codec forbids a checkpoint on "stopped"
      auto runtime=f.runtime;auto identity=f.identity;auto profile=f.profile;
      if(fault==0)f.runtime.generation++;
      if(fault==1)f.runtime.outputs[0].width+=2;
      if(fault==2)f.runtime.baselineHash=std::string(64,'a');
      if(fault==3)f.identity.value.lease++;
      if(fault==4)f.profile.resize.reset();
      f.refresh();require(!f.resolve(),"Changed generation/outputs, unbacked checkpoint, foreign private lease or missing reservation admitted the settled runtime");
      f.runtime=runtime;f.identity=identity;f.profile=profile;f.refresh();
    }
  }
  LedgerFixture old;old.resize(true);LedgerFixture fresh;
  fresh.snapshots.push_back({PrivateKind::viewport,old.snapshots.back().bytes});
  require(!fresh.resolve(),"Current-operation receipt silently ignored");
  // A durable receipt from a different old operation cannot override a new
  // launch, but is retained and pinned by the transaction during recovery.
  auto stale=old.receipt;auto before=parseNativeStreamProfileRecord(stale.profileBeforeBytes);
  before.key.operation=stale.profileOperation=std::string(64,'d');stale.profileBeforeBytes=nativeStreamProfileRecordBytes(before);stale.profileBeforeSha256=nativeBaselineDigest(stale.profileBeforeBytes);
  fresh.snapshots.back().bytes=nativeStreamViewportCommitBytes(stale);
  require(fresh.resolve().has_value(),"Unrelated old receipt wedged exact new launch");
}
void ledgerDisk() {
  LedgerFixture f(2);f.resize(true);const auto root=std::filesystem::temp_directory_path()/(L"multitap-ledger-"+randomName());std::filesystem::create_directory(root);
  struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}}cleanup{root};
  const auto trusted=[&](const auto& path){return path==root||path.parent_path()==root;};
  const auto runtime=nativeRuntimeFileAccess(root/L"native-runtime.json",trusted),identity=nativeDisplayIdentityFileAccess(root/L"native-display.dpapi",trusted),viewport=nativeStreamViewportCommitFileAccess(root/L"native-stream-viewport-commit.json",trusted);
  require(runtime.save({},f.snapshots[0].bytes)&&identity.save({},f.snapshots[1].bytes)&&viewport.save({},f.snapshots[2].bytes),"Real Windows private journal writes failed");
  const std::vector<NativeStreamExpiredConsolePrivateSnapshot> read={{PrivateKind::runtime,*runtime.load()},{PrivateKind::identity,*identity.load()},{PrivateKind::viewport,*viewport.load()}};
  require(resolveNativeStreamExpiredLedger(f.profile,f.launch,read).has_value(),"Real DPAPI and file receipt failed expiry ledger resolution");
  require(!identity.save({},f.snapshots[1].bytes)&&!viewport.save({},f.snapshots[2].bytes),"Private journal ignored source CAS");
}
}
#ifndef MULTITAP_NATIVE_STREAM_LEDGER_FIXTURE_ONLY
int main(){try{ledgerCases();ledgerDisk();std::cout<<"{\"expiredPrivateLedgerChecks\":"<<checks<<",\"actualWindowsDpapi\":true,\"actualTemporaryWindowsJournalFiles\":true,\"actualWindowsDisplayChanges\":0,\"sdkAbsenceProof\":\"separate transaction authority required\"}\n";return 0;}catch(const std::exception& error){std::cerr<<"expired private ledger: "<<error.what()<<'\n';return 1;}}
#endif
#else
#ifndef MULTITAP_NATIVE_STREAM_LEDGER_FIXTURE_ONLY
int main(){return 2;}
#endif
#endif
