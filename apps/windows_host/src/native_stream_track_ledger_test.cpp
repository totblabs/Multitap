#define MULTITAP_NATIVE_STREAM_LEDGER_FIXTURE_ONLY
#include "native_stream_expired_ledger_test.cpp"
#undef MULTITAP_NATIVE_STREAM_LEDGER_FIXTURE_ONLY
#ifdef MULTITAP_NATIVE_DISPLAY
namespace {
struct TrackLedgerFixture:LedgerFixture {
  NativeStreamTrackCommit group;
  TrackLedgerFixture(bool commit,bool rollback=false,int count=2,bool extra=false):LedgerFixture(count,extra){
    group.request={profile.key.stream,"group-ledger","00000000-0000-0000-0000-000000000250",2,60,{{"0",1600,900,0,0}},{2}};
    if(count==2){group.request.outputs.push_back({"1",1280,1024,-1280,100});group.request.epochs.push_back(2);}
    profile.resize=NativeStreamProfileResize{std::string(64,'c'),nativeBaselineDigest(nativeTrackResizeFields(group.request)),++profile.resizeSequence};++profile.revision;
    const auto baseline=captureNativeTopologyBaseline(profile.profile.applied,profile.current.targets,"0");auto baselineScope=profile.scope;baselineScope.foreign.clear();
    for(const auto& path:profile.profile.applied.paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&!nativeOwnedPath(path,profile.current.targets))baselineScope.foreign.push_back(binding(path));
    ++baselineScope.tick;const auto baselineBytes=nativeRecoveryBaselineBytes({profile.key.instance,profile.current.generation,baseline,baselineScope});
    group.instance=profile.key.instance;group.profileOperation=profile.key.operation;group.resizeOperation=profile.resize->operation;group.profileBeforeBytes=nativeStreamProfileRecordBytes(profile);group.profileBeforeSha256=nativeBaselineDigest(group.profileBeforeBytes);group.baselineSha256=nativeBaselineDigest(baselineBytes);group.before=profile.current;group.after=profile.current;++group.after.generation;
    for(size_t i=0;i<group.after.targets.size();++i){auto& target=group.after.targets[i];target.target+=100;if(!rollback){target.width=group.request.outputs[i].width;target.height=group.request.outputs[i].height;}}
    group.rollback=rollback;group.scope=baselineScope;group.scope.foreign.clear();++group.scope.tick;group.lease=launch.lease;group.outputs=launch.outputs;group.placements=rollback?baseline.placements:planNativeTrackViewport(baseline,group.request.outputs).placements;
    runtime.generation=group.after.generation;runtime.outputs.clear();for(const auto& target:group.after.targets)runtime.outputs.push_back({target.id,target.width,target.height});group.runtimeSha256=nativeBaselineDigest(nativeRuntimeBytes(runtime));
    if(commit){profile.current=group.after;profile.resize.reset();++profile.revision;}
    refresh();snapshots.push_back({PrivateKind::viewport,nativeStreamTrackCommitBytes(group)});snapshots.push_back({PrivateKind::baseline,baselineBytes});
  }
};
}
#endif
#ifndef MULTITAP_NATIVE_STREAM_TRACK_LEDGER_FIXTURE_ONLY
int main(){
#ifdef MULTITAP_NATIVE_DISPLAY
  try{int cases=0;for(const bool commit:{false,true})for(const bool rollback:{false,true}){TrackLedgerFixture f(commit,rollback);const auto resolved=f.resolve();require(resolved&&resolved->kind==(commit?NativeStreamExpiredConsoleLedgerKind::committedViewport:NativeStreamExpiredConsoleLedgerKind::pendingViewport)&&sameNativeStreamLease(resolved->current,f.group.after)&&resolved->leases.front().outputs.size()==2,"Private group ledger did not bind both outputs/generation");++cases;f.profile.phase=NativeStreamProfilePhase::recovery;f.profile.stopRequested=true;++f.profile.revision;require(f.resolve().has_value(),"Exact recovery lifecycle lost group receipt");++cases;}
    for(int scenario=0;scenario<8;++scenario){TrackLedgerFixture f(true);switch(scenario){case 0:f.snapshots.pop_back();break;case 1:f.snapshots.back().bytes+=' ';break;case 2:f.snapshots[2].bytes+=' ';break;case 3:f.profile.resizeSequence++;break;case 4:f.profile.current.targets[1].target++;break;case 5:f.runtime.outputs[1].width+=2;f.snapshots[0].bytes=nativeRuntimeBytes(f.runtime);break;case 6:{const std::string from="\"left\":-1280",to="\"left\":-1278";const auto at=f.snapshots[2].bytes.find(from);require(at!=std::string::npos,"Placement fixture missing");f.snapshots[2].bytes.replace(at,from.size(),to);break;}case 7:f.profile.key.session++;break;}require(!f.resolve(),"Invalid or incomplete group ledger granted expired restoration authority");++cases;}
    std::cout<<"{\"kind\":\"native-stream-group-expired-ledger\",\"cases\":"<<cases<<",\"actualMachineDpapi\":true,\"sdkAbsenceAndWindowsCcd\":\"not exercised\",\"runtimeEnabled\":false,\"installedDisplaysChanged\":false}\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
#else
  return 2;
#endif
}
#endif
