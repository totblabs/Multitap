// Actual SDK Controller/DisplayStore, profile journal, private RPC dispatcher,
// runtime and receipt producer. Windows CCD, executors and file ACLs are injected.
#define main upstreamNativeDisplayServiceChangeMain
#include "native_display_service_change_test.cpp"
#undef main
#include "native_stream_expired_ledger.hpp"
namespace {
using namespace host;
int cancellationChecks=0;
void check(bool ok,const char* reason){require(ok,reason);++cancellationChecks;}
struct CancelServiceFixture:WorkerFixture {
  NativeStreamProfileRecord profile;NativeStreamLaunchReceipt launch;
  std::unique_ptr<NativeStreamProfileJournal> profileJournal;std::optional<NativeStreamProfileAck> ack;
  NativeStreamCancelRequest cancellation;std::optional<std::string> profileBytes,receiptBytes;
  bool denyReceipt=false,loseReceiptAck=false,replaceRuntime=false,replaceProfile=false,replaceConfig=false,loseLeaseAfterSave=false,extraPacketField=false;
  int receiptWrites=0;bool group=false;uint64_t scopeTick=1000;
  CancelServiceFixture(bool single,bool tracks,bool previousResize=false):WorkerFixture(single),group(tracks){
    const auto snapshot=state();const auto targets=nativeDisplayTargets(snapshot);const auto consolePath=ccd.paths.front();
    profile.key={resize.sessionId,std::string(64,'b'),snapshot.get("deviceInstanceId").string(),17};profile.entry=profile.current={GetCurrentProcessId(),uint32_t(snapshot.get("generation").number()),targets};
    profile.console={{"00112233-4455-6677-8899-aabbccddeeff","std"},{{7,0},L"\\\\?\\PCI#VEN_1234&DEV_1111#0",L"PCI\\VEN_1234&DEV_1111\\3&0",L"PCI",{L"PCI\\VEN_1234&DEV_1111"},nativeConsoleDisplayClass,true,true},{consolePath.sourceInfo.adapterId,consolePath.targetInfo.adapterId,consolePath.sourceInfo.id,consolePath.targetInfo.id}};
    profile.profile=planNativeStreamPrimaryProfile(ccd,targets,NativeStreamConsolePath{consolePath.sourceInfo.adapterId,consolePath.targetInfo.adapterId,consolePath.sourceInfo.id,consolePath.targetInfo.id});profile.profileSha256=nativeBaselineDigest(nativeStreamProfilePayloadBytes(profile.profile));profile.scope=test::recoveryScope(captureNativeTopologyBaseline(profile.profile.before,targets,"0"));profile.scope.foreign[0].sourcePath=profile.scope.foreign[0].targetPath=utf8(profile.console.device.interfacePath);
    launch.key=profile.key;launch.entry=profile.entry;launch.scope=profile.scope;launch.scope.foreign.clear();launch.service=nativeStreamProcessIdentity(GetCurrentProcess());launch.worker={launch.service.pid+1,2};launch.lease=ownedLease;
    for(size_t i=0;i<targets.size();++i){const auto found=transport.store.find_temporary_display(71+i);check(found.has_value(),"Initial SDK private output absent");launch.outputs.push_back({71+i,found->connector_index});}profile.launchSha256=nativeBaselineDigest(nativeStreamLaunchBytes(launch));
    auto profiles=NativeRuntimeJournalAccess{[&]{return profileBytes;},[&](const auto& expected,const auto& desired){if(profileBytes!=expected)return false;profileBytes=desired;return true;}};
    auto receipts=NativeRuntimeJournalAccess{[&]{return receiptBytes;},[&](const auto& expected,const auto& desired){++receiptWrites;if(denyReceipt||receiptBytes!=expected)return false;receiptBytes=desired;
      if(replaceRuntime){auto changed=parseNativeRuntimeRecord(*journal);changed.generation++;journal=nativeRuntimeBytes(changed);}if(replaceProfile){auto changed=parseNativeStreamProfileRecord(*profileBytes);changed.revision++;profileBytes=nativeStreamProfileRecordBytes(changed);}if(replaceConfig)streamField(config,"deviceInstanceId",std::string("ROOT\\DISPLAY\\00a2"));if(loseLeaseAfterSave)transport.queryFault=6;return !loseReceiptAck;}};
    service->testProfileViewportStores(profiles,receipts);check(service->reserveStreamProfile(resize.sessionId,key()),"Service could not reserve authenticated stream");
    profileJournal=std::make_unique<NativeStreamProfileJournal>(profiles);profileJournal->load(profile.key.instance);
    auto beforeScope=profile.scope;const auto prepared=profileJournal->prepare(profile,{profile.key,profile.current,beforeScope,profile.console,profile.profile.before});check(prepared.has_value(),"Actual profile prepare failed");ccd=profile.profile.applied;
    ack=profileJournal->activate(*prepared,proof());check(ack.has_value(),"Actual profile activation failed");profile=*profileJournal->record();
    if(previousResize){
      ack=profileJournal->reserveResize(*ack,std::string(64,'c'),nativeBaselineDigest(nativeResizeFields(resize)));check(ack.has_value(),"Previous actual viewport reservation failed");profile=*profileJournal->record();
      const auto result=run();check(result.ok&&receiptBytes&&journal,"Previous actual SDK viewport failed");const auto receipt=parseNativeStreamViewportCommit(*receiptBytes);auto observed=proof();
      ack=profileJournal->commitResize(*ack,observed,[&](const auto& held,const auto& fresh){return nativeStreamViewportCommitProof(held,fresh,resize,receipt,*journal);});check(ack.has_value(),"Previous actual profile commit failed");profile=*profileJournal->record();
    }
    if(group){auto request=trackRequest();if(single){request.outputs.resize(1);request.epochs.resize(1);}request.requestId="cancel-group";request.resizeId="00000000-0000-0000-0000-000000000019";cancellation=request;}
    else{auto request=resize;request.requestId="cancel-single";request.resizeId="00000000-0000-0000-0000-000000000018";request.epoch=2;if(!single)request.monitor="1";cancellation=request;}
    ack=profileJournal->reserveResize(*ack,std::string(64,'e'),nativeBaselineDigest(nativeStreamCancelRequestFields(cancellation)));check(ack.has_value(),"Actual cancellation reservation failed");profile=*profileJournal->record();
  }
  NativeStreamProfileEvidence proof(uint64_t tick=0){
    scopeTick=std::max(scopeTick+1,tick);tick=scopeTick;
    const auto current=nativeStreamProfileLease(state(),GetCurrentProcessId());auto scope=profile.scope;scope.tick=tick;
    scope.foreign.erase(std::remove_if(scope.foreign.begin(),scope.foreign.end(),[&](const auto& item){return std::none_of(ccd.paths.begin(),ccd.paths.end(),[&](const auto& path){return(path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativeSameLuid(path.sourceInfo.adapterId,item.sourceAdapter)&&nativeSameLuid(path.targetInfo.adapterId,item.targetAdapter)&&path.sourceInfo.id==item.source&&path.targetInfo.id==item.target;});}),scope.foreign.end());
    return {profile.key,current,scope,profile.console,ccd};
  }
  std::string checkpoint(){const auto monitor=std::visit([](const auto& request)->std::string{if constexpr(std::is_same_v<std::decay_t<decltype(request)>,StreamResize>)return request.monitor;else return "0";},cancellation);const auto observed=proof();return nativeRecoveryBaselineBytes({profile.key.instance,profile.current.generation,captureNativeTopologyBaseline(nativeActiveCcd(observed.topology),profile.current.targets,monitor),observed.scope});}
  NativeWorkerViewportReply exchange(int faultCase=-1){
    auto scope=test::currentScope();StreamResizeBudget budget;std::optional<Json> response;const auto baseline=faultCase==19?std::string("{}"):checkpoint();auto lease=key();if(faultCase==7)lease+="-stale";if(faultCase==8)scope.logon=std::string(64,'f');
    const auto deadline=faultCase==11?streamMonotonicMillis()-1:faultCase==20?streamMonotonicMillis()+streamResizeTimeoutMs+10000:budget.deadline;
    scope.tick=++scopeTick;
    NativeWorkerRpcAccess io{[&](const std::string& bytes){auto packet=JsonParser::parse(bytes);if(extraPacketField)streamField(packet,"unapproved",1);
      NativeViewportDispatch result=std::visit([&](const auto& request){if constexpr(std::is_same_v<std::decay_t<decltype(request)>,StreamResize>){auto ctx=context(request);ctx.deadline=deadline;ctx.scope=scope;ctx.authorized=faultCase!=9;ctx.unlocked=faultCase!=10;ctx.stopped=faultCase==12;return dispatchNativeViewport(packet,ctx,service.get());}else{auto ctx=trackContext(request);ctx.deadline=deadline;ctx.scope=scope;ctx.authorized=faultCase!=9;ctx.unlocked=faultCase!=10;ctx.stopped=faultCase==12;return dispatchNativeTrackViewport(packet,ctx,service.get());}},cancellation);
      if(!result.handled||result.response.empty())return false;response=JsonParser::parse(result.response);return true;},[&](DWORD){auto value=response;response.reset();return value;},{}};
    return std::visit([&](const auto& request){if constexpr(std::is_same_v<std::decay_t<decltype(request)>,StreamResize>)return nativeWorkerViewportRpc(request,"worker.native.resize.unchanged",lease,faultCase!=13,budget,std::move(io),baseline);else return nativeWorkerTrackViewportRpc(request,"worker.native.tracks.resize.unchanged",lease,faultCase!=13,budget,std::move(io),baseline);},cancellation);
  }
  std::optional<NativeStreamExpiredConsoleLedger> ledger(){
    NativeDisplayIdentitySnapshot owner;std::memcpy(owner.value.magic,"TOTBVDD1",8);owner.value.lease=ownedLease;owner.value.displays[0]=71;owner.value.displays[1]=72;owner.value.owner.bytes[0]=42;for(size_t i=0;i<launch.outputs.size();++i)owner.value.displays[i]=launch.outputs[i].display;
    return resolveNativeStreamExpiredLedger(parseNativeStreamProfileRecord(*profileBytes),launch,{{NativeStreamExpiredConsolePrivateKind::runtime,*journal},{NativeStreamExpiredConsolePrivateKind::identity,sealNativeDisplayIdentity(owner.value)},{NativeStreamExpiredConsolePrivateKind::viewport,*receiptBytes}});
  }
};
void happyCases(){
  for(const bool single:{false,true})for(const bool group:{false,true})for(const bool previous:{false,true}){
    CancelServiceFixture f(single,group,previous);const auto runtime=f.journal,baseline=f.baselineBytes;const auto before=f.profileBytes;const int creates=f.transport.creates,removes=f.transport.removes,releases=f.transport.releases,feeds=f.transport.feeds,queries=f.transport.queries;
    if(!previous)streamField(f.config,"monitors",single?2:1);const auto result=f.exchange();check(result.received&&result.accepted&&!result.pending&&!result.recoveryRequired&&!result.driverChangeAttempted&&f.receiptBytes,"Private cancellation RPC failed");
    check(f.transport.creates==creates&&f.transport.removes==removes&&f.transport.releases==releases&&f.transport.feeds==feeds&&f.transport.queries>=queries+2&&f.journal==runtime&&f.baselineBytes==baseline&&f.profileBytes==before,"Cancellation changed SDK outputs/runtime/baseline/profile");
    const auto receipt=parseNativeStreamViewportCancel(*f.receiptBytes);check(nativeStreamViewportCancelProof(f.profile,f.proof(1002),f.cancellation,receipt,*f.journal),"Actual producer omitted request/runtime/private identity proof");
    auto pending=f.ledger();check(pending&&pending->kind==NativeStreamExpiredConsoleLedgerKind::pendingViewport,"Lost worker ACK cannot interpret durable cancellation");
    const auto writes=f.receiptWrites;check(f.exchange().accepted&&f.receiptWrites==writes,"Exact cancellation retry rewrote its durable receipt");
    const auto fresh=f.proof(1002);f.ack=f.profileJournal->cancelResize(*f.ack,fresh,[&](const auto& held,const auto& proof){return nativeStreamViewportCancelProof(held,proof,f.cancellation,receipt,*f.journal);});check(f.ack&&f.profileJournal->streamingAllowed(),"Actual journal rejected actual service cancellation");f.profile=*f.profileJournal->record();
    const auto completed=f.ledger();check(completed&&completed->kind==NativeStreamExpiredConsoleLedgerKind::committedViewport,"Completed actual cancellation cannot interpret expiry ledger");f.transport.checkForeign();
  }
}
void refusedCases(){
  for(const bool single:{false,true})for(const bool group:{false,true})for(int fault=0;fault<=20;++fault){
    CancelServiceFixture f(single,group);const auto baseline=f.baselineBytes;const int creates=f.transport.creates,removes=f.transport.removes,releases=f.transport.releases,feeds=f.transport.feeds;
    switch(fault){case 0:f.denyReceipt=true;break;case 1:f.loseReceiptAck=true;break;case 2:f.replaceRuntime=true;break;case 3:f.replaceProfile=true;break;case 4:f.replaceConfig=true;break;case 5:f.loseLeaseAfterSave=true;break;case 6:f.transport.queryFault=1;break;case 14:f.extraPacketField=true;break;case 15:{auto profile=f.profile;profile.resize->requestSha256=std::string(64,'f');f.profileBytes=nativeStreamProfileRecordBytes(profile);break;}case 16:f.journal.reset();break;case 17:f.transport.queryFault=7;break;case 18:streamField(f.config,"provider",std::string("unverified-provider"));break;}
    const auto result=f.exchange(fault);if(result.accepted||f.profileJournal->streamingAllowed())std::cerr<<"Cancellation refusal scenario single="<<single<<" group="<<group<<" fault="<<fault<<" accepted="<<result.accepted<<"\n";check(!result.accepted&&!f.profileJournal->streamingAllowed(),"Invalid/uncertain cancellation became active");
    check(f.transport.creates==creates&&f.transport.removes==removes&&f.transport.releases==releases&&f.transport.feeds==feeds&&f.baselineBytes==baseline,"Refused cancellation changed owned/foreign outputs or baseline");
    if(fault==1){check(f.receiptBytes.has_value(),"Lost ACK fixture did not save its receipt");const auto pending=f.ledger();check(pending&&pending->kind==NativeStreamExpiredConsoleLedgerKind::pendingViewport,"Lost durable-save ACK erased pending cleanup evidence");}
    f.transport.checkForeign();
  }
}
void pipeCases(){
  for(const bool single:{false,true})for(const bool group:{false,true}){
    CancelServiceFixture f(single,group);const auto baseline=f.checkpoint();auto scope=test::currentScope();scope.tick=f.scopeTick+1;const auto lease=f.key();
    const auto name=L"\\\\.\\pipe\\MultitapHost.QA.Cancellation."+randomName();WinHandle server(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr));check(server.value!=INVALID_HANDLE_VALUE,"Cancellation QA server pipe failed");WinHandle client(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));check(client.value!=INVALID_HANDLE_VALUE,"Cancellation QA client pipe failed");ConnectNamedPipe(server.value,nullptr);
    std::atomic<bool> stopped=false;std::mutex writing;std::exception_ptr failure;int interleaved=0;StreamResizeBudget budget;budget.deadline=streamMonotonicMillis()+5000;const std::atomic<int64_t> deadline{budget.deadline};
    std::jthread responder([&]{try{for(int i=0;i<2;++i){bool text=false;std::vector<uint8_t> bytes;require(readPipePacket(server.value,text,bytes,stopped,std::chrono::milliseconds(2000))&&text,"Cancellation pipe request missing");const auto packet=JsonParser::parse(std::string(bytes.begin(),bytes.end()));
      const auto response=std::visit([&](const auto& request){if constexpr(std::is_same_v<std::decay_t<decltype(request)>,StreamResize>){auto ctx=context(request);ctx.deadline=budget.deadline;ctx.scope=scope;return dispatchNativeViewport(packet,ctx,f.service.get());}else{auto ctx=trackContext(request);ctx.deadline=budget.deadline;ctx.scope=scope;return dispatchNativeTrackViewport(packet,ctx,f.service.get());}},f.cancellation);
      require(response.handled&&!response.response.empty(),"Cancellation dispatcher dropped pipe request");const std::string reset=R"({"type":"input.reset"})";require(pipePacket(server.value,true,{reset.begin(),reset.end()},stopped,writing)&&pipePacket(server.value,true,{response.response.begin(),response.response.end()},stopped,writing),"Cancellation pipe response missing");}}catch(...){failure=std::current_exception();stopped=true;}});
    NativeWorkerRpcAccess io{[&](const auto& bytes){return pipePacket(client.value,true,{bytes.begin(),bytes.end()},stopped,writing,&deadline);},[&](DWORD timeout)->std::optional<Json>{bool text=false;std::vector<uint8_t> bytes;if(!readPipePacket(client.value,text,bytes,stopped,std::chrono::milliseconds(timeout),&deadline)||!text)return {};return JsonParser::parse(std::string(bytes.begin(),bytes.end()));},[&](const Json& value){if(value.get("type").string()!="input.reset")return false;++interleaved;return true;}};
    bool accepted=true;for(int i=0;i<2;++i){const auto result=std::visit([&](const auto& request){if constexpr(std::is_same_v<std::decay_t<decltype(request)>,StreamResize>)return nativeWorkerViewportRpc(request,"worker.native.resize.unchanged",lease,true,budget,io,baseline);else return nativeWorkerTrackViewportRpc(request,"worker.native.tracks.resize.unchanged",lease,true,budget,io,baseline);},f.cancellation);accepted&=result.received&&result.accepted&&!result.pending&&!result.driverChangeAttempted;}
    responder.join();if(failure)std::rethrow_exception(failure);check(accepted&&interleaved==2&&f.receiptWrites==1,"Actual cancellation pipe lost correlation, input reset or idempotence");f.transport.checkForeign();
  }
}
}
int main(){try{happyCases();refusedCases();pipeCases();std::cout<<"{\"kind\":\"native-stream-cancellation-service\",\"checks\":"<<cancellationChecks<<",\"happyWorkflows\":8,\"refusalWorkflows\":84,\"actualSdkControllerAndDisplayStore\":true,\"actualPrivateRpcDispatcher\":true,\"actualPrivatePipeExchanges\":8,\"actualProfileJournal\":true,\"actualWindowsDpapi\":true,\"expiryLedgerInterpretation\":true,\"sdkAbsenceProof\":false,\"ccdExecutorsAndFileAcl\":\"injected\",\"windowsDisplayChanges\":0}\n";return 0;}catch(const std::exception& error){std::cerr<<"Cancellation service: "<<error.what()<<'\n';return 1;}}
