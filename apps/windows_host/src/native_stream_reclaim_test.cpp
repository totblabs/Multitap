#include "native_stream_reclaim.hpp"
#include "native_stream_profile_worker.hpp"
#include "native_stream_retirement.hpp"
#include "native_stream_expired_integration.hpp"
#include <iostream>
#include <source_location>
#ifdef MULTITAP_NATIVE_DISPLAY
#include <virtual_display/driver/ioctl_dispatcher.h>
using namespace host;
namespace api=multitap::vdisplay::api;
namespace {
int checks=0;
void require(bool value,const std::source_location at=std::source_location::current()){if(!value)throw std::runtime_error("reclaim transaction check at "+std::to_string(at.line()));++checks;}
template<class F>void rejected(F action){bool threw=false;try{action();}catch(...){threw=true;}require(threw);}
constexpr uint64_t oldLease=api::kMinOpaqueLeaseId+71,newLease=api::kMinOpaqueLeaseId+72;
const NativeStreamProcessIdentity executor{4323,300},cleanup{4324,400};
struct Memory {
  std::optional<std::string> bytes;bool fail=false,lostAck=false,corrupt=false;int saves=0;std::function<void()> afterSave;
  NativeRuntimeJournalAccess access(){return {[this]{return bytes;},[this](const auto& expected,const auto& desired){++saves;if(fail||expected!=bytes)return false;bytes=corrupt?"{}":desired;if(afterSave)afterSave();return !lostAck;}};}
};
struct Backend:api::DisplayDriverBackend {
  int arrives=0,departs=0;
  api::BackendDisplayResult arrive_temporary_display(const api::DisplayDescriptor&)override{return {api::BackendError::None,{33,-1},uint32_t(++arrives)};}
  api::BackendError depart_temporary_display(uint64_t,uint64_t)override{++departs;return api::BackendError::None;}
  api::BackendError set_permanent_display_count(const api::PermanentDisplayCountRequest&)override{throw std::runtime_error("Unexpected permanent mutation");}
  api::BackendError set_render_adapter(const api::SetRenderAdapterRequest&)override{throw std::runtime_error("Unexpected adapter mutation");}
};
struct Wire:api::ControlTransport {
  Backend backend;api::DriverController controller{api::DisplayStore{2,8},backend};api::IoctlDispatcher dispatcher{controller};int reclaims=0;bool lostAck=false,queryFailure=false;std::function<void()> afterReclaim;
  bool ioctl(uint32_t code,const void* input,size_t inSize,void* output,size_t outSize,size_t& received,uint32_t& error)override{
    received=0;error=0;if(queryFailure&&(code==api::kIoctlQueryDisplayState||code==api::kIoctlQueryLease)){error=5;return false;}
    if(code==api::kIoctlReclaimTemporaryDisplay)++reclaims;
    const auto result=dispatcher.dispatch(code,input,inSize,output,outSize,std::chrono::steady_clock::time_point{std::chrono::milliseconds(1000)});received=result.bytes_returned;error=result.status==api::IoctlStatus::Success?0:5;
    if(code==api::kIoctlReclaimTemporaryDisplay){if(afterReclaim)afterReclaim();if(lostAck){received=0;error=123;return false;}}
    return !error;
  }
};
void addPath(NativeCcdData& ccd,LUID adapter,UINT32 sourceId,UINT32 targetId,DWORD width,DWORD height,LONG x,LONG y){
  DISPLAYCONFIG_PATH_INFO path{};path.flags=DISPLAYCONFIG_PATH_ACTIVE;path.sourceInfo.adapterId=path.targetInfo.adapterId=adapter;path.sourceInfo.id=sourceId;path.targetInfo.id=targetId;path.sourceInfo.statusFlags=DISPLAYCONFIG_SOURCE_IN_USE;path.targetInfo.statusFlags=DISPLAYCONFIG_TARGET_IN_USE;path.targetInfo.targetAvailable=TRUE;path.targetInfo.outputTechnology=DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;path.targetInfo.rotation=DISPLAYCONFIG_ROTATION_IDENTITY;path.targetInfo.scaling=DISPLAYCONFIG_SCALING_IDENTITY;path.targetInfo.refreshRate={60,1};path.targetInfo.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  path.sourceInfo.modeInfoIdx=UINT32(ccd.modes.size());DISPLAYCONFIG_MODE_INFO source{};source.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;source.adapterId=adapter;source.id=sourceId;source.sourceMode={width,height,DISPLAYCONFIG_PIXELFORMAT_32BPP,{x,y}};ccd.modes.push_back(source);
  path.targetInfo.modeInfoIdx=UINT32(ccd.modes.size());DISPLAYCONFIG_MODE_INFO target{};target.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;target.adapterId=adapter;target.id=targetId;auto& signal=target.targetMode.targetVideoSignalInfo;signal.pixelRate=uint64_t(width+160)*(height+30)*60;signal.hSyncFreq={67500,1};signal.vSyncFreq={60,1};signal.activeSize={width,height};signal.totalSize={width+160,height+30};signal.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;ccd.modes.push_back(target);ccd.paths.push_back(path);
}
struct Fixture {
  Wire wire;api::ControlClient client{wire};api::OwnerCapability owner{};
  NativeStreamLaunchReceipt launch;NativeStreamProfileRecord profile;NativeRuntimeRecord runtime;NativeStreamProfileEvidence proof;
  Memory intent,launches,profiles,runtimes,identities;NativeStreamReclaimJournal journal{intent.access(),launches.access(),profiles.access(),runtimes.access(),identities.access()};
  NativeStreamReclaimAuthority authority;uint64_t now=1200;bool identity=true;
  Fixture(){
    owner.bytes.fill(42);launch.key={"stream-149",std::string(64,'a'),"ROOT\\DISPLAY\\00a1",2};launch.service={4321,100};launch.worker={4322,200};launch.scope={2,std::string(64,'b'),900,{}};launch.entry={4321,7,{}};launch.lease=oldLease;
    for(size_t i=0;i<2;++i){api::CreateTemporaryDisplayOwnedRequest r{};r.owner_capability=owner;auto& d=r.display;d.lease_id=oldLease;d.display_id=71+i;d.width=i?1280:1920;d.height=i?720:1080;d.physical_width_mm=600;d.physical_height_mm=340;d.refresh_rate_millihz=60000;d.requested_timeout_ms=15000;std::memcpy(d.display_name,"Multitap Test",sizeof("Multitap Test"));const auto made=client.create_temporary_display_owned(r);require(made.ok());launch.outputs.push_back({d.display_id,made.value.connector_index});launch.entry.targets.push_back({std::to_string(i),{33,-1},made.value.target_id,d.width,d.height});}
    profile.key=launch.key;profile.entry=profile.current=launch.entry;profile.launchSha256=nativeBaselineDigest(nativeStreamLaunchBytes(launch));profile.phase=NativeStreamProfilePhase::active;
    NativeCcdData before;addPath(before,{77,-1},0,9,1600,900,0,0);addPath(before,{33,-1},0,1,1920,1080,1600,0);addPath(before,{33,-1},1,2,1280,720,3520,0);addPath(before,{88,-1},0,5,800,600,0,1080);
    profile.profile=planNativeStreamPrimaryProfile(before,profile.entry.targets,NativeStreamConsolePath{{77,-1},{77,-1},0,9});profile.profileSha256=nativeBaselineDigest(nativeStreamProfilePayloadBytes(profile.profile));
    profile.scope={2,std::string(64,'b'),1000,{{{77,-1},{77,-1},0,9,1,"console-source","console-target","console-monitor",true},{{88,-1},{88,-1},0,5,2,"foreign-source","foreign-target","foreign-monitor",true}}};
    profile.console={{{"00112233-4455-6677-8899-aabbccddeeff"},"std"},{{77,-1},L"\\\\?\\PCI#VEN_1234&DEV_1111#0",L"PCI\\VEN_1234&DEV_1111\\3&0",L"PCI",{L"PCI\\VEN_1234&DEV_1111"},nativeConsoleDisplayClass,true,true},{{77,-1},{77,-1},0,9}};profile.scope.foreign[0].sourcePath=profile.scope.foreign[0].targetPath=utf8(profile.console.device.interfacePath);
    runtime={profile.key.instance,"stable",7,{{"0",1920,1080},{"1",1280,720}},""};persist();
    NativeDisplayIdentitySnapshot ownership;std::memcpy(ownership.value.magic,"TOTBVDD1",8);ownership.value.lease=oldLease;ownership.value.displays[0]=71;ownership.value.displays[1]=72;ownership.value.owner=owner;identities.bytes=sealNativeDisplayIdentity(ownership.value);
    auto scope=profile.scope;scope.tick=1100;scope.foreign.erase(scope.foreign.begin());auto ccd=profile.profile.applied;auto console=profile.profile.before.paths.front();console.flags&=~DISPLAYCONFIG_PATH_ACTIVE;ccd.paths.push_back(console);proof={profile.key,profile.current,scope,profile.console,ccd};
    authority.process=[this](const auto& p){if(p==launch.service||p==launch.worker)return NativeStreamProcessState::gone;if(p==executor||p==cleanup)return NativeStreamProcessState::running;return NativeStreamProcessState::unknown;};
    authority.display=[this]()->std::optional<NativeStreamProfileEvidence>{return proof;};authority.identity=[this](const auto&){return identity;};authority.now=[this]{return now;};
  }
  void persist(){launches.bytes=nativeStreamLaunchBytes(launch);profiles.bytes=nativeStreamProfileRecordBytes(profile);runtimes.bytes=nativeRuntimeBytes(runtime);}
  std::optional<NativeStreamReclaimRecord> prepare(){return journal.prepare(std::string(64,'c'),newLease,executor,cleanup,authority);}
};
void codecAndPersistence(){
  Fixture f;const auto r=f.prepare();require(r.has_value());const auto encoded=nativeStreamReclaimBytes(*r);require(nativeStreamReclaimBytes(parseNativeStreamReclaim(encoded))==encoded&&r->previous==oldLease&&r->replacement==newLease&&r->generation==8&&f.journal.current(*r,f.authority));
  require(f.prepare().has_value()&&f.intent.saves==1);require(!f.journal.prepare(std::string(64,'d'),newLease+1,executor,cleanup,f.authority)&&f.intent.bytes==encoded);
  for(const auto& bytes:{std::string(),std::string("{}"),encoded+" ",std::string(4097,'x')})rejected([&]{(void)parseNativeStreamReclaim(bytes);});
  for(const auto& mutate:std::vector<std::function<void(NativeStreamReclaimRecord&)>>{
      [](auto& r){r.replacement=r.previous;},[](auto& r){r.previous=1;},[](auto& r){r.generation=1;},[](auto& r){r.service.created=0;},[](auto& r){r.worker.pid=r.service.pid;},[](auto& r){r.launchSha256.clear();},[](auto& r){r.operation=r.key.operation;}}){auto changed=*r;mutate(changed);rejected([&]{(void)nativeStreamReclaimBytes(changed);});}
  {Fixture g;g.intent.fail=true;require(!g.prepare()&&!g.intent.bytes&&g.wire.reclaims==0);}
  {Fixture g;g.intent.lostAck=true;require(!g.prepare()&&g.intent.bytes);g.intent.lostAck=false;require(g.prepare().has_value()&&g.intent.saves==1);}
  {Fixture g;g.intent.corrupt=true;require(!g.prepare()&&g.intent.bytes=="{}");g.intent.corrupt=false;require(!g.prepare()&&g.intent.saves==1);}
  const auto directory=std::filesystem::current_path()/(L"native-reclaim-qa-"+randomName());require(std::filesystem::create_directory(directory));const auto path=directory/L"native-stream-reclaim.json";
  struct Cleanup{std::filesystem::path file,dir;~Cleanup(){std::error_code e;std::filesystem::remove(file,e);std::filesystem::remove(dir,e);}}remove{path,directory};
  const auto trusted=[&](const auto& p){return p==directory||p.parent_path()==directory;};auto disk=nativeStreamReclaimFileAccess(path,trusted);Fixture g;
  NativeStreamReclaimJournal durable(disk,g.launches.access(),g.profiles.access(),g.runtimes.access(),g.identities.access());const auto saved=durable.prepare(std::string(64,'c'),newLease,executor,cleanup,g.authority);require(saved&&disk.load()==nativeStreamReclaimBytes(*saved));
  NativeStreamReclaimJournal reopened(disk,g.launches.access(),g.profiles.access(),g.runtimes.access(),g.identities.access());require(reopened.current(*saved,g.authority));require(!disk.save({},encoded));rejected([&]{(void)nativeStreamReclaimFileAccess(directory/L"native-stream-profile.json",trusted);});
  g.wire.lostAck=true;require(reopened.reconcile(g.client,g.owner,*saved,g.authority).location==multitap::vdisplay::LiveLeaseLocation::replacement);
  require(reopened.reconcile(g.client,g.owner,*saved,g.authority).location==multitap::vdisplay::LiveLeaseLocation::replacement&&g.wire.reclaims==1&&g.wire.backend.arrives==2&&g.wire.backend.departs==0);
  auto controller=std::make_unique<multitap::vdisplay::Controller>(g.client,newLease,g.owner);require(reopened.adopt(*controller,*saved,g.authority)&&controller->state().targets.size()==2&&controller->heartbeat());
  require(g.profiles.bytes==nativeStreamProfileRecordBytes(g.profile)&&g.runtimes.bytes==nativeRuntimeBytes(g.runtime)); // Reclaim is not topology restoration.
}
void admission(){
  for(int scenario=0;scenario<22;++scenario){Fixture f;switch(scenario){
      case 0:f.authority.process={};break;case 1:f.authority.display={};break;case 2:f.authority.identity={};break;case 3:f.authority.now={};break;
      case 4:f.identity=false;break;case 5:f.now=4200;break;case 6:f.now=1000;break;case 7:f.proof.scope.logon=std::string(64,'d');break;
      case 8:f.proof.key.session++;break;case 9:f.proof.lease.targets[0].target++;break;case 10:f.proof.console.device.instanceId=L"PCI\\VEN_1234&DEV_1111\\DIFFERENT";break;
      case 11:f.proof.scope.foreign[0].monitorPath="changed";break;case 12:f.profile.phase=NativeStreamProfilePhase::restored;f.profile.stopRequested=true;f.persist();break;
      case 13:f.profile.launchSha256.clear();f.persist();break;case 14:f.runtime.generation++;f.persist();break;case 15:f.runtime.phase="recovery";f.persist();break;
      case 16:f.runtime.outputs[0].width=1280;f.persist();break;case 17:f.profile.resizeSequence=1;f.persist();break;
      case 18:f.launch.outputs[0].display+=3;f.launches.bytes=nativeStreamLaunchBytes(f.launch);break;
      case 19:f.proof.topology.paths.erase(f.proof.topology.paths.begin());break;case 20:f.proof.scope.tick=0;break;
      case 21:f.launch.outputs[0].connector=UINT32_MAX;f.profile.launchSha256=nativeBaselineDigest(nativeStreamLaunchBytes(f.launch));f.persist();break;}
    require(!f.prepare()&&!f.intent.bytes&&f.wire.reclaims==0&&f.wire.backend.departs==0);
  }
  for(const auto& process:{NativeStreamProcessIdentity{4321,100},NativeStreamProcessIdentity{4322,200},executor,cleanup})for(const auto bad:{NativeStreamProcessState::unknown,NativeStreamProcessState::running,NativeStreamProcessState::gone}){
    Fixture f;const auto normal=f.authority.process;const auto expected=normal(process);if(bad==expected)continue;f.authority.process=[=](const auto& p){return p==process?bad:normal(p);};require(!f.prepare()&&!f.intent.bytes&&f.wire.reclaims==0);
  }
}
void interrupted(){
  for(int scenario=0;scenario<8;++scenario){Fixture f;const auto r=f.prepare();require(r.has_value());switch(scenario){
      case 0:f.intent.bytes.reset();break;case 1:f.intent.bytes="{}";break;case 2:f.identity=false;break;case 3:f.now+=5000;break;
      case 4:f.profile.revision++;f.persist();break;case 5:f.runtime.generation++;f.persist();break;case 6:f.launch.worker.created++;f.launches.bytes=nativeStreamLaunchBytes(f.launch);break;
      case 7:f.authority.process=[](const auto&){return NativeStreamProcessState::unknown;};break;}
    require(f.journal.reconcile(f.client,f.owner,*r,f.authority).location==multitap::vdisplay::LiveLeaseLocation::unknown&&f.wire.reclaims==0&&f.wire.backend.departs==0);
  }
  {Fixture f;const auto original=f.authority.process;f.authority.process=[&](const auto& p){f.profiles.bytes="{}";return original(p);};require(!f.prepare()&&!f.intent.bytes);}
  {Fixture f;const auto r=f.prepare();require(r.has_value());const auto before=*f.intent.bytes;f.wire.afterReclaim=[&]{f.identity=false;};const auto result=f.journal.reconcile(f.client,f.owner,*r,f.authority);require(result.attempted&&result.location==multitap::vdisplay::LiveLeaseLocation::unknown&&f.intent.bytes==before&&f.wire.reclaims==1);f.identity=true;f.wire.afterReclaim={};require(f.journal.reconcile(f.client,f.owner,*r,f.authority).location==multitap::vdisplay::LiveLeaseLocation::replacement&&f.wire.reclaims==1);}
  {Fixture f;const auto r=f.prepare();require(r.has_value());f.wire.afterReclaim=[&]{f.wire.queryFailure=true;};require(f.journal.reconcile(f.client,f.owner,*r,f.authority).location==multitap::vdisplay::LiveLeaseLocation::unknown);f.wire.queryFailure=false;f.wire.afterReclaim={};require(f.journal.reconcile(f.client,f.owner,*r,f.authority).location==multitap::vdisplay::LiveLeaseLocation::replacement&&f.wire.reclaims==1);}
  {Fixture f;const auto r=f.prepare();require(r.has_value());auto worker=cleanup;worker.created++;require(!f.journal.prepare(std::string(64,'c'),newLease,executor,worker,f.authority)&&f.intent.bytes==nativeStreamReclaimBytes(*r));}
}
void commitCases(){
  {Fixture f;const auto r=f.prepare();require(r.has_value());require(!f.journal.commit(f.client,*r,f.authority)&&!f.identities.saves&&!f.runtimes.saves);}
  for(int scenario=0;scenario<7;++scenario){Fixture f;const auto r=f.prepare();require(r.has_value());const auto profileBefore=f.profiles.bytes,launchBefore=f.launches.bytes;require(f.journal.reconcile(f.client,f.owner,*r,f.authority).location==multitap::vdisplay::LiveLeaseLocation::replacement);
    switch(scenario){case 1:f.identities.lostAck=true;break;case 2:f.runtimes.lostAck=true;break;case 3:f.identities.fail=true;break;case 4:f.runtimes.fail=true;break;case 5:f.identities.afterSave=[&]{f.wire.queryFailure=true;};break;case 6:f.runtimes.afterSave=[&]{f.identity=false;};break;}
    const auto first=f.journal.commit(f.client,*r,f.authority);if(scenario<=2)require(first.has_value());else require(!first);
    if(scenario==3)require(f.identities.bytes==r->identityBefore&&!f.runtimes.saves);
    if(scenario==4||scenario==5)require(unsealNativeDisplayIdentity(*f.identities.bytes).value.lease==newLease&&f.runtimes.bytes==r->runtimeBefore);
    f.identities.fail=f.runtimes.fail=f.identities.lostAck=f.runtimes.lostAck=f.wire.queryFailure=false;f.identity=true;f.identities.afterSave=f.runtimes.afterSave={};
    // Reopen from durable files. No in-memory success flag is needed.
    NativeStreamReclaimJournal resumed(f.intent.access(),f.launches.access(),f.profiles.access(),f.runtimes.access(),f.identities.access());const auto intent=parseNativeStreamReclaim(*f.intent.bytes);const auto committed=resumed.commit(f.client,intent,f.authority);
    require(committed&&committed->generation==8&&committed->intentSha256==nativeBaselineDigest(*f.intent.bytes)&&committed->identitySha256==nativeBaselineDigest(*f.identities.bytes)&&committed->runtimeSha256==nativeBaselineDigest(*f.runtimes.bytes));
    const auto identity=unsealNativeDisplayIdentity(*f.identities.bytes),before=unsealNativeDisplayIdentity(r->identityBefore);require(identity.value.lease==newLease&&sameNativeDisplayOwner(identity.value,before.value));const auto runtime=parseNativeRuntimeRecord(*f.runtimes.bytes);require(runtime.phase=="recovery"&&runtime.generation==8&&runtime.outputs==f.runtime.outputs&&f.profiles.bytes==profileBefore&&f.launches.bytes==launchBefore);
    const auto writes=f.identities.saves+f.runtimes.saves;require(resumed.commit(f.client,intent,f.authority).has_value()&&writes==f.identities.saves+f.runtimes.saves&&f.wire.reclaims==1&&f.wire.backend.arrives==2&&!f.wire.backend.departs);
    {multitap::vdisplay::Controller held(f.client,newLease,f.owner);require(resumed.adopt(held,intent,f.authority)&&held.heartbeat());}
  }
  for(int scenario=0;scenario<7;++scenario){Fixture f;const auto r=f.prepare();require(r.has_value());require(f.journal.reconcile(f.client,f.owner,*r,f.authority).location==multitap::vdisplay::LiveLeaseLocation::replacement);
    if(scenario==0)f.identities.bytes.reset();if(scenario==1)f.identities.bytes="bad DPAPI";
    if(scenario>=2&&scenario<=4){auto wrong=unsealNativeDisplayIdentity(*f.identities.bytes);if(scenario==2)wrong.value.owner.bytes[0]++;if(scenario==3)wrong.value.displays[1]++;if(scenario==4)wrong.value.lease=newLease+1;f.identities.bytes=sealNativeDisplayIdentity(wrong.value);}
    if(scenario==5)f.runtimes.bytes=nativeStreamReclaimRuntime(*r); // Runtime cannot lead the DPAPI rotation.
    if(scenario==6){auto same=unsealNativeDisplayIdentity(*f.identities.bytes);f.identities.bytes=sealNativeDisplayIdentity(same.value);} // Original ciphertext CAS, even for same values.
    const auto identity=f.identities.bytes,runtime=f.runtimes.bytes;require(!f.journal.commit(f.client,*r,f.authority)&&f.identities.bytes==identity&&f.runtimes.bytes==runtime&&!f.identities.saves&&!f.runtimes.saves);
  }
  {Fixture f;auto r=f.prepare();require(r.has_value());r->identityBefore.clear();r->runtimeBefore.clear();const auto legacy=nativeStreamReclaimBytes(*r);require(parseNativeStreamReclaim(legacy).identityBefore.empty());f.intent.bytes=legacy;require(!f.journal.current(*r,f.authority)&&!f.journal.commit(f.client,*r,f.authority)&&!f.identities.saves&&!f.runtimes.saves);}
}
void restorationProof(){
  for(int scenario=0;scenario<25;++scenario){
    Fixture f;const auto intent=f.prepare();require(intent.has_value());
    require(f.journal.reconcile(f.client,f.owner,*intent,f.authority).location==multitap::vdisplay::LiveLeaseLocation::replacement);
    const auto committed=f.journal.commit(f.client,*intent,f.authority);require(committed.has_value());auto receipt=*committed;
    require(nativeStreamReclaimCommitBytes(parseNativeStreamReclaimCommit(JsonParser::parse(nativeStreamReclaimCommitBytes(receipt))))==nativeStreamReclaimCommitBytes(receipt));
    auto evidence=f.proof;evidence.lease.servicePid=executor.pid;evidence.lease.generation=receipt.generation;
    auto process=f.authority.process;auto actualService=executor,actualWorker=cleanup;
    switch(scenario){
      case 1:receipt.intentSha256=std::string(64,'f');break;
      case 2:receipt.identitySha256=std::string(64,'f');break;
      case 3:receipt.runtimeSha256=std::string(64,'f');break;
      case 4:receipt.generation++;break;
      case 5:f.intent.bytes.reset();break;
      case 6:f.launches.bytes.reset();break;
      case 7:f.profiles.bytes.reset();break;
      case 8:f.runtimes.bytes.reset();break;
      case 9:f.identities.bytes.reset();break;
      case 10:actualService.created++;break;
      case 11:actualWorker.created++;break;
      case 12:process=[&](const auto& p){return p==f.launch.worker?NativeStreamProcessState::running:f.authority.process(p);};break;
      case 13:process=[&](const auto& p){return p==f.launch.service?NativeStreamProcessState::unknown:f.authority.process(p);};break;
      case 14:process=[&](const auto& p){return p==cleanup?NativeStreamProcessState::gone:f.authority.process(p);};break;
      case 15:process={};break;
      case 16:evidence.scope.tick=f.now+1;break;
      case 17:evidence.scope.tick=0;break;
      case 18:f.now=evidence.scope.tick+3001;break;
      case 19:evidence.scope.logon=std::string(64,'f');break;
      case 20:evidence.lease.targets[0].target++;break;
      case 21:evidence.console.device.instanceId.back()=L'X';break;
      case 22:f.profile.resizeSequence=1;f.profiles.bytes=nativeStreamProfileRecordBytes(f.profile);break;
      case 23:f.profile.profileSha256=std::string(64,'f');break;
      case 24:evidence.scope.foreign[0].monitorPath="new-external-monitor";break;
    }
    const auto allowed=nativeStreamReclaimRestorationProof(f.profile,evidence,receipt,f.intent.access(),f.launches.access(),f.profiles.access(),f.runtimes.access(),f.identities.access(),actualService,actualWorker,process,f.now);
    require(allowed==(scenario==0));
    require(f.wire.backend.arrives==2&&!f.wire.backend.departs&&f.wire.reclaims==1);
  }
  {
    Fixture f;const auto intent=f.prepare();require(intent.has_value());require(f.journal.reconcile(f.client,f.owner,*intent,f.authority).location==multitap::vdisplay::LiveLeaseLocation::replacement);const auto receipt=f.journal.commit(f.client,*intent,f.authority);require(receipt.has_value());
    auto evidence=f.proof;evidence.lease.servicePid=executor.pid;evidence.lease.generation=receipt->generation;
    f.profile.current=evidence.lease;f.profile.stopRequested=true;f.profile.phase=NativeStreamProfilePhase::restoring;++f.profile.revision;f.profiles.bytes=nativeStreamProfileRecordBytes(f.profile);
    const auto verify=[&]{return nativeStreamReclaimRestorationProof(f.profile,evidence,*receipt,f.intent.access(),f.launches.access(),f.profiles.access(),f.runtimes.access(),f.identities.access(),executor,cleanup,f.authority.process,f.now);};
    require(verify()); // A bound restoration may recheck proof after its CAS.
    evidence.topology=f.profile.profile.before;evidence.scope=f.profile.scope;evidence.scope.tick=1100;
    require(verify()); // Exact console/primary completion still has new lease.
    f.profile.phase=NativeStreamProfilePhase::restored;++f.profile.revision;f.profiles.bytes=nativeStreamProfileRecordBytes(f.profile);require(verify());
    f.profile.current=f.profile.entry;f.profiles.bytes=nativeStreamProfileRecordBytes(f.profile);require(!verify());
  }
}
void committedRestoreWorker(){
  for(int scenario=0;scenario<3;++scenario){
    Fixture f;const auto intent=f.prepare();require(intent.has_value());
    require(f.journal.reconcile(f.client,f.owner,*intent,f.authority).location==multitap::vdisplay::LiveLeaseLocation::replacement);
    multitap::vdisplay::Controller held(f.client,newLease,f.owner);require(f.journal.adopt(held,*intent,f.authority));
    const auto receipt=f.journal.commit(f.client,*intent,f.authority);require(receipt.has_value());
    auto current=f.proof.topology;int applies=0,captureStops=0,proofChecks=0;bool lastProof=false;
    NativeStreamProfileWorkerAccess access;access.eligible=[](bool){return true;};
    access.snapshot=[&](bool){auto lease=f.profile.current;lease.servicePid=executor.pid;lease.generation=receipt->generation;auto packet=JsonParser::parse(nativeStreamLeaseBytes(lease));auto& fields=std::get<Json::Object>(packet.value);fields["schema"].value=1.0;fields["provider"].value=std::string("libvirtualdisplay-1.6.4");fields["deviceInstanceId"].value=f.profile.key.instance;fields["leaseActive"].value=held.state().leaseActive;fields["activationVerified"].value=false;fields["configurationPending"].value=false;fields["recoveryRequired"].value=false;return packet;};
    access.topology.query=[&]{return current;};access.topology.eligible=[]{return true;};access.topology.now=[&]{return f.now;};access.topology.pause=[&]{f.now+=50;};
    access.topology.configure=[&](const NativeCcdData& requested,UINT32 flags)->LONG{
      if(flags&SDC_VALIDATE){if(scenario==1)f.runtimes.bytes="{}";return ERROR_SUCCESS;}
      ++applies;current=requested;return ERROR_SUCCESS;
    };
    access.scope=[&](const NativeTopologyBaseline& baseline){auto scope=f.profile.scope;scope.tick=f.now;scope.foreign.erase(std::remove_if(scope.foreign.begin(),scope.foreign.end(),[&](const auto& item){return std::none_of(baseline.ccd.paths.begin(),baseline.ccd.paths.end(),[&](const auto& path){return (path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativeSameLuid(path.sourceInfo.adapterId,item.sourceAdapter)&&nativeSameLuid(path.targetInfo.adapterId,item.targetAdapter)&&path.sourceInfo.id==item.source&&path.targetInfo.id==item.target;});}),scope.foreign.end());return scope;};
    access.machineUuid=[&]{return f.profile.console.proof.machineUuid;};access.resolver=[&](LUID luid)->std::optional<NativeConsoleAdapterIdentity>{if(nativeSameLuid(luid,f.profile.console.device.adapter))return f.profile.console.device;return {};};
    access.stopCapture=[&]{++captureStops;};access.launchProof=[](const auto&,bool){return false;};
    access.reclaimProof=[&](const auto& profile,const auto& proof,const Json& selected){++proofChecks;lastProof=nativeStreamReclaimRestorationProof(profile,proof,parseNativeStreamReclaimCommit(selected),f.intent.access(),f.launches.access(),f.profiles.access(),f.runtimes.access(),f.identities.access(),executor,cleanup,f.authority.process,f.now);return lastProof;};
    const auto command=[&]{return JsonParser::parse("{\"type\":\"worker.profile.recover\",\"sessionId\":"+jsonQuote(f.profile.key.stream)+",\"deviceInstanceId\":"+jsonQuote(f.profile.key.instance)+",\"generation\":"+std::to_string(receipt->generation)+",\"journalSha256\":"+jsonQuote(nativeBaselineDigest(*f.profiles.bytes))+",\"operation\":"+jsonQuote(f.profile.key.operation)+",\"workerExited\":true,\"reclaim\":"+nativeStreamReclaimCommitBytes(*receipt)+'}');};
    if(scenario==2)f.profiles.afterSave=[&]{if(parseNativeStreamProfileRecord(*f.profiles.bytes).phase==NativeStreamProfilePhase::restored)f.profiles.lostAck=true;};
    NativeStreamProfileWorker worker(executor.pid,2,f.profiles.access(),access);const auto restored=worker.recover(command());
    if(restored!=(scenario==0))throw std::runtime_error("combined restore scenario "+std::to_string(scenario)+": "+worker.reason()+", CCD applies="+std::to_string(applies)+", proofChecks="+std::to_string(proofChecks)+", lastProof="+std::to_string(lastProof)+", profileSaves="+std::to_string(f.profiles.saves));
    require(restored==(scenario==0)&&captureStops==1&&!worker.active()&&held.state().leaseActive&&held.heartbeat());
    if(scenario==1){require(applies==0&&parseNativeStreamProfileRecord(*f.profiles.bytes).phase!=NativeStreamProfilePhase::restored);continue;}
    const auto result=parseNativeStreamProfileRecord(*f.profiles.bytes);
    require(applies==1&&result.phase==NativeStreamProfilePhase::restored&&result.current.servicePid==executor.pid&&result.current.generation==receipt->generation&&nativeStreamProfileSignatures(current)==nativeStreamProfileSignatures(f.profile.profile.before));
    require(f.launches.bytes==nativeStreamLaunchBytes(f.launch)&&parseNativeRuntimeRecord(*f.runtimes.bytes).phase=="recovery"&&f.wire.backend.arrives==2&&!f.wire.backend.departs);
    if(scenario==2){f.profiles.afterSave={};f.profiles.lostAck=false;NativeStreamProfileWorker retry(executor.pid,2,f.profiles.access(),access);require(retry.recover(command())&&!retry.active()&&applies==1);}
  }
}
struct RetirementFixture:Fixture {
  Memory marker,baseline;std::map<std::pair<std::string,size_t>,std::string> archiveBytes;std::map<std::pair<std::string,std::string>,std::string> historyBytes;NativeStreamRetirementObservation observed;NativeStreamRetirementAuthority retirementAuthority;int failArchive=-1,failErase=-1;bool eligible=true;
  RetirementFixture(){
    observed.scope={3,std::string(64,'f'),1200,{}};observed.console=profile.console;observed.console.device.adapter={177,-2};observed.console.path={{177,-2},{177,-2},7,19};observed.ownedOutputActive=false;
    addPath(observed.topology,{177,-2},7,19,1600,900,0,0);
    observed.scope.foreign={{{177,-2},{177,-2},7,19,1,utf8(observed.console.device.interfacePath),utf8(observed.console.device.interfacePath),"new-console-monitor",true}};
    retirementAuthority.process=authority.process;retirementAuthority.display=[this]()->std::optional<NativeStreamRetirementObservation>{return observed;};retirementAuthority.identity=[this](const auto&){return eligible;};retirementAuthority.now=[this]{return now;};
    retirementAuthority.leasesAbsent=[this](auto leases,auto displays){return nativeStreamRetirementLeasesAbsent(client,leases,displays);};
  }
  void expire(){api::LeaseRequest request{};request.lease_id=oldLease;require(client.release_lease(request).ok());require(multitap::vdisplay::quiescentOwnedLease(client,oldLease));}
  NativeStreamRetirementIo io(){
    NativeStreamRetirementIo value;value.marker=marker.access();value.active={profiles.access(),launches.access(),runtimes.access(),identities.access(),intent.access(),baseline.access()};
    value.archive=[this](const std::string& operation,size_t index)->std::optional<std::string>{const auto found=archiveBytes.find({operation,index});return found==archiveBytes.end()?std::nullopt:std::optional(found->second);};
    value.saveArchive=[this](const std::string& operation,size_t index,const std::string& bytes){if(int(index)==failArchive)return false;const auto [found,added]=archiveBytes.emplace(std::pair{operation,index},bytes);return added||found->second==bytes;};
    value.erase=[this](size_t index,const std::string& expected){if(int(index)==failErase)return false;Memory* file=index==0?&profiles:index==1?&launches:index==4?&intent:index==5?&baseline:nullptr;if(!file||file->bytes!=expected)return false;file->bytes.reset();return true;};
    value.history=[this](const std::string& operation,const std::string& digest)->std::optional<std::string>{const auto found=historyBytes.find({operation,digest});return found==historyBytes.end()?std::nullopt:std::optional(found->second);};
    value.saveHistory=[this](const std::string& operation,const std::string& digest,const std::string& bytes){const auto [found,added]=historyBytes.emplace(std::pair{operation,digest},bytes);return added||found->second==bytes;};return value;
  }
  std::optional<NativeStreamRetirementRecord> prepareRetirement(NativeStreamRetirementJournal& retirement){return retirement.prepare(std::string(64,'d'),newLease+100,executor,cleanup,retirementAuthority);}
};
void expiredConsoleRetirement(){
  // scenario 3 is a field case at service startup, SAME logon: the record
  // left after viewportFailed (phase recovery, stale-ACK unconfirmed resize
  // reservation, no CCD checkpoint) with its latched runtime "recovery" at the
  // same generation; the old service/worker and SDK lease are gone and the
  // console is still suppressed. It must restore only that console and retire.
  for(int scenario=0;scenario<4;++scenario){
    RetirementFixture f;
    if(scenario==3){f.profile.phase=NativeStreamProfilePhase::recovery;f.profile.stopRequested=true;f.profile.revision=4;f.profile.resizeSequence=1;f.profile.resize=NativeStreamProfileResize{std::string(64,'c'),std::string(64,'d'),1};f.runtime.phase="recovery";f.persist();}
    f.expire();const auto savedProfile=f.profiles.bytes,savedLaunch=f.launches.bytes;Memory marker;std::map<std::string,std::string> history;NativeCcdData current=nativeForeignCcd(f.profile.profile.applied,f.profile.entry.targets);auto inactive=f.profile.profile.before.paths.front();inactive.flags=0;current.paths.push_back(inactive);int applies=0;bool revoked=false;
    const auto binding=[&](const DISPLAYCONFIG_PATH_INFO& p){const auto found=std::find_if(f.profile.scope.foreign.begin(),f.profile.scope.foreign.end(),[&](const auto& b){return nativeSameLuid(b.sourceAdapter,p.sourceInfo.adapterId)&&nativeSameLuid(b.targetAdapter,p.targetInfo.adapterId)&&b.source==p.sourceInfo.id&&b.target==p.targetInfo.id;});require(found!=f.profile.scope.foreign.end());return *found;};
    const auto scope=[&]{auto result=f.profile.scope;result.tick=f.now;result.foreign.clear();for(const auto& p:current.paths)if(p.flags&DISPLAYCONFIG_PATH_ACTIVE)result.foreign.push_back(binding(p));return result;};
    NativeStreamExpiredConsoleTransactionAccess access;access.marker=marker.access();access.profiles=f.profiles.access();access.launches=f.launches.access();
    access.history=[&](const std::string& sha){return NativeRuntimeJournalAccess{[&,sha]()->std::optional<std::string>{const auto found=history.find(sha);return found==history.end()?std::nullopt:std::optional(found->second);},[&,sha](const auto& expected,const auto& bytes){if(expected||history.contains(sha))return false;history[sha]=bytes;return true;}};};
    access.observe=[&]{return NativeStreamExpiredConsoleObservation{current,scope(),f.profile.console.proof.machineUuid};};access.identity.binding=binding;access.identity.adapter=[&](LUID luid)->std::optional<NativeConsoleAdapterIdentity>{if(nativeSameLuid(luid,f.profile.console.device.adapter))return f.profile.console.device;if(nativeSameLuid(luid,{88,-1}))return NativeConsoleAdapterIdentity{{88,-1},L"\\\\?\\PCI#VEN_8086&DEV_1234#0",L"PCI\\VEN_8086&DEV_1234\\0",L"PCI",{L"PCI\\VEN_8086&DEV_1234"},nativeConsoleDisplayClass,true,true};return {};};
    access.configure=[&](const auto& ccd,UINT32 flags){if(flags&SDC_VALIDATE){if(scenario==2)revoked=true;}else{++applies;current=ccd;}return LONG(ERROR_SUCCESS);};access.now=[&]{return f.now;};
    access.privateSources={{NativeStreamExpiredConsolePrivateKind::runtime,f.runtimes.access()},{NativeStreamExpiredConsolePrivateKind::identity,f.identities.access()}};access.resolveLedger=resolveNativeStreamExpiredLedger;
    access.authority=[&](const NativeStreamExpiredConsoleAuthorityRequest& request)->std::optional<NativeStreamExpiredConsoleAuthorityProof>{
      if(revoked)return {};const auto absent=nativeExpiredSdkAbsence(f.client,request.ledger,std::array<uint64_t,2>{71,72});if(!absent)return {};NativeStreamExpiredConsoleAuthorityProof proof;proof.operation=request.record.operation;proof.profileSha256=request.record.profileSha256;proof.launchSha256=request.record.launchSha256;proof.reservation=std::string(64,'e');proof.executor=request.executor;proof.session=f.profile.scope.session;proof.logon=f.profile.scope.logon;proof.tick=f.now;for(const auto& process:{f.launch.service,f.launch.worker,request.executor.service,request.executor.worker})proof.processes.push_back({process,f.authority.process(process)});proof.leases=absent->leases;proof.displays=absent->displays;return proof;
    };
    if(scenario==1)f.wire.queryFailure=true;const auto result=NativeStreamExpiredConsoleTransaction(std::move(access)).restore(std::string(64,'e'),{executor,cleanup});
    const bool expected=scenario==0||scenario==3;
    if(result.ok!=expected)throw std::runtime_error("expired console scenario "+std::to_string(scenario)+": "+result.reason);
    require(f.profiles.bytes==savedProfile&&f.launches.bytes==savedLaunch&&applies==(expected?1:0));if(!expected)continue;
    // Fresh CCD: exactly the saved console path is active again, the owned
    // outputs stay absent and the unrelated display keeps its exact geometry.
    require(nativeExpiredCcdDigest(nativeActiveCcd(current))==nativeExpiredCcdDigest(nativeForeignCcd(f.profile.profile.before,f.profile.entry.targets)));
    f.observed={scope(),f.profile.console,current,false};require(nativeStreamRetirementObservationValid(f.profile,f.observed,f.now,true)&&!nativeStreamRetirementObservationValid(f.profile,f.observed,f.now));
    f.retirementAuthority.expiredConsole=[&](const auto& profile,const auto& observed){return nativeExpiredRetirementCompletion(profile,observed,*savedLaunch,*marker.bytes);};NativeStreamRetirementJournal retirement(f.io());const auto prepared=f.prepareRetirement(retirement);require(prepared&&retirement.commit(*prepared,f.retirementAuthority)&&!f.profiles.bytes&&!f.launches.bytes);
    const auto identity=unsealNativeDisplayIdentity(*f.identities.bytes);multitap::vdisplay::Controller recreated(f.client,identity.value.lease,identity.value.owner);const std::vector<multitap::vdisplay::Output> fresh{{identity.value.displays[0],1600,900}};require(recreated.start(fresh)&&recreated.state().leaseActive&&recreated.state().targets.size()==1&&recreated.state().targets[0].display_id==identity.value.displays[0]);
    require(f.wire.reclaims==0&&f.wire.backend.arrives==3&&f.wire.backend.departs==2);
    if(scenario==3){const auto runtime=parseNativeRuntimeRecord(*f.runtimes.bytes);require(runtime.phase=="stopped"&&runtime.generation==8&&runtime.baselineHash.empty());}
  }
}
void retirement(){
  {RetirementFixture f;NativeStreamExpiredConsoleLedger ledger;ledger.current=f.profile.current;ledger.leases={{f.launch.lease,f.launch.outputs}};require(!nativeExpiredSdkAbsence(f.client,ledger));f.expire();const auto absent=nativeExpiredSdkAbsence(f.client,ledger,std::array<uint64_t,2>{71,72});require(absent&&absent->leases.size()==1&&absent->displays.size()==2);f.wire.queryFailure=true;require(!nativeExpiredSdkAbsence(f.client,ledger));}
  for(int scenario=0;scenario<5;++scenario){
    RetirementFixture f;f.expire();f.observed.scope.session=f.profile.scope.session;f.observed.scope.logon=f.profile.scope.logon;
    if(scenario){f.retirementAuthority.expiredConsole=[&](const auto& profile,const auto& observed)->std::optional<NativeStreamRetirementExpiredConsoleProof>{return NativeStreamRetirementExpiredConsoleProof{std::string(64,'e'),scenario==2?std::string(64,'f'):nativeBaselineDigest(nativeStreamProfileRecordBytes(profile)),scenario==3?std::string(64,'f'):nativeBaselineDigest(nativeStreamLaunchBytes(f.launch)),scenario==4?std::string(64,'f'):nativeRetirementGeometryDigest(observed)};};}
    NativeStreamRetirementJournal journal(f.io());const auto prepared=f.prepareRetirement(journal);require(prepared.has_value()==(scenario==1));
    if(prepared){require(prepared->expiredConsoleSha256==std::string(64,'e')&&parseNativeStreamRetirement(nativeStreamRetirementBytes(*prepared)).expiredConsoleSha256==prepared->expiredConsoleSha256);require(journal.commit(*prepared,f.retirementAuthority)&&parseNativeStreamRetirement(*f.marker.bytes).phase=="retired");}
  }
  {
    RetirementFixture f;f.expire();const std::array<std::optional<std::string>,4> originals={f.profiles.bytes,f.launches.bytes,f.runtimes.bytes,f.identities.bytes};
    NativeStreamRetirementJournal journal(f.io());const auto prepared=f.prepareRetirement(journal);require(prepared.has_value());const auto encoded=nativeStreamRetirementBytes(*prepared);require(nativeStreamRetirementBytes(parseNativeStreamRetirement(encoded))==encoded&&prepared->phase=="prepared");
    require(journal.commit(*prepared,f.retirementAuthority));require(parseNativeStreamRetirement(*f.marker.bytes).phase=="retired"&&!f.profiles.bytes&&!f.launches.bytes&&!f.intent.bytes&&!f.baseline.bytes);
    require(unsealNativeDisplayIdentity(*f.identities.bytes).value.lease==newLease+100&&parseNativeRuntimeRecord(*f.runtimes.bytes).phase=="stopped"&&parseNativeRuntimeRecord(*f.runtimes.bytes).generation==8);
    for(size_t i=0;i<originals.size();++i)require(f.archiveBytes.at({prepared->operation,i})==*originals[i]);
    require(f.wire.backend.arrives==2&&f.wire.backend.departs==2&&f.wire.reclaims==0); // Retirement itself never applies CCD or creates a display.
    require(!journal.commit(*prepared,f.retirementAuthority)); // A retired intent cannot be rerun as a prepared one.
  }
  {
    // Field case after a reboot: a profile journal quarantined by a failed viewport
    // (phase recovery, unconfirmed reservation, no CCD checkpoint) and a latched
    // runtime "recovery" record. A new logon retires it; nothing is replayed.
    RetirementFixture f;f.profile.phase=NativeStreamProfilePhase::recovery;f.profile.stopRequested=true;f.profile.revision=4;f.profile.resizeSequence=1;f.profile.resize=NativeStreamProfileResize{std::string(64,'c'),std::string(64,'d'),1};f.runtime.phase="recovery";f.persist();f.expire();
    NativeStreamRetirementJournal journal(f.io());const auto prepared=f.prepareRetirement(journal);require(prepared.has_value()&&journal.commit(*prepared,f.retirementAuthority));
    const auto runtime=parseNativeRuntimeRecord(*f.runtimes.bytes);
    require(runtime.phase=="stopped"&&runtime.generation==8&&runtime.baselineHash.empty()&&!f.profiles.bytes&&!f.launches.bytes&&f.wire.backend.arrives==2&&f.wire.reclaims==0);
  }
  for(int scenario=0;scenario<15;++scenario){
    RetirementFixture f;if(scenario!=0)f.expire();const auto profile=f.profiles.bytes,identity=f.identities.bytes,runtime=f.runtimes.bytes;
    switch(scenario){
      case 1:f.observed.scope.logon=f.profile.scope.logon;break;
      case 2:f.observed.scope.tick=f.now+1;break;
      case 3:f.observed.scope.tick=0;break;
      case 4:f.now=f.observed.scope.tick+3001;break;
      case 5:f.observed.ownedOutputActive=true;break;
      case 6:f.observed.console.device.instanceId.back()=L'X';break;
      case 7:f.observed.topology.paths[0].flags=0;break;
      case 8:f.observed.topology.modes[0].sourceMode.position.x=50;break;
      case 9:f.retirementAuthority.process=[&](const auto& p){return p==f.launch.worker?NativeStreamProcessState::unknown:f.authority.process(p);};break;
      case 10:f.eligible=false;break;
      case 11:f.wire.queryFailure=true;break;
      case 12:f.failArchive=2;break;
      case 13:f.archiveBytes[{std::string(64,'d'),0}]="wrong snapshot";break;
      case 14:f.retirementAuthority.leasesAbsent={};break;
    }
    NativeStreamRetirementJournal journal(f.io());require(!f.prepareRetirement(journal)&&!f.marker.bytes&&f.profiles.bytes==profile&&f.identities.bytes==identity&&f.runtimes.bytes==runtime);
  }
  for(int scenario=0;scenario<6;++scenario){
    RetirementFixture f;f.expire();NativeStreamRetirementJournal journal(f.io());const auto prepared=f.prepareRetirement(journal);require(prepared.has_value());
    if(scenario==0)f.identities.fail=true;if(scenario==1)f.runtimes.fail=true;if(scenario==2)f.failErase=0;if(scenario==3)f.failErase=1;if(scenario==4)f.identities.afterSave=[&]{f.eligible=false;};if(scenario==5)f.marker.fail=true;
    require(!journal.commit(*prepared,f.retirementAuthority)&&parseNativeStreamRetirement(*f.marker.bytes).phase=="prepared");
    f.identities.fail=f.runtimes.fail=f.marker.fail=false;f.failErase=-1;f.identities.afterSave={};f.eligible=true;
    NativeStreamRetirementJournal reopened(f.io());require(reopened.commit(parseNativeStreamRetirement(*f.marker.bytes),f.retirementAuthority)&&parseNativeStreamRetirement(*f.marker.bytes).phase=="retired");
    require(f.archiveBytes.at({prepared->operation,0})==nativeStreamProfileRecordBytes(f.profile)&&f.wire.backend.arrives==2&&f.wire.backend.departs==2);
  }
  {
    RetirementFixture f;f.expire();f.marker.lostAck=true;NativeStreamRetirementJournal journal(f.io());require(!f.prepareRetirement(journal)&&f.marker.bytes);f.marker.lostAck=false;const auto same=f.prepareRetirement(journal);require(same&&f.marker.saves==1);f.identities.lostAck=f.runtimes.lostAck=f.marker.lostAck=true;require(journal.commit(*same,f.retirementAuthority)&&parseNativeStreamRetirement(*f.marker.bytes).phase=="retired");
  }
}
void retirementFiles(){
  RetirementFixture f;f.expire();const auto root=std::filesystem::current_path()/(L"native-retirement-qa-"+randomName());const auto privateDirectory=root/L"private";
  require(std::filesystem::create_directory(root)&&std::filesystem::create_directory(privateDirectory));
  const std::string operation(64,'d');const auto history=privateDirectory/L"native-stream-history"/wide(operation);
  struct Cleanup{std::filesystem::path root,privateDirectory,history;std::vector<std::filesystem::path> records;~Cleanup(){std::error_code error;for(const auto& path:records)std::filesystem::remove(path,error);for(const auto* name:nativeRetirementFiles){std::filesystem::remove(privateDirectory/name,error);std::filesystem::remove(history/name,error);}std::filesystem::remove(history,error);std::filesystem::remove(history.parent_path()/L"records",error);std::filesystem::remove(history.parent_path(),error);std::filesystem::remove(privateDirectory,error);std::filesystem::remove(root,error);}}cleanupFiles{root,privateDirectory,history,{}};
  const auto trusted=[&](const std::filesystem::path& path){const auto relative=path.lexically_relative(root);return path==root||(!relative.empty()&&*relative.begin()!=L"..");};
  auto io=nativeStreamRetirementFileAccess(privateDirectory,trusted);
  require(io.active[0].save({},*f.profiles.bytes)&&io.active[1].save({},*f.launches.bytes)&&io.active[2].save({},*f.runtimes.bytes)&&io.active[3].save({},*f.identities.bytes));
  require(!io.erase(0,"different bytes")&&io.active[0].load()==f.profiles.bytes&&!io.erase(2,*f.runtimes.bytes));
  int historyWrites=0;bool historySaved=false;const auto saveHistory=io.saveHistory;io.saveHistory=[&](const auto& op,const auto& digest,const auto& bytes){++historyWrites;historySaved=saveHistory(op,digest,bytes);return historySaved;};
  NativeStreamRetirementJournal journal(io);const auto prepared=f.prepareRetirement(journal);require(prepared.has_value());
  const auto committed=journal.commit(*prepared,f.retirementAuthority);if(!committed)throw std::runtime_error("File retirement incomplete: profile="+std::to_string(io.active[0].load().has_value())+", launch="+std::to_string(io.active[1].load().has_value())+", rotated="+std::to_string(unsealNativeDisplayIdentity(*io.active[3].load()).value.lease==prepared->replacement)+", stopped="+std::to_string(parseNativeRuntimeRecord(*io.active[2].load()).phase=="stopped")+", historyWrites="+std::to_string(historyWrites)+", historySaved="+std::to_string(historySaved));
  require(parseNativeStreamRetirement(*io.marker.load()).phase=="retired"&&!io.active[0].load()&&!io.active[1].load());
  const auto finalDigest=nativeBaselineDigest(*io.marker.load());cleanupFiles.records.push_back(history.parent_path()/L"records"/(wide(finalDigest)+L".record.json"));
  require(io.archive(operation,0)==f.profiles.bytes&&io.archive(operation,1)==f.launches.bytes&&io.archive(operation,2)==f.runtimes.bytes&&io.archive(operation,3)==f.identities.bytes&&io.history(operation,finalDigest)==io.marker.load());
  require(unsealNativeDisplayIdentity(*io.active[3].load()).value.lease==prepared->replacement&&parseNativeRuntimeRecord(*io.active[2].load()).phase=="stopped");
  rejected([&]{(void)nativeStreamRetirementFileAccess(root,trusted);});rejected([&]{(void)io.archive("../other",0);});rejected([&]{(void)io.archive(operation,7);});
}
void expiredArchiveFiles(){
  RetirementFixture f;f.expire();NativeStreamRetirementJournal retirement(f.io());const auto prepared=f.prepareRetirement(retirement);require(prepared.has_value());auto record=*prepared;
  NativeStreamExpiredConsoleRecord completed;completed.operation=std::string(64,'e');completed.profileSha256=record.pins[0];completed.launchSha256=record.pins[1];completed.contextSha256=completed.planSha256=completed.beforeSha256=completed.restoredSha256=completed.ledgerSha256=std::string(64,'f');completed.phase="completed";completed.executor={executor,cleanup};const auto bytes=nativeStreamExpiredConsoleRecordBytes(completed);record.expiredConsoleSha256=nativeBaselineDigest(bytes);
  const auto root=std::filesystem::temp_directory_path()/(L"MultitapExpiredArchive-"+randomName()),directory=root/L"private";require(std::filesystem::create_directory(root)&&std::filesystem::create_directory(directory));struct Cleanup{std::filesystem::path root;~Cleanup(){std::error_code error;std::filesystem::remove_all(root,error);}}cleanupFiles{root};
  const auto trusted=[&](const std::filesystem::path& path){const auto relative=path.lexically_relative(root);return path==root||(!relative.empty()&&*relative.begin()!=L"..");};const auto marker=nativeStreamExpiredConsoleFileAccess(directory/L"native-stream-expired-console.json",trusted),retired=nativeStreamRetirementMarkerAccess(directory/L"native-stream-retirement.json",trusted);
  require(marker.save({},bytes)&&retired.save({},nativeStreamRetirementBytes(record)));require(!nativeArchiveRetiredExpiredConsole(directory,trusted)&&marker.load()==std::optional(bytes));const auto before=nativeStreamRetirementBytes(record);record.phase="retired";require(retired.save(before,nativeStreamRetirementBytes(record))&&nativeArchiveRetiredExpiredConsole(directory,trusted)&&!marker.load());
  require(nativeStreamExpiredConsoleHistoryAccess(directory/L"native-stream-expired-history",record.expiredConsoleSha256,trusted).load()==std::optional(bytes)&&nativeArchiveRetiredExpiredConsole(directory,trusted));
    const auto previous=nativeStreamRetirementBytes(record);record.expiredConsoleSha256.clear();require(retired.save(previous,nativeStreamRetirementBytes(record))&&marker.save({},bytes)&&nativeArchiveRetiredExpiredConsole(directory,trusted)&&!marker.load());
}
void retirementHandoff(){
  const NativeStreamProcessIdentity replacementWorker{4325,500};
  {RetirementFixture f;f.expire();f.observed.scope.session=f.profile.scope.session;f.observed.scope.logon=f.profile.scope.logon;const auto launchHash=nativeBaselineDigest(*f.launches.bytes);f.retirementAuthority.expiredConsole=[&](const auto& profile,const auto& observed)->std::optional<NativeStreamRetirementExpiredConsoleProof>{return NativeStreamRetirementExpiredConsoleProof{std::string(64,'e'),nativeBaselineDigest(nativeStreamProfileRecordBytes(profile)),launchHash,nativeRetirementGeometryDigest(observed)};};NativeStreamRetirementJournal journal(f.io());const auto prepared=f.prepareRetirement(journal);require(prepared.has_value());f.runtimes.fail=true;require(!journal.commit(*prepared,f.retirementAuthority));f.runtimes.fail=false;f.observed.scope.logon=std::string(64,'f');auto authority=f.retirementAuthority;authority.expiredConsole={};authority.process=[&](const auto& p){if(p==cleanup)return NativeStreamProcessState::gone;if(p==replacementWorker)return NativeStreamProcessState::running;return f.authority.process(p);};const auto resumed=journal.handoff(executor,replacementWorker,authority);require(resumed&&resumed->expiredConsoleSha256==prepared->expiredConsoleSha256&&journal.commit(*resumed,authority)&&parseNativeStreamRetirement(*f.marker.bytes).phase=="retired");}
  for(int scenario=0;scenario<4;++scenario){
    RetirementFixture f;f.expire();f.observed.scope.session=f.profile.scope.session;f.observed.scope.logon=f.profile.scope.logon;const auto launchHash=nativeBaselineDigest(*f.launches.bytes);
    f.retirementAuthority.expiredConsole=[&](const auto& profile,const auto& observed)->std::optional<NativeStreamRetirementExpiredConsoleProof>{return NativeStreamRetirementExpiredConsoleProof{std::string(64,'e'),nativeBaselineDigest(nativeStreamProfileRecordBytes(profile)),launchHash,nativeRetirementGeometryDigest(observed)};};
    NativeStreamRetirementJournal journal(f.io());const auto prepared=f.prepareRetirement(journal);require(prepared&&prepared->expiredConsoleSha256==std::string(64,'e'));
    if(scenario==0)f.identities.fail=true;if(scenario==1)f.runtimes.fail=true;if(scenario==2)f.failErase=0;if(scenario==3)f.failErase=1;require(!journal.commit(*prepared,f.retirementAuthority));f.identities.fail=f.runtimes.fail=false;f.failErase=-1;
    auto authority=f.retirementAuthority;authority.process=[&](const auto& p){if(p==cleanup)return NativeStreamProcessState::gone;if(p==replacementWorker)return NativeStreamProcessState::running;return f.authority.process(p);};NativeStreamRetirementJournal resumed(f.io());const auto next=resumed.handoff(executor,replacementWorker,authority);require(next&&next->expiredConsoleSha256==prepared->expiredConsoleSha256&&resumed.commit(*next,authority)&&parseNativeStreamRetirement(*f.marker.bytes).phase=="retired"&&!f.profiles.bytes&&!f.launches.bytes);
  }
  for(int scenario=0;scenario<4;++scenario){
    RetirementFixture f;f.expire();NativeStreamRetirementJournal journal(f.io());const auto prepared=f.prepareRetirement(journal);require(prepared.has_value());
    if(scenario==0)f.identities.fail=true;if(scenario==1)f.runtimes.fail=true;if(scenario==2)f.failErase=0;if(scenario==3)f.failErase=1;
    require(!journal.commit(*prepared,f.retirementAuthority));const auto pending=f.marker.bytes;
    f.identities.fail=f.runtimes.fail=false;f.failErase=-1;auto authority=f.retirementAuthority;
    authority.process=[&](const auto& process){if(process==cleanup)return NativeStreamProcessState::gone;if(process==replacementWorker)return NativeStreamProcessState::running;return f.authority.process(process);};
    NativeStreamRetirementJournal restarted(f.io());const auto adopted=restarted.handoff(executor,replacementWorker,authority);
    require(adopted&&adopted->worker==replacementWorker&&adopted->previousSha256==nativeBaselineDigest(*pending)&&f.historyBytes.at({prepared->operation,adopted->previousSha256})==*pending);
    require(restarted.commit(*adopted,authority)&&parseNativeStreamRetirement(*f.marker.bytes).phase=="retired"&&f.archiveBytes.at({prepared->operation,0})==nativeStreamProfileRecordBytes(f.profile));
    require(f.wire.backend.arrives==2&&f.wire.backend.departs==2&&f.wire.reclaims==0);
  }
  for(int scenario=0;scenario<5;++scenario){
    RetirementFixture f;f.expire();NativeStreamRetirementJournal journal(f.io());const auto prepared=f.prepareRetirement(journal);require(prepared.has_value());const auto before=f.marker.bytes;auto authority=f.retirementAuthority;
    authority.process=[&](const auto& process){if(process==cleanup)return scenario==0?NativeStreamProcessState::running:scenario==1?NativeStreamProcessState::unknown:NativeStreamProcessState::gone;if(process==replacementWorker)return scenario==2?NativeStreamProcessState::gone:NativeStreamProcessState::running;return f.authority.process(process);};
    if(scenario==3)f.eligible=false;if(scenario==4)f.historyBytes[{prepared->operation,nativeBaselineDigest(*before)}]="changed previous executor";
    require(!journal.handoff(executor,replacementWorker,authority)&&f.marker.bytes==before&&f.profiles.bytes==nativeStreamProfileRecordBytes(f.profile));
  }
}
void retirementObserver(){
  RetirementFixture f;f.expire();const std::string nonce(64,'e');
  const auto resolver=[&](LUID adapter)->std::optional<NativeConsoleAdapterIdentity>{if(nativeSameLuid(adapter,f.observed.console.device.adapter))return f.observed.console.device;return {};};
  const auto observed=observeNativeStreamRetirement(f.profile,f.profile.console.proof.machineUuid,f.profile.key.instance,f.observed.topology,f.observed.scope,resolver);
  const auto encoded=nativeStreamRetirementObservationBytes(f.profile,observed,nonce);const auto decoded=parseNativeStreamRetirementObservation(JsonParser::parse(encoded),f.profile,nonce,f.now);
  require(decoded.scope.logon==observed.scope.logon&&nativeRetirementGeometryDigest(decoded)==nativeRetirementGeometryDigest(observed));
  for(int scenario=0;scenario<4;++scenario){auto packet=JsonParser::parse(encoded);auto& fields=std::get<Json::Object>(packet.value);if(scenario==0)fields["nonce"].value=std::string(64,'f');if(scenario==1)fields["ownedOutputActive"].value=true;if(scenario==2)fields["journalSha256"].value=std::string(64,'f');if(scenario==3)fields["extra"].value=false;rejected([&]{(void)parseNativeStreamRetirementObservation(packet,f.profile,nonce,f.now);});}
  NativeStreamProfileWorkerAccess access;access.eligible=[](bool){return true;};access.machineUuid=[&]{return f.profile.console.proof.machineUuid;};access.selectedInstance=[&]{return f.profile.key.instance;};access.topology.query=[&]{return f.observed.topology;};access.scope=[&](const auto&){return f.observed.scope;};access.resolver=resolver;access.restartObservationProof=[](const auto&){return true;};
  const auto command=JsonParser::parse("{\"type\":\"worker.profile.retire.observe\",\"sessionId\":"+jsonQuote(f.profile.key.stream)+",\"operation\":"+jsonQuote(f.profile.key.operation)+",\"deviceInstanceId\":"+jsonQuote(f.profile.key.instance)+",\"journalSha256\":"+jsonQuote(nativeBaselineDigest(*f.profiles.bytes))+",\"nonce\":"+jsonQuote(nonce)+'}');
  NativeStreamProfileWorker worker(executor.pid,f.observed.scope.session,f.profiles.access(),access);const auto before=f.profiles.bytes;const auto result=worker.observeRetirement(command);require(result&&f.profiles.bytes==before&&!worker.active()&&!f.profiles.saves);
  require(parseNativeStreamRetirementObservation(JsonParser::parse(*result),f.profile,nonce,f.now).scope.session==f.observed.scope.session&&!worker.observeRetirement(command));
  for(int scenario=0;scenario<5;++scenario){auto blocked=access;if(scenario==0)blocked.selectedInstance=[]{return "ROOT\\DISPLAY\\00b2";};if(scenario==1)blocked.restartObservationProof={};if(scenario==2)blocked.restartObservationProof=[](const auto&){return false;};if(scenario==3)blocked.eligible=[](bool){return false;};if(scenario==4)blocked.scope=[&](const auto&){auto old=f.observed.scope;old.logon=f.profile.scope.logon;return old;};NativeStreamProfileWorker failed(executor.pid,f.observed.scope.session,f.profiles.access(),std::move(blocked));require(!failed.observeRetirement(command)&&f.profiles.bytes==before&&!f.profiles.saves);}
}
}
int main(){try{codecAndPersistence();admission();interrupted();commitCases();restorationProof();committedRestoreWorker();expiredConsoleRetirement();retirement();retirementFiles();expiredArchiveFiles();retirementHandoff();retirementObserver();std::cout<<"Native reclaim transaction: "<<checks<<" checks passed; actual DPAPI, SDK dispatcher/controller/store and private-file flush/readback, injected process/CCD evidence; no installed driver or display changes.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
#else
int main(){std::cout<<"Native reclaim transaction unavailable in the SDK-less build; no driver operation.\n";return 0;}
#endif
