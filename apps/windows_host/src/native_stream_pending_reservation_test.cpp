#define MULTITAP_NATIVE_STREAM_TRACK_LEDGER_FIXTURE_ONLY
#include "native_stream_track_ledger_test.cpp"
#undef MULTITAP_NATIVE_STREAM_TRACK_LEDGER_FIXTURE_ONLY

#ifdef MULTITAP_NATIVE_DISPLAY
namespace {
NativeStreamProfileEvidence reservationProof(const NativeStreamProfileRecord& record,const NativeStreamProfileLease& lease,const std::vector<NativeOutputPlacement>& positions={}){
  NativeCcdData active;LONG x=0;
  for(size_t i=0;i<lease.targets.size();++i){const auto& t=lease.targets[i];const auto left=positions.empty()?x:positions[i].left,top=positions.empty()?0:positions[i].top;add(active,t.adapter,UINT32(i),t.target,t.width,t.height,left,top);x+=LONG(t.width);}
  auto scope=record.scope;scope.foreign.clear();scope.tick+=100;return {record.key,lease,scope,record.console,active};
}
struct ReservationFixture:LedgerFixture {
  std::optional<std::string> bytes;
  NativeStreamProfileJournal journal;
  NativeStreamProfileAck ack;
  ReservationFixture(int count,int previous):LedgerFixture(count),journal({[&]{return bytes;},[&](const auto& expected,const auto& next){if(bytes!=expected)return false;bytes=next;return true;}}){
    auto entry=profile;entry.phase=NativeStreamProfilePhase::prepared;entry.revision=1;journal.load(entry.key.instance);auto before=entry.scope;before.tick+=50;
    const auto prepared=journal.prepare(entry,{entry.key,entry.current,before,entry.console,entry.profile.before});require(prepared.has_value(),"Pending fixture profile prepare failed");
    auto activated=journal.activate(*prepared,reservationProof(entry,entry.current));require(activated.has_value(),"Pending fixture profile activate failed");ack=*activated;profile=*journal.record();refresh();
    if(previous==1){
      resize(false);const auto reserved=journal.reserveResize(ack,profile.resize->operation,profile.resize->requestSha256);require(reserved&&nativeStreamProfileRecordBytes(*journal.record())==receipt.profileBeforeBytes,"Previous single reserve mismatch");
      auto committed=journal.commitResize(*reserved,reservationProof(profile,receipt.after),[&](const auto& held,const auto& proof){return nativeStreamViewportCommitProof(held,proof,receipt.request,receipt,nativeRuntimeBytes(runtime));});require(committed.has_value(),"Previous single commit failed");ack=*committed;profile=*journal.record();refresh();
    }else if(previous==2||previous==4){
      TrackLedgerFixture groupFixture(false,previous==4,count);const auto& group=groupFixture.group;
      const auto reserved=journal.reserveResize(ack,group.resizeOperation,nativeBaselineDigest(nativeTrackResizeFields(group.request)));require(reserved&&nativeStreamProfileRecordBytes(*journal.record())==group.profileBeforeBytes,"Previous group reserve mismatch");
      const auto baseline=groupFixture.snapshots.back().bytes;
      auto committed=journal.commitResize(*reserved,reservationProof(*journal.record(),group.after,group.placements),[&](const auto& held,const auto& proof){return nativeStreamTrackCommitProof(held,proof,group.request,group,nativeRuntimeBytes(groupFixture.runtime),baseline);});require(committed.has_value(),"Previous group commit failed");ack=*committed;profile=*journal.record();runtime=groupFixture.runtime;snapshots=groupFixture.snapshots;
    }else if(previous==3){
      StreamResize request{profile.key.stream,"previous-noop","00000000-0000-0000-0000-000000000251","0",1920,1080,60,1};
      const auto reserved=journal.reserveResize(ack,std::string(64,'c'),nativeBaselineDigest(nativeResizeFields(request)));require(reserved.has_value(),"Previous unchanged reserve failed");
      const auto pending=*journal.record();const auto proof=reservationProof(pending,pending.current);
      const auto baselineBytes=nativeRecoveryBaselineBytes({pending.key.instance,pending.current.generation,captureNativeTopologyBaseline(proof.topology,pending.current.targets,"0"),proof.scope});
      NativeStreamViewportCancel cancel;cancel.request=request;cancel.instance=pending.key.instance;cancel.profileOperation=pending.key.operation;cancel.resizeOperation=pending.resize->operation;cancel.profileBeforeBytes=nativeStreamProfileRecordBytes(pending);cancel.profileBeforeSha256=nativeBaselineDigest(cancel.profileBeforeBytes);cancel.baselineBytes=baselineBytes;cancel.baselineSha256=nativeBaselineDigest(baselineBytes);cancel.runtimeSha256=nativeBaselineDigest(nativeRuntimeBytes(runtime));cancel.current=pending.current;cancel.scope=proof.scope;cancel.lease=launch.lease;cancel.outputs=launch.outputs;
      auto cancelled=journal.cancelResize(*reserved,proof,[&](const auto& held,const auto& observed){return nativeStreamViewportCancelProof(held,observed,cancel.request,cancel,nativeRuntimeBytes(runtime));});require(cancelled.has_value(),"Previous unchanged cancellation failed");ack=*cancelled;profile=*journal.record();refresh();snapshots.push_back({PrivateKind::viewport,nativeStreamViewportCancelBytes(cancel)});
    }
    require(resolve().has_value(),"Previous complete state has no expiry ledger");
    const auto pending=journal.reserveResize(ack,std::string(64,'e'),std::string(64,'f'));require(pending.has_value()&&!journal.streamingAllowed(),"Actual pending journal did not suspend streaming");profile=*journal.record();ack=*pending;
  }
};
}
#endif

