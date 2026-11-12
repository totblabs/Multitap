#include "native_stream_profile_worker.hpp"
#include <future>
#include <iostream>
using namespace host;
namespace {
int checks=0;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;}
LUID adapter(DWORD value){return {value,-1};}
const std::string machine="00112233-4455-6677-8899-aabbccddeeff",stream="00000000-0000-0000-0000-000000000149",operation(64,'a');
NativeStreamLaunchReceipt launchFixture(){
  NativeStreamLaunchReceipt launch;launch.key={stream,operation,"ROOT\\DISPLAY\\00a1",2};launch.service={4321,0x12345678123456ull};launch.worker={4322,0x12345678123567ull};
  launch.scope={2,std::string(64,'b'),999,{}};launch.entry={4321,7,{{"0",adapter(33),100,1920,1080},{"1",adapter(33),101,1280,720}}};
  launch.lease=0xf123456789abcdefull;launch.outputs={{0x123456789abcdef0ull,0},{0xfedcba9876543210ull,1}};return launch;
}
const std::string launchDigest=nativeBaselineDigest(nativeStreamLaunchBytes(launchFixture()));
void addPath(NativeCcdData& ccd,LUID luid,UINT32 sourceId,UINT32 targetId,DWORD width,DWORD height,LONG x,LONG y){
  DISPLAYCONFIG_PATH_INFO path{};path.flags=DISPLAYCONFIG_PATH_ACTIVE;path.sourceInfo.adapterId=path.targetInfo.adapterId=luid;path.sourceInfo.id=sourceId;path.targetInfo.id=targetId;path.targetInfo.targetAvailable=TRUE;path.sourceInfo.statusFlags=DISPLAYCONFIG_SOURCE_IN_USE;path.targetInfo.statusFlags=DISPLAYCONFIG_TARGET_IN_USE;path.targetInfo.outputTechnology=DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;path.targetInfo.rotation=DISPLAYCONFIG_ROTATION_IDENTITY;path.targetInfo.scaling=DISPLAYCONFIG_SCALING_IDENTITY;path.targetInfo.refreshRate={60,1};path.targetInfo.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  path.sourceInfo.modeInfoIdx=UINT32(ccd.modes.size());DISPLAYCONFIG_MODE_INFO source{};source.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;source.adapterId=luid;source.id=sourceId;source.sourceMode={width,height,DISPLAYCONFIG_PIXELFORMAT_32BPP,{x,y}};ccd.modes.push_back(source);
  path.targetInfo.modeInfoIdx=UINT32(ccd.modes.size());DISPLAYCONFIG_MODE_INFO target{};target.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;target.adapterId=luid;target.id=targetId;auto& signal=target.targetMode.targetVideoSignalInfo;signal.pixelRate=uint64_t(width+160)*(height+30)*60;signal.hSyncFreq={67500,1};signal.vSyncFreq={60,1};signal.activeSize={width,height};signal.totalSize={width+160,height+30};signal.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;ccd.modes.push_back(target);ccd.paths.push_back(path);
}
Json request(){return JsonParser::parse("{\"type\":\"session.start\",\"sessionId\":"+jsonQuote(stream)+",\"mode\":\"streaming\",\"profileLaunchSha256\":"+jsonQuote(launchDigest)+",\"consoleProfile\":{\"protocol\":\"console-profile-v1\",\"sessionId\":"+jsonQuote(stream)+",\"machineUuid\":\""+machine+"\",\"model\":\"std\"}}");}
struct Fixture {
  NativeCcdData original,current;NativeConsoleAdapterIdentity console{adapter(77),L"\\\\?\\PCI#VEN_1234&DEV_1111#0",L"PCI\\VEN_1234&DEV_1111\\3&0",L"PCI",{L"PCI\\VEN_1234&DEV_1111"},nativeConsoleDisplayClass,true,true};
  std::vector<NativeDisplayTarget> targets={{"0",adapter(33),100,1920,1080},{"1",adapter(33),101,1280,720}};
  std::optional<std::string> bytes;std::vector<std::string> events;
  std::optional<std::string> launchBytes=nativeStreamLaunchBytes(launchFixture());
  std::optional<std::pair<NativeStreamViewportCommit,std::string>> viewportResult;
  std::optional<NativeStreamViewportCancelEvidence> cancellationResult;
  bool denyCancellation=false,corruptCancellationRuntime=false,changeCancellationGeometry=false;int cancellationRequests=0;
  DWORD pid=4321;uint32_t generation=7;uint64_t tick=1000;std::string uuid=machine,logon=std::string(64,'b'),instance="ROOT\\DISPLAY\\00a1";
  bool enterAllowed=true,restoreAllowed=true,failStop=false,changeOnValidate=false,foreignChanged=false,failValidate=false,launchAllowed=true,launchRecoveryAllowed=true,activationVerified=true,reclaimAllowed=true;int saves=0,failSave=0,loseSaveAck=0,applies=0,validates=0;
  Fixture(){addPath(original,adapter(77),0,9,1600,900,0,0);addPath(original,adapter(33),0,100,1920,1080,1600,0);addPath(original,adapter(33),1,101,1280,720,3520,0);current=original;}
  NativeRuntimeJournalAccess store(){return {[&]{return bytes;},[&](const auto& expected,const auto& desired){++saves;events.push_back("save"+std::to_string(saves));if(expected!=bytes||saves==failSave)return false;bytes=desired;return saves!=loseSaveAck;}};}
  Json recovery(){return JsonParser::parse("{\"type\":\"worker.profile.recover\",\"sessionId\":"+jsonQuote(stream)+",\"deviceInstanceId\":"+jsonQuote(instance)+",\"generation\":"+std::to_string(generation)+",\"journalSha256\":"+jsonQuote(bytes?nativeBaselineDigest(*bytes):"")+",\"operation\":"+jsonQuote(bytes?operation:"")+",\"workerExited\":true}");}
  Json snapshot(){
    std::string out="{\"schema\":1,\"provider\":\"libvirtualdisplay-1.6.4\",\"deviceInstanceId\":"+jsonQuote(instance)+",\"servicePid\":"+std::to_string(pid)+",\"generation\":"+std::to_string(generation)+",\"leaseActive\":true,\"activationVerified\":"+(activationVerified?"true":"false")+",\"configurationPending\":false,\"recoveryRequired\":false,\"targets\":[";
    for(const auto& t:targets){if(out.back()!='[')out+=',';out+="{\"id\":"+jsonQuote(t.id)+",\"adapterHigh\":"+std::to_string(t.adapter.HighPart)+",\"adapterLow\":"+std::to_string(t.adapter.LowPart)+",\"target\":"+std::to_string(t.target)+",\"width\":"+std::to_string(t.width)+",\"height\":"+std::to_string(t.height)+'}';}return JsonParser::parse(out+"]}");
  }
  NativeStreamProfileWorkerAccess access(){
    NativeStreamProfileWorkerAccess a;a.eligible=[&](bool restoring){return restoring?restoreAllowed:enterAllowed;};a.snapshot=[&](bool){return snapshot();};
    a.topology={ [&]{return current;},[&](const NativeCcdData& ccd,UINT32 flags)->LONG{
      if(flags&SDC_VALIDATE){++validates;events.push_back("validate");if(changeOnValidate)generation++;return failValidate?ERROR_GEN_FAILURE:ERROR_SUCCESS;}
      ++applies;events.push_back("apply");current=ccd;
      const auto& initial=original.paths.front();if(std::none_of(current.paths.begin(),current.paths.end(),[&](const auto& p){return nativeSameLuid(p.targetInfo.adapterId,initial.targetInfo.adapterId)&&p.targetInfo.id==initial.targetInfo.id;})){auto inactive=initial;inactive.flags&=~DISPLAYCONFIG_PATH_ACTIVE;current.paths.push_back(inactive);}return ERROR_SUCCESS;
    },[]{return true;},[&]{return tick++;},[&]{tick+=50;}};
    a.scope=[&](const NativeTopologyBaseline& baseline){NativeRecoveryScope value{2,logon,tick++,{}};for(const auto& p:baseline.ccd.paths)if((p.flags&DISPLAYCONFIG_PATH_ACTIVE)&&!nativeOwnedPath(p,baseline.targets))value.foreign.push_back({p.sourceInfo.adapterId,p.targetInfo.adapterId,p.sourceInfo.id,p.targetInfo.id,1,utf8(console.interfacePath),utf8(console.interfacePath),foreignChanged?"changed-monitor":"console-monitor",true});return value;};
    a.machineUuid=[&]{return uuid;};a.resolver=[&](LUID luid)->std::optional<NativeConsoleAdapterIdentity>{return nativeSameLuid(luid,console.adapter)?std::optional{console}:std::nullopt;};
    a.stopCapture=[&]{events.push_back("capture-stopped");if(failStop)throw std::runtime_error("stop failed");};
    a.launchProof=[&](const NativeStreamProfileRecord& record,bool recovery){return launchAllowed&&(!recovery||launchRecoveryAllowed)&&launchBytes&&nativeStreamLaunchMatches(parseNativeStreamLaunch(*launchBytes),record);};
    a.reclaimProof=[&](const NativeStreamProfileRecord& record,const NativeStreamProfileEvidence& proof,const Json& receipt){return reclaimAllowed&&receipt.get("intentSha256").string()==std::string(64,'c')&&receipt.get("identitySha256").string()==std::string(64,'d')&&receipt.get("runtimeSha256").string()==std::string(64,'e')&&receipt.get("generation").number()==8&&proof.lease.servicePid==4323&&proof.lease.generation==8&&record.key.session==2&&nativeStreamLaunchMatches(launchFixture(),record);};
    a.restartObservationProof=[&](const NativeStreamProfileRecord& record){return launchAllowed&&launchBytes&&nativeStreamLaunchMatches(parseNativeStreamLaunch(*launchBytes),record);};a.viewportCommit=[&]{return viewportResult;};
    // This is the authenticated-service receipt boundary fixture, not a real
    // driver request. A separate service test exercises the actual producer.
    a.unchangedViewport=[&](const NativeStreamProfileRecord& record,const NativeStreamCancelRequest& request,const NativeStreamProfileEvidence& proof)->std::optional<NativeStreamViewportCancelEvidence>{
      ++cancellationRequests;if(denyCancellation)return {};
      const auto monitor=std::visit([](const auto& value)->std::string{if constexpr(std::is_same_v<std::decay_t<decltype(value)>,StreamResize>)return value.monitor;else return "0";},request);
      NativeStreamViewportCancel receipt;receipt.request=request;receipt.instance=record.key.instance;receipt.profileOperation=record.key.operation;receipt.resizeOperation=record.resize->operation;receipt.profileBeforeBytes=nativeStreamProfileRecordBytes(record);receipt.profileBeforeSha256=nativeBaselineDigest(receipt.profileBeforeBytes);receipt.baselineBytes=nativeRecoveryBaselineBytes({record.key.instance,record.current.generation,captureNativeTopologyBaseline(nativeActiveCcd(proof.topology),record.current.targets,monitor),proof.scope});receipt.baselineSha256=nativeBaselineDigest(receipt.baselineBytes);receipt.current=record.current;receipt.scope=proof.scope;receipt.lease=launchFixture().lease;receipt.outputs=launchFixture().outputs;
      NativeRuntimeRecord runtime{record.key.instance,"stable",record.current.generation,{},""};for(const auto& target:record.current.targets)runtime.outputs.push_back({target.id,target.width,target.height});const auto bytes=nativeRuntimeBytes(runtime);receipt.runtimeSha256=nativeBaselineDigest(bytes);cancellationResult=NativeStreamViewportCancelEvidence{parseNativeStreamViewportCancel(nativeStreamViewportCancelBytes(receipt)),bytes};
      if(corruptCancellationRuntime){runtime.generation++;cancellationResult->runtimeBytes=nativeRuntimeBytes(runtime);}if(changeCancellationGeometry)current.modes[nativeSourceIndex(current.paths.front())].sourceMode.position.y++;
      return cancellationResult;
    };return a;
  }
  NativeStreamProfileWorker worker(){return {4321,2,store(),access()};}
};
void lifecycle(){
  Fixture f;auto worker=f.worker();require(worker.begin(request(),operation)&&worker.active(),"Valid profile did not become active");
  require(f.applies==1&&f.saves==2&&parseNativeStreamProfileRecord(*f.bytes).phase==NativeStreamProfilePhase::active,"Capture admitted before durable active proof");
  require(f.events[0]=="save1"&&f.events[1]=="validate"&&f.events[2]=="apply"&&f.events[3]=="save2","Entry mutation preceded durable preparation");
  require(!worker.begin(request(),operation)&&f.applies==1,"Second entry replaced immutable checkpoint");
  f.events.clear();require(worker.finish()&&worker.clean()&&!worker.active(),"Normal stop failed restoration");
  require(f.events.front()=="capture-stopped"&&f.applies==2&&parseNativeStreamProfileRecord(*f.bytes).phase==NativeStreamProfilePhase::restored,"Restore preceded capture stop or durable completion missing");
  require(nativeStreamProfileSignatures(nativeActiveCcd(f.current))==nativeStreamProfileSignatures(nativeActiveCcd(f.original)),"Original console/primary geometry was not restored");
  const auto count=f.applies;require(worker.finish()&&f.applies==count,"Duplicate stop reapplied topology");
}
StreamResize viewportRequest(){return {stream,"viewport-1","00000000-0000-0000-0000-000000000150","0",1600,900,60,1};}
void viewportResult(Fixture& f,const StreamResize& request,bool rollback=false){
  const auto profile=parseNativeStreamProfileRecord(*f.bytes);++f.generation;
  for(auto& target:f.targets)target.target+=100;
  if(!rollback){f.targets[0].width=uint32_t(request.width);f.targets[0].height=uint32_t(request.height);}
  f.current={};LONG x=0;for(size_t i=0;i<f.targets.size();++i){const auto& t=f.targets[i];addPath(f.current,t.adapter,UINT32(i),t.target,t.width,t.height,x,0);x+=LONG(t.width);}auto inactive=f.original.paths[0];inactive.flags&=~DISPLAYCONFIG_PATH_ACTIVE;f.current.paths.push_back(inactive);
  NativeStreamViewportCommit receipt;receipt.request=request;receipt.instance=f.instance;receipt.profileOperation=profile.key.operation;receipt.resizeOperation=profile.resize->operation;receipt.profileBeforeBytes=*f.bytes;receipt.profileBeforeSha256=nativeBaselineDigest(*f.bytes);receipt.baselineSha256=std::string(64,'e');receipt.before=profile.current;receipt.after={f.pid,f.generation,f.targets};receipt.rollback=rollback;receipt.scope={2,f.logon,f.tick++,{}};receipt.lease=launchFixture().lease;receipt.outputs=launchFixture().outputs;
  NativeRuntimeRecord runtime{f.instance,"stable",f.generation,{},""};for(const auto& t:f.targets)runtime.outputs.push_back({t.id,t.width,t.height});const auto bytes=nativeRuntimeBytes(runtime);receipt.runtimeSha256=nativeBaselineDigest(bytes);
  f.viewportResult=std::make_pair(parseNativeStreamViewportCommit(nativeStreamViewportCommitBytes(receipt)),bytes);
}
void viewport(){
  for(int fault=0;fault<3;++fault){Fixture f;auto w=f.worker();require(w.begin(request(),operation)&&w.reserveViewport(viewportRequest()),"Cancellation refusal fixture failed");f.denyCancellation=fault==0;f.corruptCancellationRuntime=fault==1;f.changeCancellationGeometry=fault==2;require(!w.completeViewport(viewportRequest())&&!w.active()&&f.cancellationRequests==1&&parseNativeStreamProfileRecord(*f.bytes).resize.has_value(),"Missing/stale/changed cancellation proof became active");}
  {Fixture f;auto access=f.access();access.unchangedViewport={};NativeStreamProfileWorker w(4321,2,f.store(),access);require(w.begin(request(),operation)&&w.reserveViewport(viewportRequest())&&!w.completeViewport(viewportRequest())&&!w.active(),"Missing service cancellation callback fell back to readiness alone");}
  // Regression: unconfirmable cancellation of a reservation that never
  // reached the driver. The owner restores at stop on the exact unchanged lease.
  {Fixture f;auto w=f.worker();const auto resize=viewportRequest();require(w.begin(request(),operation)&&w.reserveViewport(resize),"Unforwarded reservation fixture failed");f.denyCancellation=true;
    require(!w.completeViewport(resize,false)&&!w.active()&&w.viewportStage()==3,"Unforwarded cancellation fixture did not fail at stage 3");
    const auto held=parseNativeStreamProfileRecord(*f.bytes);require(held.phase==NativeStreamProfilePhase::recovery&&held.resize&&held.stopRequested,"Failed viewport did not durably quarantine the profile");
    // A reopened (non-owner) journal can never use the owner's unchanged restore.
    const auto lease=nativeStreamProfileLease(f.snapshot(),4321);const NativeStreamProfileEvidence proof{held.key,lease,f.access().scope(captureNativeTopologyBaseline(validateNativeStreamProfileCcd(f.current,lease.targets),lease.targets,"0")),held.console,f.current};
    NativeStreamProfileJournal reopened(f.store());require(reopened.load(f.instance).has_value()&&nativeStreamProfileEvidenceMatches(held,proof,NativeStreamProfileView::restorable),"Reopened journal fixture failed");
    require(!reopened.beginUnchangedRestore(*reopened.ack(),proof)&&*f.bytes==nativeStreamProfileRecordBytes(held),"Non-owner journal dropped an unconfirmed reservation");
    f.events.clear();require(w.finish()&&w.clean()&&f.applies==2&&parseNativeStreamProfileRecord(*f.bytes).phase==NativeStreamProfilePhase::restored&&!parseNativeStreamProfileRecord(*f.bytes).resize,"Unforwarded reservation blocked stop-time console restoration");
    require(f.events.front()=="capture-stopped"&&nativeStreamProfileSignatures(nativeActiveCcd(f.current))==nativeStreamProfileSignatures(nativeActiveCcd(f.original)),"Unforwarded restore preceded capture stop or lost original console geometry");}
  for(int scenario=0;scenario<3;++scenario){Fixture f;auto w=f.worker();const auto resize=viewportRequest();require(w.begin(request(),operation)&&w.reserveViewport(resize),"Forwarded reservation fixture failed");f.denyCancellation=true;
    require(!w.completeViewport(resize,scenario!=0),"Forwarded reservation fixture completed");
    if(scenario==0)f.generation++; // unforwarded, yet the lease changed under it
    if(scenario==2)f.restoreAllowed=false;
    require(!w.finish()&&f.applies==1&&parseNativeStreamProfileRecord(*f.bytes).resize.has_value(),"Possibly-forwarded, changed-lease or ineligible reservation was dropped at stop");
    f.generation=7;f.restoreAllowed=true;auto cleanup=f.worker();require(cleanup.recover(f.recovery())&&f.applies==2,"Exit-fenced recovery could not restore a quarantined unchanged reservation");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Viewport fixture entry failed");const auto entry=parseNativeStreamProfileRecord(*f.bytes).entry;const auto resize=viewportRequest();require(w.reserveViewport(resize)&&!w.active(),"Viewport started without durable profile reservation");viewportResult(f,resize);require(w.completeViewport(resize)&&w.active(),"Matching completed native viewport did not advance profile");const auto record=parseNativeStreamProfileRecord(*f.bytes);require(!record.resize&&record.resizeSequence==1&&record.current.generation==8&&record.current.targets[0].width==1600&&sameNativeStreamLease(record.entry,entry),"Resize changed immutable entry or omitted current map");require(nativeStreamViewportCommittedProfileMatches(record,f.viewportResult->first),"Committed profile lost immutable viewport source");auto changed=record;changed.resizeSequence++;require(!nativeStreamViewportCommittedProfileMatches(changed,f.viewportResult->first),"Unrelated later profile used old viewport ledger");require(w.finish()&&f.applies==2,"Resized primary profile failed original console restore");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Viewport cancel entry failed");const auto resize=viewportRequest();require(w.reserveViewport(resize)&&w.completeViewport(resize)&&w.active()&&!parseNativeStreamProfileRecord(*f.bytes).resize&&w.finish(),"Unchanged native lease could not cancel exact reservation");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Viewport rollback entry failed");const auto resize=viewportRequest();require(w.reserveViewport(resize),"Rollback reservation failed");viewportResult(f,resize,true);require(w.completeViewport(resize)&&parseNativeStreamProfileRecord(*f.bytes).current.targets[0].width==1920&&w.finish(),"Verified native rollback generation failed profile rebind/restore");}
  for(int scenario=0;scenario<6;++scenario){Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Viewport refusal entry failed");const auto resize=viewportRequest();require(w.reserveViewport(resize),"Viewport refusal reserve failed");viewportResult(f,resize);if(scenario==0)f.viewportResult.reset();if(scenario==1)f.viewportResult->first.runtimeSha256=std::string(64,'f');if(scenario==2)f.viewportResult->first.profileBeforeSha256=std::string(64,'f');if(scenario==3)f.viewportResult->first.outputs[1].display=f.viewportResult->first.outputs[0].display;if(scenario==4)f.logon=std::string(64,'f');if(scenario==5)f.console.instanceId.back()=L'X';require(!w.completeViewport(resize)&&!w.active()&&f.applies==1,"Unverified viewport admitted media or topology replay");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Viewport lost ACK entry failed");const auto resize=viewportRequest();require(w.reserveViewport(resize),"Viewport lost ACK reserve failed");viewportResult(f,resize);f.loseSaveAck=f.saves+1;require(!w.completeViewport(resize)&&!w.active()&&!parseNativeStreamProfileRecord(*f.bytes).resize,"Lost profile resize commit ACK resumed capture");f.loseSaveAck=0;auto cleanup=f.worker();require(cleanup.recover(f.recovery())&&f.applies==2,"Durable resized profile could not restore after ACK loss");}
  for(int scenario=0;scenario<3;++scenario){Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Interrupted viewport entry failed");const auto resize=viewportRequest();require(w.reserveViewport(resize),"Interrupted viewport reserve failed");if(scenario!=0)viewportResult(f,resize);if(scenario==2){f.viewportResult->first.runtimeSha256=std::string(64,'f');require(!w.completeViewport(resize),"Bad runtime was accepted");}auto cleanup=f.worker();require(cleanup.recover(f.recovery())==(scenario!=2)&&f.applies==(scenario==2?1:2),"Interrupted viewport recovery ignored exact native receipt or unchanged lease");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation)&&w.reserveViewport(viewportRequest()),"Retry lineage fixture failed");viewportResult(f,viewportRequest());auto profile=parseNativeStreamProfileRecord(*f.bytes);profile.stopRequested=true;profile.revision++;require(nativeStreamViewportCommitSource(profile,f.viewportResult->first,true),"Stop-before-recovery window lost exact receipt authority");profile.phase=NativeStreamProfilePhase::recovery;profile.revision+=20;require(nativeStreamViewportCommitSource(profile,f.viewportResult->first,true),"Repeated exact recovery attempts exhausted immutable receipt authority");profile.resizeSequence++;require(!nativeStreamViewportCommitSource(profile,f.viewportResult->first,true),"Recovery normalization admitted a different resize lineage");}
}
void viewportFiles(){
  Fixture f;auto w=f.worker();require(w.begin(request(),operation)&&w.reserveViewport(viewportRequest()),"Receipt disk fixture failed");viewportResult(f,viewportRequest());const auto encoded=nativeStreamViewportCommitBytes(f.viewportResult->first);
  const auto directory=std::filesystem::temp_directory_path()/(L"MultitapViewport-"+randomName());std::filesystem::create_directory(directory);
  struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}}cleanup{directory};
  const auto path=directory/L"native-stream-viewport-commit.json";const auto trusted=[&](const auto& candidate){return candidate==directory||candidate.parent_path()==directory;};const auto io=nativeStreamViewportCommitFileAccess(path,trusted);
  require(!io.load()&&io.save({},encoded)&&io.load()==std::optional(encoded),"Actual Windows receipt file roundtrip failed");
  require(!io.save({},encoded)&&!io.save(encoded,std::string(131073,'x')),"Receipt file ignored CAS or bounded size");
  const auto large=std::string(70000,'x');const auto raw=nativeJournalFileAccess(path,131072,[](const std::string&){},trusted);require(raw.save(encoded,large)&&raw.load()==std::optional(large),"Specialized receipt read failed above ordinary journal limit");
  require(!io.save(large,large),"Typed receipt accepted invalid oversized-profile content");
  bool rejected=false;try{(void)readNativeJournalFile(directory/L"other.json",trusted,131072);}catch(...){rejected=true;}require(rejected,"Specialized receipt limit relaxed another journal");
  WinHandle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD written=0;const auto excessive=std::string(131073,'x');require(file.value!=INVALID_HANDLE_VALUE&&WriteFile(file.value,excessive.data(),DWORD(excessive.size()),&written,nullptr)&&written==excessive.size(),"Receipt oversize fixture write failed");file=WinHandle();rejected=false;try{(void)io.load();}catch(...){rejected=true;}require(rejected,"Receipt read accepted disk bytes above bounded limit");
}
void failures(){
  {Fixture f;f.launchBytes.reset();auto w=f.worker();require(!w.begin(request(),operation)&&!f.bytes&&!f.applies,"Missing saved launch admitted worker");}
  {Fixture f;auto launch=launchFixture();launch.worker.created++;f.launchBytes=nativeStreamLaunchBytes(launch);auto w=f.worker();require(!w.begin(request(),operation)&&!f.bytes&&!f.applies,"Changed process creation receipt admitted worker");}
  {Fixture f;auto q=request();std::get<Json::Object>(q.value).erase("profileLaunchSha256");auto w=f.worker();require(!w.begin(q,operation)&&!f.bytes&&!f.applies&&!f.validates,"Missing durable launch authority admitted mutation");}
  {Fixture f;auto q=request();std::get<Json::Object>(q.value)["profileLaunchSha256"].value=std::string(64,'f');auto w=f.worker();require(!w.begin(q,operation)&&!f.bytes&&!f.applies&&!f.validates,"Different launch receipt admitted mutation");}
  {Fixture f;f.launchAllowed=false;auto w=f.worker();require(!w.begin(request(),operation)&&!f.bytes&&!f.applies&&!f.validates,"Unverified process receipt admitted mutation");}
  {Fixture f;auto access=f.access();access.launchProof={};NativeStreamProfileWorker w(4321,2,f.store(),access);require(!w.begin(request(),operation)&&!f.bytes&&!f.applies,"Missing process-proof callback admitted mutation");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Launch proof fixture failed");f.launchAllowed=false;require(!w.finish()&&f.applies==1,"Changed launch lifetime admitted restoration");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Launch proof fixture failed");f.launchRecoveryAllowed=false;auto recovery=f.worker();require(!recovery.recover(f.recovery())&&f.applies==1,"Recovery accepted unobserved original worker lifetime");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Legacy fixture entry failed");auto old=parseNativeStreamProfileRecord(*f.bytes);old.launchSha256.clear();f.bytes=nativeStreamProfileRecordBytes(old);auto recovery=f.worker();require(!recovery.recover(f.recovery())&&f.applies==1,"Legacy schema manufactured launch authority");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Pending receipt fixture failed");auto next=launchFixture();next.key.operation=std::string(64,'c');next.worker.created++;int launchSaves=0;
    NativeRuntimeJournalAccess launches{[&]{return f.launchBytes;},[&](const auto& expected,const auto& desired){++launchSaves;if(expected!=f.launchBytes)return false;f.launchBytes=desired;return true;}};
    const auto allGone=[](const auto&){return NativeStreamProcessState::gone;};
    require(!nativeStreamUnpreparedLaunchRetirable(f.store(),launches,launchFixture().key,7,next.service,true,true,allGone),"Prepared current profile incorrectly skipped restoration");
    require(nativeStreamLaunchQuarantined(f.store(),launches,next.service,[](const auto&){return NativeStreamProcessState::gone;}),"Exited worker cleared un-restored profile quarantine");
    require(!saveNativeStreamLaunch(launches,f.store(),next,[](const auto&){return true;})&&!launchSaves,"Pending active profile lost durable receipt");
    require(w.finish(),"Receipt fixture could not restore");
    require(!nativeStreamUnpreparedLaunchRetirable(f.store(),launches,launchFixture().key,7,next.service,true,true,allGone),"Lost restored ACK skipped fresh topology confirmation");
    require(!nativeStreamLaunchQuarantined(f.store(),launches,next.service,[](const auto&){return NativeStreamProcessState::gone;}),"Restored matching receipt remained quarantined");
    const auto validLaunch=f.launchBytes;f.launchBytes.reset();require(nativeStreamLaunchQuarantined(f.store(),launches,next.service,[](const auto&){return NativeStreamProcessState::gone;}),"Missing launch authority retired a schema-2 profile");f.launchBytes=validLaunch;
    require(saveNativeStreamLaunch(launches,f.store(),next,[](const auto&){return true;}).has_value()&&launchSaves==1,"Restored profile and exited worker prevented next lifetime");
    require(nativeStreamUnpreparedLaunchRetirable(f.store(),launches,next.key,7,next.service,true,true,allGone),"Unprepared later launch wedged service reservation behind old restored journal");
    require(!nativeStreamLaunchQuarantined(f.store(),launches,next.service,[](const auto&){return NativeStreamProcessState::gone;}),"Later unprepared exited launch could not retire");
    next.key.operation=std::string(64,'e');next.worker.created++;
    require(saveNativeStreamLaunch(launches,f.store(),next,[](const auto&){return true;}).has_value()&&launchSaves==2,"Aborted launch before prepare wedged next connection");
  }
  {Fixture f;auto q=request();std::get<Json::Object>(q.value)["consoleProfile"].value=nullptr;auto w=f.worker();require(!w.begin(q,operation)&&f.applies==0&&!f.bytes,"Missing broker model proof changed topology");}
  {Fixture f;auto q=request();Json protocol;protocol.value=std::string("tracks-v1");std::get<Json::Object>(q.value)["mediaProtocol"]=protocol;auto w=f.worker();require(!w.begin(q,operation)&&f.applies==0,"Track startup mutated frozen profile lease");}
  {Fixture f;auto q=request();std::get<Json::Object>(std::get<Json::Object>(q.value)["consoleProfile"].value)["sessionId"].value=std::string("other");auto w=f.worker();require(!w.begin(q,operation)&&!f.bytes,"Cross-stream proof accepted");}
  {Fixture f;f.uuid[0]='f';auto w=f.worker();require(!w.begin(request(),operation)&&f.applies==0&&w.finish(),"Different machine mutated console or failed no-op cleanup");}
  {Fixture f;f.enterAllowed=false;auto w=f.worker();require(!w.begin(request(),operation)&&f.applies==0,"Ineligible entry touched display");}
  {Fixture f;f.failSave=1;auto w=f.worker();require(!w.begin(request(),operation)&&f.applies==0&&f.validates==0&&!w.finish(),"Unacknowledged prepare allowed OS validation/apply or false restore ACK");}
  {Fixture f;f.failSave=2;auto w=f.worker();require(!w.begin(request(),operation)&&f.applies==1&&!w.finish(),"Lost active save acknowledged capture or blind rollback");}
  {Fixture f;f.changeOnValidate=true;auto w=f.worker();require(!w.begin(request(),operation)&&f.applies==0&&!w.finish(),"Lease changed after validate still applied/restored");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Fixture entry failed");f.generation++;const auto n=f.applies;require(!w.finish()&&f.applies==n,"Changed lease received stale exit topology");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Fixture entry failed");f.logon=std::string(64,'c');const auto n=f.applies;require(!w.finish()&&f.applies==n,"New logon received prior user's topology");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Fixture entry failed");f.console.instanceId.back()=L'1';require(!w.finish()&&f.applies==1,"Changed private console identity restored");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Fixture entry failed");f.restoreAllowed=false;require(!w.finish()&&f.applies==1,"Lost restoration eligibility ignored");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Fixture entry failed");f.failStop=true;require(!w.finish()&&f.applies==1,"Restore occurred before capture stop completed");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Fixture entry failed");f.enterAllowed=false;require(w.finish()&&f.applies==2,"Same-lease cleanup could not run after entry gate closed");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Fixture entry failed");auto restarted=f.worker();require(!restarted.begin(request(),std::string(64,'c'))&&!restarted.finish()&&f.applies==1,"Reopened active journal was treated as live or blindly restored");require(w.finish(),"Original owner could not finish after rejected reentry");}
  {Fixture f;auto w=f.worker();require(!w.begin(request(),"noncanonical-operation")&&f.applies==0,"Invalid service operation accepted");}
  {Fixture f;f.pid++;auto w=f.worker();require(!w.begin(request(),operation)&&f.applies==0,"Different service PID accepted");}
}
void stopFence(){
  NativeStreamStopFence fence;fence.reset(stream,true);const auto good=JsonParser::parse(nativeStreamStoppedPacket(stream,true,true));
  require(!fence.released(),"Unacknowledged worker released reservation");require(!fence.wait(std::chrono::milliseconds(1)),"Stop wait completed without evidence");
  require(!fence.acknowledge(JsonParser::parse(nativeStreamStoppedPacket("other",true,true))),"Cross-session stop ACK accepted");
  require(!fence.acknowledge(JsonParser::parse(nativeStreamStoppedPacket(stream,false,true))),"Missing required profile accepted");
  require(fence.acknowledge(good)&&!fence.released(),"Restored ACK was mistaken for worker exit");fence.finished(true);require(fence.released(),"ACK plus verified process exit did not release");
  fence.reset(stream,true);fence.finished(true);require(!fence.released(),"Process exit alone released profile");
  fence.reset(stream,true);require(fence.acknowledge(JsonParser::parse(nativeStreamStoppedPacket(stream,true,false))),"Failed restore ACK not recognized");fence.finished(true);require(!fence.released(),"Failed restore released profile");
  fence.reset(stream,true);require(fence.acknowledge(good),"ACK fixture failed");fence.finished(false);require(!fence.released(),"Unobserved worker exit released profile");
  fence.reset(stream,true);auto waiting=std::async(std::launch::async,[&]{return fence.wait(std::chrono::milliseconds(500));});
  std::thread reader([&]{fence.acknowledge(good);});reader.join();require(waiting.get(),"Waiting service blocked its ACK reader mutex");
}
void recovery(){
  require(nativeStreamProfileWorkerCommandAllowed(true,"worker.profile.recover"),"STOP_PENDING rejected exact cleanup command");
  for(const auto* type:{"session.start","worker.native.activate","worker.display.prepare","worker.catalog.prepare","stream.resize","input"})require(!nativeStreamProfileWorkerCommandAllowed(true,type),"STOP_PENDING admitted ordinary worker mutation");
  require(nativeStreamProfileWorkerCommandAllowed(false,"session.start"),"Ordinary running service lost worker admission");
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Recovery fixture entry failed");auto command=f.recovery();auto recovery=f.worker();require(recovery.recover(command)&&recovery.clean()&&!recovery.active(),"Exited active worker could not restore exact profile");require(f.applies==2&&parseNativeStreamProfileRecord(*f.bytes).phase==NativeStreamProfilePhase::restored,"Recovery omitted durable restored proof");require(!recovery.recover(command),"Duplicate recovery replay accepted");}
  {Fixture f;f.failValidate=true;auto w=f.worker();require(!w.begin(request(),operation)&&f.applies==0&&f.bytes.has_value(),"Prepared fixture failed");require(w.entrySystemError()==ERROR_GEN_FAILURE&&w.reason()=="native_stream_profile_validate_failed","Profile entry discarded the first Windows error");f.failValidate=false;auto recovery=f.worker();require(recovery.recover(f.recovery())&&f.applies==0,"Prepared but unapplied profile unnecessarily mutated display");}
  {Fixture f;f.loseSaveAck=2;auto w=f.worker();require(!w.begin(request(),operation)&&f.applies==1,"Lost active ACK fixture failed");auto recovery=f.worker();require(recovery.recover(f.recovery())&&f.applies==2,"Durable active ACK loss could not recover");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Recovery fixture entry failed");f.loseSaveAck=4;require(!w.finish()&&f.applies==1&&parseNativeStreamProfileRecord(*f.bytes).phase==NativeStreamProfilePhase::restoring,"Lost restoring ACK fixture failed");auto recovery=f.worker();require(recovery.recover(f.recovery())&&f.applies==2,"Interrupted restoration did not recover");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Recovery fixture entry failed");f.loseSaveAck=5;require(!w.finish()&&f.applies==2&&parseNativeStreamProfileRecord(*f.bytes).phase==NativeStreamProfilePhase::restored,"Lost restored ACK fixture failed");auto recovery=f.worker();require(recovery.recover(f.recovery())&&f.applies==2,"Lost restored ACK reapplied topology");}
  {Fixture f;auto recovery=f.worker();require(recovery.recover(f.recovery())&&f.applies==0&&!f.bytes,"Exited worker without prepare could not finish without mutation");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Recovery fixture entry failed");auto command=f.recovery();std::get<Json::Object>(command.value)["workerExited"].value=false;auto recovery=f.worker();require(!recovery.recover(command)&&f.applies==1,"Unobserved process exit authorized recovery");}
  for(const auto* field:{"sessionId","deviceInstanceId","operation","journalSha256"}){Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Recovery fixture entry failed");auto command=f.recovery();std::get<Json::Object>(command.value)[field].value=std::string(field==std::string("sessionId")?"other":field==std::string("deviceInstanceId")?"ROOT\\DISPLAY\\00b2":std::string(64,'c'));auto recovery=f.worker();require(!recovery.recover(command)&&f.applies==1,"Changed recovery binding authorized topology mutation");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Recovery fixture entry failed");auto command=f.recovery();++f.generation;auto recovery=f.worker();require(!recovery.recover(command)&&f.applies==1,"Changed lease recovered without reconciliation");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Recovery fixture entry failed");f.logon=std::string(64,'d');auto recovery=f.worker();require(!recovery.recover(f.recovery())&&f.applies==1,"New Windows logon accepted prior topology");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Recovery fixture entry failed");f.console.instanceId.back()=L'2';auto recovery=f.worker();require(!recovery.recover(f.recovery())&&f.applies==1,"Changed console adapter recovered");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Recovery fixture entry failed");auto command=f.recovery();f.bytes=std::string("{}");auto recovery=f.worker();require(!recovery.recover(command)&&f.applies==1,"Malformed journal authorized recovery");}
  {Fixture f;auto w=f.worker();require(w.begin(request(),operation),"Recovery fixture entry failed");f.enterAllowed=false;auto recovery=f.worker();require(recovery.recover(f.recovery()),"Held lease recovery depended on ordinary entry gate");}
}
Json reclaimedCommand(Fixture& f){
  auto command=f.recovery();auto& fields=std::get<Json::Object>(command.value);
  fields["reclaim"]=JsonParser::parse("{\"intentSha256\":"+jsonQuote(std::string(64,'c'))+",\"identitySha256\":"+jsonQuote(std::string(64,'d'))+",\"runtimeSha256\":"+jsonQuote(std::string(64,'e'))+",\"generation\":8}");
  return command;
}
void restartObservation(){
  const std::string nonce(64,'c');
  const auto command=[&](const Fixture& f){return JsonParser::parse("{\"type\":\"worker.profile.observe\",\"sessionId\":"+jsonQuote(stream)+",\"deviceInstanceId\":"+jsonQuote(f.instance)+",\"operation\":"+jsonQuote(operation)+",\"journalSha256\":"+jsonQuote(nativeBaselineDigest(*f.bytes))+",\"nonce\":"+jsonQuote(nonce)+'}');};
  {
    Fixture f;auto original=f.worker();require(original.begin(request(),operation),"Observation fixture failed");const auto before=f.bytes;const auto record=parseNativeStreamProfileRecord(*f.bytes);
    auto access=f.access();access.snapshot=[](bool)->Json{throw std::runtime_error("No new-service lease exists before reclaim");};
    NativeStreamProfileWorker observer(4323,2,f.store(),std::move(access));const auto observed=observer.observeRestart(command(f));
    require(observed&&f.bytes==before&&f.saves==2&&f.applies==1&&!observer.active(),"Read-only restart observation mutated protected state or required a new lease");
    const auto evidence=parseNativeStreamRestartObservation(JsonParser::parse(*observed),record,nonce);
    require(nativeStreamProfileEvidenceMatches(record,evidence,NativeStreamProfileView::restorable)&&evidence.lease.servicePid==4321,"Observation lost saved lease or inactive console availability");
    require(!observer.observeRestart(command(f)),"Observation worker repeated command without a fresh lifetime");
    for(int scenario=0;scenario<3;++scenario){auto packet=JsonParser::parse(*observed);auto& fields=std::get<Json::Object>(packet.value);if(scenario==0)fields["nonce"].value=std::string(64,'d');if(scenario==1)fields["journalSha256"].value=std::string(64,'d');if(scenario==2)fields["extra"].value=true;bool failed=false;try{(void)parseNativeStreamRestartObservation(packet,record,nonce);}catch(...){failed=true;}require(failed,"Stale or expanded observation packet accepted");}
  }
  for(int scenario=0;scenario<7;++scenario){
    Fixture f;auto original=f.worker();require(original.begin(request(),operation),"Observation refusal fixture failed");auto access=f.access();
    if(scenario==0)access.restartObservationProof={};if(scenario==1)f.launchAllowed=false;if(scenario==2)f.restoreAllowed=false;if(scenario==3)f.logon=std::string(64,'d');if(scenario==4)f.console.instanceId.back()=L'X';
    if(scenario==5)f.current.paths.erase(std::remove_if(f.current.paths.begin(),f.current.paths.end(),[&](const auto& path){return nativeSameLuid(path.targetInfo.adapterId,f.console.adapter);}),f.current.paths.end());
    if(scenario==6){auto record=parseNativeStreamProfileRecord(*f.bytes);record.resizeSequence=1;f.bytes=nativeStreamProfileRecordBytes(record);}
    const auto before=f.bytes;NativeStreamProfileWorker observer(4323,2,f.store(),std::move(access));require(!observer.observeRestart(command(f))&&f.bytes==before&&f.applies==1,"Unverified restart observation admitted or mutated topology");
  }
}
void reclaimedRecovery(){
  {
    Fixture f;auto original=f.worker();require(original.begin(request(),operation),"Reclaim fixture entry failed");
    f.pid=4323;f.generation=8;f.activationVerified=false;f.launchAllowed=false;
    NativeStreamProfileWorker cleanup(f.pid,2,f.store(),f.access());
    require(cleanup.recover(reclaimedCommand(f))&&cleanup.clean()&&!cleanup.active()&&f.applies==2,"Committed new service could not restore without opening capture readiness");
    const auto record=parseNativeStreamProfileRecord(*f.bytes);
    require(record.phase==NativeStreamProfilePhase::restored&&record.current.servicePid==4323&&record.current.generation==8&&record.entry.servicePid==4321&&record.profileSha256==nativeBaselineDigest(nativeStreamProfilePayloadBytes(record.profile)),"Recovery lost immutable entry or new committed lease binding");
  }
  for(int scenario=0;scenario<10;++scenario){
    Fixture f;auto original=f.worker();require(original.begin(request(),operation),"Reclaim refusal fixture failed");
    f.pid=4323;f.generation=8;f.activationVerified=false;auto command=reclaimedCommand(f);auto access=f.access();
    auto& root=std::get<Json::Object>(command.value);auto& receipt=std::get<Json::Object>(root["reclaim"].value);
    if(scenario==0)access.reclaimProof={};
    if(scenario==1)f.reclaimAllowed=false;
    if(scenario==2)receipt["intentSha256"].value=std::string(64,'f');
    if(scenario==3)receipt["identitySha256"].value=std::string("invalid");
    if(scenario==4)receipt["generation"].value=9.0;
    if(scenario==5)receipt["extra"].value=true;
    if(scenario==6)f.logon=std::string(64,'f');
    if(scenario==7)f.console.instanceId.back()=L'2';
    if(scenario==8)f.generation=9;
    if(scenario==9)f.bytes.reset();
    NativeStreamProfileWorker cleanup(f.pid,2,f.store(),std::move(access));
    require(!cleanup.recover(command)&&!cleanup.active()&&f.applies==1,"Unverified reclaim commit or changed identity authorized a restore");
  }
  {
    Fixture f;auto original=f.worker();require(original.begin(request(),operation),"Reclaim revocation fixture failed");f.pid=4323;f.generation=8;f.activationVerified=false;
    auto access=f.access();const auto configure=access.topology.configure;
    access.topology.configure=[&](const auto& ccd,UINT32 flags){const auto result=configure(ccd,flags);if(flags&SDC_VALIDATE)f.reclaimAllowed=false;return result;};
    NativeStreamProfileWorker cleanup(f.pid,2,f.store(),std::move(access));
    require(!cleanup.recover(reclaimedCommand(f))&&f.applies==1,"Revoked reclaim proof was not rechecked after validation");
  }
  {
    Fixture f;auto original=f.worker();require(original.begin(request(),operation),"Reclaim lost-ACK fixture failed");f.pid=4323;f.generation=8;f.activationVerified=false;f.loseSaveAck=4;
    NativeStreamProfileWorker cleanup(f.pid,2,f.store(),f.access());
    require(!cleanup.recover(reclaimedCommand(f))&&f.applies==2&&parseNativeStreamProfileRecord(*f.bytes).phase==NativeStreamProfilePhase::restored,"Lost restore ACK fixture did not persist completion");
    f.loseSaveAck=0;NativeStreamProfileWorker retry(f.pid,2,f.store(),f.access());
    require(retry.recover(reclaimedCommand(f))&&f.applies==2&&!retry.active(),"Lost restored ACK reapplied display geometry or started capture");
  }
  {
    Fixture f;auto original=f.worker();require(original.begin(request(),operation),"Ordinary cleanup fixture failed");f.activationVerified=false;
    auto ordinary=f.worker();require(!ordinary.recover(f.recovery())&&f.applies==1,"Reclaim readiness exception leaked into ordinary cleanup");
  }
}
void cleanupHeartbeat(){
  NativeStreamProfileCleanupHeartbeat heartbeat;std::atomic<int> calls{0};
  require(!heartbeat.start({},std::chrono::milliseconds(100)),"Missing cleanup proof accepted");
  require(!heartbeat.start([&]{return true;},std::chrono::seconds(46)),"Unbounded cleanup heartbeat accepted");
  // Condition waits instead of fixed sleeps: an 80 ms lifetime let a loaded
  // machine schedule the thread only after it had already expired. A refresh
  // that began right before the lifetime ended may still count, so the bound
  // is checked after a grace period well beyond one period.
  const auto lifetime=std::chrono::milliseconds(1000);const auto started=std::chrono::steady_clock::now();
  require(heartbeat.start([&]{++calls;return true;},lifetime,std::chrono::milliseconds(10)),"Bounded held-lease heartbeat failed to start");
  require(calls.load()>=1,"Cleanup did not refresh before potentially slow teardown");
  while(calls.load()<2&&std::chrono::steady_clock::now()<started+lifetime)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(calls.load()>=2,"Cleanup did not refresh existing lease");
  std::this_thread::sleep_until(started+lifetime+std::chrono::milliseconds(500));const auto completed=calls.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));require(calls.load()==completed,"Cleanup heartbeat exceeded bounded lifetime");heartbeat.stop();
  require(heartbeat.start([&]{++calls;return true;},std::chrono::milliseconds(80),std::chrono::milliseconds(10)),"Stopped heartbeat could not be reused");heartbeat.stop();const auto stopped=calls.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(25));require(calls.load()==stopped,"Heartbeat continued after restoration teardown completed");
}
void retryGate(){
  const NativeStreamProfileRecoveryBinding binding{stream,operation,"ROOT\\DISPLAY\\00a1",2,7};
  NativeStreamProfileRetryGate gate;
  require(!gate.arm(binding,false)&&!gate.pending(),"Unobserved worker exit scheduled recovery");
  require(gate.arm(binding,true)&&gate.pending(),"Verified pending lifetime was not retained");
  require(!gate.begin(2,false,1000)&&!gate.begin(3,true,1000),"Locked or different session consumed recovery");
  auto other=binding;other.operation=std::string(64,'c');require(!gate.arm(other,true),"Another stream lifetime replaced pending recovery");
  require(gate.begin(2,true,1000)==binding&&!gate.begin(2,true,1000),"Concurrent recovery attempts started");
  require(!gate.complete(other,true,true,2000)&&gate.pending(),"Unrelated completion released pending recovery");
  require(!gate.complete(binding,true,false,2000),"Failed recovery reported completion");
  require(!gate.begin(2,true,6999)&&gate.begin(2,true,7000)==binding,"First recovery backoff ignored");
  require(!gate.complete(binding,true,false,8000)&&!gate.begin(2,true,22999)&&gate.begin(2,true,23000)==binding,"Second recovery backoff ignored");
  require(!gate.complete(binding,true,false,24000)&&!gate.begin(2,true,INT64_MAX)&&gate.pending(),"Recovery exceeded three-attempt bound");
  require(!gate.rearm(3)&&!gate.begin(2,true,INT64_MAX),"Another session's unlock rearmed recovery");
  require(gate.rearm(2)&&gate.begin(2,true,30000)==binding&&!gate.rearm(2)&&!gate.begin(2,true,30000),"Unlock did not grant one bounded fresh budget or rearmed a running attempt");
  require(!gate.complete(binding,true,false,31000)&&!gate.begin(2,true,35999)&&gate.begin(2,true,36000)==binding,"Rearmed recovery skipped its backoff");
  require(!gate.complete(binding,true,false,37000)&&gate.begin(2,true,52000)==binding&&!gate.complete(binding,true,false,53000)&&!gate.begin(2,true,INT64_MAX),"Rearmed recovery exceeded three attempts");
  NativeStreamProfileRetryGate unsafe;require(unsafe.arm(binding,true)&&unsafe.begin(2,true,0)==binding,"Retry fixture failed");
  require(!unsafe.complete(binding,false,false,1000)&&!unsafe.begin(2,true,100000),"Unobserved recovery process exit allowed another mutator");
  require(!unsafe.rearm(2)&&!unsafe.begin(2,true,200000),"Unlock rearmed recovery after an unobserved recovery-worker exit");
  NativeStreamProfileRetryGate success;require(success.arm(binding,true)&&success.begin(2,true,1000)==binding,"Retry fixture failed");
  require(success.complete(binding,true,true,2000)&&!success.pending()&&!success.begin(2,true,100000),"Restored ACK and exit did not clear pending retry");
  {Fixture f;auto original=f.worker();require(original.begin(request(),operation),"Unlock fixture entry failed");f.restoreAllowed=false;auto blocked=f.worker();require(!blocked.recover(f.recovery())&&f.applies==1,"Locked recovery mutated topology");f.restoreAllowed=true;auto unlocked=f.worker();require(unlocked.recover(f.recovery())&&f.applies==2,"Same unchanged logon could not recover after unlock");}
}
}
int main(){try{lifecycle();viewport();viewportFiles();failures();stopFence();recovery();reclaimedRecovery();restartObservation();cleanupHeartbeat();retryGate();std::cout<<"Native stream profile worker: "<<checks<<" checks passed. Injected CCD and actual temporary receipt files; no Windows display mutation.\n";return 0;}catch(const std::exception& error){std::cerr<<"Profile worker test failed: "<<error.what()<<'\n';return 1;}}
