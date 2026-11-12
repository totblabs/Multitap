// Real journal/worker and file codec; Windows display/SDK/process observations
// remain injected. This executable does not change installed displays.
#define main upstreamProfileWorkerMain
#include "native_stream_profile_worker_test.cpp"
#undef main
#include "native_stream_track_commit.hpp"
namespace {
NativeTrackResize groupRequest(){return {stream,"group-1","00000000-0000-0000-0000-000000000250",2,60,{{"0",1600,900,0,0},{"1",1280,1024,-1280,100}},{2,2}};}
struct GroupFixture:Fixture {
  std::optional<NativeStreamTrackCommitEvidence> result;
  NativeStreamProfileWorker groupWorker(){auto a=access();a.trackViewportCommit=[&]{return result;};return {4321,2,store(),std::move(a)};}
  void completed(const NativeTrackResize& resize,bool rollback=false,bool noGeneration=false){
    const auto profile=parseNativeStreamProfileRecord(*bytes);
    const auto before=captureNativeTopologyBaseline(current,profile.current.targets,"0");
    const auto scope=access().scope(before);
    const auto baseline=nativeRecoveryBaselineBytes({instance,generation,before,scope});
    const auto plan=planNativeTrackViewport(before,resize.outputs);
    if(!noGeneration)++generation;
    for(size_t i=0;i<targets.size();++i){if(!noGeneration)targets[i].target+=100;if(!rollback){targets[i].width=resize.outputs[i].width;targets[i].height=resize.outputs[i].height;}}
    const auto placements=rollback?before.placements:plan.placements;
    current={};for(size_t i=0;i<targets.size();++i){const auto& t=targets[i];const auto& p=placements[i];addPath(current,t.adapter,UINT32(i),t.target,t.width,t.height,p.left,p.top);}auto inactive=original.paths.front();inactive.flags&=~DISPLAYCONFIG_PATH_ACTIVE;current.paths.push_back(inactive);
    NativeStreamTrackCommit receipt;receipt.request=resize;receipt.instance=instance;receipt.profileOperation=profile.key.operation;receipt.resizeOperation=profile.resize->operation;receipt.profileBeforeBytes=*bytes;receipt.profileBeforeSha256=nativeBaselineDigest(*bytes);receipt.baselineSha256=nativeBaselineDigest(baseline);receipt.before=profile.current;receipt.after={pid,generation,targets};receipt.rollback=rollback;receipt.scope={2,logon,tick++,{}};receipt.lease=launchFixture().lease;receipt.outputs=launchFixture().outputs;receipt.placements=placements;
    NativeRuntimeRecord runtime{instance,"stable",generation,{},""};for(const auto& t:targets)runtime.outputs.push_back({t.id,t.width,t.height});const auto runtimeBytes=nativeRuntimeBytes(runtime);receipt.runtimeSha256=nativeBaselineDigest(runtimeBytes);
    result=NativeStreamTrackCommitEvidence{parseNativeStreamTrackCommit(nativeStreamTrackCommitBytes(receipt)),runtimeBytes,baseline};
  }
};
void groupLifecycle(){
  for(const bool rollback:{false,true}){
    GroupFixture f;auto w=f.groupWorker();require(w.begin(request(),operation),"Group entry failed");const auto r=groupRequest();require(w.reserveTracks(r)&&!w.active(),"Group not durably reserved");
    f.completed(r,rollback);require(nativeStreamTrackCommitBaselineMatches(f.result->receipt,f.result->baselineBytes),"Actual baseline proof rejected");require(w.completeTracks(r,!rollback)&&w.active(),"Group did not commit all outputs");
    const auto profile=parseNativeStreamProfileRecord(*f.bytes);require(profile.current.targets[1].height==(rollback?720U:1024U)&&profile.current.generation==8&&!profile.resize,"Second output or generation was omitted");
    require(nativeStreamViewportCommittedProfileMatches(profile,f.result->receipt),"Committed source normalization failed");require(w.finish()&&w.clean(),"Group could not restore original console");
    require(nativeStreamProfileSignatures(f.current,f.targets)==nativeStreamProfileSignatures(f.original,profile.entry.targets)&&nativeStreamOriginalPrimaryMatches(profile.profile,f.current,f.targets),"Group restore lost original foreign/console layout");
  }
  {GroupFixture f;auto w=f.groupWorker();require(w.begin(request(),operation)&&w.reserveTracks(groupRequest()),"Unchanged group reserve failed");require(w.completeTracks(groupRequest(),false)&&w.finish(),"Rejected unmutated group could not cancel and restore");}
  {GroupFixture f;auto w=f.groupWorker();auto r=groupRequest();r.outputs[0].width=1920;r.outputs[0].height=1080;r.outputs[1].height=720;r.epochs={1,1};require(w.begin(request(),operation)&&w.reserveTracks(r),"Position-only group reserve failed");f.completed(r,false,true);require(w.completeTracks(r,true)&&w.finish(),"Position-only group failed exact receipt/restore");}
}
void groupRefusals(){
  for(int scenario=0;scenario<17;++scenario){
    GroupFixture f;auto w=f.groupWorker();require(w.begin(request(),operation)&&w.reserveTracks(groupRequest()),"Refusal fixture entry failed");f.completed(groupRequest());auto& e=*f.result;
    switch(scenario){case 0:f.result.reset();break;case 1:e.receipt.runtimeSha256=std::string(64,'f');break;case 2:e.runtimeBytes+=' ';break;case 3:e.baselineBytes+=' ';break;case 4:e.receipt.placements[1].left+=2;break;case 5:e.receipt.placements[1].primary=true;break;case 6:e.receipt.after.targets[1].height+=2;break;case 7:e.receipt.outputs[1]=e.receipt.outputs[0];break;case 8:e.receipt.request.epochs[1]++;break;case 9:e.receipt.request.topologyEpoch++;break;case 10:e.receipt.profileBeforeSha256=std::string(64,'f');break;case 11:e.receipt.scope.logon=std::string(64,'f');break;case 12:f.logon=std::string(64,'f');break;case 13:f.console.instanceId.back()=L'X';break;case 14:{const auto at=nativeSourceIndex(f.current.paths[1]);f.current.modes[at].sourceMode.position.y+=2;break;}case 15:e.receipt.rollback=true;break;case 16:e.receipt.before.targets[1].target++;break;}
    require(!w.completeTracks(groupRequest(),true)&&!w.active()&&f.applies==1,"Invalid group receipt resumed media or applied restoration");
  }
}
void groupFiles(){
  GroupFixture f;auto w=f.groupWorker();require(w.begin(request(),operation)&&w.reserveTracks(groupRequest()),"File fixture entry failed");f.completed(groupRequest());const auto encoded=nativeStreamTrackCommitBytes(f.result->receipt);
  require(std::holds_alternative<NativeStreamTrackCommit>(parseNativeStreamViewportReceipt(encoded)),"Group codec became single resize");bool refused=false;try{(void)parseNativeStreamViewportCommit(encoded);}catch(...){refused=true;}require(refused,"Legacy decoder accepted a group receipt");
  for(const auto* key:{"request","placements","after","runtimeSha256"}){auto malformed=encoded;const auto name=jsonQuote(key)+':';const auto at=malformed.find(name);require(at!=std::string::npos,"Missing field fixture");malformed.replace(at,name.size(),"\"unexpected-field\":");refused=false;try{(void)parseNativeStreamViewportReceipt(malformed);}catch(...){refused=true;}require(refused,"Partial group receipt accepted");}
  const auto directory=std::filesystem::temp_directory_path()/(L"MultitapGroup-"+randomName());std::filesystem::create_directory(directory);
  struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove_all(path,e);}}cleanup{directory};
  const auto path=directory/L"native-stream-viewport-commit.json";const auto trusted=[&](const auto& p){return p==directory||p.parent_path()==directory;};const auto io=nativeStreamViewportReceiptFileAccess(path,trusted);
  require(!io.load()&&io.save({},encoded)&&io.load()==std::optional(encoded),"Actual Windows group receipt file roundtrip failed");require(!io.save({},encoded)&&!io.save(encoded,std::string(131073,'x')),"Group receipt ignored CAS/size guard");
  require(w.completeTracks(groupRequest(),true)&&w.finish(),"File fixture cleanup failed");
}
void groupRecovery(){
  for(int scenario=0;scenario<6;++scenario){
    GroupFixture f;auto w=f.groupWorker();require(w.begin(request(),operation)&&w.reserveTracks(groupRequest()),"Recovery fixture entry failed");
    if(scenario!=0)f.completed(groupRequest(),scenario==2);
    if(scenario==3){f.loseSaveAck=f.saves+1;require(!w.completeTracks(groupRequest(),true)&&!w.active(),"Lost group journal ACK resumed capture");f.loseSaveAck=0;}
    if(scenario==4)f.result->baselineBytes+=' ';
    if(scenario==5){f.result->receipt.runtimeSha256=std::string(64,'f');require(!w.completeTracks(groupRequest(),true),"Invalid group completed");}
    auto cleanup=f.groupWorker();require(cleanup.recover(f.recovery())==(scenario<4),"Restart recovery ignored typed group completion or protected baseline");
    require(f.applies==(scenario<4?2:1),"Group recovery altered Windows on refused evidence");
  }
}
}
int main(){try{groupLifecycle();groupRefusals();groupFiles();groupRecovery();std::cout<<"{\"kind\":\"native-stream-group-commit\",\"checks\":"<<checks<<",\"actualWorkerAndJournal\":true,\"actualWindowsFileCas\":true,\"ccdAndSdk\":\"injected\",\"runtimeEnabled\":false,\"actualVm149\":false}\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