int main(){
#ifdef MULTITAP_NATIVE_DISPLAY
  try{int workflows=0;
    for(const int count:{1,2})for(int previous=0;previous<5;++previous){
      ReservationFixture f(count,previous);const auto pending=f.profile;const auto records=f.snapshots;
      auto ledger=f.resolve();require(ledger&&ledger->kind==NativeStreamExpiredConsoleLedgerKind::pendingReservation&&sameNativeStreamLease(ledger->current,pending.current)&&ledger->pendingOperation==pending.resize->operation&&ledger->pendingRequestSha256==pending.resize->requestSha256,"Reservation before SDK/receipt publication lost exact expiry-only predecessor authority");
      require(!f.journal.streamingAllowed()&&!f.journal.cancelResize(f.ack,reservationProof(pending,pending.current)),"Expiry-only predecessor proof authorized capture or completed a cancellation");
      f.profile.phase=NativeStreamProfilePhase::recovery;f.profile.stopRequested=true;++f.profile.revision;require(f.resolve().has_value(),"Recovery lifecycle lost pending predecessor");f.profile=pending;
      for(int fault=0;fault<10;++fault){f.profile=pending;f.snapshots=records;switch(fault){
        case 0:f.profile.current.targets.front().target++;break;
        case 1:f.profile.current.targets.front().width+=2;break;
        case 2:f.profile.resizeSequence++;f.profile.resize->sequence=f.profile.resizeSequence;break;
        case 3:f.profile.current.generation++;break;
        case 4:f.snapshots[0].bytes="{}";break;
        case 5:f.snapshots[1].bytes[0]^=1;break;
        case 6:f.snapshots.erase(f.snapshots.begin()+1);break;
        case 7:f.profile.key.operation=std::string(64,'0');break;
        case 8:f.profile.scope.logon=std::string(64,'0');break;
        case 9:if(previous)f.snapshots.pop_back();else f.profile.entry.targets.front().target++;break;
      }require(!f.resolve(),"Unbound pending reservation granted expiry-only console authority");}
      ++workflows;
    }
    std::cout<<"{\"kind\":\"pending-reservation-expiry-ledger\",\"workflows\":"<<workflows<<",\"checks\":"<<checks<<",\"actualProfileJournal\":true,\"actualMachineDpapi\":true,\"sdkOrWindowsDisplayChanges\":0,\"captureAuthorized\":false}\n";return 0;
  }catch(const std::exception& error){std::cerr<<"pending reservation: "<<error.what()<<'\n';return 1;}
#else
  return 2;
#endif
}
