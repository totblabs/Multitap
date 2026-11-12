#define main upstreamNativeDisplayServiceChangeMain
#include "native_display_service_change_test.cpp"
#undef main
#include "initial_profile_tracks.hpp"
namespace {
[[maybe_unused]] void setFixtureFlag(host::Json& value,const char* name,bool flag){std::get<host::Json::Object>(value.value)[name].value=flag;}
host::Json profileTracksCommand(WorkerFixture& f){using namespace host;
  auto command=trackCurrent(f);auto r=trackRequest();r.topologyEpoch=1;r.epochs={1,1};r.outputs[1].left=-1280;if(f.probe().monitors.size()==1){r.outputs.resize(1);r.epochs.resize(1);}
  const auto fields=JsonParser::parse('{'+nativeTrackResizeFields(r)+'}');std::get<Json::Object>(command.value)["tracks"]=fields.get("tracks");streamField(command,"streamProfileTracksProtocol",std::string("group-v1"));streamField(command,"maxWidth",3840);streamField(command,"maxHeight",2160);
  std::get<Json::Object>(command.value).erase("requestId");std::get<Json::Object>(command.value).erase("resizeId");
  std::get<Json::Object>(command.value)["consoleProfile"]=JsonParser::parse("{\"protocol\":\"console-profile-v1\",\"sessionId\":"+jsonQuote(r.sessionId)+",\"machineUuid\":\"00112233-4455-6677-8899-aabbccddeeff\",\"model\":\"std\"}");return command;
}
host::Json groupPreparedReply(const host::NativeTrackResize& r,const host::NativeWorkerTrackResult& result){using namespace host;return JsonParser::parse(initialProfileTrackResultPacket(r,result,true,jsonObject,workerDisplayJson));}
// What a v2 group start reports: the requested outputs with "0" primary at the
// origin and "1" at its relative place (and what GDI lists for them).
[[maybe_unused]] host::DisplayState v2GroupDisplay(const host::NativeTrackResize& r){using namespace host;
  DisplayState display;display.installed=display.deviceReady=display.active=display.owned=display.viewportProvisioning=true;display.foreign=0;
  const auto& root=r.outputs.front();LONG left=0,top=0,right=0,bottom=0;
  for(const auto& o:r.outputs){VirtualMonitor m;m.id=o.id;m.name=L"\\\\.\\DISPLAY"+std::to_wstring(o.id=="0"?2:3);m.left=o.left-root.left;m.top=o.top-root.top;m.width=o.width;m.height=o.height;m.primary=o.id=="0";
    left=std::min(left,m.left);top=std::min(top,m.top);right=std::max(right,m.left+LONG(m.width));bottom=std::max(bottom,m.top+LONG(m.height));display.monitors.push_back(std::move(m));}
  display.name=display.monitors.front().name;display.left=left;display.top=top;display.width=DWORD(right-left);display.height=DWORD(bottom-top);return display;
}
[[maybe_unused]] std::vector<host::CaptureMonitor> v2GroupObserved(const host::DisplayState& display){std::vector<host::CaptureMonitor> out;for(const auto& m:display.monitors)out.push_back({m.name,L"fixture-"+m.name,{m.left,m.top,m.left+LONG(m.width),m.top+LONG(m.height)},m.primary});return out;}
struct PreparedProfileFixture {
  WorkerFixture& f;std::optional<std::string> bytes;uint64_t tick=10000;int stopped=0;
  host::NativeConsoleAdapterIdentity console;host::NativeStreamLaunchReceipt launch;DISPLAYCONFIG_PATH_INFO availableConsole{};
  explicit PreparedProfileFixture(WorkerFixture& fixture,const host::NativeTrackResize& request):f(fixture){using namespace host;
    const auto targets=nativeDisplayTargets(f.state());const auto path=std::find_if(f.ccd.paths.begin(),f.ccd.paths.end(),[&](const auto& p){return (p.flags&DISPLAYCONFIG_PATH_ACTIVE)&&!nativeOwnedPath(p,targets);});require(path!=f.ccd.paths.end(),"Missing console fixture");
    console={path->targetInfo.adapterId,L"\\\\?\\PCI#VEN_1234&DEV_1111#FIXTURE#{GUID}",L"PCI\\VEN_1234&DEV_1111&REV_02\\4&ABCD&0&0010",L"PCI",{L"PCI\\VEN_1234&DEV_1111&REV_02"},nativeConsoleDisplayClass,true,true};
    availableConsole=*path;availableConsole.flags&=~DISPLAYCONFIG_PATH_ACTIVE;
    launch.key={request.sessionId,std::string(64,'a'),f.state().get("deviceInstanceId").string(),2};launch.service={GetCurrentProcessId(),1000};launch.worker={GetCurrentProcessId()+1,1001};launch.scope={2,std::string(64,'b'),tick,{}};launch.entry=nativeStreamProfileLease(f.state(),GetCurrentProcessId());launch.lease=0x6000000000000001ull;
    for(size_t i=0;i<targets.size();++i)launch.outputs.push_back({uint64_t(71+i),uint32_t(i)});
    launch=parseNativeStreamLaunch(nativeStreamLaunchBytes(launch));
  }
  host::NativeRuntimeJournalAccess store(){return {[&]{return bytes;},[&](const auto& expected,const auto& desired){if(expected!=bytes)return false;bytes=desired;return true;}};}
  host::NativeStreamProfileWorkerAccess access(){using namespace host;NativeStreamProfileWorkerAccess a;
    a.eligible=[&](bool){return f.eligible;};a.snapshot=[&](bool){return f.state();};StreamResizeBudget budget;a.topology=f.access(budget).topology;a.topology.now=[&]{return tick;};a.topology.pause=[&]{tick+=50;};
    a.loginScope=[&]{return NativeRecoveryScope{2,std::string(64,'b'),tick++,{}};};
    // QDC_ALL_PATHS retains the present console's inactive source/target path.
    // The resize-only fixture previously returned just the supplied active set.
    a.topology.query=[&]{f.sync();auto value=f.ccd;if(std::none_of(value.paths.begin(),value.paths.end(),[&](const auto& p){return nativeSameLuid(p.targetInfo.adapterId,availableConsole.targetInfo.adapterId)&&p.targetInfo.id==availableConsole.targetInfo.id;}))value.paths.push_back(availableConsole);return value;};
    a.scope=[&](const NativeTopologyBaseline& baseline){NativeRecoveryScope scope{2,std::string(64,'b'),tick++,{}};for(const auto& p:baseline.ccd.paths)if((p.flags&DISPLAYCONFIG_PATH_ACTIVE)&&!nativeOwnedPath(p,baseline.targets))scope.foreign.push_back({p.sourceInfo.adapterId,p.targetInfo.adapterId,p.sourceInfo.id,p.targetInfo.id,1,utf8(console.interfacePath),utf8(console.interfacePath),"console-monitor",true});return scope;};
    a.machineUuid=[]{return std::string("00112233-4455-6677-8899-aabbccddeeff");};a.resolver=[&](LUID adapter)->std::optional<NativeConsoleAdapterIdentity>{return nativeSameLuid(adapter,console.adapter)?std::optional{console}:std::nullopt;};a.stopCapture=[&]{++stopped;};a.launchProof=[&](const NativeStreamProfileRecord& profile,bool recovery){return !recovery&&nativeStreamLaunchMatches(launch,profile);};return a;
  }
  host::Json start(host::Json command){using namespace host;streamField(command,"profileOperation",launch.key.operation);streamField(command,"profileLaunchSha256",nativeBaselineDigest(nativeStreamLaunchBytes(launch)));return command;}
};
}
#ifndef MULTITAP_INITIAL_PROFILE_TRACKS_FIXTURE_ONLY
int main(){using namespace host;int passed=0;try{
  {
    const auto config=JsonParser::parse("{\"provider\":\"libvirtualdisplay-1.6.4\",\"enabled\":true,\"streamProfileTrialEnabled\":true,\"streamProfileTracksTrialEnabled\":true}");
    require(initialProfileTracksEnabled(config,true)&&!initialProfileTracksEnabled(config,false),"Candidate group gate lost exact native selection");++passed;
    for(const auto* name:{"enabled","streamProfileTrialEnabled","streamProfileTracksTrialEnabled"})for(int variant=0;variant<3;++variant){auto wrong=config;if(variant==0)std::get<Json::Object>(wrong.value).erase(name);else if(variant==1)setFixtureFlag(wrong,name,false);else streamField(wrong,name,1);require(!initialProfileTracksEnabled(wrong,true),"Absent, disabled or mistyped candidate group flag accepted");++passed;}
    auto wrong=config;streamField(wrong,"provider",std::string("other-provider"));require(!initialProfileTracksEnabled(wrong,true),"Candidate group gate accepted an unrelated provider");++passed;
  }
  for(const bool single:{false,true}){
    WorkerFixture f(single);const auto command=profileTracksCommand(f);const auto request=*initialProfileTrackRequest(command);InitialProfileTrackCompletion gate;require(gate.arm(command,true)&&!gate.arm(command,true),"Initial group gate was not single-use");
    bool refused=false;try{(void)nativeStreamProfileRequest(command);}catch(...){refused=true;}require(refused,"Unprepared group could use fixed profile entry");
    StreamResizeBudget budget;const auto generation=f.state().get("generation").number();auto access=trackWorkerAccess(f,request,budget);const auto probe=access.probe;access.probe=[probe]{auto display=probe();display.viewportProvisioning=true;return display;};const auto result=applyInitialProfileTracks(command,access,budget,true);
    if(!result.ok||f.applies!=1||f.exchanges!=2){std::cerr<<"initial group fixture: ok="<<result.ok<<" reason="<<result.reason<<" applies="<<f.applies<<" exchanges="<<f.exchanges<<" rollback="<<result.rollbackAttempted<<'/'<<result.rollbackSucceeded<<" viewport="<<result.display.viewportProvisioning<<" display="<<initialProfileTrackDisplayMatches(result.display,request,false)<<'\n';for(const auto& m:result.display.monitors)std::cerr<<m.id<<':'<<m.width<<'x'<<m.height<<'@'<<m.left<<','<<m.top<<'\n';}
    require(result.ok&&f.state().get("generation").number()>generation&&f.applies==1&&f.exchanges==2,"Initial group did not complete exactly one SDK/Windows transaction");
    require(initialProfileTrackPreparedMatches(command,result.command,result.display,request)&&!result.display.monitors.front().primary,"Initial preparation activated the profile early");
    const auto snapshot=f.state();const auto instance=snapshot.get("deviceInstanceId").string();
    require(initialProfileTrackLeaseMatches(snapshot,result.display,request,GetCurrentProcessId(),instance),"Actual fresh service lease did not bind the prepared outputs");++passed;
    for(const auto* name:{"leaseActive","activationVerified","configurationPending","recoveryRequired"}){auto wrong=snapshot;streamField(wrong,name,1);require(!initialProfileTrackLeaseMatches(wrong,result.display,request,GetCurrentProcessId(),instance),"Mistyped private status flag reached profile reservation");++passed;}
    for(int variant=0;variant<9;++variant){auto wrong=snapshot;auto display=result.display;switch(variant){case 0:streamField(wrong,"servicePid",GetCurrentProcessId()+1);break;case 1:streamField(wrong,"deviceInstanceId",std::string("different-device"));break;case 2:setFixtureFlag(wrong,"leaseActive",false);break;case 3:setFixtureFlag(wrong,"activationVerified",false);break;case 4:setFixtureFlag(wrong,"configurationPending",true);break;case 5:setFixtureFlag(wrong,"recoveryRequired",true);break;case 6:streamField(wrong,"generation",0);break;case 7:display.monitors.front().width+=2;break;case 8:display.monitors.pop_back();break;}require(!initialProfileTrackLeaseMatches(wrong,display,request,GetCurrentProcessId(),instance),"Stale, foreign or incomplete prepared lease reached profile reservation");++passed;}
    const auto early=JsonParser::parse(trackSourceInfo(bindOwnedTrackSources(result.command,result.display,f.observed()),false));require(!gate.acceptMetadata(early,result.display,true),"Media metadata admitted before profile activation");
    int published=0;const auto publish=[&](const DisplayState& display){++published;return display.ready();};const auto packet=groupPreparedReply(request,result);
    for(int variant=0;variant<13;++variant){InitialProfileTrackCompletion denied;require(denied.arm(command,true),"Initial group refusal fixture did not arm");auto wrong=packet;switch(variant){case 0:streamField(wrong,"type",std::string("worker.initial.result"));break;case 1:streamField(wrong,"sessionId",std::string("other-session"));break;case 2:streamField(wrong,"requestId",std::string("other-request"));break;case 3:streamField(wrong,"resizeId",std::string("other-resize"));break;case 4:streamField(wrong,"topologyEpoch",2);break;case 5:setFixtureFlag(wrong,"ok",false);break;case 6:setFixtureFlag(wrong,"desktopAvailable",false);break;case 7:streamField(wrong,"extra",true);break;case 8:std::get<Json::Object>(wrong.value).erase("display");break;case 9:streamField(std::get<Json::Object>(wrong.value)["command"],"audio",17);break;case 10:streamField(wrong,"ok",1);break;}
      int writes=0;require(!denied.acceptWorker(wrong,variant!=11,[&](const DisplayState&){++writes;return variant!=12;})&&!denied.workerVerified(),"Invalid or unacknowledged initial group reply enabled phase two");require((variant==12&&writes==1)||(variant!=12&&writes==0),"Invalid initial group reply published state before validation");++passed;
    }
    require(gate.acceptWorker(packet,true,publish)&&published==1&&gate.workerVerified()&&!gate.complete()&&!gate.acceptWorker(packet,true,publish),"Private initial group ACK was not exact/once");
    auto start=*gate.preparedCommand();streamField(start,"profileOperation",std::string(64,'a'));streamField(start,"profileLaunchSha256",std::string(64,'b'));require(initialProfileTrackPhase2Matches(*gate.preparedCommand(),start),"Service launch proof could not bind prepared group");
    for(const auto* key:{"fps","bitrate","maxWidth","audio","sessionId"}){auto wrong=start;streamField(wrong,key,1);require(!initialProfileTrackPhase2Matches(*gate.preparedCommand(),wrong),"Phase two changed original authorization or quality");++passed;}
    require(!gate.acceptMetadata(early,result.display,true),"Prepared group still bypassed profile activation");
    const auto beforeProfile=nativeActiveSignatures(f.ccd);PreparedProfileFixture entry(f,request);auto launched=entry.start(*gate.preparedCommand());
    {NativeStreamProfileWorker denied(GetCurrentProcessId(),2,entry.store(),entry.access());require(!denied.begin(launched,entry.launch.key.operation)&&!entry.bytes&&f.applies==1,"Unprepared group activated a console profile");++passed;}
    NativeStreamProfileWorker worker(GetCurrentProcessId(),2,entry.store(),entry.access());require(initialProfileTrackPhase2Matches(*gate.preparedCommand(),launched)&&worker.begin(launched,entry.launch.key.operation,true),"Prepared SDK generation did not enter the actual profile worker");
    const auto record=parseNativeStreamProfileRecord(*entry.bytes);require(record.phase==NativeStreamProfilePhase::active&&record.entry.generation==snapshot.get("generation").number()&&record.entry.targets.size()==request.outputs.size()&&f.applies==2,"Profile reserved an old generation or resized after entry");++passed;
    bool missingConsoleRefused=false;try{(void)planNativeStreamPrimaryRestore(record.profile,f.ccd,record.current.targets);}catch(...){missingConsoleRefused=true;}require(missingConsoleRefused&&f.applies==2,"Restore invented a missing inactive console path");++passed;
    auto active=f.probe();active.viewportProvisioning=true;const auto info=JsonParser::parse(trackSourceInfo(bindOwnedTrackSources(launched,active,f.observed()),false));require(gate.acceptMetadata(info,active,true)&&gate.complete()&&!gate.acceptMetadata(info,active,true),"Profile group did not require exact newly active output zero and complete metadata");++passed;
    const bool restored=worker.finish();const auto restoredRecord=parseNativeStreamProfileRecord(*entry.bytes);
    if(!restored||entry.stopped!=1||f.applies!=3||nativeActiveSignatures(f.ccd)!=beforeProfile||restoredRecord.phase!=NativeStreamProfilePhase::restored)std::cerr<<"profile group restore: ok="<<restored<<" reason="<<worker.reason()<<" stops="<<entry.stopped<<" applies="<<f.applies<<" exactTopology="<<(nativeActiveSignatures(f.ccd)==beforeProfile)<<" phase="<<int(restoredRecord.phase)<<'\n';
    require(restored&&entry.stopped==1&&f.applies==3&&nativeActiveSignatures(f.ccd)==beforeProfile&&restoredRecord.phase==NativeStreamProfilePhase::restored,"Group disconnection failed to restore the prepared topology and console");f.transport.checkForeign();++passed;
  }
  for(int scenario=0;scenario<12;++scenario){
    WorkerFixture f;auto command=profileTracksCommand(f);switch(scenario){case 0:streamField(command,"streamProfileTracksProtocol",std::string("unknown"));break;case 1:std::get<Json::Object>(command.value).erase("consoleProfile");break;case 2:streamField(command,"mode",std::string("rdp"));break;case 3:streamField(command,"mediaProtocol",std::string("media-v1"));break;case 4:streamField(command,"resizeProtocol",std::string("epoch-v1"));break;case 5:streamField(command,"epoch",0);break;case 6:streamField(command,"requestId",std::string("external-request"));break;case 7:streamField(command,"maxWidth",320);break;case 8:streamField(command,"topologyEpoch",2);break;case 9:streamField(command,"profileOperation",std::string(64,'a'));break;case 10:streamField(command,"workerDeadlineMs",1);break;case 11:streamField(command,"monitor",std::string("0"));break;}
    const auto creates=f.transport.creates;InitialProfileTrackCompletion gate;require(!gate.arm(command,true),"Malformed initial group armed a transaction");StreamResizeBudget budget;const auto r=nativeInitialTrackViewport(profileTracksCommand(f));const auto result=applyInitialProfileTracks(command,trackWorkerAccess(f,r,budget),budget,true);require(!result.ok&&f.applies==0&&f.transport.creates==creates,"Malformed initial group reached a driver/Windows mutation");++passed;
  }
  {WorkerFixture f;const auto command=profileTracksCommand(f);InitialProfileTrackCompletion gate;require(!gate.arm(command,false)&&!gate.arm(command,true,true),"Unadvertised or concurrent profile prepared initial tracks");++passed;}
  {WorkerFixture f;const auto command=profileTracksCommand(f);const auto r=*initialProfileTrackRequest(command);const auto creates=f.transport.creates;StreamResizeBudget budget;const auto result=applyInitialProfileTracks(command,trackWorkerAccess(f,r,budget),budget,true);require(!result.ok&&f.applies==0&&f.transport.creates==creates,"Lost fresh viewport support reached SDK mutation");++passed;}
  // Display manager v2 group start: no profile reservation, launch receipt or
  // journal. The service accepts the worker's start only for the request on
  // its own fresh lease ("1" left of and below "0" here), the phase-two
  // command is the broker's unchanged and metadata completes the same gate.
  {WorkerFixture f;const auto command=profileTracksCommand(f);const auto request=*initialProfileTrackRequest(command);const auto instance=f.state().get("deviceInstanceId").string();
    const auto display=v2GroupDisplay(request);
    require(!initialProfileTrackV2Matches(display,request,f.state(),GetCurrentProcessId(),instance),"A lease without the requested sizes accepted a v2 group start");
    f.service->setDisplayManagerV2(true);std::vector<NativeRuntimeOutput> sizes;for(const auto& o:request.outputs)sizes.push_back({o.id,o.width,o.height});
    require(f.service->setOwnedSizesV2(sizes).has_value()&&!f.service->hasStreamProfileReservation(),"Fixture v2 sizes failed or reserved a profile");
    require(initialProfileTrackV2Matches(display,request,f.state(),GetCurrentProcessId(),instance),"A v2 group start on the fresh lease was refused");++passed;
    for(int variant=0;variant<7;++variant){auto wrong=display;auto snapshot=f.state();auto pid=GetCurrentProcessId();std::string other=instance;switch(variant){
      case 0:wrong.monitors[0].left=10;wrong.monitors[0].primary=false;break;case 1:wrong.monitors[1].left+=2;break;case 2:wrong.monitors[1].width+=2;break;
      case 3:streamField(snapshot,"servicePid",GetCurrentProcessId()+1);break;case 4:other="ROOT\\DISPLAY\\9999";break;case 5:wrong.monitors.pop_back();break;
      case 6:for(auto& m:wrong.monitors){m.left+=10;m.primary=false;}break;}   // arrangement right, "0" not at the origin
      require(!initialProfileTrackV2Matches(wrong,request,snapshot,pid,other),"A misplaced, resized, foreign or incomplete v2 group start was accepted");++passed;}
    InitialProfileTrackCompletion gate;require(gate.arm(command,true,f.service->hasStreamProfileReservation()),"The v2 group gate did not arm without a profile reservation");
    int published=0;const auto publish=[&](const DisplayState& value){++published;return value.ready();};
    require(!gate.acceptV2Worker(display,false,publish)&&!published,"An ineligible v2 start published state");
    {auto wrong=display;for(auto& m:wrong.monitors){m.left+=10;m.primary=false;}require(!gate.acceptV2Worker(wrong,true,publish)&&!published,"A v2 start without output 0 primary at the origin was accepted");}
    require(!gate.acceptV2Worker(display,true,[](const DisplayState&){return false;})&&!gate.workerVerified(),"An unpublished v2 start verified the gate");
    require(gate.acceptV2Worker(display,true,publish)&&published==1&&gate.workerVerified()&&!gate.complete()&&initialViewportJsonEqual(*gate.preparedCommand(),command),"A v2 start did not verify the gate with the broker's unchanged command");
    require(!gate.acceptV2Worker(display,true,publish)&&!gate.acceptWorker(groupPreparedReply(request,NativeWorkerTrackResult{}),true,publish),"A verified v2 gate accepted a second worker result");
    const auto info=JsonParser::parse(trackSourceInfo(bindOwnedTrackSources(command,display,v2GroupObserved(display)),false));
    require(gate.acceptMetadata(info,display,true)&&gate.complete(),"v2 group metadata did not complete the gate");++passed;
    InitialProfileTrackCompletion unarmed;require(!unarmed.acceptV2Worker(display,true,publish),"An unarmed gate accepted a v2 start");++passed;}
  // The v1 stage-15 failure reaches the service as a fixed code.
  {NativeWorkerTrackResult failed;require(initialProfileTrackFailureCode(failed)==2,"Default v1 group failure code is wrong");
    failed.reason="unsupported_display_mode";failed.rollbackAttempted=true;require(initialProfileTrackFailureCode(failed)==17,"Rollback bit missing");
    failed.reason="initial_profile_tracks_invalid";failed.rollbackSucceeded=true;require(initialProfileTrackFailureCode(failed)==51,"Rollback success bit missing");
    failed.reason="arbitrary text";failed.rollbackAttempted=failed.rollbackSucceeded=false;require(initialProfileTrackFailureCode(failed)==15,"Unknown reason not mapped to the fixed other code");++passed;}
  std::cout<<"{\"kind\":\"initial-profile-tracks\",\"cases\":"<<passed<<",\"actualSdkAndGroupCoordinator\":true,\"actualProfileWorkerAndJournal\":true,\"journalStorage\":\"memory\",\"ccdCaptureProcessAndFileAcl\":\"injected\",\"actualPrivatePipeExchanges\":0,\"installedSdkAuthority\":false,\"captureStarted\":false,\"runtimeEnabled\":false,\"actualVm149\":false}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<" after "<<passed<<" cases\n";return 1;}}
#endif
