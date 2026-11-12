#define main upstreamNativeDisplayServiceChangeMain
#include "native_display_service_change_test.cpp"
#undef main
#include "initial_viewport.hpp"

namespace {
host::Json initialCommand(WorkerFixture& fixture,int encodeWidth=1280,int encodeHeight=720) {
  auto command=fixture.current();host::streamField(command,"mode",std::string("stream"));host::streamField(command,"width",encodeWidth);host::streamField(command,"height",encodeHeight);host::streamField(command,"fps",fixture.resize.fps);host::streamField(command,"bitrate",12000000);
  std::get<host::Json::Object>(command.value)["audio"].value=true;std::get<host::Json::Object>(command.value)["clipboardPolicy"]=host::JsonParser::parse(R"({"textToRemote":false,"textToLocal":false,"filesToRemote":false,"filesToLocal":false})");
  auto initial=host::JsonParser::parse('{'+host::nativeResizeFields(fixture.resize)+",\"protocol\":\"epoch-v1\"}");std::get<host::Json::Object>(command.value)["initialViewport"]=initial;return command;
}
host::Json reply(const host::StreamResize& request,const host::WorkerViewportResult& result) {
  return host::JsonParser::parse("{\"type\":\"worker.initial.result\","+host::streamResizeFields(request)+",\"ok\":"+(result.ok?"true":"false")+",\"desktopAvailable\":true,\"display\":"+host::workerDisplayJson(result.display)+",\"command\":"+host::jsonObject(result.command)+'}');
}
host::Json metadata(const host::StreamResize& request,int encodedWidth=1280,int encodedHeight=720) {
  return host::JsonParser::parse("{\"type\":\"stream.info\","+host::streamResizeFields(request)+",\"codec\":\"h264\",\"sourceWidth\":"+std::to_string(request.width)+",\"sourceHeight\":"+std::to_string(request.height)+",\"width\":"+std::to_string(encodedWidth)+",\"height\":"+std::to_string(encodedHeight)+",\"fpsTarget\":"+std::to_string(request.fps)+'}');
}
}
int main(){using namespace host;int passed=0;try{
  {WorkerFixture fixture(true);auto command=initialCommand(fixture);const auto before=fixture.state().get("generation").number();StreamResizeBudget budget;const auto result=applyInitialNativeViewport(command,fixture.access(budget),budget,true);
    require(result.ok&&result.display.ready()&&fixture.exchanges==2&&fixture.applies==1,"Initial native transaction did not reserve/apply/commit exactly once");require(fixture.state().get("generation").number()>before&&!fixture.state().get("configurationPending").boolean(),"Initial transaction did not commit a fresh generation");
    require(result.command.get("width").number()==1280&&result.command.get("height").number()==720&&result.command.get("captureWidth").number()==1600&&result.command.get("captureHeight").number()==900,"Encoder ceilings replaced the real physical viewport");
    require(!hasInitialViewport(result.command)&&fixture.resize.matches(result.command)&&initialViewportPreparedCommandMatches(command,result.command,result.display,fixture.resize),"Prepared command lost epoch/correlation or retained recursive initial work");
    const auto console=nativeCcdSource(fixture.ccd,fixture.ccd.paths.front());require(console.width==1600&&console.height==900&&console.position.x==0&&console.position.y==0,"Initial resize altered the console display before the profile transaction");fixture.transport.checkForeign();++passed;
    InitialViewportCompletion gate;require(gate.arm(command,true)&&!gate.arm(command,true),"Completion gate was not single-use");const auto info=metadata(fixture.resize);require(!gate.acceptMetadata(info,result.display,true),"Metadata advanced epoch before private preparation ACK");int published=0;
    require(gate.acceptWorker(reply(fixture.resize,result),true,[&](const DisplayState& display){++published;return display.ready();})&&published==1&&gate.workerVerified()&&gate.preparedCommand()&&!gate.complete(),"Private preparation did not bind verified command/display");
    require(!gate.acceptWorker(reply(fixture.resize,result),true,[&](const DisplayState&){++published;return true;})&&published==1,"Duplicate ACK was published twice");require(gate.acceptMetadata(info,result.display,true)&&gate.complete()&&!gate.pending()&&!gate.acceptMetadata(info,result.display,true),"Initial metadata did not commit exactly one epoch");++passed;
  }
  {WorkerFixture fixture(true);fixture.resize.width=2560;fixture.resize.height=1440;const auto command=initialCommand(fixture);StreamResizeBudget budget;const auto result=applyInitialNativeViewport(command,fixture.access(budget),budget,true);require(result.ok&&result.command.get("captureWidth").number()==2560&&result.command.get("captureHeight").number()==1440&&result.command.get("width").number()==1280&&result.command.get("height").number()==720,"Large source was substituted with encoder limits");fixture.transport.checkForeign();++passed;}
  // Malformed or unnegotiated requests must fail before any SDK/CCD change.
  for(int scenario=0;scenario<23;++scenario){WorkerFixture fixture(true);auto command=initialCommand(fixture);auto& nested=std::get<Json::Object>(command.value)["initialViewport"];bool supported=true,reserved=false;StreamResizeBudget budget;
    switch(scenario){case 0:streamField(nested,"protocol",std::string("unknown"));break;case 1:streamField(nested,"extra",1);break;case 2:streamField(nested,"epoch",0);break;case 3:streamField(nested,"epoch",2);break;case 4:streamField(nested,"monitor",std::string("1"));break;case 5:streamField(command,"monitor",std::string("all"));break;case 6:streamField(nested,"sessionId",std::string("00000000-0000-0000-0000-000000000003"));break;case 7:streamField(command,"fps",30);break;case 8:streamField(command,"mode",std::string("rdp"));break;case 9:streamField(command,"mediaProtocol",std::string("tracks-v1"));break;case 10:std::get<Json::Object>(command.value)["tracks"].value=Json::Array{};break;case 11:std::get<Json::Object>(command.value)["consoleProfile"]=JsonParser::parse(R"({"session":"intent"})");break;case 12:streamField(command,"width",1279);break;case 13:streamField(nested,"width",1601);break;case 14:supported=false;break;case 15:reserved=true;break;case 16:budget.cancelled=[]{return true;};break;case 17:budget.deadline=streamMonotonicMillis()-1;break;case 18:fixture.eligible=false;break;}
    switch(scenario){case 19:std::get<Json::Object>(command.value)["mediaProtocol"].value=true;break;case 20:streamField(command,"profileOperation",std::string(64,'a'));break;case 21:streamField(command,"profileLaunchSha256",std::string(64,'b'));break;case 22:streamField(command,"workerDeadlineMs",double(streamMonotonicMillis()+90000));break;}
    const auto result=applyInitialNativeViewport(command,fixture.access(budget),budget,supported,reserved);require(!result.ok&&fixture.exchanges==0&&fixture.applies==0,"Rejected initial viewport reached SDK/Windows");fixture.transport.checkForeign();++passed;
  }
  {WorkerFixture fixture;auto command=initialCommand(fixture);StreamResizeBudget budget;const auto result=applyInitialNativeViewport(command,fixture.access(budget),budget,true);require(!result.ok&&fixture.exchanges==0&&fixture.applies==0,"Single-output initial request mutated a two-output lease");++passed;}
  {WorkerFixture fixture(true);auto command=initialCommand(fixture);auto& fields=std::get<Json::Object>(command.value);fields["consoleProfile"]=JsonParser::parse(R"({"sessionId":"00000000-0000-0000-0000-000000000001","privateIntent":"preserve-exactly"})");StreamResizeBudget budget;
    require(!InitialViewportCompletion{}.arm(command,true),"Deferred profile was accepted without explicit two-phase authorization");const auto result=applyInitialNativeViewport(command,fixture.access(budget),budget,true,false,true);require(result.ok&&initialViewportJsonEqual(result.command.get("consoleProfile"),command.get("consoleProfile")),"Preparation bypassed or rewrote deferred profile intent");InitialViewportCompletion gate;require(gate.arm(command,true,false,true)&&gate.acceptWorker(reply(fixture.resize,result),true,[](const DisplayState&){return true;}),"Explicit two-phase profile preparation could not bind command");++passed;
  }
  {WorkerFixture fixture(true);const auto command=initialCommand(fixture);fixture.profilePending=true;StreamResizeBudget budget;const auto result=applyInitialNativeViewport(command,fixture.access(budget),budget,true);require(!result.ok&&fixture.applies==0,"Actual NativeDisplayService profile guard was bypassed");fixture.transport.checkForeign();++passed;}
  {WorkerFixture fixture(true);auto display=fixture.probe();display.viewportProvisioning=true;require(initialOwnedViewportReady(display,true,true,false)&&!initialOwnedViewportReady(display,false,true,false)&&!initialOwnedViewportReady(display,true,false,false)&&!initialOwnedViewportReady(display,true,true,true),"Capability gate ignored provider/epoch/profile evidence");display.remoteSession=true;require(!initialOwnedViewportReady(display,true,true,false),"RDP state advertised initial viewport");++passed;}
  // Wrong worker replies cannot authorize capture or update the public cache.
  {WorkerFixture fixture(true);const auto command=initialCommand(fixture);StreamResizeBudget budget;const auto result=applyInitialNativeViewport(command,fixture.access(budget),budget,true);require(result.ok,"ACK fixture failed");const auto valid=reply(fixture.resize,result);
    for(int scenario=0;scenario<18;++scenario){auto value=valid;auto& root=std::get<Json::Object>(value.value);auto& prepared=root["command"];bool eligible=true,publication=true;switch(scenario){case 0:root["ok"].value=false;break;case 1:root["ok"].value=std::string("true");break;case 2:root["desktopAvailable"].value=false;break;case 3:streamField(value,"requestId",std::string("other"));break;case 4:streamField(value,"epoch",0);break;case 5:streamField(value,"type",std::string("worker.resize.result"));break;case 6:streamField(prepared,"sessionId",std::string("00000000-0000-0000-0000-000000000003"));break;case 7:streamField(prepared,"bitrate",1);break;case 8:streamField(prepared,"width",1600);break;case 9:streamField(prepared,"captureLeft",0);break;case 10:std::get<Json::Object>(prepared.value)["clipboardPolicy"].value=true;break;case 11:streamField(prepared,"injected",1);break;case 12:std::get<Json::Object>(prepared.value)["initialViewport"]=command.get("initialViewport");break;case 13:root["display"].value=Json::Object{};break;case 14:eligible=false;break;case 15:publication=false;break;case 16:std::get<Json::Object>(prepared.value).erase("captureDisplays");break;case 17:std::get<Json::Array>(std::get<Json::Object>(prepared.value)["captureDisplays"].value)[0].value=Json::Object{};break;}
      InitialViewportCompletion gate;require(gate.arm(command,true),"ACK rejection gate not armed");int publishes=0;require(!gate.acceptWorker(value,eligible,[&](const DisplayState&){++publishes;return publication;})&&!gate.workerVerified()&&!gate.preparedCommand(),"Bad private ACK permitted actual start");require(publishes==(scenario==15?1:0),"Unverified worker fields reached public cache");require(!gate.acceptMetadata(metadata(fixture.resize),result.display,true),"Rejected ACK was later accepted through metadata");++passed;
    }
    for(int scenario=0;scenario<12;++scenario){auto info=metadata(fixture.resize);bool eligible=true;auto display=result.display;switch(scenario){case 0:streamField(info,"sourceWidth",1280);break;case 1:streamField(info,"sourceHeight",720);break;case 2:streamField(info,"fpsTarget",30);break;case 3:streamField(info,"epoch",0);break;case 4:streamField(info,"resizeId",std::string("00000000-0000-0000-0000-000000000003"));break;case 5:streamField(info,"width",1282);break;case 6:streamField(info,"height",721);break;case 7:streamField(info,"width",0);break;case 8:streamField(info,"codec",std::string("other"));break;case 9:display.active=false;break;case 10:eligible=false;break;case 11:streamField(info,"type",std::string("stream.stats"));break;}
      InitialViewportCompletion gate;require(gate.arm(command,true)&&gate.acceptWorker(valid,true,[](const DisplayState&){return true;}),"Metadata rejection gate not armed");require(!gate.acceptMetadata(info,display,eligible)&&!gate.complete(),"Wrong source/epoch metadata committed initial display");++passed;
    }
  }
  for(bool profile:{false,true}){WorkerFixture fixture(true);auto command=initialCommand(fixture);if(profile)std::get<Json::Object>(command.value)["consoleProfile"]=JsonParser::parse(R"({"privateIntent":"preserved"})");StreamResizeBudget budget;const auto result=applyInitialNativeViewport(command,fixture.access(budget),budget,true,false,profile);require(result.ok,"Phase2 fixture could not prepare");auto phase2=result.command;if(profile){streamField(phase2,"profileOperation",std::string(64,'a'));streamField(phase2,"profileLaunchSha256",std::string(64,'b'));}require(initialViewportPhase2Matches(result.command,phase2),"Exact phase2 was rejected");++passed;
    for(int scenario=0;scenario<10;++scenario){auto changed=phase2;switch(scenario){case 0:streamField(changed,"epoch",0);break;case 1:streamField(changed,"sessionId",std::string("00000000-0000-0000-0000-000000000003"));break;case 2:streamField(changed,"width",1600);break;case 3:streamField(changed,"captureWidth",1920);break;case 4:streamField(changed,"type",std::string("worker.initial.prepare"));break;case 5:std::get<Json::Object>(changed.value)["initialViewport"]=command.get("initialViewport");break;case 6:streamField(changed,"extra",1);break;case 7:std::get<Json::Object>(changed.value)["consoleProfile"].value=false;break;case 8:streamField(changed,"profileOperation",std::string("bad"));break;case 9:streamField(changed,"profileLaunchSha256",std::string(64,'z'));break;}require(!initialViewportPhase2Matches(result.command,changed),"Unprepared phase2 command reached actual start");++passed;}
  }
  // Real native coordinator rollback remains exercised; no display/driver calls
  // below are native OS calls: the SDK transport + CCD/SetDisplayConfig are injected.
  {WorkerFixture fixture(true);fixture.fault="capture";const auto command=initialCommand(fixture);StreamResizeBudget budget;const auto result=applyInitialNativeViewport(command,fixture.access(budget),budget,true);require(result.ok&&!result.nativeStage&&!result.nativeError&&!result.helper.get("rollbackAttempted").boolean(),"Transient initial capture observation did not settle without SDK rollback");++passed;}
  for(const auto* fault:{"validate","capture-persistent","prepare-lost","commit-publication"}){WorkerFixture fixture(true);fixture.fault=fault;fixture.failValidate=std::string(fault)=="validate";const auto command=initialCommand(fixture);StreamResizeBudget budget;const auto result=applyInitialNativeViewport(command,fixture.access(budget),budget,true);
    const uint32_t expected=std::string(fault)=="validate"?4:std::string(fault)=="capture-persistent"?5:std::string(fault)=="prepare-lost"?3:6;
    require(!result.ok&&result.nativeStage==expected&&result.nativeError==uint32_t(std::string(fault)=="validate"?ERROR_BAD_CONFIGURATION:0)&&result.helper.get("rollbackSucceeded").boolean(),"Initial rollback erased the first failure phase or Windows code");fixture.transport.checkForeign();++passed;}
  for(const auto* fault:{"capture-persistent","prepare-lost","commit-publication"}){WorkerFixture fixture(true);fixture.fault=fault;const auto command=initialCommand(fixture);StreamResizeBudget budget;const auto result=applyInitialNativeViewport(command,fixture.access(budget),budget,true);require(!result.ok&&result.helper.get("rollbackAttempted").boolean()&&result.helper.get("rollbackSucceeded").boolean()&&!fixture.state().get("configurationPending").boolean()&&!fixture.state().get("recoveryRequired").boolean(),"Initial failure lost native rollback");require(fixture.probe().monitors.front().width==1920,"Initial rollback did not restore previous mode");fixture.transport.checkForeign();++passed;}
  for(const auto* fault:{"console","foreign","commit-lost"}){WorkerFixture fixture(true);fixture.fault=fault;const auto command=initialCommand(fixture);StreamResizeBudget budget;const auto result=applyInitialNativeViewport(command,fixture.access(budget),budget,true);require(!result.ok&&(!result.display.ready()||result.helper.get("rollbackSucceeded").boolean()),"Ambiguous initial failure reported ready");fixture.transport.checkForeign();++passed;}
  // Field case: every initial viewport failed in the read-only phase2
  // (worker.initial.result nativeStage 2, error 0) before any SDK prepare/CCD
  // apply. Each read-only rejection site must report its own fixed code, prove
  // nothing changed, and admit the service fallback that degrades the advertised
  // capability so the NEXT connection streams the current owned geometry.
  int fallbackCases=0;
  for(int scenario=0;scenario<9;++scenario){
    WorkerFixture f(true);const auto command=initialCommand(f);StreamResizeBudget budget;auto access=f.access(budget);
    const auto startKey=f.key();const auto startGeneration=f.state().get("generation").number();
    const auto owned=[&]()->DISPLAYCONFIG_PATH_INFO&{f.sync();for(auto& p:f.ccd.paths)if(p.targetInfo.adapterId.LowPart==44)return p;throw std::runtime_error("fixture owned path missing");};
    uint32_t expectedReason=0,expectedStage=2,expectedError=0;bool expectOk=false;int calls=0,observedCalls=0;
    switch(scenario){
      case 0:owned().targetInfo.refreshRate={59,1};expectedReason=15;break; // native_topology_refresh_mismatch (deterministic)
      case 1:owned().targetInfo.scaling=DISPLAYCONFIG_SCALING_ASPECTRATIOCENTEREDMAX;expectedReason=14;break; // transform mismatch
      case 2:access.recoveryScope=[](const NativeTopologyBaseline&)->NativeRecoveryScope{throw std::runtime_error("native_recovery_scope_unavailable");};expectedReason=uint32_t(NativeViewportReason::recoveryScopeUnavailable);break;
      case 3:{auto query=access.topology.query;access.topology.query=[query,&calls]{if(++calls>=2)throw NativeTopologyWindowsError(ERROR_ACCESS_DENIED);return query();};expectedReason=2;expectedError=ERROR_ACCESS_DENIED;break;}
      case 4:{auto snapshot=access.snapshot;access.snapshot=[snapshot,&calls]{auto value=snapshot();if(++calls>=2)std::get<Json::Object>(value.value)["configurationPending"].value=true;return value;};expectedReason=uint32_t(NativeViewportReason::snapshotPending);expectedStage=1;break;}
      case 5:{auto observed=access.viewport.observed;access.viewport.observed=[observed,&calls]{auto value=observed();if(++calls>=3&&!value.empty())value.back().bounds.right+=2;return value;};expectedReason=uint32_t(NativeViewportReason::captureSelectionInvalid);break;}
      case 6:{auto query=access.topology.query;access.topology.query=[query,&calls]{auto value=query();if(++calls==1)for(auto& p:value.paths)if(p.targetInfo.adapterId.LowPart==44)p.targetInfo.refreshRate={59,1};return value;};expectOk=true;break;} // one stale CCD sample settles
      case 7:{ // Windows renumbered \\.\DISPLAYn after the outer probe: resolve by owned id.
        const auto rename=[](std::wstring name){return name+L"7";};
        auto probe=access.viewport.probe;access.viewport.probe=[probe,rename,&calls](DWORD timeout){auto value=probe(timeout);if(++calls>=3){for(auto& m:value.monitors)m.name=rename(m.name);if(!value.monitors.empty())value.name=value.monitors.front().name;}return value;};
        auto observed=access.viewport.observed;access.viewport.observed=[observed,rename,&observedCalls,&f]{auto value=observed();const auto display=f.probe();if(++observedCalls>=3)for(auto& m:value)if(std::any_of(display.monitors.begin(),display.monitors.end(),[&](const auto& o){return o.name==m.name;}))m.name=rename(m.name);return value;};
        expectOk=true;break;}
      case 8:access.viewport.interactive=[]{return false;};expectedReason=0;expectedStage=1;break; // ineligible before the coordinator: no apply
    }
    const auto result=applyInitialNativeViewport(command,std::move(access),budget,true);
    if(expectOk){require(result.ok&&f.applies==1&&!result.unchanged&&!result.nativeReason,"Transient/renamed phase2 observation did not settle within the bounded retry");f.transport.checkForeign();++fallbackCases;continue;}
    if(result.ok||result.nativeStage!=expectedStage||result.nativeReason!=expectedReason||result.nativeError!=expectedError)throw std::runtime_error("phase2 scenario "+std::to_string(scenario)+": stage="+std::to_string(result.nativeStage)+" reason="+std::to_string(result.nativeReason)+" error="+std::to_string(result.nativeError));
    require(result.unchanged&&f.exchanges==0&&f.applies==0&&f.state().get("generation").number()==startGeneration&&!f.state().get("configurationPending").boolean()&&!f.state().get("recoveryRequired").boolean()&&f.key()==startKey,"Read-only phase2 rejection crossed the SDK/CCD mutation boundary");
    f.transport.checkForeign();
    // Exactly the worker.initial.result bytes the worker now sends.
    const auto reply=JsonParser::parse("{\"type\":\"worker.initial.result\","+streamResizeFields(f.resize)+",\"ok\":false,\"nativeStage\":"+std::to_string(result.nativeStage)+",\"nativeError\":"+std::to_string(result.nativeError)+",\"nativeReason\":"+std::to_string(result.nativeReason)+",\"unchanged\":true,\"desktopAvailable\":true,\"display\":"+workerDisplayJson(result.display)+",\"command\":"+jsonObject(result.command)+'}');
    const auto fallback=initialViewportFallbackAdmitted(reply,1,startKey,f.state());
    require(fallback&&fallback->stage==result.nativeStage&&fallback->reason==result.nativeReason&&fallback->lease==startKey,"No-change initial failure did not admit the current-geometry fallback");
    InitialViewportFallbackLatch latch;latch.arm(*fallback);require(latch.active(1,startKey)&&latch.active(1,startKey)&&!latch.active(2,startKey)&&!latch.armed(),"Fallback latch did not bind session+lease or failed to clear on change");
    latch.arm(*fallback);require(!latch.active(1,startKey+"/other")&&!latch.armed(),"Fallback survived an SDK lease change");
    // The plain non-initial start path binds exactly the CURRENT owned output.
    const auto display=f.probe();const auto selection=selectOwnedCapture(display,"0",f.observed());
    require(display.ready()&&selection.monitors.size()==1&&selection.bounds.right-selection.bounds.left==1920&&selection.bounds.bottom-selection.bounds.top==1080,"Current owned geometry is not streamable by the plain path");
    ++fallbackCases;
  }
  {
    // Admission negatives: the service never trusts the worker bit alone.
    WorkerFixture f(true);const auto key=f.key();
    const auto base=[&]{return JsonParser::parse("{\"type\":\"worker.initial.result\","+streamResizeFields(f.resize)+",\"ok\":false,\"nativeStage\":2,\"nativeError\":0,\"nativeReason\":15,\"unchanged\":true}");};
    require(initialViewportFallbackAdmitted(base(),1,key,f.state()).has_value(),"Admission baseline failed");
    for(int scenario=0;scenario<11;++scenario){auto value=base();auto state=f.state();std::string lease=key;DWORD session=1;auto& fields=std::get<Json::Object>(value.value);
      switch(scenario){case 0:fields["unchanged"].value=false;break;case 1:fields.erase("unchanged");break;case 2:fields["ok"].value=true;break;case 3:fields["nativeStage"].value=3.0;break;case 4:fields["nativeReason"].value=100.0;break;case 5:std::get<Json::Object>(state.value)["configurationPending"].value=true;break;case 6:std::get<Json::Object>(state.value)["generation"].value=state.get("generation").number()+1;break;case 7:lease.clear();break;case 8:session=0xffffffff;break;case 9:std::get<Json::Object>(state.value)["recoveryRequired"].value=true;break;case 10:fields["type"].value=std::string("worker.resize.result");break;}
      require(!initialViewportFallbackAdmitted(value,session,lease,state),"Fallback admitted without service-side no-change proof");++fallbackCases;}
  }
  {
    // Resize path (active stream): the same refresh mismatch reports phase2 +
    // its fixed site code instead of an anonymous stage102/error0.
    WorkerFixture f(true);f.sync();for(auto& p:f.ccd.paths)if(p.targetInfo.adapterId.LowPart==44)p.targetInfo.refreshRate={59,1};const auto result=f.run();
    require(!result.ok&&result.nativeStage==2&&result.nativeReason==15&&result.unchanged&&f.applies==0&&f.exchanges==0,"Resize phase2 rejection lost its fixed reason");
    const auto packet=JsonParser::parse("{\"ok\":false,\"nativeStage\":2,\"nativeError\":0,\"nativeReason\":36,\"profileStage\":0}");require(streamViewportDiagnostic(packet)&&streamViewportDiagnostic(packet)->nativeReason==36,"Worker reason 36 rejected");
    const auto unknown=JsonParser::parse("{\"ok\":false,\"nativeStage\":2,\"nativeError\":0,\"nativeReason\":39,\"profileStage\":0}");require(!streamViewportDiagnostic(unknown),"Unassigned worker reason accepted");++fallbackCases;
  }
  // Field case: owned primary at x=0 with the QEMU console adjacent
  // at its right edge. Shrinking the owned output makes Windows move the
  // console left by the same width delta BEFORE our CCD apply; phase 4 used to
  // reject that as native_topology_foreign_changed (stage104/reason 11).
  int reflowCases=0;
  for(int scenario=0;scenario<6;++scenario){
    WorkerFixture f(true);const auto command=initialCommand(f);
    const auto foreignMode=[&]()->DISPLAYCONFIG_SOURCE_MODE&{f.sync();for(auto& p:f.ccd.paths)if((p.flags&DISPLAYCONFIG_PATH_ACTIVE)&&p.targetInfo.adapterId.LowPart==7)return f.ccd.modes[nativeSourceIndex(p)].sourceMode;throw std::runtime_error("fixture console missing");};
    const auto ownedMode=[&]()->DISPLAYCONFIG_SOURCE_MODE&{f.sync();for(auto& p:f.ccd.paths)if((p.flags&DISPLAYCONFIG_PATH_ACTIVE)&&p.targetInfo.adapterId.LowPart==44)return f.ccd.modes[nativeSourceIndex(p)].sourceMode;throw std::runtime_error("fixture owned output missing");};
    ownedMode().position={0,0};foreignMode().position=scenario==1?POINTL{0,1080}:POINTL{1920,0}; // 1: console below
    const auto startKey=f.key();const auto startState=f.state();StreamResizeBudget budget;auto access=f.access(budget);
    // Windows keeps the console adjacent to the owned output's CURRENT width.
    const auto query=access.topology.query;
    access.topology.query=[&f,query,scenario,foreignMode]{auto value=query();const auto owned=nativeDisplayTargets(f.state()).front();const auto width=LONG(owned.width);(void)value;auto& console=foreignMode();
      if(scenario==1)console.position={0,LONG(owned.height)};else console.position.x=scenario==2&&width!=1920?width-100:width;  // 2: an arbitrary move, not the reflow delta
      if(scenario==3)console.height=width!=1920?1000:900;            // 3: transient foreign mode change
      if(scenario==4)for(auto& p:f.ccd.paths)if(p.targetInfo.adapterId.LowPart==7&&width!=1920)p.targetInfo.refreshRate={75,1};else if(p.targetInfo.adapterId.LowPart==7)p.targetInfo.refreshRate={60,1};
      return f.ccd;};
    if(scenario==5){f.resize.width=2560;f.resize.height=1440;} // growth moves the console right
    const auto launch=scenario==5?initialCommand(f):command;
    const auto result=applyInitialNativeViewport(launch,std::move(access),budget,true);
    if(scenario==0||scenario==1||scenario==5){
      require(result.ok&&f.applies>=1,("Adjacent console re-layout was rejected (scenario "+std::to_string(scenario)+" stage "+std::to_string(result.nativeStage)+" reason "+std::to_string(result.nativeReason)+")").c_str());
      require((scenario==1?foreignMode().position.x==0&&foreignMode().position.y==LONG(f.resize.height):foreignMode().position.x==LONG(f.resize.width)&&foreignMode().position.y==0)&&foreignMode().width==1600&&foreignMode().height==900,"Console geometry other than the automatic re-layout changed");
      require(result.display.monitors.size()==1&&result.display.monitors.front().left==0&&result.display.monitors.front().width==DWORD(f.resize.width),"Owned output did not keep the desktop origin");
      f.transport.checkForeign();++reflowCases;continue;
    }
    // A non-reflow move or any foreign mode change is still a foreign change.
    require(!result.ok&&result.nativeStage==4&&result.nativeReason==11,("Foreign change accepted (scenario "+std::to_string(scenario)+" stage "+std::to_string(result.nativeStage)+" reason "+std::to_string(result.nativeReason)+")").c_str());
    require(result.restored&&!result.unchanged&&result.helper.get("rollbackSucceeded").boolean()&&!f.state().get("configurationPending").boolean()&&!f.state().get("recoveryRequired").boolean()&&nativeDisplayTargets(f.state()).front().width==1920,"Rollback did not restore the original owned geometry");
    require(foreignMode().position.x==1920&&foreignMode().height==900,"Rollback did not return the console to its original position");
    const auto reply=JsonParser::parse("{\"type\":\"worker.initial.result\","+streamResizeFields(f.resize)+",\"ok\":false,\"nativeStage\":4,\"nativeError\":0,\"nativeReason\":11,\"unchanged\":false,\"restored\":true}");
    const auto fallback=initialViewportFallbackAdmitted(reply,1,startKey,f.state(),startState);
    require(fallback&&fallback->stage==4&&fallback->reason==11&&fallback->lease==f.key()&&fallback->lease!=startKey,"Verified rollback did not admit the current-geometry fallback bound to the restored lease");
    {auto changed=f.state();std::get<Json::Object>(std::get<Json::Array>(std::get<Json::Object>(changed.value)["targets"].value)[0].value)["width"].value=1600.0;require(!initialViewportFallbackAdmitted(reply,1,startKey,changed,startState),"Fallback admitted with a non-restored owned size");}
    {auto pending=f.state();std::get<Json::Object>(pending.value)["configurationPending"].value=true;require(!initialViewportFallbackAdmitted(reply,1,startKey,pending,startState),"Fallback admitted while a change is pending");}
    require(!initialViewportFallbackAdmitted(reply,1,startKey,f.state()),"Rollback fallback admitted without the start snapshot");
    {auto bad=reply;std::get<Json::Object>(bad.value)["restored"].value=false;require(!initialViewportFallbackAdmitted(bad,1,startKey,f.state(),startState),"Unverified phase-4 failure admitted");}
    f.transport.checkForeign();++reflowCases;
  }
  {
    // Direct planner rule: only right/below neighbours of the changed output
    // may follow its size delta; nothing else about them may differ.
    WorkerFixture f(true);f.sync();auto& owned=f.ccd.modes[nativeSourceIndex(*std::find_if(f.ccd.paths.begin(),f.ccd.paths.end(),[](const auto& p){return (p.flags&DISPLAYCONFIG_PATH_ACTIVE)&&p.targetInfo.adapterId.LowPart==44;}))].sourceMode;owned.position={0,0};
    auto& console=f.ccd.modes[nativeSourceIndex(f.ccd.paths.front())].sourceMode;console.position={1920,0};
    const auto targets=nativeDisplayTargets(f.state());const auto baseline=captureNativeTopologyBaseline(f.ccd,targets,"0");auto resized=targets;resized[0].width=1264;resized[0].height=760;
    auto moved=f.ccd;moved.modes[nativeSourceIndex(moved.paths.front())].sourceMode.position.x=1264;
    const auto expected=nativeForeignTransitionExpectation(baseline,moved,resized);require(expected&&nativeCcdSource(*expected,expected->paths.front()).position.x==1264,"Exact reflow delta rejected");
    auto left=f.ccd;left.modes[nativeSourceIndex(left.paths.front())].sourceMode.position={-1600,0};owned.position={0,0};
    const auto leftBaseline=captureNativeTopologyBaseline(left,targets,"0");auto leftMoved=left;leftMoved.modes[nativeSourceIndex(leftMoved.paths.front())].sourceMode.position.x=-1600-656;
    require(!nativeForeignTransitionExpectation(leftBaseline,leftMoved,resized),"A left neighbour may not follow a right-edge size change");
    require(nativeForeignTransitionExpectation(leftBaseline,left,resized).has_value(),"Unmoved left neighbour rejected");
    auto refresh=moved;refresh.paths.front().targetInfo.refreshRate={75,1};require(!nativeForeignTransitionExpectation(baseline,refresh,resized),"Moved console with a refresh change accepted");
    auto inactive=moved;inactive.paths.front().flags&=~DISPLAYCONFIG_PATH_ACTIVE;require(!nativeForeignTransitionExpectation(baseline,inactive,resized),"Console turned off accepted as re-layout");
    ++reflowCases;
  }
  passed+=fallbackCases+reflowCases;
  std::cout<<"{\"initialViewportCases\":"<<passed<<",\"phase2FallbackCases\":"<<fallbackCases<<",\"twoPhasePrepareBeforeCapture\":true,\"actualNativeWorkerCoordinator\":true,\"actualSdkControllerAndLeaseStore\":true,\"actualPrivateRpcDispatcher\":true,\"encoderCeilingsPreserved\":true,\"queryAndSetDisplayConfig\":\"substituted\",\"journals\":\"memory\",\"windowsDeviceRegistryCertificateChanges\":0,\"actualRemoteSessionTested\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<"initial viewport fixture failed after "<<passed<<" cases: "<<error.what()<<'\n';return 1;}}
