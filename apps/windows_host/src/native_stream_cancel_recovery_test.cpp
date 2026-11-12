// Exercise the actual profile journal and expiry ledger. Native display,
// process and driver absence are deliberately NOT proved by this fixture.
#define MULTITAP_NATIVE_STREAM_LEDGER_FIXTURE_ONLY
#include "native_stream_expired_ledger_test.cpp"
#include "native_stream_viewport_cancel.hpp"
#ifdef MULTITAP_NATIVE_DISPLAY
namespace {
NativeStreamProfileEvidence currentProof(const NativeStreamProfileRecord& record,const NativeStreamProfileLease& lease){
  NativeCcdData active;LONG x=0;
  for(size_t i=0;i<lease.targets.size();++i){const auto& t=lease.targets[i];add(active,t.adapter,UINT32(i),t.target,t.width,t.height,x,0,false,false);x+=LONG(t.width);}
  auto scope=record.scope;scope.foreign.clear();scope.tick+=100;
  return {record.key,lease,scope,record.console,active};
}
void cancellationCases(){
  for(const int count:{1,2})for(const bool previousResize:{false,true})for(const bool group:{false,true}){
    LedgerFixture f(count);std::optional<std::string> disk;
    NativeStreamProfileJournal journal({[&]{return disk;},[&](const auto& expected,const auto& next){if(expected!=disk)return false;disk=next;return true;}});
    auto entry=f.profile;entry.phase=NativeStreamProfilePhase::prepared;entry.revision=1;
    journal.load(entry.key.instance);
    auto beforeScope=entry.scope;beforeScope.tick+=50;
    const auto prepared=journal.prepare(entry,{entry.key,entry.current,beforeScope,entry.console,entry.profile.before});
    require(prepared.has_value(),"Cancellation fixture did not durably prepare");
    auto ack=journal.activate(*prepared,currentProof(entry,entry.current));require(ack.has_value(),"Cancellation fixture did not activate");
    f.profile=*journal.record();f.refresh();require(f.resolve().has_value(),"Initial launch ledger unavailable before cancellation");
    if(previousResize){
      f.resize(false);
      const auto reserved=journal.reserveResize(*ack,f.profile.resize->operation,f.profile.resize->requestSha256);
      require(reserved&&nativeStreamProfileRecordBytes(*journal.record())==f.receipt.profileBeforeBytes,"Actual reserve diverged from completed resize fixture");
      const auto proof=currentProof(f.profile,f.receipt.after);
      ack=journal.commitResize(*reserved,proof,[&](const auto& held,const auto& observed){return nativeStreamViewportCommitProof(held,observed,f.receipt.request,f.receipt,nativeRuntimeBytes(f.runtime));});
      require(ack.has_value(),"Actual previous resize could not commit");f.profile=*journal.record();f.refresh();require(f.resolve().has_value(),"Previous resize ledger unavailable before cancellation");
    }
    const StreamResize single{f.profile.key.stream,"cancel-qa","00000000-0000-0000-0000-000000000151",count==2&&!group?"1":"0",1600,900,60,2};
    NativeTrackResize tracks;tracks.sessionId=single.sessionId;tracks.requestId=single.requestId;tracks.resizeId=single.resizeId;tracks.topologyEpoch=2;tracks.fps=60;
    for(int i=0;i<count;++i){tracks.outputs.push_back({std::to_string(i),i?1280u:1600u,i?720u:900u,i?1600:0,0});tracks.epochs.push_back(2);}
    const auto requestHash=nativeBaselineDigest(group?nativeTrackResizeFields(tracks):nativeResizeFields(single));
    const auto reserved=journal.reserveResize(*ack,std::string(64,'e'),requestHash);require(reserved.has_value(),"Actual cancellation reservation failed");
    const auto pending=*journal.record();const auto proof=currentProof(pending,pending.current);
    require(!journal.cancelResize(*reserved,proof)&&!journal.streamingAllowed(),"A cancellation without durable completion proof became active");
    f.profile=*journal.record();f.refresh();const auto predecessor=f.resolve();require(predecessor&&predecessor->kind==NativeStreamExpiredConsoleLedgerKind::pendingReservation&&!journal.streamingAllowed(),"Previous completion was lost or treated as completion of a different pending reservation");
    const NativeRecoveryBaseline baseline{pending.key.instance,pending.current.generation,captureNativeTopologyBaseline(proof.topology,pending.current.targets,group?"0":single.monitor),proof.scope};
    const auto baselineBytes=nativeRecoveryBaselineBytes(baseline);
    NativeStreamViewportCancel cancel;cancel.request=group?NativeStreamCancelRequest(tracks):NativeStreamCancelRequest(single);cancel.instance=pending.key.instance;cancel.profileOperation=pending.key.operation;cancel.resizeOperation=pending.resize->operation;cancel.profileBeforeBytes=nativeStreamProfileRecordBytes(pending);cancel.profileBeforeSha256=nativeBaselineDigest(cancel.profileBeforeBytes);cancel.baselineSha256=nativeBaselineDigest(baselineBytes);cancel.baselineBytes=baselineBytes;cancel.runtimeSha256=nativeBaselineDigest(nativeRuntimeBytes(f.runtime));cancel.current=pending.current;cancel.scope=proof.scope;cancel.lease=f.launch.lease;cancel.outputs=f.launch.outputs;
    const auto cancelBytes=nativeStreamViewportCancelBytes(cancel);const auto parsedCancel=parseNativeStreamViewportCancel(cancelBytes);
    const auto cancelled=journal.cancelResize(*reserved,proof,[&](const auto& held,const auto& observed){return nativeStreamViewportCancelProof(held,observed,cancel.request,parsedCancel,nativeRuntimeBytes(f.runtime));});require(cancelled&&journal.streamingAllowed(),"A verified cancellation could not release its actual reservation");f.profile=*journal.record();f.refresh();
    require(nativeStreamViewportCancelBytes(parsedCancel)==cancelBytes&&nativeStreamViewportCancelProof(pending,proof,cancel.request,parsedCancel,nativeRuntimeBytes(f.runtime))&&nativeStreamViewportCancelledProfileMatches(f.profile,parsedCancel),"Self-contained cancellation receipt lost request/checkpoint/profile proof");
    const auto previousSnapshots=f.snapshots;f.snapshots.erase(std::remove_if(f.snapshots.begin(),f.snapshots.end(),[](const auto& value){return value.kind==PrivateKind::viewport||value.kind==PrivateKind::baseline;}),f.snapshots.end());f.snapshots.push_back({PrivateKind::viewport,cancelBytes});
    require(std::holds_alternative<NativeStreamViewportCancel>(parseNativeStreamViewportReceipt(cancelBytes)),"Cancellation decoded as a different request type");
    auto ledger=f.resolve();require(ledger&&ledger->kind==NativeStreamExpiredConsoleLedgerKind::committedViewport,"Completed cancellation lost its expiry ledger");
    const auto committed=f.profile;f.profile=pending;ledger=f.resolve();require(ledger&&ledger->kind==NativeStreamExpiredConsoleLedgerKind::pendingViewport,"Durable cancellation with lost worker ACK lost pending recovery");f.profile=committed;
    f.profile.phase=NativeStreamProfilePhase::recovery;f.profile.stopRequested=true;f.profile.revision++;require(f.resolve().has_value(),"Cancellation ledger failed stop/recovery transition");f.profile=committed;
    f.profile.resizeSequence++;require(!f.resolve(),"Another cancellation reused a self-contained receipt");f.profile=committed;
    f.snapshots=previousSnapshots;
    for(int scenario=0;scenario<12;++scenario){auto value=cancel;switch(scenario){case 0:value.instance="ROOT\\DISPLAY\\00a2";break;case 1:value.profileOperation=std::string(64,'f');break;case 2:value.resizeOperation=std::string(64,'d');break;case 3:value.profileBeforeSha256=std::string(64,'0');break;case 4:value.baselineSha256=std::string(64,'0');break;case 5:value.baselineBytes.clear();break;case 6:value.current.generation++;break;case 7:value.outputs.pop_back();break;case 8:value.outputs[0].display=0;break;case 9:value.scope.logon=std::string(64,'0');break;case 10:value.scope.tick+=streamResizeTimeoutMs+1;break;case 11:value.lease=1;break;}rejects([&]{(void)nativeStreamViewportCancelBytes(value);},"Malformed cancellation receipt encoded");}
    for(int scenario=0;scenario<8;++scenario){auto bad=cancelBytes;const auto replace=[&](const std::string& old,const std::string& next){const auto offset=bad.find(old);require(offset!=std::string::npos,"Malformed codec fixture field missing");bad.replace(offset,old.size(),next);};
      switch(scenario){case 0:replace("\"schema\":1","\"schema\":2");break;case 1:replace("\"kind\":\"native-stream-viewport-cancel\"","\"kind\":\"native-stream-track-commit\"");break;case 2:replace("\"requestKind\":"+jsonQuote(group?"tracks":"single"),"\"requestKind\":"+jsonQuote(group?"single":"tracks"));break;case 3:replace("\"requestKind\":"+jsonQuote(group?"tracks":"single"),"\"requestKind\":true");break;case 4:bad.pop_back();bad+=",\"unknown\":true}";break;case 5:replace("\"request\":{","\"request\":{\"unknown\":true,");break;case 6:replace("\"baseline\":"+jsonQuote(cancel.baselineBytes)+',',"");break;case 7:replace("\"baseline\":"+jsonQuote(cancel.baselineBytes),"\"baseline\":17");break;}
      rejects([&]{(void)parseNativeStreamViewportCancel(bad);},"Noncanonical/cross-kind cancellation receipt decoded");}
    rejects([&]{(void)parseNativeStreamViewportCancel(cancelBytes+" ");},"Trailing cancellation bytes accepted");
    rejects([&]{(void)parseNativeStreamViewportCancel(std::string(131073,'x'));},"Oversized cancellation bytes accepted");
    auto wrongRequest=cancel.request;std::visit([](auto& request){request.requestId="different";},wrongRequest);require(!nativeStreamViewportCancelProof(pending,proof,wrongRequest,cancel,nativeRuntimeBytes(f.runtime)),"Cancellation authorized a different request");
    auto wrongProof=proof;wrongProof.lease.generation++;require(!nativeStreamViewportCancelProof(pending,wrongProof,cancel.request,cancel,nativeRuntimeBytes(f.runtime)),"Cancellation authorized a changed lease");
    wrongProof=proof;wrongProof.topology.modes[nativeSourceIndex(wrongProof.topology.paths.back())].sourceMode.position.y+=1;require(!nativeStreamViewportCancelProof(pending,wrongProof,cancel.request,cancel,nativeRuntimeBytes(f.runtime)),"Cancellation authorized changed CCD geometry");
    auto wrongRuntime=f.runtime;wrongRuntime.generation++;require(!nativeStreamViewportCancelProof(pending,proof,cancel.request,cancel,nativeRuntimeBytes(wrongRuntime)),"Cancellation ignored changed runtime bytes");
    auto recovery=pending;recovery.phase=NativeStreamProfilePhase::recovery;recovery.stopRequested=true;recovery.revision++;
    require(!nativeStreamViewportCancelSourceMatches(recovery,cancel)&&nativeStreamViewportCancelSourceMatches(recovery,cancel,true),"Recovery normalization was accepted outside explicit cleanup");
    if(count==2&&previousResize&&group){
      const auto root=std::filesystem::temp_directory_path()/(L"multitap-cancel-"+randomName());std::filesystem::create_directory(root);
      struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}}cleanup{root};
      const auto trusted=[&](const auto& path){return path==root||path.parent_path()==root;};const auto store=nativeStreamViewportCancelFileAccess(root/L"native-stream-viewport-commit.json",trusted);
      require(store.save({},cancelBytes)&&store.load()==std::optional(cancelBytes)&&!store.save({},cancelBytes),"Actual cancellation file CAS/readback failed");
      require(nativeStreamViewportCancelProof(pending,proof,cancel.request,parseNativeStreamViewportCancel(*store.load()),nativeRuntimeBytes(f.runtime)),"Cancellation depends on a separate global baseline file");
    }
    NativeStreamViewportCommit receipt;receipt.request=single;receipt.instance=pending.key.instance;receipt.profileOperation=pending.key.operation;receipt.resizeOperation=pending.resize->operation;receipt.profileBeforeBytes=nativeStreamProfileRecordBytes(pending);receipt.profileBeforeSha256=nativeBaselineDigest(receipt.profileBeforeBytes);receipt.baselineSha256=nativeBaselineDigest(baselineBytes);receipt.runtimeSha256=nativeBaselineDigest(nativeRuntimeBytes(f.runtime));receipt.rollback=true;receipt.before=receipt.after=pending.current;receipt.scope=proof.scope;receipt.lease=f.launch.lease;receipt.outputs=f.launch.outputs;
    std::string bytes;
    if(group){
      NativeStreamTrackCommit r;r.request=tracks;r.instance=receipt.instance;r.profileOperation=receipt.profileOperation;r.resizeOperation=receipt.resizeOperation;r.profileBeforeBytes=receipt.profileBeforeBytes;r.profileBeforeSha256=receipt.profileBeforeSha256;r.baselineSha256=receipt.baselineSha256;r.runtimeSha256=receipt.runtimeSha256;r.rollback=true;r.before=r.after=receipt.before;r.scope=receipt.scope;r.lease=receipt.lease;r.outputs=receipt.outputs;r.placements=baseline.topology.placements;
      require(nativeStreamTrackCommitProof(pending,proof,tracks,r,nativeRuntimeBytes(f.runtime),baselineBytes),"Typed unchanged group receipt failed existing proof");bytes=nativeStreamTrackCommitBytes(r);
    }else{
      require(nativeStreamViewportCommitProof(pending,proof,single,receipt,nativeRuntimeBytes(f.runtime)),"Typed unchanged single receipt failed existing proof");bytes=nativeStreamViewportCommitBytes(receipt);
    }
    f.snapshots.erase(std::remove_if(f.snapshots.begin(),f.snapshots.end(),[](const auto& value){return value.kind==PrivateKind::viewport||value.kind==PrivateKind::baseline;}),f.snapshots.end());
    f.snapshots.push_back({PrivateKind::viewport,bytes});f.snapshots.push_back({PrivateKind::baseline,baselineBytes});
    const auto resolved=f.resolve();require(resolved&&resolved->kind==NativeStreamExpiredConsoleLedgerKind::committedViewport&&sameNativeStreamLease(resolved->current,pending.current),"Exact nonmutating receipt did not close cancellation expiry gap");
    const auto valid=f.profile;
    f.profile.resizeSequence++;require(!f.resolve(),"Unrelated later cancellation reused receipt");f.profile=valid;
    f.profile.current.generation++;require(!f.resolve(),"Changed generation reused cancellation receipt");f.profile=valid;
    f.profile.key.stream="another-stream";require(!f.resolve(),"Different stream reused cancellation receipt");f.profile=valid;
    f.profile.phase=NativeStreamProfilePhase::recovery;f.profile.stopRequested=true;f.profile.revision++;
    require(f.resolve().has_value(),"Stop/recovery transition lost exact nonmutating receipt");
  }
}
}
int main(){try{cancellationCases();std::cout<<"{\"kind\":\"unchanged-profile-expiry-gap\",\"checks\":"<<checks<<",\"workflowVariants\":8,\"actualProfileJournal\":true,\"actualWindowsDpapi\":true,\"unreceiptedCancellationRefused\":true,\"typedReceiptModelAccepted\":true,\"selfContainedCancellationCodec\":true,\"actualTemporaryFileCas\":true,\"serviceProducerCovered\":false,\"expiryCancelDecoderAdmitted\":true,\"windowsDisplayChanges\":0}\n";return 0;}catch(const std::exception& error){std::cerr<<"Cancellation recovery fixture: "<<error.what()<<'\n';return 1;}}
#else
int main(){return 2;}
#endif
