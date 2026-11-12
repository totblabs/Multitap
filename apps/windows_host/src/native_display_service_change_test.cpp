#include "service.hpp"
#include "native_track_change_rpc.hpp"
#include "native_display_worker_tracks.hpp"
#include "../tests/native-recovery-scope-fixture.hpp"
#include <iostream>
#include <source_location>
#ifdef MULTITAP_NATIVE_DISPLAY
#include <virtual_display/driver/lease_store.h>
using namespace multitap::vdisplay;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
constexpr uint64_t ownedLease=api::kMinOpaqueLeaseId+321,foreignLease=api::kMinOpaqueLeaseId+654;
struct Transport final:api::ControlTransport {
  api::DisplayStore store{2,8};int creates=0,removes=0,releases=0,feeds=0,failCreate=0,queries=0,queryFault=0,queryFaultAt=0;bool stuck=false,badReply=false,feedFailure=false;
  std::vector<api::LeaseDisplayRequest> pending;
  bool ioctl(uint32_t code,const void* input,size_t,void* output,size_t,size_t& size,uint32_t& error)override{
    error=0;size=0;const auto now=std::chrono::steady_clock::now();
    if(code==api::kIoctlGetProtocolVersion){*static_cast<api::ProtocolVersion*>(output)={};size=sizeof(api::ProtocolVersion);return true;}
    if(code==api::kIoctlQueryLease){++queries;const int fault=queryFaultAt&&queryFaultAt!=queries?0:queryFault;if(fault==1){error=5;return false;}if(!stuck){for(const auto& item:pending)require(store.remove_temporary_display(item).error==api::StoreError::None,"Fixture departure failed");pending.clear();}auto value=store.query_lease(static_cast<const api::LeaseRequest*>(input)->lease_id,now);if(fault==2)value.lease_id++;if(fault==3)value.lease_exists=true;if(fault==4)value.temporary_display_count=1;if(fault==5)value.pending_departure_count=1;if(fault==6)value.lease_exists=false;if(fault==7)value.temporary_display_count=3;*static_cast<api::QueryLeaseResult*>(output)=value;size=sizeof(api::QueryLeaseResult);return true;}
    if(code==api::kIoctlCreateTemporaryDisplayOwned){++creates;if(creates==failCreate){error=5;return false;}const auto& request=*static_cast<const api::CreateTemporaryDisplayOwnedRequest*>(input);auto result=store.create_temporary_display_owned(request,now);if(result.status.error!=api::StoreError::None){error=5;return false;}result.result.target_id=uint32_t(creates);result.result.os_adapter_luid.low_part=44;if(badReply)result.result.lease_id++;*static_cast<api::CreateTemporaryDisplayResult*>(output)=result.result;size=sizeof(result.result);return true;}
    if(code==api::kIoctlRemoveTemporaryDisplay){++removes;const auto request=*static_cast<const api::LeaseDisplayRequest*>(input);require(request.lease_id==ownedLease&&request.display_id!=999,"Foreign display removal attempted");require(store.mark_temporary_display_pending_departure(request).error==api::StoreError::None,"Fixture remove failed");pending.push_back(request);return true;}
    if(code==api::kIoctlFeedLease){++feeds;if(feedFailure){error=5;return false;}error=store.feed_lease(*static_cast<const api::LeaseRequest*>(input),now).error==api::StoreError::None?0:5;return !error;}
    if(code==api::kIoctlReleaseLease){++releases;const auto request=*static_cast<const api::LeaseRequest*>(input);require(request.lease_id==ownedLease,"Foreign lease release attempted");error=store.release_lease(request).error==api::StoreError::None?0:5;return !error;}
    throw std::runtime_error("Unexpected driver control");
  }
  void foreign(){api::CreateTemporaryDisplayOwnedRequest request{};request.display.lease_id=foreignLease;request.display.display_id=999;request.display.width=1280;request.display.height=720;request.display.physical_width_mm=600;request.display.physical_height_mm=340;request.display.refresh_rate_millihz=60000;request.display.requested_timeout_ms=15000;std::memcpy(request.display.display_name,"Other product",14);request.owner_capability.bytes.fill(9);require(store.create_temporary_display_owned(request,std::chrono::steady_clock::now()).status.error==api::StoreError::None,"Fixture foreign lease failed");}
  void checkForeign(){const auto display=store.find_temporary_display(999);require(display&&display->lease_id==foreignLease&&!display->pending_departure&&display->width==1280,"Foreign lease changed");}
};
std::string fixtureBaseline(const host::Json& state,const std::string& monitor);
struct Fixture {
  Transport transport;api::ControlClient client{transport};std::vector<std::string> published;int configReads=0;std::unique_ptr<host::NativeDisplayService> service;host::Json config;bool failPublish=false,failJournal=false,failBaseline=false,allowRecovery=false,profilePending=false;uint64_t recoveryTick=1000;std::optional<std::string> journal,baselineBytes;int journalWrites=0,failJournalAt=0,baselineWrites=0;std::vector<std::tuple<std::string,int,int>> journalEvents;
  Fixture(bool single=false,std::optional<host::NativeRuntimeRecord> boot={},bool verified=true,std::optional<std::string> savedBaseline={},bool failInitialCreate=false){transport.foreign();if(failInitialCreate)transport.failCreate=1;baselineBytes=std::move(savedBaseline);api::OwnerCapability owner{};owner.bytes[0]=42;std::vector<Output> outputs{{71,1920,1080},{72,1280,720}};
    config=host::JsonParser::parse(R"({"provider":"libvirtualdisplay-1.6.4","enabled":true,"deviceInstanceId":"ROOT\\DISPLAY\\00a1"})");host::streamField(config,"monitors",single?1:2);
    host::NativeRuntimeRecord record{"ROOT\\DISPLAY\\00a1","stopped",1,{{"0",1920,1080}}};if(!single)record.outputs.push_back({"1",1280,720});journal=host::nativeRuntimeBytes(boot?*boot:record);
    service=std::make_unique<host::NativeDisplayService>([this,owner]{return std::make_unique<Controller>(client,ownedLease,owner,std::chrono::milliseconds(0));},outputs,[this]{++configReads;return config;},[this](const auto& bytes){if(failPublish)throw std::runtime_error("Fixture snapshot failure");published.push_back(bytes);},host::NativeRuntimeJournalAccess{[this]{return journal;},[this](const auto& expected,const std::string& bytes){++journalWrites;if(failJournal||journalWrites==failJournalAt||expected!=journal)return false;const auto record=host::parseNativeRuntimeRecord(bytes);if(!record.baselineHash.empty())require(baselineBytes&&host::nativeBaselineDigest(*baselineBytes)==record.baselineHash,"Runtime journal referred to an unstored CCD checkpoint");journalEvents.emplace_back(record.phase,transport.creates,transport.removes);journal=bytes;return true;}},host::NativeRuntimeJournalAccess{[this]{return baselineBytes;},[this](const auto& expected,const std::string& bytes){++baselineWrites;if(failBaseline||expected!=baselineBytes)return false;baselineBytes=bytes;return true;}},[this]{return allowRecovery&&quiescentOwnedLease(client,ownedLease);},[this]{return recoveryTick;},[this]{return profilePending;});
    service->tick();if(verified)require(service->activationReady(DWORD(state().get("generation").number())),"Fixture initial verified activation failed");
  }
  ~Fixture(){service.reset();}
  host::Json state(){return host::JsonParser::parse(service->viewportSnapshot());}
  std::string key(){return host::nativeDisplayLeaseKey(state());}
  std::string baseline(const std::string& monitor="0"){return fixtureBaseline(state(),monitor);}
  host::NativeViewportReply finish(const host::StreamResize& request,const std::string& lease,bool rollback,bool verified){return service->finishViewport(request,lease,rollback,verified,host::test::currentScope());}
  host::NativeViewportReply prepare(const host::StreamResize& request,int64_t deadline=0){return service->prepareViewport(request,key(),deadline?deadline:host::streamMonotonicMillis()+80000,baseline(request.monitor),host::test::currentScope());}
};
host::StreamResize request(){return host::streamResizeRequest(host::JsonParser::parse(R"({"sessionId":"00000000-0000-0000-0000-000000000001","requestId":"native-viewport-qa","resizeId":"00000000-0000-0000-0000-000000000002","monitor":"0","width":1600,"height":900,"fps":60,"epoch":1})"));}
host::NativeViewportContext context(const host::StreamResize& request){host::NativeViewportContext value;value.pending=request;value.deadline=host::streamMonotonicMillis()+80000;value.authorized=value.console=value.unlocked=true;value.stopped=false;value.scope=host::test::currentScope();return value;}
host::Json frame(const host::StreamResize& request,const char* phase,const std::string& key,bool verified=false){return host::JsonParser::parse("{\"type\":"+host::jsonQuote(phase)+','+host::nativeResizeFields(request)+",\"leaseKey\":"+host::jsonQuote(key)+",\"windowsVerified\":"+(verified?"true":"false")+'}');}
host::Json dispatch(const host::Json& value,const host::NativeViewportContext& ctx,Fixture& fixture){auto incoming=value;if(incoming.get("type").string()=="worker.native.resize.prepare"&&std::holds_alternative<std::nullptr_t>(incoming.get("baseline").value))host::streamField(incoming,"baseline",fixture.baseline(incoming.get("monitor").string()));const auto reply=host::dispatchNativeViewport(incoming,ctx,fixture.service.get());require(reply.handled&&!reply.response.empty(),"Expected native RPC reply missing");return host::JsonParser::parse(reply.response);}
struct WorkerFixture:Fixture {
  host::NativeCcdData ccd;bool eligible=true,failValidate=false,badCapture=false;int applies=0,exchanges=0;uint64_t ticks=0;std::string fault;host::StreamResize resize=request();
  static DISPLAYCONFIG_PATH_INFO path(LUID adapter,UINT32 source,UINT32 target){DISPLAYCONFIG_PATH_INFO p{};p.sourceInfo.adapterId=p.targetInfo.adapterId=adapter;p.sourceInfo.id=source;p.targetInfo.id=target;p.sourceInfo.modeInfoIdx=p.targetInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;p.targetInfo.targetAvailable=true;p.targetInfo.rotation=DISPLAYCONFIG_ROTATION_IDENTITY;p.targetInfo.scaling=DISPLAYCONFIG_SCALING_IDENTITY;p.targetInfo.refreshRate={60,1};p.targetInfo.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;return p;}
  static host::NativeCcdData materialize(host::NativeCcdData value){for(auto& p:value.paths)if(host::nativeTargetIndex(p)==DISPLAYCONFIG_PATH_MODE_IDX_INVALID){const auto s=host::nativeCcdSource(value,p);DISPLAYCONFIG_MODE_INFO mode{};mode.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;mode.adapterId=p.targetInfo.adapterId;mode.id=p.targetInfo.id;auto& signal=mode.targetMode.targetVideoSignalInfo;signal.activeSize={s.width,s.height};signal.totalSize={s.width+160,s.height+30};signal.pixelRate=uint64_t(s.width+160)*(s.height+30)*60;signal.vSyncFreq={60,1};signal.hSyncFreq={67500,1};signal.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;p.targetInfo.modeInfoIdx=UINT32(value.modes.size());value.modes.push_back(mode);}return value;}
  WorkerFixture(bool single=false,std::optional<host::NativeRuntimeRecord> recovery={}):Fixture(single,recovery,!recovery){if(recovery){allowRecovery=true;recoveryTick=2000;service->tick();}auto foreign=path({7,0},0,9);foreign.flags=DISPLAYCONFIG_PATH_ACTIVE;foreign.sourceInfo.modeInfoIdx=0;DISPLAYCONFIG_MODE_INFO mode{};mode.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;mode.adapterId={7,0};mode.id=0;mode.sourceMode={1600,900,DISPLAYCONFIG_PIXELFORMAT_32BPP,{0,0}};ccd.paths.push_back(foreign);ccd.modes.push_back(mode);ccd=materialize(ccd);sync();if(!recovery)ccd=materialize(host::planNativeTopology(ccd,host::nativeDisplayTargets(state())).supplied);}
  void sync(){const auto targets=host::nativeDisplayTargets(state());ccd.paths.erase(std::remove_if(ccd.paths.begin(),ccd.paths.end(),[&](const auto& p){return p.targetInfo.adapterId.LowPart==44&&std::none_of(targets.begin(),targets.end(),[&](const auto& target){return host::nativeSameTarget(p,target);});}),ccd.paths.end());for(const auto& target:targets)if(std::none_of(ccd.paths.begin(),ccd.paths.end(),[&](const auto& p){return host::nativeSameTarget(p,target);}))ccd.paths.push_back(path(target.adapter,target.target+10,target.target));}
  static std::wstring name(const DISPLAYCONFIG_PATH_INFO& p){return L"\\\\.\\DISPLAY"+std::to_wstring(p.sourceInfo.adapterId.LowPart==7?1:p.sourceInfo.id+2);}
  host::DisplayState probe(){sync();host::DisplayState display;display.installed=display.deviceReady=display.owned=true;display.foreign=0;display.configurationPending=state().get("configurationPending").boolean();display.recoveryRequired=state().get("recoveryRequired").boolean();try{display.monitors=host::nativeDisplayMonitors(host::nativeDisplayTargets(state()),ccd.paths,ccd.modes,name);const auto& first=display.monitors.front();display.name=first.name;display.left=first.left;display.top=first.top;LONG right=first.left+first.width,bottom=first.top+first.height;for(const auto& monitor:display.monitors){display.left=std::min(display.left,monitor.left);display.top=std::min(display.top,monitor.top);right=std::max(right,monitor.left+LONG(monitor.width));bottom=std::max(bottom,monitor.top+LONG(monitor.height));}display.width=DWORD(right-display.left);display.height=DWORD(bottom-display.top);display.active=true;}catch(...){display.configurationPending=true;}return display;}
  std::vector<host::CaptureMonitor> observed(){sync();std::vector<host::CaptureMonitor> out;for(const auto& p:ccd.paths)if(p.flags&DISPLAYCONFIG_PATH_ACTIVE){const auto s=host::nativeCcdSource(ccd,p);out.push_back({name(p),L"fixture-"+std::to_wstring(p.targetInfo.adapterId.LowPart)+L"-"+std::to_wstring(p.targetInfo.id),{s.position.x,s.position.y,s.position.x+LONG(s.width),s.position.y+LONG(s.height)},s.position.x==0&&s.position.y==0});}if(badCapture||(fault=="capture-persistent"&&applies==1)){badCapture=false;out.back().bounds.right+=2;}return out;}
  host::Json current(){auto command=host::JsonParser::parse("{\"type\":\"session.start\",\"resizeProtocol\":\"epoch-v1\",\"sessionId\":"+host::jsonQuote(resize.sessionId)+",\"monitor\":"+host::jsonQuote(resize.monitor)+",\"epoch\":0}");host::attachFreshCapture(command,probe(),observed());return command;}
  host::NativeWorkerViewportAccess access(const host::StreamResizeBudget& budget){
    host::NativeTopologyAccess topology{[this]{sync();return ccd;},[this](const host::NativeCcdData& value,UINT32 flags){require(!(flags&(SDC_ALLOW_CHANGES|SDC_SAVE_TO_DATABASE|SDC_TOPOLOGY_EXTEND|SDC_TOPOLOGY_CLONE)),"Worker used global/persistent Windows display flags");if(!(flags&SDC_APPLY)){if(failValidate){failValidate=false;return LONG(ERROR_BAD_CONFIGURATION);}return LONG(ERROR_SUCCESS);}++applies;ccd=materialize(value);if(fault=="capture"&&applies==1)badCapture=true;return LONG(ERROR_SUCCESS);},[this]{return eligible;},[this]{return ticks;},[this]{ticks+=50;}};
    return {{[this](DWORD){return probe();},[this]{return observed();},[this]{return eligible;},[]{return true;},[](const std::wstring&,int,int,DWORD)->host::DisplayHelperOutput{throw std::runtime_error("Native worker called legacy helper");}},[this]{return state();},topology,[this,budget](const std::string& phase,const std::string& key,bool verified,const std::string& baseline){++exchanges;std::optional<host::Json> response;host::NativeWorkerRpcAccess rpc{[&](const std::string& bytes){if(fault=="commit-publication"&&phase=="worker.native.resize.commit")failPublish=true;response=dispatch(host::JsonParser::parse(bytes),context(resize),*this);failPublish=false;sync();if(phase=="worker.native.resize.prepare"){if(fault=="console")eligible=false;if(fault=="foreign")ccd.modes[host::nativeSourceIndex(ccd.paths.front())].sourceMode.width+=2;}return true;},[&](DWORD)->std::optional<host::Json>{if((fault=="prepare-lost"&&phase=="worker.native.resize.prepare")||(fault=="commit-lost"&&phase=="worker.native.resize.commit"))return {};auto value=response;response.reset();return value;},{}};return host::nativeWorkerViewportRpc(resize,phase,key,verified,budget,rpc,baseline);},[](const host::NativeTopologyBaseline& value){return host::test::recoveryScope(value);}};
  }
  host::WorkerViewportResult run(){host::StreamResizeBudget budget;return host::applyNativeWorkerViewport(current(),resize,resize.monitor,access(budget),budget);}
};
std::string fixtureBaseline(const host::Json& state,const std::string& monitor){using namespace host;const auto targets=nativeDisplayTargets(state);NativeCcdData ccd;auto foreign=WorkerFixture::path({7,0},0,9);foreign.flags=DISPLAYCONFIG_PATH_ACTIVE;foreign.sourceInfo.modeInfoIdx=0;DISPLAYCONFIG_MODE_INFO mode{};mode.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;mode.adapterId={7,0};mode.id=0;mode.sourceMode={1600,900,DISPLAYCONFIG_PIXELFORMAT_32BPP,{0,0}};ccd.paths.push_back(foreign);ccd.modes.push_back(mode);ccd=WorkerFixture::materialize(ccd);for(const auto& target:targets)ccd.paths.push_back(WorkerFixture::path(target.adapter,target.target+10,target.target));ccd=WorkerFixture::materialize(planNativeTopology(ccd,targets).supplied);const auto chosen=std::any_of(targets.begin(),targets.end(),[&](const auto& target){return target.id==monitor;})?monitor:std::string("0");const auto baseline=captureNativeTopologyBaseline(ccd,targets,chosen);return nativeRecoveryBaselineBytes({state.get("deviceInstanceId").string(),DWORD(state.get("generation").number()),baseline,test::recoveryScope(baseline)});}
host::NativeTrackResize trackRequest(){return host::nativeTrackResizeRequest(host::JsonParser::parse(R"({"sessionId":"00000000-0000-0000-0000-000000000001","requestId":"native-tracks-qa","resizeId":"00000000-0000-0000-0000-000000000007","topologyEpoch":2,"fps":60,"tracks":[{"id":"0","width":1600,"height":900,"x":0,"y":0,"epoch":2},{"id":"1","width":1280,"height":1024,"x":1600,"y":100,"epoch":2}]})"));}
int profileViewportLifecycleCases(){
  using namespace host;
  int passed=0;
  for(int scenario=0;scenario<22;++scenario){
  WorkerFixture f(true);
  f.resize.width=1380;f.resize.height=970;
  if(scenario==20){auto extra=WorkerFixture::path({8,0},0,19);extra.flags=DISPLAYCONFIG_PATH_ACTIVE;extra.sourceInfo.modeInfoIdx=UINT32(f.ccd.modes.size());DISPLAYCONFIG_MODE_INFO source{};source.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;source.adapterId={8,0};source.id=0;source.sourceMode={800,600,DISPLAYCONFIG_PIXELFORMAT_32BPP,{3520,0}};f.ccd.modes.push_back(source);f.ccd.paths.push_back(extra);f.ccd=WorkerFixture::materialize(f.ccd);}
  const auto original=f.ccd;
  const auto consolePath=original.paths.front();
  const NativeConsoleAdapterIdentity console{{7,0},L"\\\\?\\PCI#VEN_1234&DEV_1111#0",L"PCI\\VEN_1234&DEV_1111\\3&0",L"PCI",{L"PCI\\VEN_1234&DEV_1111"},nativeConsoleDisplayClass,true,true};
  const std::string machine="00112233-4455-6677-8899-aabbccddeeff",operation(64,'b');
  auto observedMachine=machine;auto observedConsole=console;std::string observedLogon(64,'a');bool launchValid=true;
  std::optional<std::vector<std::vector<int64_t>>> returnedForeign;
  std::optional<std::string> profileBytes,receiptBytes;
  const NativeRuntimeJournalAccess profileStore{[&]{return profileBytes;},[&](const auto& expected,const auto& desired){if(profileBytes!=expected)return false;profileBytes=desired;return true;}};
  const NativeRuntimeJournalAccess receiptStore{[&]{return receiptBytes;},[&](const auto& expected,const auto& desired){if(receiptBytes!=expected)return false;receiptBytes=desired;return true;}};
  f.service->testProfileViewportStores(profileStore,receiptStore);
  require(f.service->reserveStreamProfile(f.resize.sessionId,f.key()),"Lifecycle service reservation failed");
  uint64_t tick=1000;bool rejectFirstViewport=scenario==21;
  const auto scope=[&](const NativeTopologyBaseline& baseline){
    auto value=test::recoveryScope(baseline);value.tick=++tick;
    for(auto& foreign:value.foreign)if(nativeSameLuid(foreign.targetAdapter,console.adapter))foreign.sourcePath=foreign.targetPath=utf8(console.interfacePath);
    return value;
  };
  const auto apply=[&](const NativeCcdData& supplied,UINT32 flags)->LONG{
    if(scenario==19&&(flags&SDC_VALIDATE)&&f.applies==1)observedLogon=std::string(64,'f');
    if(rejectFirstViewport&&(flags&SDC_VALIDATE)&&f.applies==1){rejectFirstViewport=false;return ERROR_BAD_CONFIGURATION;}
    if(!(flags&SDC_APPLY))return ERROR_SUCCESS;
    ++f.applies;f.ccd=WorkerFixture::materialize(supplied);
    // QDC_ALL_PATHS retains the positively identified, available console even
    // while it is inactive. Inactive paths have no usable mode indices.
    if(std::none_of(f.ccd.paths.begin(),f.ccd.paths.end(),[&](const auto& path){return nativeSameLuid(path.targetInfo.adapterId,console.adapter);}))
      f.ccd.paths.push_back(WorkerFixture::path(console.adapter,consolePath.sourceInfo.id,consolePath.targetInfo.id));
    return ERROR_SUCCESS;
  };
  NativeTopologyAccess topology{[&]{f.sync();return f.ccd;},apply,[&]{return f.eligible;},[&]{return ++tick;},[&]{tick+=50;}};
  NativeStreamLaunchReceipt launch;
  launch.key={f.resize.sessionId,operation,f.state().get("deviceInstanceId").string(),17};
  // Process/console/CCD observations are injected fixtures. SDK controller,
  // private viewport dispatcher, durable journal and receipt codecs are real.
  launch.service={GetCurrentProcessId(),100};launch.worker={0x232323,101};
  launch.scope={17,std::string(64,'a'),999,{}};
  launch.entry=nativeStreamProfileLease(f.state(),GetCurrentProcessId());
  launch.lease=ownedLease;launch.outputs={{71,0}};
  const auto launchBytes=nativeStreamLaunchBytes(launch);
  auto command=JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"stream\",\"resizeProtocol\":\"epoch-v1\",\"epoch\":0,\"monitor\":\"0\",\"sessionId\":"+jsonQuote(f.resize.sessionId)+",\"profileLaunchSha256\":"+jsonQuote(nativeBaselineDigest(launchBytes))+",\"consoleProfile\":{\"protocol\":\"console-profile-v1\",\"sessionId\":"+jsonQuote(f.resize.sessionId)+",\"machineUuid\":"+jsonQuote(machine)+",\"model\":\"std\"}}");
  NativeStreamProfileWorkerAccess access;
  access.eligible=[&](bool){return f.eligible;};access.snapshot=[&](bool){return f.state();};access.topology=topology;access.scope=scope;
  access.loginScope=[&]{return NativeRecoveryScope{17,observedLogon,++tick,{}};};
  access.machineUuid=[&]{return observedMachine;};access.resolver=[&](LUID luid)->std::optional<NativeConsoleAdapterIdentity>{if(nativeSameLuid(luid,console.adapter))return observedConsole;if(scenario==20&&nativeSameLuid(luid,LUID{8,0}))return NativeConsoleAdapterIdentity{{8,0},L"\\\\?\\PCI#VEN_10DE&DEV_1EB8#OTHER",L"PCI\\VEN_10DE&DEV_1EB8\\4&OTHER",L"PCI",{L"PCI\\VEN_10DE&DEV_1EB8"},nativeConsoleDisplayClass,true,true};return {};};
  int captureStops=0;access.stopCapture=[&]{++captureStops;};
  access.launchProof=[&](const NativeStreamProfileRecord& record,bool){return launchValid&&nativeStreamLaunchMatches(parseNativeStreamLaunch(launchBytes),record);};
  access.viewportCommit=[&]()->std::optional<std::pair<NativeStreamViewportCommit,std::string>>{if(!receiptBytes||!f.journal)return {};return std::make_pair(parseNativeStreamViewportCommit(*receiptBytes),*f.journal);};
  NativeStreamProfileWorker worker(GetCurrentProcessId(),17,profileStore,access);
  require(worker.begin(command,operation)&&worker.active(),"Lifecycle worker entry failed");
  require(std::count_if(f.ccd.paths.begin(),f.ccd.paths.end(),[](const auto& path){return bool(path.flags&DISPLAYCONFIG_PATH_ACTIVE);})==(scenario==20?2:1),"Lifecycle entry did not suppress only console");
  attachFreshCapture(command,f.probe(),f.observed());
  const auto entryCapture=f.observed();
  require(worker.reserveViewport(f.resize)&&!worker.active(),"Lifecycle worker reservation failed");
  StreamResizeBudget budget;auto viewport=f.access(budget);viewport.topology=topology;viewport.recoveryScope=scope;
  viewport.console=[&](const NativeTopologyBaseline& baseline){auto candidate=baseline;auto request=f.resize;if(scenario==15)candidate.placements.front().left+=2;if(scenario==16)request.requestId+="-other";if(scenario==17)request.sessionId="00000000-0000-0000-0000-000000000003";return worker.viewportConsole(request,candidate);};viewport.consoleCurrent=[&](const NativeConsoleBinding& binding){return worker.viewportConsoleCurrent(f.resize,binding);};
  if(scenario==4){viewport.console={};viewport.consoleCurrent={};}
  if(scenario==14)viewport.consoleCurrent={};
  if(scenario==18)observedLogon=std::string(64,'f');
  int delayedProbes=0,delayedCaptures=0;
  viewport.viewport.probe=[&](DWORD){auto display=f.probe();if(scenario==1&&f.applies==2&&delayedProbes++<2){display.active=false;display.monitors.clear();}return display;};
  viewport.viewport.observed=[&]{if(scenario==2&&f.applies==2&&delayedCaptures++<2)return entryCapture;return f.observed();};
  viewport.exchange=[&](const std::string& phase,const std::string& key,bool verified,const std::string& baseline){
    std::optional<Json> response;
    NativeWorkerRpcAccess rpc{[&](const std::string& bytes){
      auto ctx=context(f.resize);ctx.scope->tick=++tick;response=dispatch(JsonParser::parse(bytes),ctx,f);
      if(scenario>=3&&phase=="worker.native.resize.prepare"){
        f.sync();f.ccd.paths.erase(std::remove_if(f.ccd.paths.begin(),f.ccd.paths.end(),[&](const auto& path){return nativeSameLuid(path.targetInfo.adapterId,console.adapter);}),f.ccd.paths.end());
        auto returned=consolePath;
        returned.sourceInfo.modeInfoIdx=UINT32(f.ccd.modes.size());f.ccd.modes.push_back(nativeCcdMode(original,nativeSourceIndex(consolePath),DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE));
        returned.targetInfo.modeInfoIdx=UINT32(f.ccd.modes.size());f.ccd.modes.push_back(nativeCcdMode(original,nativeTargetIndex(consolePath),DISPLAYCONFIG_MODE_INFO_TYPE_TARGET));
        f.ccd.paths.insert(f.ccd.paths.begin(),returned);
        if(scenario==5)observedMachine[0]='1';
        if(scenario==6)observedConsole.instanceId+=L"X";
        if(scenario==7)observedLogon=std::string(64,'f');
        if(scenario==8){auto changed=parseNativeStreamProfileRecord(*profileBytes);++changed.revision;profileBytes=nativeStreamProfileRecordBytes(changed);}
        if(scenario==9)launchValid=false;
        if(scenario==10)f.eligible=false;
        if(scenario==11){auto extra=returned;extra.sourceInfo.adapterId=extra.targetInfo.adapterId={8,0};extra.targetInfo.id=19;auto source=nativeCcdMode(original,nativeSourceIndex(consolePath),DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE);source.adapterId={8,0};source.sourceMode.position.x=1600;extra.sourceInfo.modeInfoIdx=UINT32(f.ccd.modes.size());f.ccd.modes.push_back(source);auto target=nativeCcdMode(original,nativeTargetIndex(consolePath),DISPLAYCONFIG_MODE_INFO_TYPE_TARGET);target.adapterId={8,0};target.id=19;extra.targetInfo.modeInfoIdx=UINT32(f.ccd.modes.size());f.ccd.modes.push_back(target);f.ccd.paths.push_back(extra);}
        if(scenario==12){++f.ccd.paths.front().sourceInfo.id;f.ccd.modes[nativeSourceIndex(f.ccd.paths.front())].id=f.ccd.paths.front().sourceInfo.id;}
        if(scenario==13){++f.ccd.paths.front().targetInfo.id;f.ccd.modes[nativeTargetIndex(f.ccd.paths.front())].id=f.ccd.paths.front().targetInfo.id;}
        returnedForeign=nativeStreamProfileSignatures(f.ccd,nativeDisplayTargets(f.state()));
      }
      return true;
    },[&](DWORD)->std::optional<Json>{auto value=response;response.reset();return value;},{}};
    return nativeWorkerViewportRpc(f.resize,phase,key,verified,budget,rpc,baseline);
  };
  const auto result=applyNativeWorkerViewport(command,f.resize,"0",viewport,budget);
  if(scenario==21){require(!result.ok&&result.helper.get("rollbackSucceeded").boolean()&&result.nativeStage==4&&result.nativeError==ERROR_BAD_CONFIGURATION&&result.consoleReturned,"Returned-console rollback lost first Windows error or failed suppression");require(worker.completeViewport(f.resize)&&worker.active()&&worker.finish(),"Returned-console SDK rollback did not rebind profile and restore original console");f.transport.checkForeign();++passed;continue;}
  if(scenario>=4&&scenario!=20){
    require(!result.ok&&!worker.active()&&f.applies==1,"Unscoped/stale profile or unrelated display reached Windows mutation");
    if(scenario==4||scenario==11)require(result.nativeReason==11,"Independent/unscoped console rejection lost fixed foreign-topology diagnosis");
    if(scenario==12)require(result.nativeReason==22,"Rebound console source lost its fixed identity diagnosis");
    if(returnedForeign)require(*returnedForeign==nativeStreamProfileSignatures(f.ccd,nativeDisplayTargets(f.state())),"Refused viewport overwrote returned or unrelated console geometry");
    f.transport.checkForeign();++passed;continue;
  }
  if(!result.ok)std::cerr<<"Lifecycle viewport scenario="<<scenario<<" failed: "<<result.reason<<" stage="<<result.nativeStage<<" error="<<result.nativeError<<'\n';
  require(result.ok,"Lifecycle actual SDK resize failed");
  require(result.consoleReturned==(scenario==3||scenario==20),"Private viewport diagnosis invented or lost scoped console reappearance");
  require(worker.completeViewport(f.resize)&&worker.active(),"Lifecycle consumer rejected actual SDK completion");
  const auto record=parseNativeStreamProfileRecord(*profileBytes);
  require(!record.resize&&record.current.targets.size()==1&&record.current.targets[0].width==1380&&record.current.targets[0].height==970,"Lifecycle resize lost current source geometry");
  require(worker.finish()&&worker.clean()&&captureStops==1,"Lifecycle resized profile restoration failed");
  const auto restored=nativeActiveCcd(f.ccd);
  require(nativeStreamProfileSignatures(original,launch.entry.targets)==nativeStreamProfileSignatures(restored,record.current.targets)&&nativeStreamOriginalPrimaryMatches(record.profile,restored,record.current.targets),"Lifecycle did not restore original console and primary");
  const auto output=std::find_if(restored.paths.begin(),restored.paths.end(),[&](const auto& path){return nativeSameTarget(path,record.current.targets.front());});
  require(output!=restored.paths.end(),"Lifecycle lost current owned output during restore");
  const auto& source=nativeCcdSource(restored,*output);
  require(source.width==1380&&source.height==970&&source.position.x==1600&&source.position.y==0,"Lifecycle restored retired SDK targets or lost current owned viewport");
  f.transport.checkForeign();++passed;
  }
  return passed;
}
int profileViewportServiceCases(){using namespace host;int passed=0;
  for(int scenario=0;scenario<4;++scenario){
    WorkerFixture f;const auto resize=f.resize;const auto state=f.state();const auto targets=nativeDisplayTargets(state);const auto consolePath=f.ccd.paths.front();
    NativeStreamProfileRecord profile;profile.key={resize.sessionId,std::string(64,'b'),state.get("deviceInstanceId").string(),17};profile.phase=NativeStreamProfilePhase::active;profile.revision=3;profile.resizeSequence=1;profile.entry=profile.current={GetCurrentProcessId(),uint32_t(state.get("generation").number()),targets};
    profile.console={{"00112233-4455-6677-8899-aabbccddeeff","std"},{{7,0},L"\\\\?\\PCI#VEN_1234&DEV_1111#0",L"PCI\\VEN_1234&DEV_1111\\3&0",L"PCI",{L"PCI\\VEN_1234&DEV_1111"},nativeConsoleDisplayClass,true,true},{consolePath.sourceInfo.adapterId,consolePath.targetInfo.adapterId,consolePath.sourceInfo.id,consolePath.targetInfo.id}};
    profile.profile=planNativeStreamPrimaryProfile(f.ccd,targets,NativeStreamConsolePath{consolePath.sourceInfo.adapterId,consolePath.targetInfo.adapterId,consolePath.sourceInfo.id,consolePath.targetInfo.id});profile.profileSha256=nativeBaselineDigest(nativeStreamProfilePayloadBytes(profile.profile));profile.scope=test::recoveryScope(captureNativeTopologyBaseline(profile.profile.before,targets,"0"));profile.scope.foreign[0].sourcePath=profile.scope.foreign[0].targetPath=utf8(profile.console.device.interfacePath);profile.resize=NativeStreamProfileResize{std::string(64,'c'),nativeBaselineDigest(nativeResizeFields(resize)),1};profile.launchSha256=std::string(64,'d');
    require(f.service->reserveStreamProfile(resize.sessionId,f.key()),"SDK profile fixture reservation failed");f.ccd=profile.profile.applied;
    std::optional<std::string> profileBytes=nativeStreamProfileRecordBytes(profile),receiptBytes;
    f.service->testProfileViewportStores({[&]{return profileBytes;},[&](const auto& expected,const auto& desired){if(profileBytes!=expected)return false;profileBytes=desired;return true;}},{[&]{return receiptBytes;},[&](const auto& expected,const auto& desired){if(scenario==2||receiptBytes!=expected)return false;receiptBytes=desired;return true;}});
    const auto creates=f.transport.creates;
    if(scenario==3){auto changed=profile;changed.resize->requestSha256=std::string(64,'f');profileBytes=nativeStreamProfileRecordBytes(changed);}
    if(scenario==1)f.failValidate=true;
    const auto result=f.run();
    if(scenario==3){require(!result.ok&&f.transport.creates==creates&&!receiptBytes,"Unreserved profile request reached SDK viewport mutation");++passed;continue;}
    if(scenario==2){require(!result.ok&&(f.state().get("configurationPending").boolean()||f.state().get("recoveryRequired").boolean()),"Lost durable viewport receipt admitted a stable profile media source");f.service->cancelViewportSession(resize.sessionId,true);require(f.state().get("recoveryRequired").boolean(),"Lost profile receipt disappeared on worker cancellation");++passed;continue;}
    require(receiptBytes.has_value(),"Actual SDK profile completion omitted protected native receipt");const auto receipt=parseNativeStreamViewportCommit(*receiptBytes);const auto lease=nativeStreamProfileLease(f.state(),GetCurrentProcessId());const auto topology=f.ccd;NativeStreamProfileEvidence proof{profile.key,lease,test::recoveryScope(captureNativeTopologyBaseline(nativeActiveCcd(topology),lease.targets,"0")),profile.console,topology};proof.scope.tick=receipt.scope.tick+1;
    require(nativeStreamViewportCommitProof(profile,proof,resize,receipt,*f.journal)&&receipt.lease==ownedLease&&receipt.outputs.size()==2&&receipt.outputs[0].display==71&&receipt.outputs[1].display==72&&receipt.rollback==(scenario==1),"Profile receipt failed actual SDK/runtime/current-CCD binding");
    require(result.ok==(scenario==0)&&!f.state().get("configurationPending").boolean(),"Profile SDK viewport or verified rollback result was misreported");f.transport.checkForeign();++passed;
  }
  return passed;
}
host::NativeTrackViewportContext trackContext(const host::NativeTrackResize& request){host::NativeTrackViewportContext value;value.pending=request;value.deadline=host::streamMonotonicMillis()+80000;value.authorized=value.console=value.unlocked=true;value.stopped=false;value.scope=host::test::currentScope();return value;}
host::Json trackFrame(const host::NativeTrackResize& request,const std::string& phase,const std::string& key,bool verified=false){return host::JsonParser::parse("{\"type\":"+host::jsonQuote(phase)+','+host::nativeTrackResizeFields(request)+",\"leaseKey\":"+host::jsonQuote(key)+",\"windowsVerified\":"+(verified?"true":"false")+'}');}
host::Json dispatchTrack(const host::Json& value,const host::NativeTrackViewportContext& context,Fixture& fixture){auto incoming=value;if(incoming.get("type").string()=="worker.native.tracks.resize.prepare")host::streamField(incoming,"baseline",fixture.baseline());const auto response=host::dispatchNativeTrackViewport(incoming,context,fixture.service.get());require(response.handled&&!response.response.empty(),"Expected group viewport reply missing");return host::JsonParser::parse(response.response);}
int trackServiceCases(){using namespace host;int passed=0;const auto request=trackRequest();
  const auto prepare=[&](Fixture& f){return f.service->prepareTrackViewport(request,f.key(),streamMonotonicMillis()+80000,f.baseline(),test::currentScope());};
  const auto finish=[&](Fixture& f,bool rollback,bool verified=true){return f.service->finishTrackViewport(request,f.key(),rollback,verified,test::currentScope());};
  try{
  {Fixture f;const auto before=parseNativeRuntimeRecord(*f.journal);const auto prepared=prepare(f);const auto targets=nativeDisplayTargets(f.state());
    require(prepared.accepted&&prepared.pending&&prepared.driverChangeAttempted&&targets[0].width==1600&&targets[1].height==1024&&parseNativeRuntimeRecord(*f.journal).outputs==before.outputs,"Group SDK prepare published one changed output as stable");++passed;
    require(!finish(f,false,false).accepted&&finish(f,false).accepted&&!f.state().get("configurationPending").boolean()&&parseNativeRuntimeRecord(*f.journal).outputs[1].height==1024,"Group completion bypassed Windows verification or lost both dimensions");f.transport.checkForeign();++passed;}
  {Fixture f;require(prepare(f).accepted,"Group rollback fixture failed");auto old=request;old.topologyEpoch--;require(!f.service->rollbackTrackViewport(old).accepted&&!f.service->finishTrackViewport(old,f.key(),false,true,test::currentScope()).accepted,"Old topology completed a newer group");++passed;
    require(f.service->rollbackTrackViewport(request).accepted,"Group SDK rollback failed");const auto removes=f.transport.removes;require(f.service->rollbackTrackViewport(request).accepted&&f.transport.removes==removes&&finish(f,true).accepted,"Group rollback reply retry repeated target removal");const auto targets=nativeDisplayTargets(f.state());require(targets[0].width==1920&&targets[1].height==720,"Group rollback restored only one original output");f.transport.checkForeign();++passed;}
  {Fixture f;require(prepare(f).accepted,"Group pending fixture failed");const auto creates=f.transport.creates;
    require(!f.prepare(::request()).accepted&&!f.service->prepareCount({{request.resizeId,std::string(43,'a')},1},f.key(),streamMonotonicMillis()+20000,f.baseline(),test::currentScope()).accepted&&f.transport.creates==creates,"Single/count transaction crossed an active group");
    StreamResize legacy;legacy.sessionId=request.sessionId;legacy.requestId=request.requestId;legacy.resizeId=request.resizeId;require(!f.service->rollbackViewport(legacy).accepted&&!f.service->finishViewport(legacy,f.key(),false,true,test::currentScope()).accepted,"Legacy request stole group rollback ownership");++passed;}
  {Fixture f;auto single=request;single.outputs.resize(1);single.epochs.resize(1);const auto creates=f.transport.creates;const auto reply=f.service->prepareTrackViewport(single,f.key(),streamMonotonicMillis()+80000,f.baseline(),test::currentScope());require(!reply.accepted&&!reply.pending&&f.transport.creates==creates,"Partial group implicitly removed an existing monitor");++passed;}
  {Fixture f(true);auto single=request;single.outputs.resize(1);single.epochs.resize(1);const auto prepared=f.service->prepareTrackViewport(single,f.key(),streamMonotonicMillis()+80000,f.baseline(),test::currentScope());require(prepared.accepted&&f.service->finishTrackViewport(single,f.key(),false,true,test::currentScope()).accepted&&nativeDisplayTargets(f.state()).size()==1,"One-output group failed or changed actual count");f.transport.checkForeign();++passed;}
  for(int scenario=0;scenario<8;++scenario){Fixture f;auto r=request;switch(scenario){case 0:r.outputs[1].id="0";break;case 1:r.outputs[0].width=1601;break;case 2:r.epochs[1]=0;break;case 3:r.topologyEpoch=0;break;case 4:r.fps=59;break;case 5:r.outputs[1].left=1599;break;case 6:r.epochs.pop_back();break;case 7:r.outputs[1].top=INT32_MAX-100;break;}
    const auto creates=f.transport.creates;const auto reply=f.service->prepareTrackViewport(r,f.key(),streamMonotonicMillis()+80000,f.baseline(),test::currentScope());require(!reply.accepted&&!reply.pending&&f.transport.creates==creates,"Malformed group changed the SDK lease");++passed;}
  for(int scenario=0;scenario<5;++scenario){Fixture f;auto scope=test::currentScope();auto checkpoint=f.baseline();auto key=f.key();auto deadline=streamMonotonicMillis()+80000;if(scenario==0)key+="-stale";if(scenario==1)deadline=streamMonotonicMillis()-1;if(scenario==2)scope.session++;if(scenario==3)checkpoint="{}";if(scenario==4){auto baseline=parseNativeRecoveryBaseline(checkpoint);baseline.generation++;checkpoint=nativeRecoveryBaselineBytes(baseline);}const auto creates=f.transport.creates;require(!f.service->prepareTrackViewport(request,key,deadline,checkpoint,scope).accepted&&f.transport.creates==creates,"Unbound baseline, lease, session or deadline mutated the group");++passed;}
  {Fixture f;f.failBaseline=true;const auto creates=f.transport.creates;require(!prepare(f).accepted&&f.transport.creates==creates&&f.state().get("recoveryRequired").boolean(),"Unstored group checkpoint reached SDK control");++passed;}
  {Fixture f;f.failJournal=true;const auto creates=f.transport.creates;require(!prepare(f).accepted&&f.transport.creates==creates&&f.state().get("recoveryRequired").boolean(),"Unstored group changing record reached SDK control");++passed;}
  {Fixture f;require(prepare(f).accepted,"Group commit publication fixture failed");f.failPublish=true;require(!finish(f,false).accepted&&f.state().get("configurationPending").boolean(),"Unpublished group commit lost rollback ownership");f.failPublish=false;require(f.service->rollbackTrackViewport(request).accepted&&finish(f,true).accepted,"Group commit publication failure could not restore");f.transport.checkForeign();++passed;}
  {Fixture f;f.transport.failCreate=f.transport.creates+2;const auto reply=prepare(f);require(!reply.accepted&&reply.pending,"Partial group SDK create falsely completed");f.transport.failCreate=0;require(f.service->rollbackTrackViewport(request).accepted&&finish(f,true).accepted,"Partial group SDK create could not restore both outputs");f.transport.checkForeign();++passed;}
  {Fixture f;require(prepare(f).accepted,"Group cancel fixture failed");const auto key=f.key();f.service->cancelViewportSession("00000000-0000-0000-0000-000000000099");require(f.state().get("configurationPending").boolean(),"Another session cancelled group ownership");f.service->cancelViewportSession(request.sessionId);require(!f.state().get("configurationPending").boolean()&&f.state().get("recoveryRequired").boolean()&&!f.service->finishTrackViewport(request,key,false,true,test::currentScope()).accepted,"Cancelled group became ready without Windows restoration");f.transport.checkForeign();++passed;}
  for(int scenario=0;scenario<8;++scenario){Fixture f;auto context=trackContext(request);auto message=trackFrame(request,"worker.native.tracks.resize.prepare",f.key());if(scenario==0)context.authorized=false;if(scenario==1)context.console=false;if(scenario==2)context.unlocked=false;if(scenario==3)context.stopped=true;if(scenario==4)context.scope.reset();if(scenario==5)context.deadline=context.now()-1;if(scenario==6)streamField(message,"topologyEpoch",1);if(scenario==7)streamField(message,"resizeId",std::string("00000000-0000-0000-0000-000000000099"));const auto creates=f.transport.creates;const auto reply=dispatchTrack(message,context,f);require(!reply.get("accepted").boolean()&&f.transport.creates==creates,"Unauthenticated/stale group RPC changed displays");++passed;}
  {Fixture f;auto context=trackContext(request);const auto prepared=dispatchTrack(trackFrame(request,"worker.native.tracks.resize.prepare",f.key()),context,f);require(prepared.get("accepted").boolean(),"Group RPC prepare failed");context.console=context.unlocked=false;context.scope.reset();context.deadline=context.now()-1;require(dispatchTrack(trackFrame(request,"worker.native.tracks.resize.rollback",""),context,f).get("accepted").boolean(),"Lost console blocked owned SDK rollback");require(!dispatchTrack(trackFrame(request,"worker.native.tracks.resize.restored",f.key(),true),context,f).get("accepted").boolean(),"SDK rollback substituted for fresh Windows/session proof");++passed;}
  for(int scenario=0;scenario<10;++scenario){Fixture f;const auto context=trackContext(request);const auto valid=dispatchTrack(trackFrame(request,"worker.native.tracks.resize.prepare",f.key()),context,f);bool read=false;NativeWorkerRpcAccess io{[&](const std::string&){return scenario!=0;},[&](DWORD)->std::optional<Json>{if(read||scenario==1)return {};read=true;auto value=valid;switch(scenario){case 2:streamField(value,"topologyEpoch",1);break;case 3:streamField(value,"phase",std::string("worker.native.tracks.resize.commit"));break;case 4:std::get<Json::Object>(value.value)["pending"].value=false;break;case 5:std::get<Json::Object>(value.value)["accepted"].value=std::string("true");break;case 6:std::get<Json::Object>(value.value)["recoveryRequired"].value=true;break;case 7:streamField(std::get<Json::Object>(value.value)["snapshot"],"generation",0);break;case 8:std::get<Json::Object>(value.value)["tracks"].value=Json::Array{};break;case 9:throw std::runtime_error("Fixture group read failure");}return value;},{}};StreamResizeBudget budget;const auto reply=nativeWorkerTrackViewportRpc(request,"worker.native.tracks.resize.prepare",f.key(),false,budget,io,f.baseline());require(reply.requestStarted&&!reply.received&&!reply.accepted&&reply.recoveryRequired,"Lost/mismatched group RPC became an unchanged/success claim");++passed;}
  {Fixture f;const auto context=trackContext(request);const auto name=L"\\\\.\\pipe\\MultitapHost.QA.NativeTracks."+randomName();WinHandle server(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr));require(server.value!=INVALID_HANDLE_VALUE,"Group QA server pipe failed");WinHandle client(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));require(client.value!=INVALID_HANDLE_VALUE,"Group QA client pipe failed");ConnectNamedPipe(server.value,nullptr);std::atomic<bool> stopped=false;std::mutex writing;std::exception_ptr failure;int interleaved=0;
    std::jthread responder([&]{try{for(int i=0;i<2;++i){bool text=false;std::vector<uint8_t> bytes;require(readPipePacket(server.value,text,bytes,stopped,std::chrono::milliseconds(2000))&&text,"Actual group RPC request missing");const auto response=dispatchNativeTrackViewport(JsonParser::parse(std::string(bytes.begin(),bytes.end())),context,f.service.get());require(response.handled&&!response.response.empty(),"Actual group dispatcher failed");const std::string reset=R"({"type":"input.reset"})";require(pipePacket(server.value,true,{reset.begin(),reset.end()},stopped,writing)&&pipePacket(server.value,true,{response.response.begin(),response.response.end()},stopped,writing),"Actual group RPC response missing");}}catch(...){failure=std::current_exception();}});
    NativeWorkerRpcAccess io{[&](const std::string& message){return pipePacket(client.value,true,{message.begin(),message.end()},stopped,writing);},[&](DWORD timeout)->std::optional<Json>{bool text=false;std::vector<uint8_t> bytes;if(!readPipePacket(client.value,text,bytes,stopped,std::chrono::milliseconds(timeout))||!text)return {};return JsonParser::parse(std::string(bytes.begin(),bytes.end()));},[&](const Json& value){if(value.get("type").string()!="input.reset")return false;++interleaved;return true;}};StreamResizeBudget budget;budget.deadline=streamMonotonicMillis()+5000;const auto prepared=nativeWorkerTrackViewportRpc(request,"worker.native.tracks.resize.prepare",f.key(),false,budget,io,f.baseline());require(prepared.received&&prepared.accepted,"Actual group prepare pipe failed");const auto committed=nativeWorkerTrackViewportRpc(request,"worker.native.tracks.resize.commit",nativeDisplayLeaseKey(prepared.snapshot),true,budget,io);responder.join();if(failure)std::rethrow_exception(failure);require(committed.received&&committed.accepted&&!committed.pending&&interleaved==2,"Actual group pipe lost control interleaving or completion");f.transport.checkForeign();++passed;}
  }catch(...){std::cerr<<"Group service failure after "<<passed<<" checkpoints\n";throw;}
  return passed;
}
host::Json trackCurrent(WorkerFixture& f){using namespace host;const auto display=f.probe();auto initial=trackRequest();initial.topologyEpoch=1;initial.outputs.clear();initial.epochs.clear();const auto& anchor=display.monitors.front();for(const auto& monitor:display.monitors){initial.outputs.push_back({monitor.id,monitor.width,monitor.height,monitor.left-anchor.left,monitor.top-anchor.top});initial.epochs.push_back(1);}auto command=JsonParser::parse('{'+nativeTrackResizeFields(initial)+'}');streamField(command,"type",std::string("session.start"));streamField(command,"mode",std::string("stream"));streamField(command,"mediaProtocol",std::string("tracks-v1"));streamField(command,"monitor",display.monitors.size()==2?std::string("all"):anchor.id);streamField(command,"bitrate",8000000);streamField(command,"maxWidth",3840);streamField(command,"maxHeight",2160);std::get<Json::Object>(command.value)["audio"].value=false;return command;}
host::NativeWorkerTrackAccess trackWorkerAccess(WorkerFixture& f,const host::NativeTrackResize& request,const host::StreamResizeBudget& budget){using namespace host;
  return {[&f]{return f.eligible;},[&f]{return f.probe();},[&f]{return f.observed();},[&f]{return f.state();},f.access(budget).topology,[](const NativeTopologyBaseline& baseline){return test::recoveryScope(baseline);},[&f,request](const std::string& phase,const std::string& key,bool verified,const std::string& baseline,const StreamResizeBudget& phaseBudget){
    ++f.exchanges;std::optional<Json> response;NativeWorkerRpcAccess io{[&](const std::string& bytes){if(f.fault=="commit-publication"&&phase=="worker.native.tracks.resize.commit")f.failPublish=true;const auto dispatched=dispatchNativeTrackViewport(JsonParser::parse(bytes),trackContext(request),f.service.get());if(dispatched.handled&&!dispatched.response.empty())response=JsonParser::parse(dispatched.response);f.failPublish=false;f.sync();if(phase=="worker.native.tracks.resize.prepare"){if(f.fault=="console")f.eligible=false;if(f.fault=="foreign")f.ccd.modes[nativeSourceIndex(f.ccd.paths.front())].sourceMode.width+=2;}return true;},[&](DWORD)->std::optional<Json>{if((f.fault=="prepare-lost"&&phase=="worker.native.tracks.resize.prepare")||(f.fault=="commit-lost"&&phase=="worker.native.tracks.resize.commit"))return {};auto value=response;response.reset();return value;},{}};
    return nativeWorkerTrackViewportRpc(request,phase,key,verified,phaseBudget,io,baseline);
  }};
}
int trackWorkerCases(){using namespace host;int passed=0;const auto request=trackRequest();
  const auto run=[&](WorkerFixture& f,const Json& current,const NativeTrackResize& next){StreamResizeBudget budget;return applyNativeWorkerTracks(current,next,trackWorkerAccess(f,next,budget),budget);};
  {WorkerFixture f;const auto first=run(f,trackCurrent(f),request);require(first.ok&&!first.rollbackAttempted&&f.applies==1&&f.exchanges==2&&!f.state().get("configurationPending").boolean(),"Exact group worker did not commit both outputs in one Windows apply");const auto bound=bindOwnedTrackSources(first.command,first.display,f.observed());require(bound.outputs.size()==2&&bound.outputs[0].media.width==1600&&bound.outputs[1].media.height==1024&&bound.outputs[1].media.top-bound.outputs[0].media.top==100,"Committed group could not bind two exact owned source rectangles");f.transport.checkForeign();++passed;
    auto second=request;second.topologyEpoch=3;second.resizeId="00000000-0000-0000-0000-000000000008";second.outputs[0].width=1440;second.outputs[1].left=1440;second.epochs[0]=3;const auto changed=run(f,first.command,second);require(changed.ok&&f.applies==2&&f.exchanges==4&&changed.command.get("topologyEpoch").number()==3&&bindOwnedTrackSources(changed.command,changed.display,f.observed()).outputs[0].media.width==1440,"Second group resize lost its epoch or fresh capture mapping");f.transport.checkForeign();++passed;}
  {WorkerFixture f;const auto current=trackCurrent(f);auto fps=request;fps.outputs[0].width=1920;fps.outputs[0].height=1080;fps.outputs[1].height=720;fps.outputs[1].left=1920;fps.outputs[1].top=0;fps.fps=30;const auto creates=f.transport.creates;const auto result=run(f,current,fps);require(result.ok&&f.transport.creates==creates&&f.applies==0&&result.command.get("fps").number()==30,"FPS-only group recreated outputs or changed Windows layout");++passed;}
  {WorkerFixture f;const auto current=trackCurrent(f);auto moved=request;moved.outputs[0].width=1920;moved.outputs[0].height=1080;moved.outputs[1].height=720;moved.outputs[1].left=2200;moved.epochs={1,1};const auto creates=f.transport.creates;const auto result=run(f,current,moved);require(result.ok&&f.transport.creates==creates&&f.applies==1&&bindOwnedTrackSources(result.command,result.display,f.observed()).outputs[1].media.epoch==1,"Placement-only group invented an encoder epoch or recreated the driver lease");++passed;}
  {WorkerFixture f(true);auto single=request;single.outputs.resize(1);single.epochs.resize(1);const auto result=run(f,trackCurrent(f),single);require(result.ok&&result.display.monitors.size()==1&&bindOwnedTrackSources(result.command,result.display,f.observed()).outputs.size()==1,"Single owned track could not use the same worker transaction");f.transport.checkForeign();++passed;}
  {WorkerFixture f;auto current=trackCurrent(f);streamField(current,"maxWidth",1920);streamField(current,"maxHeight",1080);auto large=request;large.outputs[0].width=2560;large.outputs[0].height=1440;large.outputs[1].height=720;large.outputs[1].left=2560;large.outputs[1].top=0;large.epochs[1]=1;const auto result=run(f,current,large);require(result.ok&&result.command.get("maxWidth").number()==2560&&result.command.get("maxHeight").number()==1440&&bindOwnedTrackSources(result.command,result.display,f.observed()).outputs[0].media.width==2560,"Initial viewport size remained an artificial cap on later real resolution changes");++passed;}
  for(const auto fault:{"validate","capture","prepare-lost","commit-publication","partial-sdk"}){WorkerFixture f;const auto current=trackCurrent(f);if(std::string(fault)=="validate")f.failValidate=true;else if(std::string(fault)=="partial-sdk")f.transport.failCreate=f.transport.creates+2;else f.fault=fault;const auto result=run(f,current,request);require(!result.ok&&result.rollbackAttempted&&result.rollbackSucceeded&&!result.display.recoveryRequired&&!f.state().get("configurationPending").boolean(),"Recoverable group worker failure did not restore the complete Windows layout");const auto targets=nativeDisplayTargets(f.state());require(targets[0].width==1920&&targets[1].height==720,"Worker recovery restored only one SDK output");f.transport.checkForeign();++passed;}
  for(const auto fault:{"console","foreign","commit-lost"}){WorkerFixture f;const auto current=trackCurrent(f);f.fault=fault;const auto result=run(f,current,request);require(!result.ok&&result.rollbackAttempted&&!result.rollbackSucceeded&&result.display.recoveryRequired&&!result.display.ready(),"Uncertain group worker failure became a ready source");if(std::string(fault)=="foreign")require(f.ccd.modes[nativeSourceIndex(f.ccd.paths.front())].sourceMode.width==1602,"Group recovery overwrote an independent foreign display edit");f.service->cancelViewportSession(request.sessionId,true);require(f.state().get("recoveryRequired").boolean(),"Failed group verification was not retained by Session0 recovery");f.transport.checkForeign();++passed;}
  for(int scenario=0;scenario<6;++scenario){WorkerFixture f;const auto current=trackCurrent(f);auto invalid=request;if(scenario==0)f.eligible=false;if(scenario==1)invalid.topologyEpoch=1;if(scenario==2)invalid.epochs[1]=1;if(scenario==3)invalid.outputs.resize(1);if(scenario==4)invalid.fps=59;if(scenario==5)f.badCapture=true;const auto creates=f.transport.creates;const auto result=run(f,current,invalid);require(!result.ok&&!result.rollbackAttempted&&f.exchanges==0&&f.applies==0&&f.transport.creates==creates,"Invalid/stale group or stale capture reached SDK prepare");++passed;}
  return passed;
}
host::Json trackInitialCommand(WorkerFixture& f,bool unchanged=false){using namespace host;auto command=trackCurrent(f);auto& object=std::get<Json::Object>(command.value);object.erase("requestId");object.erase("resizeId");if(!unchanged){auto target=trackRequest();target.topologyEpoch=1;target.epochs={1,1};if(f.probe().monitors.size()==1){target.outputs.resize(1);target.epochs.resize(1);}object["tracks"]=JsonParser::parse('{'+nativeTrackResizeFields(target)+'}').get("tracks");}return command;}
int trackInitialCases(){using namespace host;int passed=0;
  const auto run=[&](WorkerFixture& f,const Json& command){const auto request=nativeInitialTrackViewport(command);StreamResizeBudget budget;return applyNativeWorkerTracks(command,request,trackWorkerAccess(f,request,budget),budget,true);};
  {WorkerFixture f;const auto command=trackInitialCommand(f);const auto request=nativeInitialTrackViewport(command);const auto result=run(f,command);require(result.ok&&f.applies==1&&f.exchanges==2&&result.command.get("topologyEpoch").number()==1&&std::holds_alternative<std::nullptr_t>(result.command.get("requestId").value)&&std::holds_alternative<std::nullptr_t>(result.command.get("resizeId").value),"Initial viewport did not commit exact outputs without leaking private resize IDs");const auto plan=bindOwnedTrackSources(result.command,result.display,f.observed());const auto info=JsonParser::parse(trackSourceInfo(plan,false));require(plan.outputs[0].media.width==1600&&plan.outputs[1].media.height==1024&&nativeTrackMetadataMayComplete(info,request,result.display,true,true)&&!nativeTrackMetadataMayComplete(info,request,result.display,false,true),"Initial source became ready before worker/Windows verification");f.transport.checkForeign();++passed;
    auto next=trackRequest();next.outputs[0].width=1440;next.outputs[1].left=1440;next.epochs={2,1};StreamResizeBudget budget;const auto resized=applyNativeWorkerTracks(result.command,next,trackWorkerAccess(f,next,budget),budget);require(resized.ok&&bindOwnedTrackSources(resized.command,resized.display,f.observed()).outputs[0].media.epoch==2&&f.applies==2&&f.exchanges==4,"Real resize could not follow initially provisioned topology one");++passed;
    auto leaked=info;streamField(leaked,"requestId",request.requestId);streamField(leaked,"resizeId",request.resizeId);require(!nativeTrackMetadataMayComplete(leaked,request,result.display,true,true),"Private initial IDs escaped into accepted public source metadata");++passed;}
  {WorkerFixture f;const auto command=trackInitialCommand(f,true);const auto creates=f.transport.creates;const auto result=run(f,command);require(result.ok&&f.transport.creates==creates&&f.applies==0&&f.exchanges==2,"Already matching initial viewport skipped verification or recreated live outputs");++passed;}
  {WorkerFixture f(true);const auto result=run(f,trackInitialCommand(f));require(result.ok&&result.display.monitors.size()==1&&bindOwnedTrackSources(result.command,result.display,f.observed()).outputs.size()==1,"Initial one-screen viewport changed the active count");f.transport.checkForeign();++passed;}
  for(const auto fault:{"validate","capture","prepare-lost","commit-publication","partial-sdk"}){WorkerFixture f;const auto command=trackInitialCommand(f);if(std::string(fault)=="validate")f.failValidate=true;else if(std::string(fault)=="partial-sdk")f.transport.failCreate=f.transport.creates+2;else f.fault=fault;const auto result=run(f,command);require(!result.ok&&result.rollbackAttempted&&result.rollbackSucceeded&&!result.display.recoveryRequired&&std::holds_alternative<std::nullptr_t>(result.command.value),"Failed initial setup acknowledged a source or lost its verified rollback");const auto targets=nativeDisplayTargets(f.state());require(targets[0].width==1920&&targets[1].height==720,"Initial failure restored only one prior output");f.transport.checkForeign();++passed;}
  for(const auto fault:{"console","foreign","commit-lost"}){WorkerFixture f;const auto command=trackInitialCommand(f);f.fault=fault;const auto result=run(f,command);require(!result.ok&&result.rollbackAttempted&&!result.rollbackSucceeded&&result.display.recoveryRequired&&!result.display.ready(),"Uncertain initial setup was exposed as a ready stream");if(std::string(fault)=="foreign")require(f.ccd.modes[nativeSourceIndex(f.ccd.paths.front())].sourceMode.width==1602,"Initial recovery overwrote an independent console edit");f.service->cancelViewportSession(nativeInitialTrackViewport(command).sessionId,true);f.transport.checkForeign();++passed;}
  for(int scenario=0;scenario<7;++scenario){WorkerFixture f;auto command=trackInitialCommand(f);auto request=nativeInitialTrackViewport(command);switch(scenario){case 0:request.topologyEpoch=2;break;case 1:request.epochs[1]=2;break;case 2:request.outputs[0].width+=2;break;case 3:streamField(command,"type",std::string("stream.tracks.resize"));break;case 4:streamField(command,"requestId",std::string("injected-private-id"));break;case 5:streamField(command,"mediaProtocol",std::string("media-epoch-v1"));break;case 6:f.eligible=false;break;}StreamResizeBudget budget;const auto creates=f.transport.creates;const auto result=applyNativeWorkerTracks(command,request,trackWorkerAccess(f,request,budget),budget,true);require(!result.ok&&!result.rollbackAttempted&&f.transport.creates==creates&&f.exchanges==0&&f.applies==0,"Unbound/replayed initial request reached SDK preparation");++passed;}
  {WorkerFixture f;const auto command=trackInitialCommand(f);const auto request=nativeInitialTrackViewport(command);StreamResizeBudget budget;budget.cancelled=[]{return true;};const auto result=applyNativeWorkerTracks(command,request,trackWorkerAccess(f,request,budget),budget,true);require(!result.ok&&!result.rollbackAttempted&&f.exchanges==0,"Cancelled initial source still configured a display");++passed;}
  {WorkerFixture f;const auto command=trackInitialCommand(f);const auto request=nativeInitialTrackViewport(command);const auto context=trackContext(request);const auto name=L"\\\\.\\pipe\\MultitapHost.QA.InitialTracks."+randomName();WinHandle server(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr));require(server.value!=INVALID_HANDLE_VALUE,"Initial adapter QA pipe failed");WinHandle client(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));require(client.value!=INVALID_HANDLE_VALUE,"Initial adapter client pipe failed");ConnectNamedPipe(server.value,nullptr);std::atomic<bool> stopped=false;std::mutex writing;std::exception_ptr failure;int interleaved=0;
    std::jthread responder([&]{try{for(int i=0;i<2;++i){bool text=false;std::vector<uint8_t> bytes;require(readPipePacket(server.value,text,bytes,stopped,std::chrono::milliseconds(2000))&&text,"Production initial adapter request missing");const auto response=dispatchNativeTrackViewport(JsonParser::parse(std::string(bytes.begin(),bytes.end())),context,f.service.get());require(response.handled&&!response.response.empty(),"Initial adapter dispatcher failed");const std::string reset=R"({"type":"input.reset"})";require(pipePacket(server.value,true,{reset.begin(),reset.end()},stopped,writing)&&pipePacket(server.value,true,{response.response.begin(),response.response.end()},stopped,writing),"Initial adapter response missing");}}catch(...){failure=std::current_exception();}});
    StreamResizeBudget budget;budget.deadline=streamMonotonicMillis()+5000;
    // Only the production pipe exchange is invoked. Its Windows query/apply
    // callbacks remain unused; Session0 uses the actual SDK memory transport.
    auto access=nativeWorkerTrackAccess(client.value,request,stopped,writing,budget,[&](const Json& value){if(value.get("type").string()!="input.reset")return false;++interleaved;return true;});const auto prepared=access.exchange("worker.native.tracks.resize.prepare",f.key(),false,f.baseline(),budget);require(prepared.received&&prepared.accepted,"Production initial adapter prepare failed");const auto committed=access.exchange("worker.native.tracks.resize.commit",nativeDisplayLeaseKey(prepared.snapshot),true,"",budget);responder.join();if(failure)std::rethrow_exception(failure);require(committed.received&&committed.accepted&&!committed.pending&&interleaved==2,"Production initial adapter lost correlation or interleaved controls");f.transport.checkForeign();++passed;}
  return passed;
}
int trackMetadataCases(){using namespace host;int passed=0;WorkerFixture f;auto current=trackCurrent(f);std::get<Json::Object>(current.value).erase("requestId");std::get<Json::Object>(current.value).erase("resizeId");const auto initial=nativeInitialTrackViewport(current);const auto initialInfo=JsonParser::parse(trackSourceInfo(bindOwnedTrackSources(current,f.probe(),f.observed()),false));
  require(nativeTrackMetadataMatches(initialInfo,initial,false)&&nativeTrackMetadataMatchesDisplay(initialInfo,f.probe())&&!nativeTrackMetadataMatches(initialInfo,initial,true),"Initial metadata required fabricated public resize IDs or ignored physical geometry");++passed;
  const auto request=trackRequest();StreamResizeBudget budget;const auto changed=applyNativeWorkerTracks(current,request,trackWorkerAccess(f,request,budget),budget);require(changed.ok,"Metadata fixture group failed");const auto valid=JsonParser::parse(trackSourceInfo(bindOwnedTrackSources(changed.command,changed.display,f.observed()),false));require(nativeTrackMetadataMatches(valid,request,true)&&nativeTrackMetadataMatchesDisplay(valid,changed.display),"Actual source metadata did not match the exact applied group");++passed;
  for(int scenario=0;scenario<13;++scenario){auto value=valid;auto& rows=std::get<Json::Array>(std::get<Json::Object>(value.value)["tracks"].value);switch(scenario){case 0:streamField(value,"topologyEpoch",1);break;case 1:streamField(value,"resizeId",std::string("00000000-0000-0000-0000-000000000099"));break;case 2:streamField(value,"fpsTarget",30);break;case 3:streamField(rows[1],"epoch",1);break;case 4:streamField(rows[0],"sourceWidth",1280);break;case 5:streamField(rows[1],"left",rows[1].get("left").number()+1);break;case 6:std::get<Json::Object>(rows[0].value)["primary"].value=std::string("false");break;case 7:streamField(rows[1],"id",std::string("0"));break;case 8:rows.pop_back();break;case 9:streamField(rows[0],"width",1600.5);streamField(rows[0],"sourceWidth",1600.5);break;case 10:streamField(value,"sessionId",std::string("00000000-0000-0000-0000-000000000099"));break;case 11:streamField(value,"requestId",std::string("old-window"));break;case 12:std::get<Json::Object>(rows[0].value)["primary"].value=true;std::get<Json::Object>(rows[1].value)["primary"].value=true;break;}require(!nativeTrackMetadataMatches(value,request,true),"Stale, stretched or malformed metadata satisfied group completion");++passed;}
  {auto moved=valid;auto& rows=std::get<Json::Array>(std::get<Json::Object>(moved.value)["tracks"].value);for(auto& row:rows)streamField(row,"left",row.get("left").number()+2000);require(nativeTrackMetadataMatches(moved,request,true)&&!nativeTrackMetadataMatchesDisplay(moved,changed.display),"Relative geometry substituted for the verified actual Windows positions");++passed;}
  {auto stale=changed.display;stale.configurationPending=true;require(!nativeTrackMetadataMatchesDisplay(valid,stale),"Pending Windows output authenticated a media completion");++passed;}
  {const auto failed=JsonParser::parse(nativeTrackResizeFailure(request,"display_change_failed"));require(std::get<Json::Object>(failed.value).size()==6&&failed.get("sessionId").string()==request.sessionId&&failed.get("topologyEpoch").number()==request.topologyEpoch,"Wire failure exposed private rollback fields or lost resize correlation");++passed;}
  return passed;
}
int workerCoordinatorCases(){using namespace host;int passed=0;
  {WorkerFixture f;const auto config=JsonParser::parse(R"({"provider":"libvirtualdisplay-1.6.4","enabled":true,"viewportTrialEnabled":true})");
    const auto run=[&](const Json& command){StreamResizeBudget budget;auto access=f.access(budget);const auto probe=access.viewport.probe;access.viewport.probe=[probe,config](DWORD timeout){auto display=probe(timeout);display.viewportProvisioning=nativeViewportTrialReady(display,config,true);return display;};return applyNativeWorkerViewport(command,f.resize,f.resize.monitor,access,budget);};
    const auto first=run(f.current());require(first.ok&&first.display.viewportProvisioning,"Committed viewport lost the ready provider capability");
    f.resize.width=1760;f.resize.height=990;f.resize.epoch=2;f.resize.resizeId="00000000-0000-0000-0000-000000000003";
    const auto second=run(first.command);require(second.ok&&second.display.viewportProvisioning&&second.command.get("epoch").number()==2&&second.command.get("width").number()==1760&&f.exchanges==4,"A second native viewport could not follow the first committed media epoch");f.transport.checkForeign();++passed;
  }
  for(int scenario=0;scenario<5;++scenario){WorkerFixture f(scenario==1);if(scenario==2){f.resize.monitor="1";f.resize.width=1024;f.resize.height=768;}if(scenario==3){f.resize.width=1920;f.resize.height=1080;f.resize.fps=30;}if(scenario==4){std::set<UINT32> indices;for(const auto& p:f.ccd.paths)if(indices.insert(nativeSourceIndex(p)).second)f.ccd.modes[nativeSourceIndex(p)].sourceMode.position.x-=1600;}
    const auto before=f.current();const int removes=f.transport.removes;const auto result=f.run();require(result.ok&&result.reason.empty()&&result.command.get("width").number()==f.resize.width&&result.command.get("height").number()==f.resize.height&&result.command.get("epoch").number()==1&&result.command.get("fps").number()==f.resize.fps&&!f.state().get("configurationPending").boolean()&&!result.display.recoveryRequired,"Actual worker/SDK/CCD coordinator did not complete a verified resize");require(!result.display.viewportProvisioning,"Worker-only staged capability leaked into final public readiness");require(f.exchanges==2&&f.transport.releases==0&&(scenario==3?f.transport.removes==removes:f.transport.removes==removes+1),"Worker released neighbors or recreated a no-op output");if(scenario!=3)require(before.get("displayDeviceName").string()!=result.command.get("displayDeviceName").string(),"New source name was not attached to media restart");f.transport.checkForeign();++passed;
  }
  for(const auto& fault:{std::string("validate"),std::string("capture-persistent"),std::string("prepare-lost"),std::string("commit-publication")}){WorkerFixture f;f.fault=fault;f.failValidate=fault=="validate";const auto result=f.run();require(!result.ok&&result.helper.get("rollbackAttempted").boolean()&&result.helper.get("rollbackSucceeded").boolean()&&result.display.ready()&&!result.display.recoveryRequired&&!f.state().get("configurationPending").boolean()&&nativeDisplayTargets(f.state())[0].width==1920,"Failed worker resize did not verify SDK and Windows restoration");f.transport.checkForeign();++passed;}
  for(const auto& fault:{std::string("console"),std::string("foreign"),std::string("commit-lost")}){WorkerFixture f;f.fault=fault;const auto result=f.run();require(!result.ok&&!result.helper.get("rollbackSucceeded").boolean()&&result.display.recoveryRequired&&!result.display.ready(),"Unverified worker recovery was advertised as restored");if(fault=="foreign")require(f.ccd.modes[nativeSourceIndex(f.ccd.paths.front())].sourceMode.width==1602,"Worker rollback overwrote independent foreign display changes");f.service->cancelViewportSession(f.resize.sessionId,true);require(f.state().get("recoveryRequired").boolean(),"Uncertain worker result did not latch service recovery");f.transport.checkForeign();++passed;}
  {WorkerFixture f;const auto before=f.current();auto wrong=before;streamField(wrong,"displayDeviceName",std::string("\\\\.\\DISPLAY999"));StreamResizeBudget budget;const auto result=applyNativeWorkerViewport(wrong,f.resize,f.resize.monitor,f.access(budget),budget);require(!result.ok&&f.exchanges==0&&f.transport.removes==0,"Stale streaming source changed a native output");++passed;}
  {WorkerFixture f;auto& p=f.ccd.paths[1];p.targetInfo.refreshRate={30,1};f.ccd.modes[nativeTargetIndex(p)].targetMode.targetVideoSignalInfo.vSyncFreq={30,1};const auto result=f.run();require(!result.ok&&f.exchanges==0&&f.transport.removes==0,"Unverified original refresh rate entered a native transaction");++passed;}
  {WorkerFixture f;StreamResizeBudget budget;auto access=f.access(budget);const auto exchange=access.exchange;access.exchange=[exchange](const std::string& phase,const std::string& key,bool verified,const std::string& baseline){auto reply=exchange(phase,key,verified,baseline);if(phase=="worker.native.resize.prepare")throw std::runtime_error("Fixture exception after SDK mutation");return reply;};const auto result=applyNativeWorkerViewport(f.current(),f.resize,f.resize.monitor,access,budget);require(!result.ok&&result.helper.get("rollbackSucceeded").boolean()&&result.display.ready()&&!f.state().get("configurationPending").boolean(),"A throwing prepare erased an already-started driver change");++passed;}
  {WorkerFixture f;StreamResizeBudget budget;budget.cancelled=[]{return true;};const auto result=applyNativeWorkerViewport(f.current(),f.resize,f.resize.monitor,f.access(budget),budget);require(!result.ok&&f.exchanges==0&&f.applies==0&&f.transport.removes==0,"Cancelled worker request reached the driver or Windows apply boundary");++passed;}
  return passed;
}
int v2CleanupHeartbeatCases(){using namespace host;int passed=0;
  {Fixture f;f.service->setDisplayManagerV2(true);require(!f.service->hasStreamProfileReservation(),"V2 cleanup unexpectedly reserved a v1 profile");
    const int creates=f.transport.creates,removes=f.transport.removes,releases=f.transport.releases;
    const auto firstTick=uint64_t(f.state().get("tick").number());
    NativeStreamProfileCleanupHeartbeat heartbeat;
    require(heartbeat.start([&]{return f.service->refreshOwnedCleanupLease();},std::chrono::milliseconds(6200),std::chrono::milliseconds(250)),"V2 cleanup heartbeat did not start");
    Sleep(5500);heartbeat.stop();const auto current=f.state();
    require(GetTickCount64()-firstTick>nativeDisplaySnapshotMaxAgeMs&&nativeDisplayCleanupSnapshotAllowed(f.config,current,SERVICE_STOP_PENDING,GetCurrentProcessId(),GetCurrentProcessId(),"ROOT\\DISPLAY\\00a1",uint32_t(current.get("generation").number()),GetTickCount64()),"Cleanup heartbeat did not preserve the existing v2 proof beyond the snapshot TTL");
    require(f.transport.creates==creates&&f.transport.removes==removes&&f.transport.releases==releases&&f.transport.feeds>0,"Cleanup heartbeat created, removed or replaced driver objects");f.transport.checkForeign();++passed;
  }
  for(int scenario=0;scenario<4;++scenario){Fixture f;f.service->setDisplayManagerV2(true);
    if(scenario==0)streamField(f.config,"deviceInstanceId","ROOT\\DISPLAY\\00b2");
    if(scenario==1){auto record=parseNativeRuntimeRecord(*f.journal);record.phase="recovery";f.journal=nativeRuntimeBytes(record);}
    if(scenario==2)f.service->shutdown();
    if(scenario==3)f.service->setDisplayManagerV2(false);
    const int creates=f.transport.creates,removes=f.transport.removes,releases=f.transport.releases,feeds=f.transport.feeds;
    require(!f.service->refreshOwnedCleanupLease(),"Cleanup heartbeat accepted a foreign, modified, stopped or unreserved v1 lease");
    require(f.transport.creates==creates&&f.transport.removes==removes&&f.transport.releases==releases&&f.transport.feeds==feeds,"Refused cleanup heartbeat changed driver objects");f.transport.checkForeign();++passed;
  }
  {Fixture f;f.service->setDisplayManagerV2(true);f.transport.feedFailure=true;const int creates=f.transport.creates;
    require(!f.service->refreshOwnedCleanupLease()&&f.state().get("recoveryRequired").boolean(),"Cleanup heartbeat failure was hidden");
    require(!f.service->refreshOwnedCleanupLease()&&f.transport.creates==creates,"Cleanup heartbeat reacquired a failed lease");f.transport.checkForeign();++passed;
  }
  return passed;
}
int runtimeServiceCases(){using namespace host;int passed=0;
  {Fixture f(false,{},false);const auto generation=DWORD(f.state().get("generation").number());require(parseNativeRuntimeRecord(*f.journal).phase=="starting"&&!f.state().get("activationVerified").boolean()&&std::get<1>(f.journalEvents.front())==0,"SDK startup was not preceded by a durable unverified marker");require(!f.prepare(request()).accepted&&!f.service->activationReady(generation-1),"Unverified/stale initial activation accepted a viewport");require(f.service->activationReady(generation)&&parseNativeRuntimeRecord(*f.journal).phase=="stable"&&f.state().get("activationVerified").boolean(),"Fresh Windows activation did not durably verify its generation");++passed;}
  {Fixture f;const auto r=request();require(f.prepare(r).accepted&&f.finish(r,f.key(),false,true).accepted,"Fixture committed resize failed");const auto generation=DWORD(f.state().get("generation").number());f.service->shutdown();const auto stopped=parseNativeRuntimeRecord(*f.journal);require(stopped.phase=="stopped"&&stopped.outputs[0].width==1600,"Clean update shutdown lost the committed viewport");f.service->tick();const auto targets=nativeDisplayTargets(f.state());require(targets[0].width==1600&&targets[0].height==900&&targets[1].width==1280&&f.state().get("generation").number()>generation&&!f.state().get("activationVerified").boolean(),"Service restart reset dimensions or reused prior Windows verification");require(f.service->activationReady(DWORD(f.state().get("generation").number())),"Restarted service could not accept fresh Windows verification");f.transport.checkForeign();++passed;}
  for(const auto phase:{"starting","changing","recovery"}){NativeRuntimeRecord record{"ROOT\\DISPLAY\\00a1",phase,30,{{"0",1600,900},{"1",1280,720}}};Fixture f(false,record,false);require(f.state().get("recoveryRequired").boolean()&&!f.state().get("leaseActive").boolean()&&f.transport.creates==0&&!f.service->activationReady(30),"Interrupted journal was silently activated on a new service");f.service->tick();require(f.transport.creates==0,"Repeated heartbeat retried an uncertain device operation");++passed;}
  {NativeRuntimeRecord record{"ROOT\\DISPLAY\\00a2","stopped",30,{{"0",1600,900},{"1",1280,720}}};Fixture f(false,record,false);require(f.state().get("recoveryRequired").boolean()&&f.transport.creates==0&&parseNativeRuntimeRecord(*f.journal).instance==record.instance,"A different PnP device's recovery record was overwritten");++passed;}
  {NativeRuntimeRecord record{"ROOT\\DISPLAY\\00a1","stopped",88,{{"0",1584,884},{"1",1024,768}}};Fixture f(false,record,false);const auto targets=nativeDisplayTargets(f.state());require(targets[0].width==1584&&targets[0].height==884&&targets[1].width==1024&&targets[1].height==768&&f.state().get("generation").number()==89&&!f.state().get("activationVerified").boolean(),"A new service object ignored the prior process's committed sizes or generation");++passed;}
  {NativeRuntimeRecord record{"ROOT\\DISPLAY\\00a1","stopped",UINT32_MAX,{{"0",1600,900},{"1",1280,720}}};Fixture f(false,record,false);require(f.state().get("recoveryRequired").boolean()&&f.transport.creates==0&&f.state().get("generation").number()==UINT32_MAX,"Exhausted persisted generation wrapped and reused a lease key");++passed;}
  {Fixture f;f.failJournal=true;const auto result=f.prepare(request());require(!result.accepted&&!result.driverChangeAttempted&&!result.pending&&result.recoveryRequired&&f.transport.removes==0,"Failed write-ahead storage still changed the SDK");++passed;}
  {Fixture f;f.failJournalAt=f.journalWrites+2;const auto result=f.prepare(request());require(!result.accepted&&!result.driverChangeAttempted&&result.recoveryRequired&&f.transport.removes==0&&parseNativeRuntimeRecord(*f.journal).phase=="changing","Failed generation checkpoint still crossed the SDK boundary");++passed;}
  {Fixture f;const auto r=request();require(f.prepare(r).accepted,"Fixture prepare failed");f.failJournal=true;const auto result=f.finish(r,f.key(),false,true);require(!result.accepted&&result.pending&&result.recoveryRequired&&parseNativeRuntimeRecord(*f.journal).uncertain(),"Unpersisted completion was acknowledged or discarded pending recovery");++passed;}
  {Fixture f;const auto r=request();require(f.prepare(r).accepted,"Fixture prepare failed");f.service->shutdown();require(parseNativeRuntimeRecord(*f.journal).uncertain(),"Stopping mid-change erased Windows restoration uncertainty");++passed;}
  {Fixture f;auto foreign=parseNativeRuntimeRecord(*f.journal);foreign.generation+=100;f.journal=nativeRuntimeBytes(foreign);const auto before=f.journal;const auto result=f.prepare(request());require(!result.accepted&&!result.driverChangeAttempted&&f.transport.removes==0&&f.journal==before,"External journal replacement was overwritten by a viewport operation");++passed;}
  {Fixture f;const auto r=request();require(f.prepare(r).accepted,"Fixture prepare failed");require(parseNativeRuntimeRecord(*f.journal).phase=="changing"&&std::any_of(f.journalEvents.begin(),f.journalEvents.end(),[](const auto& event){return std::get<0>(event)=="changing"&&std::get<2>(event)==0;}),"SDK removal preceded the durable changing checkpoint");require(f.service->rollbackViewport(r).accepted&&f.finish(r,f.key(),true,true).accepted&&parseNativeRuntimeRecord(*f.journal).phase=="stable"&&parseNativeRuntimeRecord(*f.journal).outputs[0].width==1920,"Verified rollback did not retain the original committed dimensions");++passed;}
  return passed;
}
int liveActivationRecoveryCases(){
  using namespace host;int passed=0;
  for(const bool single:{false,true}){
    Fixture f(single);const auto before=f.state();const auto generation=DWORD(before.get("generation").number());const auto creates=f.transport.creates;
    f.service->activationRecovery(generation,true);f.service->tick();
    require(f.state().get("recoveryRequired").boolean()&&f.state().get("generation").number()==generation,"Live activation recovery skipped its retry delay");
    f.recoveryTick=2000;f.service->tick();const auto fresh=f.state();
    require(fresh.get("generation").number()==generation+1&&!fresh.get("recoveryRequired").boolean()&&!nativeActivationPublished(fresh)&&!f.prepare(request()).accepted,"Live recovery reused Windows proof or allowed an unverified viewport");
    require(nativeDisplayTargets(fresh).size()==size_t(single?1:2)&&f.transport.creates==creates&&!f.transport.removes&&!f.transport.releases,"Activation retry recreated or removed an existing/foreign output");
    f.service->activationRecovery(generation,true);require(!f.state().get("recoveryRequired").boolean()&&!f.service->activationReady(generation),"An old worker relocked or acknowledged the new generation");
    require(f.service->activationReady(generation+1)&&nativeActivationPublished(f.state()),"Fresh Windows verification could not complete live recovery");
    f.transport.checkForeign();++passed;
  }
  {Fixture f;const auto generation=DWORD(f.state().get("generation").number());f.service->activationRecovery(generation);f.recoveryTick=60000;f.service->tick();require(f.state().get("recoveryRequired").boolean()&&f.state().get("generation").number()==generation&&!f.transport.removes&&!f.transport.releases,"An uncertain worker lifetime allowed live recovery");++passed;}
  // backlog #70: a changed lease snapshot is a display change for the display
  // probe (display_cache.hpp); a pass that changes nothing is not.
  {Fixture f;const auto before=host::hostDisplayChangeEpoch().load();f.service->tick();f.service->tick();
    require(host::hostDisplayChangeEpoch().load()==before,"An unchanged lease snapshot counted as a display change");
    const auto generation=DWORD(f.state().get("generation").number());f.service->activationRecovery(generation,true);f.recoveryTick=2000;f.service->tick();
    require(f.state().get("generation").number()==generation+1&&host::hostDisplayChangeEpoch().load()>before,"A changed lease snapshot was not counted as a display change");++passed;}
  {Fixture f;const auto generation=DWORD(f.state().get("generation").number());const auto creates=f.transport.creates;
    f.service->activationRecovery(generation,true);f.service->activationRecovery(generation,false);f.recoveryTick=2000;f.service->tick();
    require(f.state().get("generation").number()==generation+1&&!nativeActivationPublished(f.state())&&f.transport.creates==creates&&!f.transport.removes&&!f.transport.releases,"A later read-only probe erased the completed activation worker exit proof");f.transport.checkForeign();++passed;}
  {Fixture f;const auto generation=DWORD(f.state().get("generation").number());
    f.service->activationRecovery(generation,false);f.recoveryTick=2000;f.service->tick();require(f.state().get("generation").number()==generation,"Missing exit proof permitted activation recovery");
    f.service->activationRecovery(generation,true);f.service->activationRecovery(generation,false);f.service->tick();
    require(f.state().get("generation").number()==generation+1&&!nativeActivationPublished(f.state()),"A delayed worker exit proof was lost or restarted the retry delay");f.transport.checkForeign();++passed;}
  {Fixture f;const auto generation=DWORD(f.state().get("generation").number());f.service->activationRecovery(generation,true);f.recoveryTick=2000;f.service->tick();
    require(f.service->activationReady(generation+1),"New recovery generation fixture was not verified");f.service->activationRecovery(generation+1,false);f.recoveryTick=60000;f.service->tick();
    require(f.state().get("generation").number()==generation+1&&f.state().get("recoveryRequired").boolean(),"A new recovery episode reused an old worker exit proof");f.transport.checkForeign();++passed;}
  for(const int fault:{1,2,4,5,6,7}){
    Fixture f;const auto generation=DWORD(f.state().get("generation").number());const auto creates=f.transport.creates;
    f.service->activationRecovery(generation,true);f.transport.queryFault=fault;f.recoveryTick=2000;f.service->tick();
    require(f.state().get("recoveryRequired").boolean()&&f.state().get("generation").number()==generation,"Mismatched/unavailable live lease was armed for Windows activation");
    const auto queries=f.transport.queries;f.service->tick();require(f.transport.queries==queries,"Live lease query ignored recovery backoff");
    f.transport.queryFault=0;f.recoveryTick=4000;f.service->tick();require(f.state().get("generation").number()==generation+1&&!nativeActivationPublished(f.state())&&f.transport.creates==creates&&!f.transport.removes&&!f.transport.releases,"A healthy later observation failed to re-verify only existing targets");
    f.transport.checkForeign();++passed;
  }
  for(int fault=0;fault<3;++fault){
    Fixture f;const auto generation=DWORD(f.state().get("generation").number());f.service->activationRecovery(generation,true);
    if(fault==0)f.failJournal=true;
    if(fault==1){auto other=parseNativeRuntimeRecord(*f.journal);other.generation+=100;f.journal=nativeRuntimeBytes(other);}
    if(fault==2)streamField(f.config,"deviceInstanceId",std::string("ROOT\\DISPLAY\\00a2"));
    const auto journal=f.journal;f.recoveryTick=2000;f.service->tick();
    require(f.state().get("recoveryRequired").boolean()&&f.state().get("generation").number()==generation&&f.journal==journal&&!f.transport.removes&&!f.transport.releases,"Failed durability/device binding crossed a live activation generation boundary");f.transport.checkForeign();++passed;
  }
  for(const bool pending:{false,true}){
    Fixture f;const auto generation=DWORD(f.state().get("generation").number());
    if(pending)require(f.prepare(request()).accepted,"Pending viewport fixture failed");else f.service->cancelViewportSession("lost-result",true);
    const auto actualGeneration=f.state().get("generation").number();f.service->activationRecovery(DWORD(actualGeneration),true);f.recoveryTick=2000;f.service->tick();
    require(f.state().get("recoveryRequired").boolean()&&f.state().get("generation").number()==actualGeneration&&!f.service->activationReady(generation),"Uncertain/pending viewport was bypassed by activation-only recovery");f.transport.checkForeign();++passed;
  }
  {WorkerFixture f;const auto generation=DWORD(f.state().get("generation").number());const auto foreign=nativeActiveSignatures(f.ccd,nativeDisplayTargets(f.state()));const auto creates=f.transport.creates;
    f.ccd.paths[1].flags&=~DISPLAYCONFIG_PATH_ACTIVE;f.service->activationRecovery(generation,true);f.recoveryTick=2000;f.service->tick();
    StreamResizeBudget budget;auto topology=f.access(budget).topology;
    std::get<Json::Object>(f.config.value)["viewportTrialEnabled"].value=true;
    const auto probe=[&]{auto display=f.probe();if(display.active&&!nativeActivationPublished(f.state())){display.configurationPending=true;display.error="native_display_activation_pending";}display.viewportProvisioning=nativeViewportTrialReady(display,f.config,true);display.monitorControlSupported=display.ready()&&f.state().get("countControlAvailable").boolean();return display;};
    NativeBootstrapAccess access{[]{return true;},[&]{return f.eligible;},probe,[&]{return f.state();},[&](auto targets,const std::string&){return activateNativeTopology(targets,topology,2000);}};
    const auto result=bootstrapNativeDisplays(access);require(result.display.ready()&&f.applies==1&&!nativeActivationPublished(f.state()),"Recovered live worker did not freshly activate/verify its Windows paths");
    require(nativeActiveSignatures(f.ccd,nativeDisplayTargets(f.state()))==foreign&&f.transport.creates==creates&&!f.transport.removes&&!f.transport.releases,"Live activation changed foreign modes or recreated driver targets");
    require(!result.display.viewportProvisioning&&!result.display.monitorControlSupported,"Pre-ACK activation sample fabricated feature readiness");
    const bool committed=f.service->activationReady(generation+1);
    require(committed&&probe().ready(),"Fresh live worker result was not acknowledged");
    DisplayReadinessCache cache;const auto now=Clock::now();const DisplayProbeKey key{};
    require(cache.complete(cache.begin(2,key),result.display,now,true,committed)&&cache.due(2,key,now),"Durable service activation retained its stale feature flags");
    require(!cache.snapshot(2,true).viewportProvisioning&&!cache.snapshot(2,true).monitorControlSupported,"Post-ACK scheduling substituted for a fresh Windows probe");
    require(cache.complete(cache.begin(2,key),probe(),now)&&!cache.due(2,key,now)&&cache.snapshot(2,true).viewportProvisioning&&cache.snapshot(2,true).monitorControlSupported,"Fresh committed lease/CCD observation did not replace staged capabilities");
    f.transport.checkForeign();++passed;
  }
  return passed;
}
int quiescentRecoveryCases(){
  using namespace host;int passed=0;
  for(int scenario=0;scenario<3;++scenario){
    const NativeRuntimeRecord boot{"ROOT\\DISPLAY\\00a1","recovery",30,{{"0",1600,900},{"1",1280,720}}};
    WorkerFixture f(false,boot);StreamResizeBudget budget;auto topology=f.access(budget).topology;
    const auto foreignBefore=nativeActiveSignatures(f.ccd);
    const auto probe=[&]{auto display=f.probe();if(display.active&&!f.state().get("activationVerified").boolean()){display.configurationPending=true;display.error="native_display_activation_pending";}return display;};
    require(!probe().ready()&&!f.state().get("activationVerified").boolean(),"Recovered driver targets were already advertised as a usable Windows desktop");
    if(scenario==1)f.failValidate=true;if(scenario==2)f.eligible=false;
    NativeBootstrapAccess access{[]{return true;},[&]{return f.eligible;},probe,[&]{return f.state();},[&](auto targets,const std::string&){return activateNativeTopology(targets,topology,2000);}};
    const auto result=bootstrapNativeDisplays(access);
    require(result.display.ready()==(scenario==0)&&f.applies==(scenario==0?1:0),"Recovered worker bootstrap bypassed eligibility or Windows validation");
    require(!f.state().get("activationVerified").boolean()&&parseNativeRuntimeRecord(*f.journal).phase=="starting","Windows worker result marked service completion without its acknowledgement");
    if(scenario==0){require(f.service->activationReady(31)&&probe().ready(),"Fresh recovered CCD verification could not be acknowledged");}
    require(nativeActiveSignatures(f.ccd,nativeDisplayTargets(f.state()))==foreignBefore,"Recovered Windows activation changed a foreign display mode or priority");
    f.transport.checkForeign();++passed;
  }
  for(const auto phase:{"starting","changing","recovery"})for(const bool single:{false,true}){
    NativeRuntimeRecord boot{"ROOT\\DISPLAY\\00a1",phase,30,{{"0",1600,900}}};
    if(!single)boot.outputs.push_back({"1",1280,720});
    Fixture f(single,boot,false);
    require(f.transport.creates==0&&f.state().get("recoveryRequired").boolean(),"Absent quiescence observation was treated as permission to recover");
    f.allowRecovery=true;f.recoveryTick=2000;f.service->tick();
    const auto state=f.state();const auto targets=nativeDisplayTargets(state);
    require(state.get("leaseActive").boolean()&&!state.get("recoveryRequired").boolean()&&!state.get("activationVerified").boolean(),"Quiescent recovery either stayed latched or fabricated Windows verification");
    require(targets.size()==boot.outputs.size()&&targets[0].width==1600&&targets[0].height==900&&state.get("generation").number()==31,"Recovery lost committed sizes/count or reused an old generation");
    require(parseNativeRuntimeRecord(*f.journal).phase=="starting"&&f.transport.removes==0&&f.transport.releases==0,"Recovery rewrote success before Windows verification or released a driver lease");
    require(!f.service->activationReady(30)&&!f.prepare(request()).accepted,"Old Windows completion or resize crossed the recovery generation boundary");
    require(f.service->activationReady(31)&&f.state().get("activationVerified").boolean()&&parseNativeRuntimeRecord(*f.journal).phase=="stable","Fresh Windows verification did not complete recovered startup");
    f.transport.checkForeign();++passed;
  }
  for(int fault=1;fault<=5;++fault){
    Fixture f(true,NativeRuntimeRecord{"ROOT\\DISPLAY\\00a1","recovery",30,{{"0",1600,900}}},false);
    f.allowRecovery=true;f.transport.queryFault=fault;f.recoveryTick=2000;f.service->tick();
    require(f.transport.creates==0&&f.transport.removes==0&&f.transport.releases==0&&f.state().get("recoveryRequired").boolean()&&parseNativeRuntimeRecord(*f.journal).generation==30,"Uncertain/mismatched/busy lease query changed the driver or journal");
    const auto queries=f.transport.queries;f.service->tick();
    require(f.transport.queries==queries,"Recovery ignored its backoff and repeated a driver query");
    f.transport.queryFault=0;f.recoveryTick=4000;f.service->tick();
    require(f.transport.creates==1&&f.state().get("leaseActive").boolean()&&!f.state().get("activationVerified").boolean(),"Later quiescent observation did not recover without an OS reboot");
    f.transport.checkForeign();++passed;
  }
  {
    Fixture source;require(source.prepare(request()).accepted,"Checkpoint fixture failed");
    const auto record=parseNativeRuntimeRecord(*source.journal);
    Fixture f(false,record,false,source.baselineBytes);f.allowRecovery=true;f.recoveryTick=2000;f.service->tick();
    require(f.state().get("leaseActive").boolean()&&!f.state().get("activationVerified").boolean()&&parseNativeRuntimeRecord(*f.journal).baselineHash==record.baselineHash&&f.state().get("recoveryBaselineAvailable").boolean(),"Recovery dropped the durable bound baseline before new Windows verification");
    const auto generation=DWORD(f.state().get("generation").number());
    require(f.service->activationReady(generation)&&parseNativeRuntimeRecord(*f.journal).baselineHash.empty()&&!f.state().get("recoveryBaselineAvailable").boolean(),"Completed recovery kept an armed stale checkpoint");
    f.transport.checkForeign();++passed;
  }
  {
    Fixture f(true,NativeRuntimeRecord{"ROOT\\DISPLAY\\00a1","recovery",30,{{"0",1600,900}}},false);
    f.allowRecovery=true;f.failJournal=true;f.recoveryTick=2000;f.service->tick();
    require(f.transport.creates==0&&parseNativeRuntimeRecord(*f.journal).generation==30&&f.state().get("recoveryRequired").boolean(),"Recovery crossed the mutation boundary without a durable generation");
    f.transport.checkForeign();++passed;
  }
  {
    Fixture f(true,NativeRuntimeRecord{"ROOT\\DISPLAY\\00a1","recovery",30,{{"0",1600,900}}},false);
    api::CreateTemporaryDisplayOwnedRequest request{};request.owner_capability.bytes[0]=42;
    request.display.lease_id=ownedLease;request.display.display_id=71;request.display.width=1600;request.display.height=900;
    request.display.physical_width_mm=600;request.display.physical_height_mm=340;request.display.refresh_rate_millihz=60000;request.display.requested_timeout_ms=15000;
    std::memcpy(request.display.display_name,"Multitap Display 1",sizeof("Multitap Display 1"));
    require(f.client.create_temporary_display_owned(request).ok(),"Busy owned lease fixture failed");
    f.allowRecovery=true;f.recoveryTick=2000;f.service->tick();
    require(f.transport.creates==1&&f.transport.removes==0&&f.transport.releases==0&&!f.state().get("leaseActive").boolean(),"Recovery displaced a still-live owned lease");
    // The memory driver now reports external departure. Product code never
    // sends a release to make its quiescence guard succeed.
    api::LeaseRequest released{};released.lease_id=ownedLease;
    require(f.transport.store.release_lease(released).error==api::StoreError::None,"External departure fixture failed");
    f.recoveryTick=4000;f.service->tick();
    require(f.transport.creates==2&&f.transport.releases==0&&f.state().get("leaseActive").boolean(),"An externally departed lease could not recover without revocation");
    f.transport.checkForeign();++passed;
  }
  {
    Fixture f(true,NativeRuntimeRecord{"ROOT\\DISPLAY\\00a1","recovery",30,{{"0",1600,900}}},false);
    f.allowRecovery=true;f.transport.queryFault=1;f.transport.queryFaultAt=2;f.recoveryTick=2000;f.service->tick();
    require(f.transport.queries==2&&f.transport.creates==0&&f.transport.releases==0&&f.state().get("recoveryRequired").boolean(),"A failed second query raced through the guard into display creation");
    f.transport.queryFault=0;f.recoveryTick=4000;f.service->tick();
    require(f.transport.creates==1&&f.state().get("generation").number()==32&&!f.service->activationReady(31),"Retried recovery reused the failed attempt generation");
    f.transport.checkForeign();++passed;
  }
  return passed;
}
int baselineBindingCases(){using namespace host;int passed=0;
  {Fixture f;const auto r=request();require(f.prepare(r).accepted&&f.baselineBytes&&parseNativeRuntimeRecord(*f.journal).baselineHash==nativeBaselineDigest(*f.baselineBytes)&&f.state().get("recoveryBaselineAvailable").boolean(),"SDK change was not bound to its saved CCD checkpoint");require(f.finish(r,f.key(),false,true).accepted&&parseNativeRuntimeRecord(*f.journal).baselineHash.empty()&&!f.state().get("recoveryBaselineAvailable").boolean(),"Completed viewport retained an armed recovery checkpoint");++passed;}
  {Fixture f;f.failBaseline=true;const auto reply=f.prepare(request());require(!reply.accepted&&!reply.driverChangeAttempted&&!reply.pending&&reply.recoveryRequired&&f.transport.removes==0&&parseNativeRuntimeRecord(*f.journal).phase=="stable","Failed CCD checkpoint save still changed the SDK");++passed;}
  for(int scenario=0;scenario<7;++scenario){Fixture f;const auto r=request();auto record=parseNativeRecoveryBaseline(f.baseline());std::string bytes;switch(scenario){case 0:break;case 1:bytes="{}";break;case 2:record.instance="ROOT\\DISPLAY\\00a2";bytes=nativeRecoveryBaselineBytes(record);break;case 3:record.generation--;bytes=nativeRecoveryBaselineBytes(record);break;case 4:record.topology.replacedMonitor="1";bytes=nativeRecoveryBaselineBytes(record);break;case 5:{for(auto& target:record.topology.targets)target.adapter.LowPart=45;for(auto& path:record.topology.ccd.paths){if(path.sourceInfo.adapterId.LowPart==44)path.sourceInfo.adapterId.LowPart=45;if(path.targetInfo.adapterId.LowPart==44)path.targetInfo.adapterId.LowPart=45;}for(auto& mode:record.topology.ccd.modes)if(mode.adapterId.LowPart==44)mode.adapterId.LowPart=45;bytes=nativeRecoveryBaselineBytes(record);break;}case 6:bytes=std::string(nativeBaselineByteLimit+1,'x');break;}
    const auto reply=f.service->prepareViewport(r,f.key(),streamMonotonicMillis()+80000,bytes,test::currentScope());require(!reply.accepted&&!reply.driverChangeAttempted&&!reply.pending&&f.transport.removes==0&&!f.baselineBytes,"Missing/forged/stale CCD checkpoint reached SDK mutation");++passed;
  }
  {Fixture f;const auto r=request();require(f.prepare(r).accepted&&f.finish(r,f.key(),false,true).accepted,"Fixture resize completion failed");f.baselineBytes="independent saved baseline";const auto before=f.baselineBytes;const int removals=f.transport.removes;const auto reply=f.prepare(r);require(!reply.accepted&&!reply.driverChangeAttempted&&f.transport.removes==removals&&f.baselineBytes==before,"A subsequent viewport overwrote an independent CCD checkpoint edit");++passed;}
  {Fixture f;require(f.prepare(request()).accepted,"Fixture prepare failed");const auto runtime=parseNativeRuntimeRecord(*f.journal);Fixture reopened(false,runtime,false,f.baselineBytes);require(reopened.state().get("recoveryRequired").boolean()&&reopened.state().get("recoveryBaselineAvailable").boolean()&&reopened.transport.creates==0,"New service lost a valid bound checkpoint or silently changed devices");++passed;
    for(int scenario=0;scenario<4;++scenario){auto header=runtime;auto file=f.baselineBytes;switch(scenario){case 0:file.reset();break;case 1:*file+=' ';break;case 2:{auto other=parseNativeRecoveryBaseline(*file);other.instance="ROOT\\DISPLAY\\00a2";file=nativeRecoveryBaselineBytes(other);header.baselineHash=nativeBaselineDigest(*file);break;}case 3:header.outputs[0].width=1280;break;}Fixture rejected(false,header,false,file);require(rejected.state().get("recoveryRequired").boolean()&&!rejected.state().get("recoveryBaselineAvailable").boolean()&&rejected.transport.creates==0,"Missing/mismatched CCD checkpoint was treated as a usable recovery record");++passed;}
  }
  {Fixture f;bool wrote=false;NativeWorkerRpcAccess access{[&](const std::string&){wrote=true;return true;},{},{}};const auto reply=nativeWorkerViewportRpc(request(),"worker.native.resize.prepare",f.key(),false,{},access,std::string(65536,'x'));require(!wrote&&!reply.requestStarted,"Oversized baseline envelope reached the private pipe");++passed;}
  return passed;
}
int scopeBindingCases(){using namespace host;int passed=0;
  for(int scenario=0;scenario<6;++scenario){Fixture f;const auto r=request();auto record=parseNativeRecoveryBaseline(f.baseline());std::optional<NativeRecoveryScope> expected=test::currentScope();switch(scenario){case 0:expected->session++;break;case 1:expected->logon[0]='b';break;case 2:expected->tick=999;break;case 3:expected->tick=1000+streamResizeTimeoutMs+1;break;case 4:expected.reset();break;case 5:record.scope.reset();break;}const auto reply=f.service->prepareViewport(r,f.key(),streamMonotonicMillis()+80000,nativeRecoveryBaselineBytes(record),expected);require(!reply.accepted&&!reply.driverChangeAttempted&&!f.baselineBytes&&f.transport.removes==0,"Unverified current logon or old unscoped checkpoint changed the SDK");++passed;}
  {Fixture f;const auto r=request();require(f.prepare(r).accepted&&f.state().get("recoveryScopeAvailable").boolean(),"Verified session checkpoint lost its scope marker");auto changed=test::currentScope();changed.logon[0]='b';require(!f.service->finishViewport(r,f.key(),false,true,changed).accepted&&f.state().get("configurationPending").boolean(),"Another logon session acknowledged Windows completion");require(f.finish(r,f.key(),false,true).accepted&&!f.state().get("recoveryScopeAvailable").boolean(),"Verified completion retained the active scope marker");++passed;}
  {Fixture f;const auto r=request();auto record=parseNativeRecoveryBaseline(f.baseline());record.scope->foreign[0].monitorPath.clear();record.scope->foreign[0].complete=false;const auto reply=f.service->prepareViewport(r,f.key(),streamMonotonicMillis()+80000,nativeRecoveryBaselineBytes(record),test::currentScope());require(reply.accepted&&!f.state().get("recoveryScopeAvailable").boolean()&&f.state().get("recoveryBaselineAvailable").boolean(),"Incomplete identity either fabricated recovery eligibility or blocked the verified live viewport transaction");require(f.finish(r,f.key(),false,true).accepted,"Live Windows confirmation was blocked by unavailable persistent identity");++passed;}
  {Fixture f;const auto r=request();auto ctx=context(r);ctx.scope.reset();auto incoming=frame(r,"worker.native.resize.prepare",f.key());streamField(incoming,"baseline",f.baseline());streamField(incoming,"logon",std::string(64,'a'));const auto reply=dispatch(incoming,ctx,f);require(!reply.get("accepted").boolean()&&!f.baselineBytes&&f.transport.removes==0,"Worker-supplied identity replaced the service's current logon evidence");++passed;}
  {Fixture f;const auto r=request();auto ctx=context(r);require(dispatch(frame(r,"worker.native.resize.prepare",f.key()),ctx,f).get("accepted").boolean(),"Fixture scoped prepare failed");ctx.scope.reset();ctx.console=ctx.unlocked=false;require(dispatch(frame(r,"worker.native.resize.rollback",""),ctx,f).get("accepted").boolean(),"Lost logon evidence blocked best-effort owned SDK cleanup");require(!dispatch(frame(r,"worker.native.resize.restored",f.key(),true),ctx,f).get("accepted").boolean(),"Missing current logon evidence confirmed Windows restoration");++passed;}
  return passed;
}
// Regression (field receipts): a profile-bound resize failed at
// native phase 2 (stage 102, before any prepare RPC), the unchanged-lease
// cancellation could not be confirmed (profile stage 203), the worker forced
// recoveryRequired and the service latched driver recovery with nothing
// pending. Each observation is printed so a pre-fix run shows every symptom.
// Process/console/CCD observations are injected fixtures; the SDK controller,
// NativeDisplayService, profile journal/worker, RPC and display codecs are real.
struct UnforwardedObservations {
  int failed=0;
  void check(bool value,const char* name){std::cout<<"{\"unforwardedViewportCase\":"<<host::jsonQuote(name)<<",\"ok\":"<<(value?"true":"false")<<"}\n";if(!value)++failed;}
};
int profileViewportUnforwardedCases(UnforwardedObservations& observed){
  using namespace host;int passed=0;
  // 0: console unlocked at stop, the same worker restores in-process.
  // 1: session locked/remote at stop; an exit-fenced recovery worker restores after unlock.
  // 2: an attempt that may have reached the driver (old/uncertain classification) keeps the latch.
  for(int scenario=0;scenario<3;++scenario)try{
    WorkerFixture f(true);f.resize.width=2324;f.resize.height=952;
    const auto original=f.ccd;const auto consolePath=original.paths.front();
    const NativeConsoleAdapterIdentity console{{7,0},L"\\\\?\\PCI#VEN_1234&DEV_1111#0",L"PCI\\VEN_1234&DEV_1111\\3&0",L"PCI",{L"PCI\\VEN_1234&DEV_1111"},nativeConsoleDisplayClass,true,true};
    const std::string machine="00112233-4455-6677-8899-aabbccddeeff",operation(64,'b');
    std::optional<std::string> profileBytes,receiptBytes;
    const NativeRuntimeJournalAccess profileStore{[&]{return profileBytes;},[&](const auto& expected,const auto& desired){if(profileBytes!=expected)return false;profileBytes=desired;return true;}};
    const NativeRuntimeJournalAccess receiptStore{[&]{return receiptBytes;},[&](const auto& expected,const auto& desired){if(receiptBytes!=expected)return false;receiptBytes=desired;return true;}};
    f.service->testProfileViewportStores(profileStore,receiptStore);
    require(f.service->reserveStreamProfile(f.resize.sessionId,f.key()),"Unforwarded viewport reservation failed");
    uint64_t tick=1000;bool remote=false;int probes=0,cancellations=0,exchanges=0;
    const auto scope=[&](const NativeTopologyBaseline& baseline){auto value=test::recoveryScope(baseline);value.tick=++tick;for(auto& foreign:value.foreign)if(nativeSameLuid(foreign.targetAdapter,console.adapter))foreign.sourcePath=foreign.targetPath=utf8(console.interfacePath);return value;};
    const auto apply=[&](const NativeCcdData& supplied,UINT32 flags)->LONG{
      if(!(flags&SDC_APPLY))return ERROR_SUCCESS;
      ++f.applies;f.ccd=WorkerFixture::materialize(supplied);
      if(std::none_of(f.ccd.paths.begin(),f.ccd.paths.end(),[&](const auto& path){return nativeSameLuid(path.targetInfo.adapterId,console.adapter);}))f.ccd.paths.push_back(WorkerFixture::path(console.adapter,consolePath.sourceInfo.id,consolePath.targetInfo.id));
      return ERROR_SUCCESS;
    };
    NativeTopologyAccess topology{[&]{f.sync();return f.ccd;},apply,[&]{return f.eligible;},[&]{return ++tick;},[&]{tick+=50;}};
    NativeStreamLaunchReceipt launch;launch.key={f.resize.sessionId,operation,f.state().get("deviceInstanceId").string(),17};
    launch.service={GetCurrentProcessId(),100};launch.worker={0x232323,101};launch.scope={17,std::string(64,'a'),999,{}};
    launch.entry=nativeStreamProfileLease(f.state(),GetCurrentProcessId());launch.lease=ownedLease;launch.outputs={{71,0}};
    const auto launchBytes=nativeStreamLaunchBytes(launch);const auto generation=launch.entry.generation;
    auto command=JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"stream\",\"resizeProtocol\":\"epoch-v1\",\"epoch\":0,\"monitor\":\"0\",\"sessionId\":"+jsonQuote(f.resize.sessionId)+",\"profileLaunchSha256\":"+jsonQuote(nativeBaselineDigest(launchBytes))+",\"consoleProfile\":{\"protocol\":\"console-profile-v1\",\"sessionId\":"+jsonQuote(f.resize.sessionId)+",\"machineUuid\":"+jsonQuote(machine)+",\"model\":\"std\"}}");
    NativeStreamProfileWorkerAccess access;
    access.eligible=[&](bool){return f.eligible;};access.snapshot=[&](bool){return f.state();};access.topology=topology;access.scope=scope;
    access.loginScope=[&]{return NativeRecoveryScope{17,std::string(64,'a'),++tick,{}};};
    access.machineUuid=[&]{return machine;};access.resolver=[&](LUID luid)->std::optional<NativeConsoleAdapterIdentity>{if(nativeSameLuid(luid,console.adapter))return console;return {};};
    access.stopCapture=[]{};access.launchProof=[&](const NativeStreamProfileRecord& record,bool){return nativeStreamLaunchMatches(parseNativeStreamLaunch(launchBytes),record);};
    access.viewportCommit=[&]()->std::optional<std::pair<NativeStreamViewportCommit,std::string>>{if(!receiptBytes||!f.journal)return {};return std::make_pair(parseNativeStreamViewportCommit(*receiptBytes),*f.journal);};
    // Production refuses before its RPC while the console is remote/unready.
    access.unchangedViewport=[&](const NativeStreamProfileRecord&,const NativeStreamCancelRequest&,const NativeStreamProfileEvidence&)->std::optional<NativeStreamViewportCancelEvidence>{++cancellations;return {};};
    NativeStreamProfileWorker worker(GetCurrentProcessId(),17,profileStore,access);
    require(worker.begin(command,operation)&&worker.active(),"Unforwarded viewport profile entry failed");
    attachFreshCapture(command,f.probe(),f.observed());
    require(worker.reserveViewport(f.resize)&&!worker.active(),"Unforwarded viewport profile reservation failed");
    const auto consoleRestored=[&]{return nativeStreamProfileSignatures(original,launch.entry.targets)==nativeStreamProfileSignatures(nativeActiveCcd(f.ccd),launch.entry.targets);};
    require(!consoleRestored(),"Profile entry did not suppress the console");
    StreamResizeBudget budget;auto viewport=f.access(budget);viewport.topology=topology;viewport.recoveryScope=scope;
    viewport.console=[&](const NativeTopologyBaseline& baseline){return worker.viewportConsole(f.resize,baseline);};viewport.consoleCurrent=[&](const NativeConsoleBinding& binding){return worker.viewportConsoleCurrent(f.resize,binding);};
    // The console session becomes remote right after admission (RDP takeover).
    viewport.viewport.probe=[&](DWORD){auto display=f.probe();if(probes++>=1)remote=true;if(remote)display.remoteSession=true;return display;};
    viewport.exchange=[&](const std::string&,const std::string&,bool,const std::string&)->NativeWorkerViewportReply{++exchanges;throw std::runtime_error("Unexpected native viewport prepare");};
    const auto creates=f.transport.creates,removes=f.transport.removes,applies=f.applies;
    auto result=applyNativeWorkerViewport(command,f.resize,"0",viewport,budget);
    // The read-only phase2 names its rejection site.
    require(!result.ok&&result.nativeStage==2&&result.nativeReason==uint32_t(NativeViewportReason::remoteSession)&&result.unchanged&&exchanges==0&&f.transport.creates==creates&&f.transport.removes==removes&&f.applies==applies,"Fixture did not reproduce the phase-2 failure before any prepare (stage 102)");
    const bool possible=scenario==2?true:result.helper.get("driverChangePossible").boolean(true);
    completeNativeProfileViewport(result,worker,f.resize,possible);
    require(!result.ok&&!worker.active()&&worker.viewportStage()==3&&cancellations==1,"Fixture did not reproduce unconfirmable unchanged cancellation (profile stage 203)");
    // Exact service consumer of worker.resize.result, through the wire codec.
    const auto wire=parseDisplayState(JsonParser::parse(workerDisplayJson(result.display)));
    if(wire.recoveryRequired)f.service->cancelViewportSession(f.resize.sessionId,true);
    f.service->tick();
    const auto latched=f.service->diagnosticSnapshot();
    if(scenario==2){
      require(f.state().get("recoveryRequired").boolean()&&latched.get("phase").string()=="recovery","Possibly-changed viewport lost its uncertain driver latch");
      observed.check(!latched.get("failureReason").string().empty(),"latched-profile-blocked-failure-reason");
    }else{
      observed.check(!wire.recoveryRequired&&!f.state().get("recoveryRequired").boolean()&&latched.get("phase").string()=="stable","phase2-failure-no-driver-latch");
    }
    if(scenario==1)f.eligible=false;
    const auto stopApplies=f.applies;const bool finished=worker.finish();
    if(scenario==0){
      observed.check(finished&&worker.clean()&&f.applies==stopApplies+1&&consoleRestored()&&parseNativeStreamProfileRecord(*profileBytes).phase==NativeStreamProfilePhase::restored,"stop-restores-console-in-process");
      observed.check(f.service->releaseStreamProfile(f.resize.sessionId)&&!f.state().get("recoveryRequired").boolean()&&f.state().get("countControlAvailable").boolean()&&f.service->diagnosticSnapshot().get("phase").string()=="stable","released-profile-stable");
      const std::string next="00000000-0000-0000-0000-000000000009";
      observed.check(f.service->reserveStreamProfile(next,f.key())&&f.service->releaseStreamProfile(next),"next-stream-admitted");
    }else{
      require(!finished&&f.applies==stopApplies&&!consoleRestored(),"Ineligible or possibly-changed stop dropped the reservation or touched Windows");
      // service.hpp recoverProfileWorker reads the held lease before launching.
      bool readable=false;try{(void)nativeStreamProfileLease(JsonParser::parse(f.service->viewportSnapshot()),GetCurrentProcessId());readable=true;}catch(...){ }
      if(scenario==2){require(!readable,"Uncertain driver latch admitted a profile recovery worker");f.transport.checkForeign();++passed;continue;}
      observed.check(readable,"recovery-worker-lease-available");
      f.eligible=true;remote=false;
      NativeStreamProfileWorker recovery(GetCurrentProcessId(),17,profileStore,access);
      const auto request=JsonParser::parse("{\"type\":\"worker.profile.recover\",\"sessionId\":"+jsonQuote(f.resize.sessionId)+",\"deviceInstanceId\":"+jsonQuote(launch.key.instance)+",\"generation\":"+std::to_string(generation)+",\"journalSha256\":"+jsonQuote(nativeBaselineDigest(*profileBytes))+",\"operation\":"+jsonQuote(operation)+",\"workerExited\":true}");
      observed.check(recovery.recover(request)&&f.applies==stopApplies+1&&consoleRestored(),"exit-fenced-recovery-restores-console");
      observed.check(f.service->releaseStreamProfile(f.resize.sessionId)&&!f.state().get("recoveryRequired").boolean()&&f.state().get("countControlAvailable").boolean(),"released-profile-stable");
    }
    f.transport.checkForeign();++passed;
  }catch(const std::exception& error){std::cout<<"{\"unforwardedViewportAborted\":"<<scenario<<",\"reason\":"<<jsonQuote(error.what())<<"}\n";++observed.failed;}
  try{
    // Service START (reboot/update) with a persisted runtime "recovery" and no
    // CCD checkpoint, while an unrestored profile journal is still pending.
    NativeRuntimeRecord boot{"ROOT\\DISPLAY\\00a1","recovery",5,{{"0",1920,1080},{"1",1280,720}},""};
    Fixture f(false,boot,false);f.profilePending=true;f.service->tick();
    const auto blocked=f.service->diagnosticSnapshot();
    observed.check(!blocked.get("leaseActive").boolean()&&blocked.get("phase").string()=="recovery"&&blocked.get("failureReason").string()=="native_stream_profile_recovery_required"&&!f.transport.creates,"start-pending-profile-reports-reason");
    // Retirement/expired restoration resolves the journal; the quiescent restart
    // then re-verifies the lease within the bounded backoff, never immediately.
    f.profilePending=false;f.allowRecovery=true;for(int attempt=0;attempt<4&&!f.state().get("leaseActive").boolean();++attempt){f.recoveryTick+=40000;f.service->tick();}
    const auto restarted=DWORD(f.state().get("generation").number());
    observed.check(restarted==6&&f.service->activationReady(restarted)&&!f.state().get("recoveryRequired").boolean()&&f.state().get("countControlAvailable").boolean()&&f.service->diagnosticSnapshot().get("failureReason").string().empty(),"start-recovery-returns-stable");
    f.transport.checkForeign();++passed;
  }catch(const std::exception& error){std::cout<<"{\"unforwardedViewportAborted\":\"start\",\"reason\":"<<jsonQuote(error.what())<<"}\n";++observed.failed;}
  return passed;
}
}
#include "../tests/native-count-cases.hpp"
int main(){try{using namespace host;int passed=0;
  passed+=v2CleanupHeartbeatCases();
  {UnforwardedObservations unforwarded;const auto cases=profileViewportUnforwardedCases(unforwarded);std::cout<<"{\"profileViewportUnforwardedCases\":"<<cases<<",\"failedObservations\":"<<unforwarded.failed<<",\"windowsDisplayChanges\":0}\n";
#ifdef MULTITAP_PROFILE_VIEWPORT_RECOVERY_ONLY
    return unforwarded.failed?1:0;
#else
    require(!unforwarded.failed,"Unforwarded profile viewport recovery regression failed");passed+=cases;
#endif
  }
  const auto profileViewportCases=profileViewportServiceCases();const auto lifecycleCases=profileViewportLifecycleCases();std::cout<<"{\"profileViewportSdkCases\":"<<profileViewportCases<<",\"profileViewportLifecycleCases\":"<<lifecycleCases<<",\"windowsDisplayChanges\":0}\n";
  {WorkerFixture f(true);auto display=f.probe();display.viewportProvisioning=display.monitorControlSupported=true;auto config=f.config;std::get<Json::Object>(config.value)["streamProfileTrialEnabled"].value=true;require(nativeStreamProfileViewportCapability(display,config,true,true,false)&&!nativeStreamProfileViewportCapability(display,config,true,true,true),"Entry capability required already-primary output or active profile skipped primary proof");display.monitors.front().primary=true;display.monitorControlSupported=false;require(nativeStreamProfileViewportCapability(display,config,true,true,true)&&!nativeStreamProfileViewportCapability(display,config,true,true,false),"Active profile either lost viewport capability or allowed monitor-count mutation");std::get<Json::Object>(config.value)["streamProfileTrialEnabled"].value=false;require(!nativeStreamProfileViewportCapability(display,config,true,true,true)&&nativeStreamProfileCapabilityFields(false).empty(),"Unselected rollout enabled profile capability");++passed;}
  {
    Fixture f;const auto lease=f.key(),stream=std::string("00000000-0000-0000-0000-000000000001");
    require(!f.service->reserveStreamProfile(stream,lease+"stale")&&!f.service->hasStreamProfileReservation(),"Stale stream profile lease reserved");++passed;
    require(f.service->reserveStreamProfile(stream,lease)&&f.service->hasStreamProfileReservation()&&f.service->streamProfilePending(),"Current stream profile could not reserve lease");++passed;
    const auto creates=f.transport.creates,removes=f.transport.removes,releases=f.transport.releases,feeds=f.transport.feeds;
    require(!f.service->reserveStreamProfile("other",lease)&&!f.service->releaseStreamProfile("other"),"Another stream stole or released profile");++passed;
    require(!f.state().get("countControlAvailable").boolean()&&!f.prepare(request()).accepted&&!f.service->activationReady(DWORD(f.state().get("generation").number())),"Profile allowed a concurrent display transaction");++passed;
    f.profilePending=true;for(int i=0;i<3;++i)f.service->tick();
    require(f.transport.creates==creates&&f.transport.removes==removes&&f.transport.releases==releases&&f.transport.feeds>feeds&&!f.state().get("recoveryRequired").boolean(),"Active stream profile changed lease or stopped heartbeat");++passed;
    require(!f.service->releaseStreamProfile(stream)&&!f.service->hasStreamProfileReservation()&&f.service->streamProfilePending()&&f.state().get("recoveryRequired").boolean(),"Unrestored profile was cleared by worker exit");++passed;
    f.service->tick();require(f.transport.creates==creates&&f.transport.removes==removes&&f.transport.releases==releases&&!f.service->reserveStreamProfile("next",lease),"Pending recovery recreated lease or admitted another stream");++passed;
    f.profilePending=false;f.service->tick();require(!f.service->streamProfilePending()&&f.state().get("countControlAvailable").boolean()&&f.service->reserveStreamProfile("next",lease)&&f.service->releaseStreamProfile("next"),"Verified restoration did not release hold");++passed;
    f.transport.checkForeign();
  }
  {Fixture f;const auto r=request();const auto old=f.key();const auto neighbor=nativeDisplayTargets(f.state())[1].target;const auto reply=f.prepare(r);require(reply.accepted&&reply.driverChangeAttempted&&reply.pending&&!reply.recoveryRequired&&f.key()!=old,"Prepare did not hold a changed generation pending Windows verification");require(f.state().get("configurationPending").boolean()&&nativeDisplayTargets(f.state())[0].width==1600&&nativeDisplayTargets(f.state())[1].target==neighbor&&f.transport.releases==0,"Resize replaced the neighbor or released the whole lease");require(!f.finish(r,f.key(),false,false).accepted,"Unverified Windows result was committed");const auto done=f.finish(r,f.key(),false,true);require(done.accepted&&!done.pending&&!f.state().get("configurationPending").boolean(),"Verified viewport did not commit");f.transport.checkForeign();++passed;}
  {Fixture f;const auto r=request();require(f.prepare(r).accepted,"Fixture prepare failed");const auto rollback=f.service->rollbackViewport(r);require(rollback.accepted&&rollback.pending&&nativeDisplayTargets(f.state())[0].width==1920,"Rollback did not restore SDK dimensions before Windows acknowledgement");require(!f.finish(r,f.key(),false,true).accepted,"Rollback was committed as requested new dimensions");require(f.finish(r,f.key(),true,true).accepted&&!f.state().get("configurationPending").boolean(),"Verified SDK/Windows rollback was not finalized");f.transport.checkForeign();++passed;}
  {Fixture f;auto r=request();r.width=1920;r.height=1080;const auto key=f.key();const int creates=f.transport.creates;const auto reply=f.prepare(r);require(reply.accepted&&!reply.driverChangeAttempted&&reply.pending&&f.key()==key&&f.transport.creates==creates&&f.transport.removes==0,"No-op resize recreated a monitor");require(f.finish(r,f.key(),false,true).accepted,"No-op verification did not commit");++passed;}
  {Fixture f;const auto r=request();const auto reply=f.service->prepareViewport(r,"stale-lease",streamMonotonicMillis()+80000);require(!reply.accepted&&!reply.pending&&!reply.driverChangeAttempted&&f.transport.removes==0,"Stale lease changed an output");++passed;}
  for(int scenario=0;scenario<4;++scenario){Fixture f;auto r=request();switch(scenario){case 0:r.monitor="all";break;case 1:r.width=1599;break;case 2:r.epoch=0;break;case 3:r.requestId="invalid id";break;}const auto reply=f.prepare(r);require(!reply.accepted&&!reply.pending&&!reply.driverChangeAttempted&&f.transport.removes==0,"Invalid resize reached SDK mutation");++passed;}
  {Fixture f(true);auto r=request();r.monitor="1";const auto reply=f.prepare(r);require(!reply.accepted&&!reply.pending&&f.transport.removes==0,"Absent monitor was resized");++passed;}
  {Fixture f;const auto r=request();require(f.prepare(r).accepted,"Fixture prepare failed");auto other=r;other.resizeId="00000000-0000-0000-0000-000000000003";require(!f.service->prepareViewport(other,f.key(),streamMonotonicMillis()+80000).accepted&&!f.service->rollbackViewport(other).accepted&&!f.finish(other,f.key(),false,true).accepted,"Concurrent/stale transaction modified pending change");f.service->cancelViewportSession("another-session");require(f.state().get("configurationPending").boolean(),"Unrelated session cancelled a viewport");++passed;}
  {Fixture f;const auto r=request();const auto before=f.key();require(f.prepare(r).accepted,"Fixture prepare failed");require(!f.finish(r,before,false,true).accepted&&f.state().get("configurationPending").boolean(),"Old generation committed new targets");++passed;}
  {Fixture f;const auto r=request();require(f.prepare(r).accepted,"Fixture prepare failed");f.service->cancelViewportSession(r.sessionId);require(!f.state().get("configurationPending").boolean()&&f.state().get("recoveryRequired").boolean()&&nativeDisplayTargets(f.state())[0].width==1920,"Disconnect either lost SDK rollback or fabricated Windows restoration");require(!f.service->prepareViewport(r,"old",streamMonotonicMillis()+80000).accepted,"Uncertain Windows restoration accepted another resize");f.transport.checkForeign();++passed;}
  // A 1 s deadline (a loaded machine could spend 200 ms in prepare itself), then wait until it has actually passed.
  {Fixture f;auto r=request();r.width=1920;r.height=1080;const auto deadline=streamMonotonicMillis()+1000;require(f.prepare(r,deadline).accepted,"Fixture deadline prepare failed");while(streamMonotonicMillis()<=deadline+10)std::this_thread::sleep_for(std::chrono::milliseconds(5));require(!f.finish(r,f.key(),false,true).accepted,"Expired transaction committed");f.service->tick();require(f.state().get("recoveryRequired").boolean()&&!f.state().get("configurationPending").boolean(),"Expired viewport was left ready or pending indefinitely");++passed;}
  {Fixture f;const auto r=request();f.transport.failCreate=f.transport.creates+1;const auto reply=f.prepare(r);require(!reply.accepted&&reply.pending&&reply.driverChangeAttempted&&!reply.recoveryRequired&&nativeDisplayTargets(f.state())[0].width==1920,"Clean SDK rollback was reported as successful new viewport");require(!f.finish(r,f.key(),false,true).accepted,"Failed SDK prepare was committed");require(f.service->rollbackViewport(r).accepted&&f.finish(r,f.key(),true,true).accepted,"Clean SDK rollback could not be freshly verified");f.transport.checkForeign();++passed;}
  {Fixture f;const auto r=request();f.transport.stuck=true;const auto reply=f.prepare(r);require(!reply.accepted&&reply.recoveryRequired&&f.state().get("recoveryRequired").boolean(),"Unsettled departure was marked ready");require(!f.service->rollbackViewport(r).accepted&&!f.service->rollbackViewport(r).accepted,"Uncertain driver removal fabricated rollback on retry");f.transport.checkForeign();++passed;}
  {Fixture f;auto& values=std::get<Json::Object>(f.config.value);values["deviceInstanceId"].value=std::string("ROOT\\DISPLAY\\00a2");const auto reply=f.prepare(request());require(!reply.accepted&&!reply.driverChangeAttempted&&f.transport.removes==0,"Changed PnP selection reused an older controller");++passed;}
  {Fixture f;const auto r=request();const auto ctx=context(r);const auto prepared=dispatch(frame(r,"worker.native.resize.prepare",f.key()),ctx,f);require(prepared.get("accepted").boolean()&&nativeSameResize(streamResizeRequest(prepared),r)&&prepared.get("phase").string()=="worker.native.resize.prepare","Actual native RPC lost authorization/correlation");require(!dispatch(frame(r,"worker.native.resize.commit",f.key()),ctx,f).get("accepted").boolean(),"RPC committed without Windows verification");const auto complete=dispatch(frame(r,"worker.native.resize.commit",f.key(),true),ctx,f);require(complete.get("accepted").boolean()&&!complete.get("pending").boolean(),"Authorized verified native RPC did not commit");f.transport.checkForeign();++passed;}
  for(int scenario=0;scenario<8;++scenario){Fixture f;const auto r=request();auto ctx=context(r);auto incoming=frame(r,"worker.native.resize.prepare",f.key());switch(scenario){case 0:ctx.authorized=false;break;case 1:ctx.stopped=true;break;case 2:ctx.console=false;break;case 3:ctx.unlocked=false;break;case 4:ctx.now=[deadline=ctx.deadline]{return deadline;};break;case 5:streamField(incoming,"monitor",std::string("1"));break;case 6:streamField(incoming,"width",1280);break;case 7:streamField(incoming,"resizeId",std::string("00000000-0000-0000-0000-000000000003"));break;}
    std::get<Json::Object>(incoming.value)["authorized"].value=true;const auto reply=dispatch(incoming,ctx,f);require(!reply.get("accepted").boolean()&&f.transport.removes==0&&!f.state().get("configurationPending").boolean(),"A forged/stale/cancelled native RPC changed the SDK");++passed;
  }
  {Fixture f;const auto r=request();auto ctx=context(r);require(dispatch(frame(r,"worker.native.resize.prepare",f.key()),ctx,f).get("accepted").boolean(),"Fixture RPC prepare failed");ctx.console=false;ctx.unlocked=false;require(dispatch(frame(r,"worker.native.resize.rollback",""),ctx,f).get("accepted").boolean()&&nativeDisplayTargets(f.state())[0].width==1920,"Console loss blocked restoring the owned SDK configuration");require(!dispatch(frame(r,"worker.native.resize.restored",f.key(),true),ctx,f).get("accepted").boolean(),"Locked/RDP worker confirmed Windows restoration");ctx.console=ctx.unlocked=true;require(dispatch(frame(r,"worker.native.resize.restored",f.key(),true),ctx,f).get("accepted").boolean(),"Fresh verified rollback was not finalized through RPC");++passed;}
  {Fixture f;const auto r=request();const auto ctx=context(r);require(dispatch(frame(r,"worker.native.resize.prepare",f.key()),ctx,f).get("accepted").boolean(),"Fixture RPC prepare failed");auto incoming=frame(r,"worker.native.resize.commit",f.key());std::get<Json::Object>(incoming.value)["windowsVerified"].value=std::string("true");require(!dispatch(incoming,ctx,f).get("accepted").boolean()&&f.state().get("configurationPending").boolean(),"A string forged a Windows verification acknowledgement");++passed;}
  {Fixture f;const auto r=request();auto ctx=context(r);ctx.pending.reset();const auto missing=dispatchNativeViewport(frame(r,"worker.native.resize.prepare",f.key()),ctx,f.service.get());const auto unknown=dispatchNativeViewport(JsonParser::parse(R"({"type":"worker.native.unexpected","private":"fixture"})"),context(r),f.service.get());const auto ordinary=dispatchNativeViewport(JsonParser::parse(R"({"type":"input"})"),context(r),f.service.get());require(missing.handled&&missing.response.empty()&&unknown.handled&&unknown.response.empty()&&!ordinary.handled&&f.transport.removes==0,"Unexpected private RPC escaped to the remote stream");++passed;}
  {Fixture f;f.failPublish=true;const auto reply=f.prepare(request());require(!reply.accepted&&!reply.pending&&reply.recoveryRequired&&nativeDisplayTargets(f.state())[0].width==1920,"Failed prepare publication left an acknowledged changed output");f.transport.checkForeign();++passed;}
  {Fixture f;const auto r=request();require(f.prepare(r).accepted,"Fixture prepare failed");f.failPublish=true;const auto reply=f.finish(r,f.key(),false,true);require(!reply.accepted&&reply.pending&&f.state().get("configurationPending").boolean(),"Unpublished completion discarded the rollback transaction");f.failPublish=false;require(f.service->rollbackViewport(r).accepted&&f.finish(r,f.key(),true,true).accepted,"Completion publication failure could not roll back");++passed;}
  {Fixture f;const auto r=request();require(f.prepare(r).accepted,"Fixture prepare failed");f.failPublish=true;require(!f.service->rollbackViewport(r).accepted,"Unpublished SDK rollback was acknowledged");const int removes=f.transport.removes;f.failPublish=false;require(f.service->rollbackViewport(r).accepted&&f.transport.removes==removes&&f.finish(r,f.key(),true,true).accepted,"Rollback publication retry repeated driver removal");++passed;}
  {Fixture f;auto r=request();r.width=1920;r.height=1080;require(f.prepare(r).accepted,"Fixture no-op prepare failed");f.failPublish=true;require(!f.finish(r,f.key(),false,true).accepted&&f.state().get("configurationPending").boolean(),"No-op completion bypassed snapshot publication");++passed;}
  {Fixture f;const auto r=request();const auto ctx=context(r);const auto name=L"\\\\.\\pipe\\MultitapHost.QA.NativeViewport."+randomName();
    WinHandle server(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr));require(server.value!=INVALID_HANDLE_VALUE,"Fixture native pipe server failed");
    WinHandle client(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));require(client.value!=INVALID_HANDLE_VALUE,"Fixture native pipe client failed");ConnectNamedPipe(server.value,nullptr);std::atomic<bool> stopped=false;std::mutex writing;std::exception_ptr failure;int interleaved=0;
    std::jthread responder([&]{try{for(int i=0;i<2;++i){bool text=false;std::vector<uint8_t> bytes;require(readPipePacket(server.value,text,bytes,stopped,std::chrono::milliseconds(2000))&&text,"Actual worker RPC request missing");const auto message=JsonParser::parse(std::string(bytes.begin(),bytes.end()));const auto reply=dispatchNativeViewport(message,ctx,f.service.get());require(reply.handled&&!reply.response.empty(),"Actual private dispatcher declined fixture request");const std::string reset=R"({"type":"input.reset"})";require(pipePacket(server.value,true,{reset.begin(),reset.end()},stopped,writing)&&pipePacket(server.value,true,{reply.response.begin(),reply.response.end()},stopped,writing),"Actual worker RPC reply missing");}}catch(...){failure=std::current_exception();}});
    StreamResizeBudget budget;budget.deadline=streamMonotonicMillis()+5000;const auto hook=[&](const Json& value){if(value.get("type").string()!="input.reset")return false;++interleaved;return true;};const auto prepared=requestNativeWorkerViewport(client.value,r,"worker.native.resize.prepare",f.key(),false,budget,stopped,writing,hook,f.baseline());require(prepared.received&&prepared.accepted&&prepared.pending,"Worker did not receive actual SDK prepare over its pipe framing");const auto committed=requestNativeWorkerViewport(client.value,r,"worker.native.resize.commit",nativeDisplayLeaseKey(prepared.snapshot),true,budget,stopped,writing,hook);responder.join();if(failure)std::rethrow_exception(failure);require(committed.received&&committed.accepted&&!committed.pending&&interleaved==2&&!f.state().get("configurationPending").boolean(),"Worker lost interleaved controls or completion correlation");f.transport.checkForeign();++passed;
  }
  for(int scenario=0;scenario<12;++scenario){Fixture f;const auto r=request();const auto ctx=context(r);auto valid=dispatch(frame(r,"worker.native.resize.prepare",f.key()),ctx,f);bool wrote=false,consumed=false;NativeWorkerRpcAccess access{[&](const std::string&){wrote=true;return scenario!=0;},[&](DWORD)->std::optional<Json>{if(consumed||scenario==1)return std::nullopt;consumed=true;auto value=valid;switch(scenario){case 2:streamField(value,"phase",std::string("worker.native.resize.commit"));break;case 3:streamField(value,"width",1280);break;case 4:streamField(value,"resizeId",std::string("00000000-0000-0000-0000-000000000003"));break;case 5:std::get<Json::Object>(value.value)["pending"].value=std::string("true");break;case 6:std::get<Json::Object>(value.value)["pending"].value=false;break;case 7:std::get<Json::Object>(value.value)["recoveryRequired"].value=true;break;case 8:streamField(std::get<Json::Object>(value.value)["snapshot"],"generation",0);break;case 9:std::get<Json::Object>(value.value)["snapshot"].value=Json::Object{};break;case 10:streamField(value,"type",std::string("session.ended"));break;case 11:throw std::runtime_error("Fixture read failure");}return value;},{}};const auto reply=nativeWorkerViewportRpc(r,"worker.native.resize.prepare",f.key(),false,{},access);require(wrote&&reply.requestStarted&&!reply.received&&!reply.accepted&&reply.recoveryRequired,"Malformed/lost worker reply fabricated unchanged or verified state");++passed;}
  {Fixture f;bool wrote=false;StreamResizeBudget budget;budget.deadline=streamMonotonicMillis()-1;NativeWorkerRpcAccess access{[&](const std::string&){wrote=true;return true;},{},{}};const auto reply=nativeWorkerViewportRpc(request(),"worker.native.resize.prepare",f.key(),false,budget,access);require(!wrote&&!reply.requestStarted&&!reply.recoveryRequired,"Expired worker transaction reached the private pipe");++passed;}
  {Fixture f(false,{},false,{},true);const auto observation=f.service->diagnosticSnapshot();require(observation.get("failureReason").string()=="controller_start_failed"&&observation.get("phase").string()=="recovery"&&!observation.get("leaseActive").boolean(),"Actual SDK startup failure was not reported without claiming a monitor");f.service->tick();require(f.service->diagnosticSnapshot().get("failureReason").string()=="controller_start_failed","Later recovery tick erased the original initialization cause");f.transport.checkForeign();++passed;}
  {Fixture f(false,{},false);const auto generation=DWORD(f.state().get("generation").number());const auto before=f.journal;
    f.service->observeActivation(generation,{"native_topology_validate_failed",ERROR_BAD_CONFIGURATION,false,false});const auto failure=f.service->diagnosticSnapshot();
    require(failure.get("activationCurrent").boolean()&&failure.get("activation").get("systemError").number()==ERROR_BAD_CONFIGURATION&&!nativeActivationPublished(f.state())&&f.journal==before,"A diagnostic changed runtime readiness or lost Windows failure details");++passed;
    f.service->observeActivation(generation-1,{"",0,false,true});require(f.service->diagnosticSnapshot().get("activation").get("reason").string()=="native_topology_validate_failed","Retired worker overwrote the current diagnostic");++passed;
    f.service->observeActivation(generation,{"",0,false,true});require(!nativeActivationPublished(f.state())&&f.journal==before,"Diagnostic verification substituted for durable activationReady");++passed;
    require(f.service->activationReady(generation)&&nativeActivationPublished(f.state()),"Existing explicit activation commit stopped working");++passed;
    f.service->shutdown();f.service->tick();const auto older=f.service->diagnosticSnapshot();require(!older.get("activationCurrent").boolean()&&!older.get("activationVerified").boolean(),"Restart treated previous activation observation as current proof");f.transport.checkForeign();++passed;
  }
  // backlog #70: each one-second loop pass rewrote (and flushed) the snapshot
  // although only its tick moved. Unchanged, it is published again only before
  // readers would call it stale; a change is published at once.
  {Fixture f;const auto settled=f.published.size();const int reads=f.configReads;
    for(int pass=0;pass<5;++pass)f.service->tick();
    require(f.configReads==reads+5,"A loop pass read the provider marker more than once");++passed;
    require(f.published.size()==settled,"An unchanged snapshot was published on every loop pass");++passed;
    require(f.prepare(request()).accepted&&f.published.size()==settled+1&&JsonParser::parse(f.published.back()).get("configurationPending").boolean(),"A changed snapshot was not published at once");++passed;
    f.service->tick();require(f.published.size()==settled+1,"The changed snapshot was published again on the next pass");
    Sleep(DWORD(nativeDisplaySnapshotRefreshMs)+100);f.service->tick();
    require(f.published.size()==settled+2&&JsonParser::parse(f.published.back()).get("tick").number()>=JsonParser::parse(f.published[settled]).get("tick").number()+double(nativeDisplaySnapshotRefreshMs),"An unchanged snapshot was not refreshed before readers call it stale");
    require(nativeDisplaySnapshotFresh(JsonParser::parse(f.published.back()),GetTickCount64(),GetCurrentProcessId()),"The refreshed snapshot is not fresh");++passed;
  }
  const int workerCases=workerCoordinatorCases(),runtimeCases=runtimeServiceCases(),baselineCases=baselineBindingCases(),scopeCases=scopeBindingCases();const int countCases=nativeCountCases(),recoveryCases=quiescentRecoveryCases(),liveRecoveryCases=liveActivationRecoveryCases();passed+=workerCases+runtimeCases+baselineCases+scopeCases+countCases+recoveryCases+liveRecoveryCases;
  const int trackCases=trackServiceCases();passed+=trackCases;std::cout<<"{\"trackServiceCases\":"<<trackCases<<",\"actualTrackPipeExchanges\":2,\"originalDriverUnchanged\":true,\"windowsDisplayChanges\":0}\n";
  const int trackWorkers=trackWorkerCases();passed+=trackWorkers;std::cout<<"{\"trackWorkerCases\":"<<trackWorkers<<",\"exactGroupWorkerAndSourceBinding\":true,\"queryAndSetDisplayConfig\":\"substituted\",\"windowsDisplayChanges\":0}\n";
  const int trackMetadata=trackMetadataCases();passed+=trackMetadata;std::cout<<"{\"trackMetadataCases\":"<<trackMetadata<<",\"actualSourceMetadata\":true,\"windowsDisplayChanges\":0}\n";
  const int trackInitial=trackInitialCases();passed+=trackInitial;std::cout<<"{\"initialTrackViewportCases\":"<<trackInitial<<",\"privateInitialIdsExcluded\":true,\"queryAndSetDisplayConfig\":\"substituted\",\"windowsDisplayChanges\":0}\n";
  std::cout<<"{\"cases\":"<<passed<<",\"quiescentRecoveryCases\":"<<recoveryCases<<",\"liveActivationRecoveryCases\":"<<liveRecoveryCases<<",\"nativeCountCases\":"<<countCases<<",\"workerCoordinatorCases\":"<<workerCases<<",\"runtimeJournalCases\":"<<runtimeCases<<",\"baselineBindingCases\":"<<baselineCases<<",\"scopeBindingCases\":"<<scopeCases<<",\"actualServiceTransaction\":true,\"actualPrivateRpcDispatcher\":true,\"actualSdkControllerAndLeaseStore\":true,\"actualWorkerRpcAndPipeExchanges\":2,\"actualCountPipeExchanges\":2,\"snapshotFailureCases\":4,\"osDeparture\":\"simulated\",\"snapshotPublisherAndJournal\":\"memory\",\"windowsDeviceRegistryCertificateChanges\":0,\"windowsTopologyVerification\":\"CCD/configure/capture substituted\"}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
#else
int main(){host::NativeDisplayService service;const auto request=host::StreamResize{};const auto tracks=host::NativeTrackResize{};if(service.prepareViewport(request,"",0).accepted||service.rollbackViewport(request).accepted||service.finishViewport(request,"",false,true).accepted||service.prepareTrackViewport(tracks,"",0,"",{}).accepted||service.rollbackTrackViewport(tracks).accepted||service.finishTrackViewport(tracks,"",false,true,{}).accepted)return 1;std::cout<<"{\"cases\":2,\"nativeProviderUnavailable\":true,\"legacyGroupApisUnavailable\":true,\"driverCalls\":0}\n";return 0;}
#endif
