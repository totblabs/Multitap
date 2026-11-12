#pragma once
#include "native_stream_expired_console_transaction.hpp"
#include "native_stream_track_commit.hpp"
#include "native_display_identity.hpp"
#include "native_display_baseline_journal.hpp"

#ifdef MULTITAP_NATIVE_DISPLAY
namespace host {
// Resolves private SDK identities for *expiry-only* console recovery. This is
// not permission to adopt a display, replay a resize, or start capture. The
// transaction still proves that every resolved lease/display is absent.
// settledRuntime: only for the predecessor of a durable, never-forwarded
// reservation (see below); never accepted for a top-level record.
// rejection (optional, diagnostics only): receives the FIRST failed rule as a
// fixed code from native_recovery_trace.hpp's ledger vocabulary. It never
// changes a decision; the boolean result is identical with or without it.
inline std::optional<NativeStreamExpiredConsoleLedger> resolveNativeStreamExpiredLedgerImpl(
    const NativeStreamProfileRecord& profile,const NativeStreamLaunchReceipt& launch,
    const std::vector<NativeStreamExpiredConsolePrivateSnapshot>& snapshots,bool settledPredecessor,std::string* rejection=nullptr) noexcept;
inline std::optional<NativeStreamExpiredConsoleLedger> resolveNativeStreamExpiredLedger(
    const NativeStreamProfileRecord& profile,const NativeStreamLaunchReceipt& launch,
    const std::vector<NativeStreamExpiredConsolePrivateSnapshot>& snapshots) noexcept {
  return resolveNativeStreamExpiredLedgerImpl(profile,launch,snapshots,false);
}
inline std::optional<NativeStreamExpiredConsoleLedger> resolveNativeStreamExpiredLedgerTraced(
    const NativeStreamProfileRecord& profile,const NativeStreamLaunchReceipt& launch,
    const std::vector<NativeStreamExpiredConsolePrivateSnapshot>& snapshots,std::string& rejection) noexcept {
  rejection.clear();auto result=resolveNativeStreamExpiredLedgerImpl(profile,launch,snapshots,false,&rejection);
  if(result)rejection="ok";else if(rejection.empty())rejection="failed";return result;
}
inline std::optional<NativeStreamExpiredConsoleLedger> resolveNativeStreamExpiredLedgerImpl(
    const NativeStreamProfileRecord& profile,const NativeStreamLaunchReceipt& launch,
    const std::vector<NativeStreamExpiredConsolePrivateSnapshot>& snapshots,bool settledPredecessor,std::string* rejection) noexcept {
  const auto need=[rejection](bool condition,const char* code){if(!condition){if(rejection&&rejection->empty())*rejection=code;throw std::runtime_error("native_stream_journal_invalid");}};
  const auto parsed=[rejection](const char* code,auto&& body){try{return body();}catch(...){if(rejection&&rejection->empty())*rejection=code;throw;}};
  try {
    using Kind=NativeStreamExpiredConsolePrivateKind;
    using LedgerKind=NativeStreamExpiredConsoleLedgerKind;
    need(nativeStreamLaunchMatches(launch,profile),"launch_mismatch");need(profile.phase!=NativeStreamProfilePhase::restored,"profile_restored");need(snapshots.size()>=2&&snapshots.size()<=5,"snapshot_shape");
    std::set<int> kinds;
    const auto find=[&](Kind kind)->const std::string* {
      const auto item=std::find_if(snapshots.begin(),snapshots.end(),[&](const auto& value){return value.kind==kind;});
      return item==snapshots.end()?nullptr:&item->bytes;
    };
    for(const auto& value:snapshots)need(int(value.kind)>=0&&int(value.kind)<=4&&kinds.insert(int(value.kind)).second&&!value.bytes.empty(),"snapshot_shape");
    const auto runtimeBytes=find(Kind::runtime),identityBytes=find(Kind::identity);
    need(runtimeBytes!=nullptr,"runtime_missing");need(identityBytes!=nullptr,"identity_missing");
    const auto runtime=parsed("runtime_invalid",[&]{return parseNativeRuntimeRecord(*runtimeBytes);});
    const auto identity=parsed("identity_invalid",[&]{return unsealNativeDisplayIdentity(*identityBytes);});
    need(runtime.instance==profile.key.instance,"runtime_instance");
    // The current candidate does not adopt live leases after a service crash.
    // An unfinished historical reclaim needs its own proven ledger; never
    // discard it by falling back to the launch receipt.
    need(!find(Kind::reclaim),"reclaim_present");
    NativeStreamExpiredConsoleLedger result;
    result.profileSha256=nativeBaselineDigest(nativeStreamProfileRecordBytes(profile));
    result.launchSha256=nativeBaselineDigest(nativeStreamLaunchBytes(launch));
    result.current=profile.current;
    result.leases.push_back({launch.lease,launch.outputs});
    const auto identityMatches=[&](uint64_t lease,const std::vector<NativeStreamLaunchOutput>& outputs) {
      if(identity.value.lease!=lease||outputs.empty()||outputs.size()>2)return false;
      for(size_t i=0;i<outputs.size();++i)if(identity.value.displays[i]!=outputs[i].display||outputs[i].connector==UINT32_MAX)return false;
      return true;
    };
    const auto runtimeMatches=[&](const NativeStreamProfileLease& lease) {
      if(runtime.generation!=lease.generation||runtime.outputs.size()!=lease.targets.size())return false;
      for(size_t i=0;i<runtime.outputs.size();++i){const auto& a=runtime.outputs[i];const auto& b=lease.targets[i];if(a.id!=b.id||a.width!=b.width||a.height!=b.height)return false;}
      return true;
    };
    // A runtime left "changing"/"recovery"/"stopped" WITHOUT a CCD checkpoint at
    // exactly the reserved generation/outputs never started an SDK change after
    // that lease: every reconfigure first persists generation+1 and every
    // prepare first binds a baseline hash. This shape is left when a
    // resize fails before prepare and the driver latch/shutdown writes
    // "recovery". Its receipts are bound to the canonical stable bytes.
    const bool settled=runtime.baselineHash.empty()&&(runtime.phase=="changing"||runtime.phase=="recovery"||runtime.phase=="stopped");
    const auto runtimeDigest=settled&&settledPredecessor?nativeBaselineDigest(nativeRuntimeBytes({runtime.instance,"stable",runtime.generation,runtime.outputs,""})):nativeBaselineDigest(*runtimeBytes);
    if(settled&&!settledPredecessor) {
      // Same disposition as the stable pendingReservation branch below; the
      // caller still proves whole-lease/display absence and both original
      // processes gone, then restores only the verified console from fresh CCD.
      need(profile.resize&&profile.resizeSequence&&runtimeMatches(profile.current)&&identityMatches(launch.lease,launch.outputs),"settled_shape");
      auto previous=profile;previous.resize.reset();--previous.resizeSequence;
      auto predecessor=resolveNativeStreamExpiredLedgerImpl(previous,launch,snapshots,true,rejection);
      need(predecessor&&(predecessor->kind==LedgerKind::launch||predecessor->kind==LedgerKind::committedViewport)&&sameNativeStreamLease(predecessor->current,profile.current),"settled_predecessor");
      predecessor->kind=LedgerKind::pendingReservation;
      predecessor->profileSha256=nativeBaselineDigest(nativeStreamProfileRecordBytes(profile));
      predecessor->pendingOperation=profile.resize->operation;predecessor->pendingRequestSha256=profile.resize->requestSha256;
      (void)parsed("ledger_bytes",[&]{return nativeExpiredLedgerBytes(*predecessor,snapshots);});return predecessor;
    }
    if(!settledPredecessor&&(runtime.phase=="changing"||runtime.phase=="recovery")) {
      // A service can exit between SDK reconfigure and receipt publication.
      // Resolve only the predecessor and fixed private IDs, never an imagined
      // new CCD target map. Whole-lease absence is required by the caller.
      const auto baselineBytes=find(Kind::baseline);need(profile.resize&&baselineBytes&&nativeRecoveryHex(runtime.baselineHash)&&runtime.baselineHash==nativeBaselineDigest(*baselineBytes),"pending_baseline");
      const auto baseline=parsed("pending_baseline",[&]{return parseNativeRecoveryBaseline(*baselineBytes);});
      need(baseline.instance==profile.key.instance&&baseline.generation==profile.current.generation&&baseline.scope&&nativeRecoveryLogonMatches(profile.scope,*baseline.scope)&&runtime.generation>=profile.current.generation&&runtime.generation-profile.current.generation<=2,"pending_scope");
      auto predecessor=profile.current;predecessor.targets=baseline.topology.targets;
      need(sameNativeStreamLease(predecessor,profile.current)&&identityMatches(launch.lease,launch.outputs),"pending_identity");
      auto oldRuntime=runtime;oldRuntime.generation=profile.current.generation;
      for(size_t i=0;i<profile.current.targets.size();++i){need(i<oldRuntime.outputs.size(),"pending_outputs");const auto& a=oldRuntime.outputs[i];const auto& b=profile.current.targets[i];need(a.id==b.id&&a.width==b.width&&a.height==b.height,"pending_outputs");}
      need(oldRuntime.outputs.size()==profile.current.targets.size(),"pending_outputs");need(nativeStreamProfileEvidenceMatches(profile,{profile.key,profile.current,*baseline.scope,profile.console,baseline.topology.ccd},NativeStreamProfileView::active),"pending_evidence");
      if(const auto old=find(Kind::viewport))(void)parsed("viewport_invalid",[&]{return parseNativeStreamViewportReceipt(*old);});
      result.kind=LedgerKind::pendingNative;result.pendingOperation=profile.resize->operation;result.pendingRequestSha256=profile.resize->requestSha256;
      (void)parsed("ledger_bytes",[&]{return nativeExpiredLedgerBytes(result,snapshots);});return result;
    }
    need((settledPredecessor&&settled)||(runtime.phase=="stable"&&runtime.baselineHash.empty()),"runtime_not_stable");
    need(!settledPredecessor||!profile.resize,"settled_resize");
    if(profile.resize&&profile.resizeSequence&&runtimeMatches(profile.current)&&identityMatches(launch.lease,launch.outputs)){
      // A durable reservation precedes any SDK mutation. If the current
      // runtime/private slots still match the *previous proven completion*,
      // retain that predecessor solely for expired-console restoration. This
      // is a distinct disposition, never completion of the pending resize.
      // The caller still proves the entire SDK lease/display set absent and
      // both original processes gone before making any Windows change.
      auto previous=profile;previous.resize.reset();--previous.resizeSequence;
      if(auto predecessor=resolveNativeStreamExpiredLedger(previous,launch,snapshots);
          predecessor&&(predecessor->kind==LedgerKind::launch||predecessor->kind==LedgerKind::committedViewport)&&sameNativeStreamLease(predecessor->current,profile.current)){
        predecessor->kind=LedgerKind::pendingReservation;
        predecessor->profileSha256=nativeBaselineDigest(nativeStreamProfileRecordBytes(profile));
        predecessor->pendingOperation=profile.resize->operation;predecessor->pendingRequestSha256=profile.resize->requestSha256;
        (void)nativeExpiredLedgerBytes(*predecessor,snapshots);return predecessor;
      }
    }
    const bool unchanged=!profile.resize&&!profile.resizeSequence&&sameNativeStreamLease(profile.current,launch.entry);
    if(unchanged) {
      need(identityMatches(launch.lease,launch.outputs),"launch_identity");need(runtimeMatches(profile.current),"launch_runtime");
      result.kind=LedgerKind::launch;
      // A receipt from a completed older session is pinned by the coordinator,
      // but cannot change this session's exact launch identity.
      if(const auto old=find(Kind::viewport)) {
        std::visit([&](const auto& receipt){need(receipt.profileOperation!=profile.key.operation||nativeStreamReceiptSession(receipt)!=profile.key.stream,"launch_viewport_conflict");},parsed("viewport_invalid",[&]{return parseNativeStreamViewportReceipt(*old);}));
      }
    } else {
      const auto viewportBytes=find(Kind::viewport);need(viewportBytes!=nullptr,"viewport_missing");
      std::visit([&](const auto& receipt){
      if constexpr(std::is_same_v<std::decay_t<decltype(receipt)>,NativeStreamTrackCommit>){const auto baseline=find(Kind::baseline);need(baseline&&nativeStreamTrackCommitBaselineMatches(receipt,*baseline),"viewport_baseline");}
      const auto original=parsed("viewport_invalid",[&]{return parseNativeStreamProfileRecord(receipt.profileBeforeBytes);});
      const auto& before=nativeStreamReceiptBefore(receipt);const auto& after=nativeStreamReceiptAfter(receipt);
      need(nativeStreamLaunchMatches(launch,original)&&receipt.instance==profile.key.instance&&receipt.profileOperation==profile.key.operation&&nativeStreamReceiptSession(receipt)==profile.key.stream&&nativeRecoveryLogonMatches(profile.scope,receipt.scope)&&receipt.runtimeSha256==runtimeDigest&&runtimeMatches(after)&&identityMatches(receipt.lease,receipt.outputs),"viewport_source");
      // Native resize preserves the private owner and its display slots. A
      // different lease needs an explicit reclaim history, unsupported here.
      need(receipt.lease==launch.lease,"viewport_lease");
      for(size_t i=0;i<launch.outputs.size();++i)need(launch.outputs[i].display==receipt.outputs[i].display,"viewport_outputs");
      if(profile.resize) {
        need(profile.revision>=original.revision&&sameNativeStreamLease(profile.current,before),"viewport_pending_profile");
        auto normalized=profile;normalized.revision=original.revision;normalized.phase=original.phase;normalized.stopRequested=original.stopRequested;
        need(nativeStreamProfileRecordBytes(normalized)==receipt.profileBeforeBytes,"viewport_pending_profile");
        result.kind=LedgerKind::pendingViewport;result.current=after;
        result.pendingOperation=profile.resize->operation;result.pendingRequestSha256=profile.resize->requestSha256;
      } else {
        need(sameNativeStreamLease(profile.current,after)&&profile.resizeSequence==original.resizeSequence&&profile.revision>original.revision,"viewport_committed_profile");
        auto normalized=profile;normalized.current=before;normalized.resize=original.resize;normalized.revision=original.revision;normalized.phase=original.phase;normalized.stopRequested=original.stopRequested;
        need(nativeStreamProfileRecordBytes(normalized)==receipt.profileBeforeBytes,"viewport_committed_profile");
        result.kind=LedgerKind::committedViewport;
      }
      // Keep historical connector mappings if Windows reindexed this output.
      for(const auto& output:receipt.outputs) {
        auto& outputs=result.leases.front().outputs;
        const auto same=std::find_if(outputs.begin(),outputs.end(),[&](const auto& saved){return saved.display==output.display;});
        need(same==outputs.end()||same->connector==output.connector,"viewport_connector");
        if(same==outputs.end())outputs.push_back(output);
      }
      },parsed("viewport_invalid",[&]{return parseNativeStreamViewportReceipt(*viewportBytes);}));
    }
    (void)parsed("ledger_bytes",[&]{return nativeExpiredLedgerBytes(result,snapshots);});return result;
  } catch(...) {if(rejection&&rejection->empty())*rejection="failed";return {};}
}
} // namespace host
#endif
