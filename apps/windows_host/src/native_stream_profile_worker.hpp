#pragma once
#include "native_stream_launch.hpp"
#include "native_stream_retirement.hpp"
#include "native_stream_track_commit.hpp"
#include <condition_variable>

namespace host {
inline NativeConsoleModelProof nativeStreamProfileRequest(const Json& request,bool preparedTracks=false){
  const auto& value=request.get("consoleProfile");const auto* fields=std::get_if<Json::Object>(&value.value);
  if(request.get("type").string()!="session.start"||request.get("mode").string()=="rdp"||!fields||fields->size()!=4||
     value.get("protocol").string()!="console-profile-v1"||value.get("sessionId").string()!=request.get("sessionId").string()||
     !nativeStreamJournalId(request.get("sessionId").string())||value.get("model").string()!="std"||!nativeConsoleUuid(value.get("machineUuid").string()))
    throw std::runtime_error("native_stream_profile_request_invalid");
  const auto* object=std::get_if<Json::Object>(&request.value);const bool marker=object&&object->contains("streamProfileTracksProtocol");
  if(request.get("mediaProtocol").string()=="tracks-v1"){
    if(!preparedTracks||!marker||request.get("mode").string()!="stream"||request.get("streamProfileTracksProtocol").string()!="group-v1")throw std::runtime_error("native_stream_profile_tracks_unavailable");
  }else if(marker)throw std::runtime_error("native_stream_profile_tracks_unavailable");
  return {value.get("machineUuid").string(),value.get("model").string()};
}
struct NativeStreamProfileWorkerAccess {
  // Restore eligibility may allow the SAME held service in STOP_PENDING. It
  // must not relax the same lease/device/logon proof or perform new activation.
  std::function<bool(bool restoring)> eligible;
  std::function<Json(bool restoring)> snapshot;
  NativeTopologyAccess topology;
  std::function<NativeRecoveryScope(const NativeTopologyBaseline&)> scope;
  std::function<NativeRecoveryScope()> loginScope;
  std::function<std::string()> machineUuid;
  NativeConsoleIdentityResolver resolver;
  std::function<void()> stopCapture;
  std::function<bool(const NativeStreamProfileRecord&,bool recoveryWorker)> launchProof;
  // Restart recovery only. The command's commit digests must independently
  // match protected reclaim/identity/runtime/launch files, exact executors and
  // fresh same-logon topology. Ordinary entry never consults this callback.
  std::function<bool(const NativeStreamProfileRecord&,const NativeStreamProfileEvidence&,const Json&)> reclaimProof;
  std::function<bool(const NativeStreamProfileRecord&)> restartObservationProof;
  std::function<std::string()> selectedInstance;
  std::function<std::optional<NativeStreamProfileRecord>()> retirementRecord;
  std::function<std::optional<std::pair<NativeStreamViewportCommit,std::string>>()> viewportCommit;
  std::function<std::optional<NativeStreamTrackCommitEvidence>()> trackViewportCommit;
  std::function<std::optional<NativeStreamViewportCancelEvidence>(const NativeStreamProfileRecord&,const NativeStreamCancelRequest&,const NativeStreamProfileEvidence&)> unchangedViewport;
#ifdef MULTITAP_NATIVE_DISPLAY
  std::function<std::optional<NativeStreamRetirementExpiredConsoleProof>(const NativeStreamProfileRecord&,const NativeStreamRetirementObservation&)> expiredConsole;
#endif
};
inline std::string nativeStreamRestartObservationBytes(const NativeStreamProfileRecord& profile,const NativeStreamProfileEvidence& observed,const std::string& nonce){
  nativeStreamJournalRequire(nativeRecoveryHex(nonce)&&nativeStreamProfileEvidenceMatches(profile,observed,NativeStreamProfileView::restorable));
  const auto active=validateNativeStreamProfileCcd(observed.topology,observed.lease.targets);const auto& console=*profile.profile.suppressedConsole;
  nativeStreamJournalRequire(std::count_if(observed.topology.paths.begin(),observed.topology.paths.end(),[&](const auto& path){return nativeStreamConsolePathMatches(path,console)&&path.targetInfo.targetAvailable;})==1);
  std::string paths="[";for(const auto& path:active.paths){if(paths.size()>1)paths+=',';paths+='[';const auto row=nativeBaselinePathRow(active,path);for(size_t i=0;i<row.size();++i){if(i)paths+=',';paths+=jsonQuote(std::to_string(row[i]));}paths+=']';}
  const auto packet="{\"type\":\"worker.profile.observed\",\"nonce\":"+jsonQuote(nonce)+",\"journalSha256\":"+jsonQuote(nativeBaselineDigest(nativeStreamProfileRecordBytes(profile)))+",\"scope\":"+nativeRecoveryScopeBytes(observed.scope)+",\"console\":"+nativeStreamConsoleBindingBytes(observed.console)+",\"paths\":"+paths+"]}";
  nativeStreamJournalRequire(packet.size()<=65536);return packet;
}
inline NativeStreamProfileEvidence parseNativeStreamRestartObservation(const Json& packet,const NativeStreamProfileRecord& profile,const std::string& nonce){
  nativeStreamJournalFields(packet,6);nativeStreamJournalRequire(nativeRecoveryHex(nonce)&&packet.get("type").string()=="worker.profile.observed"&&nativeStreamJournalString(packet.get("nonce"))==nonce&&nativeStreamJournalString(packet.get("journalSha256"))==nativeBaselineDigest(nativeStreamProfileRecordBytes(profile)));
  auto topology=parseNativeStreamProfilePaths(packet.get("paths"));const auto& console=*profile.profile.suppressedConsole;
  if(std::none_of(topology.paths.begin(),topology.paths.end(),[&](const auto& path){return nativeStreamConsolePathMatches(path,console);})){
    // Inactive availability was proved by the authenticated observer. Restore
    // planners only consume this exact saved source/target key, never inactive
    // modes or caller-selected device IDs.
    DISPLAYCONFIG_PATH_INFO available{};available.sourceInfo.adapterId=console.sourceAdapter;available.sourceInfo.id=console.source;available.targetInfo.adapterId=console.targetAdapter;available.targetInfo.id=console.target;available.targetInfo.targetAvailable=TRUE;topology.paths.push_back(available);
  }
  NativeStreamProfileEvidence result{profile.key,profile.current,parseNativeRecoveryScope(packet.get("scope")),parseNativeStreamConsoleBinding(packet.get("console")),std::move(topology)};
  nativeStreamJournalRequire(nativeStreamProfileEvidenceMatches(profile,result,NativeStreamProfileView::restorable));return result;
}
inline NativeStreamProfileLease nativeStreamProfileLease(const Json& snapshot,DWORD expectedService,bool reclaimedCleanup=false){
  const auto pid=snapshot.get("servicePid").number(),generation=snapshot.get("generation").number();
  if(!expectedService||pid!=expectedService||!nativeDisplayInteger(generation,1,UINT32_MAX)||
     !snapshot.get("leaseActive").boolean()||snapshot.get("configurationPending").boolean()||snapshot.get("recoveryRequired").boolean()||
     (!reclaimedCleanup&&!snapshot.get("activationVerified").boolean())||!nativeDisplayInstanceId(snapshot.get("deviceInstanceId").string()))
    throw std::runtime_error("native_stream_profile_lease_changed");
  return {expectedService,uint32_t(generation),nativeDisplayTargets(snapshot)};
}
inline bool nativeStreamProfileWorkerCommandAllowed(bool cleanupOnly,const std::string& type){
  return !cleanupOnly||type=="worker.profile.recover";
}

// All display operations and persistence are injectable. Production supplies a
// protected per-machine journal; tests supply in-memory CAS and CCD fixtures.
// This class never starts capture: the caller may do so only after begin()==true
// and must run finish() even after a failed entry or a broken private pipe.
class NativeStreamProfileWorker {
 public:
  NativeStreamProfileWorker(DWORD servicePid,DWORD session,NativeRuntimeJournalAccess store,NativeStreamProfileWorkerAccess access)
      :servicePid_(servicePid),session_(session),journal_(std::move(store)),access_(std::move(access)){}
  std::optional<std::string> observeRestart(const Json& request){
    if(attempted_)return {};attempted_=true;
    try{
      nativeStreamJournalFields(request,6);const auto instance=nativeStreamJournalString(request.get("deviceInstanceId")),nonce=nativeStreamJournalString(request.get("nonce"));
      if(request.get("type").string()!="worker.profile.observe"||!nativeDisplayInstanceId(instance)||!nativeRecoveryHex(nonce)||!session_||session_==UINT32_MAX||!access_.eligible||!access_.eligible(true)||!access_.restartObservationProof||!access_.scope||!access_.machineUuid||!access_.resolver||!access_.topology.query)return {};
      const auto held=journal_.load(instance);if(!held||held->key.session!=session_||held->key.stream!=request.get("sessionId").string()||held->key.operation!=request.get("operation").string()||nativeBaselineDigest(nativeStreamProfileRecordBytes(*held))!=request.get("journalSha256").string()||held->phase==NativeStreamProfilePhase::restored||held->resize||held->resizeSequence||!access_.restartObservationProof(*held))return {};
      const auto uuid=nativeConsoleUuid(access_.machineUuid());if(!uuid||*uuid!=held->console.proof.machineUuid)return {};
      const auto device=access_.resolver(held->console.device.adapter);if(!device)return {};auto console=held->console;console.device=*device;if(!sameNativeConsoleBinding(console,held->console))return {};
      const auto topology=access_.topology.query(),active=validateNativeStreamProfileCcd(topology,held->current.targets);
      NativeStreamProfileEvidence observed{held->key,held->current,access_.scope(captureNativeTopologyBaseline(active,held->current.targets,"0")),console,topology};
      if(!access_.eligible(true)||!access_.restartObservationProof(*held))return {};
      return nativeStreamRestartObservationBytes(*held,observed,nonce);
    }catch(...){return {};}
  }
  std::optional<std::string> observeRetirement(const Json& request){
    if(attempted_)return {};attempted_=true;
#ifdef MULTITAP_NATIVE_DISPLAY
    try{
      const bool expired=!std::holds_alternative<std::nullptr_t>(request.get("expiredConsoleSha256").value);nativeStreamJournalFields(request,expired?7:6);if(expired&&!nativeRecoveryHex(request.get("expiredConsoleSha256").string()))return {};const auto instance=nativeStreamJournalString(request.get("deviceInstanceId")),nonce=nativeStreamJournalString(request.get("nonce"));
      if(request.get("type").string()!="worker.profile.retire.observe"||!nativeDisplayInstanceId(instance)||!nativeRecoveryHex(nonce)||!session_||session_==UINT32_MAX||!access_.eligible||!access_.eligible(true)||!access_.restartObservationProof||!access_.selectedInstance||!access_.scope||!access_.machineUuid||!access_.resolver||!access_.topology.query)return {};
      auto held=journal_.load(instance);if(!held&&access_.retirementRecord)held=access_.retirementRecord();if(!held||held->key.instance!=instance||held->key.stream!=request.get("sessionId").string()||held->key.operation!=request.get("operation").string()||nativeBaselineDigest(nativeStreamProfileRecordBytes(*held))!=request.get("journalSha256").string()||held->phase==NativeStreamProfilePhase::restored||!access_.restartObservationProof(*held))return {};
      NativeTopologyBaseline baseline;baseline.ccd=nativeActiveCcd(access_.topology.query());const auto scope=access_.scope(baseline);if(scope.session!=session_)return {};
      const auto observed=observeNativeStreamRetirement(*held,access_.machineUuid(),access_.selectedInstance(),baseline.ccd,scope,access_.resolver,expired);
      if(expired){const auto proof=access_.expiredConsole?access_.expiredConsole(*held,observed):std::nullopt;if(!proof||proof->recordSha256!=request.get("expiredConsoleSha256").string())return {};}
      if(!access_.eligible(true)||!access_.restartObservationProof(*held))return {};return nativeStreamRetirementObservationBytes(*held,observed,nonce,expired);
    }catch(...){return {};}
#else
    (void)request;return {};
#endif
  }
  bool begin(const Json& request,const std::string& operation,bool preparedTracks=false){
    if(attempted_)return false;attempted_=true;entrySystemError_=0;
    try{
      proof_=nativeStreamProfileRequest(request,preparedTracks);
      if(!nativeRecoveryHex(operation)||!nativeRecoveryHex(request.get("profileLaunchSha256").string())||!session_||session_==UINT32_MAX||!access_.eligible||!access_.snapshot||!access_.scope||!access_.machineUuid||!access_.resolver||!access_.stopCapture||!access_.launchProof||!access_.eligible(false))return fail("native_stream_profile_identity_changed");
      const auto snapshot=access_.snapshot(false);const auto lease=nativeStreamProfileLease(snapshot,servicePid_);
      const auto instance=snapshot.get("deviceInstanceId").string();const auto previous=journal_.load(instance);
      if(previous&&previous->phase!=NativeStreamProfilePhase::restored)return fail("native_stream_profile_recovery_required");
      clean_=true;
      const auto fresh=access_.topology.query();const auto console=identifyNativeStreamConsole(proof_,access_.machineUuid(),false,fresh.paths,lease.targets,access_.resolver);
      if(!console.binding)return fail("native_stream_profile_console_unverified");
      const auto& path=console.binding->path;
      record_.key={request.get("sessionId").string(),operation,instance,session_};record_.entry=record_.current=lease;record_.console=*console.binding;
      record_.profile=planNativeStreamPrimaryProfile(fresh,lease.targets,NativeStreamConsolePath{path.sourceAdapter,path.targetAdapter,path.source,path.target});
      record_.profileSha256=nativeBaselineDigest(nativeStreamProfilePayloadBytes(record_.profile));
      record_.scope=access_.scope(captureNativeTopologyBaseline(record_.profile.before,lease.targets,"0"));
      record_.launchSha256=request.get("profileLaunchSha256").string();
      validateNativeStreamProfileRecord(record_);
      if(!access_.launchProof(record_,false))return fail("native_stream_profile_launch_unverified");
      auto transaction=transactionAccess(false);
      transaction.durablePrepared=[&](const NativeCcdData& before,const NativeCcdData&){clean_=false;ack_=journal_.prepare(record_,evidence(before,false));return ack_.has_value();};
      transaction.durableComplete=[&](const NativeCcdData& observed){if(!ack_)return false;ack_=journal_.activate(*ack_,evidence(observed,false));return ack_.has_value();};
      const auto result=enterNativeStreamPrimaryProfile(record_.profile,std::move(transaction));
      entrySystemError_=result.systemError;
      if(!result.ok)return fail(result.reason.c_str());if(!journal_.streamingAllowed())return fail("native_stream_profile_entry_failed");
      active_=true;reason_.clear();return true;
    }catch(const NativeTopologyWindowsError& error){entrySystemError_=error.code;return fail("native_stream_profile_query_failed");}
    catch(...){return fail("native_stream_profile_entry_failed");}
  }
  bool finish(){
    active_=false;
    try{access_.stopCapture();}catch(...){return fail("native_stream_profile_capture_stop_failed");}
    if(clean_)return true;
    if(!ack_||!journal_.record())return fail("native_stream_profile_recovery_required");
    std::string failure="native_stream_profile_recovery_required";
    try{
      ack_=journal_.requestStop(*ack_);if(!ack_)return fail("native_stream_profile_recovery_required");
      // A reservation that may have reached the driver keeps the receipt/exit-
      // fenced recovery path; the ordinary restore below would refuse it anyway.
      const bool reserved=record_.resize.has_value();
      if(!reserved||unforwardedResize_){
        auto transaction=transactionAccess(true);
        transaction.durablePrepared=[&](const NativeCcdData& before,const NativeCcdData&){if(!ack_)return false;const auto proof=evidence(before,true);ack_=reserved?journal_.beginUnchangedRestore(*ack_,proof):journal_.beginRestore(*ack_,proof);return ack_.has_value();};
        transaction.durableComplete=[&](const NativeCcdData& observed){if(!ack_)return false;ack_=journal_.restored(*ack_,evidence(observed,true));return ack_.has_value();};
        const auto result=restoreNativeStreamPrimaryProfile(record_.profile,record_.current.targets,std::move(transaction));
        if(result.ok&&ack_){clean_=true;reason_.clear();return true;}
        failure=result.reason;
      }
    }catch(...){ }
    if(const auto current=journal_.ack())(void)journal_.recovery(*current);
    return fail(failure.c_str());
  }
  bool reserveViewport(const StreamResize& request){
    try{
      if(!active_||!ack_||!nativeResizeValid(request)||request.sessionId!=record_.key.stream||record_.resize||!identityCurrent(false))return false;
      ack_=journal_.reserveResize(*ack_,nativeBaselineDigest(utf8(randomName())+request.resizeId),nativeBaselineDigest(nativeResizeFields(request)));
      active_=false;if(!ack_)return fail("native_stream_profile_recovery_required");const auto held=journal_.record();if(!held)return fail("native_stream_profile_recovery_required");record_=*held;return true;
    }catch(...){return fail("native_stream_profile_recovery_required");}
  }
  bool viewportConsoleCurrent(const NativeStreamCancelRequest& request,const NativeConsoleBinding& console)const{
    try{
      const auto session=std::visit([](const auto& value){return value.sessionId;},request);
      if(!ack_||!record_.resize||record_.phase!=NativeStreamProfilePhase::active||record_.stopRequested||session!=record_.key.stream||record_.resize->requestSha256!=nativeBaselineDigest(nativeStreamCancelRequestFields(request))||!sameNativeConsoleBinding(console,record_.console)||!journal_.verified(*ack_)||!access_.eligible||!access_.eligible(false)||!access_.launchProof||!access_.launchProof(record_,false)||!access_.loginScope||!nativeRecoveryLogonMatches(record_.scope,access_.loginScope()))return false;
      const auto uuid=nativeConsoleUuid(access_.machineUuid());const auto device=access_.resolver(record_.console.device.adapter);if(!uuid||*uuid!=record_.console.proof.machineUuid||!device)return false;
      auto fresh=record_.console;fresh.device=*device;return sameNativeConsoleBinding(fresh,record_.console)&&journal_.verified(*ack_)&&access_.eligible(false);
    }catch(...){return false;}
  }
  std::optional<NativeConsoleBinding> viewportConsole(const NativeStreamCancelRequest& request,const NativeTopologyBaseline& baseline){
    try{
      const auto monitor=std::visit([](const auto& value)->std::string{if constexpr(std::is_same_v<std::decay_t<decltype(value)>,StreamResize>)return value.monitor;else return "0";},request);
      if(!viewportConsoleCurrent(request,record_.console)||baseline.replacedMonitor!=monitor)return {};
      const auto proof=viewportEvidence(false);
      if(!sameNativeStreamLease(proof.lease,record_.current)||baseline.targets.size()!=record_.current.targets.size()||nativeStreamProfileSignatures(baseline.ccd)!=nativeStreamProfileSignatures(nativeActiveCcd(proof.topology)))return {};
      const auto canonical=captureNativeTopologyBaseline(baseline.ccd,baseline.targets,monitor);if(canonical.placements.size()!=baseline.placements.size())return {};
      for(size_t i=0;i<canonical.placements.size();++i){const auto& a=canonical.placements[i];const auto& b=baseline.placements[i];if(a.id!=b.id||a.left!=b.left||a.top!=b.top||a.primary!=b.primary)return {};}
      for(size_t i=0;i<baseline.targets.size();++i){const auto& a=baseline.targets[i];const auto& b=record_.current.targets[i];if(a.id!=b.id||!nativeSameLuid(a.adapter,b.adapter)||a.target!=b.target||a.width!=b.width||a.height!=b.height)return {};}
      return record_.console;
    }catch(...){return {};}
  }
  // driverChangePossible=false is the native coordinator's own proof that this
  // exact reservation never reached a service mutation (failed before prepare,
  // or the service answered that it changed nothing). Only then may finish()
  // drop the reservation itself, and still only on the unchanged exact lease.
  bool completeViewport(const StreamResize& request,bool driverChangePossible=true){
    viewportStage_=1;unforwardedResize_=false;
    try{
      if(!ack_||!record_.resize||record_.resize->requestSha256!=nativeBaselineDigest(nativeResizeFields(request)))return fail("native_stream_profile_recovery_required");
      unforwardedResize_=!driverChangePossible;
      viewportStage_=2;
      const auto observed=viewportEvidence(false);
      if(sameNativeStreamLease(record_.current,observed.lease)){
        viewportStage_=3;
        if(!cancelUnchangedViewport(request,observed))return viewportFailed();
      }
      else{
        viewportStage_=4;
        const auto receipt=access_.viewportCommit?access_.viewportCommit():std::nullopt;
        viewportStage_=5;
        if(!receipt||!nativeStreamViewportCommitProof(record_,observed,request,receipt->first,receipt->second))return viewportFailed();
        viewportStage_=6;
        ack_=journal_.commitResize(*ack_,observed,[&](const auto& held,const auto& proof){return nativeStreamViewportCommitProof(held,proof,request,receipt->first,receipt->second);});
      }
      viewportStage_=7;
      if(!ack_)return viewportFailed();const auto held=journal_.record();if(!held)return viewportFailed();record_=*held;active_=journal_.streamingAllowed();if(active_){viewportStage_=0;unforwardedResize_=false;}return active_;
    }catch(...){return viewportFailed();}
  }
  bool reserveTracks(const NativeTrackResize& request){
    try{
      if(!active_||!ack_||!nativeTrackResizeValid(request)||request.topologyEpoch<2||request.sessionId!=record_.key.stream||record_.resize||request.outputs.size()!=record_.current.targets.size()||!identityCurrent(false))return false;
      const auto observed=evidence(access_.topology.query(),false);
      if(!nativeStreamProfileEvidenceMatches(record_,observed,NativeStreamProfileView::active))return false;
      const auto baseline=captureNativeTopologyBaseline(observed.topology,record_.current.targets,"0");(void)planNativeTrackViewport(baseline,request.outputs);
      ack_=journal_.reserveResize(*ack_,nativeBaselineDigest(utf8(randomName())+request.resizeId),nativeBaselineDigest(nativeTrackResizeFields(request)));active_=false;
      if(!ack_)return fail("native_stream_profile_recovery_required");const auto held=journal_.record();if(!held)return fail("native_stream_profile_recovery_required");record_=*held;trackBefore_=baseline;return true;
    }catch(...){return fail("native_stream_profile_recovery_required");}
  }
  bool completeTracks(const NativeTrackResize& request,bool applied){
    unforwardedResize_=false;
    try{
      if(!ack_||!trackBefore_||!record_.resize||record_.resize->requestSha256!=nativeBaselineDigest(nativeTrackResizeFields(request)))return viewportFailed();
      const auto observed=viewportEvidence(false);
      const auto receipt=access_.trackViewportCommit?access_.trackViewportCommit():std::nullopt;
      if(sameNativeStreamLease(record_.current,observed.lease)&&!applied){
        if(nativeStreamProfileSignatures(observed.topology)!=nativeStreamProfileSignatures(trackBefore_->ccd))return viewportFailed();
        if(!cancelUnchangedViewport(request,observed))return viewportFailed();
      }else{
        if(!receipt||receipt->receipt.rollback==applied||!nativeStreamTrackCommitProof(record_,observed,request,receipt->receipt,receipt->runtimeBytes,receipt->baselineBytes))return viewportFailed();
        if(sameNativeStreamLease(record_.current,observed.lease))ack_=journal_.cancelResize(*ack_,observed,[&](const auto& held,const auto& proof){return nativeStreamTrackCommitProof(held,proof,request,receipt->receipt,receipt->runtimeBytes,receipt->baselineBytes);});
        else ack_=journal_.commitResize(*ack_,observed,[&](const auto& held,const auto& proof){return nativeStreamTrackCommitProof(held,proof,request,receipt->receipt,receipt->runtimeBytes,receipt->baselineBytes);});
      }
      if(!ack_)return viewportFailed();const auto held=journal_.record();if(!held)return viewportFailed();record_=*held;trackBefore_.reset();active_=journal_.streamingAllowed();return active_;
    }catch(...){return viewportFailed();}
  }
  // Only the authenticated service may send this command, after observing the
  // prior owned process HANDLE signalled. It is cleanup of an existing exact
  // lifetime, never authority to resume capture or recreate a lost lease.
  bool recover(const Json& request){
    if(attempted_)return false;attempted_=true;active_=false;recoveryWorker_=true;
    try{
      const auto* fields=std::get_if<Json::Object>(&request.value);const auto digest=request.get("journalSha256").string(),operation=request.get("operation").string();
      reclaimedRecovery_=fields&&fields->contains("reclaim");
      if(!session_||session_==UINT32_MAX||!fields||fields->size()!=(reclaimedRecovery_?8:7)||request.get("type").string()!="worker.profile.recover"||!std::holds_alternative<bool>(request.get("workerExited").value)||!request.get("workerExited").boolean()||
         !nativeStreamJournalId(request.get("sessionId").string())||!nativeDisplayInstanceId(request.get("deviceInstanceId").string())||
         !nativeDisplayInteger(request.get("generation").number(),1,UINT32_MAX)||!std::holds_alternative<std::string>(request.get("journalSha256").value)||!std::holds_alternative<std::string>(request.get("operation").value)||
         (digest.empty()!=operation.empty())||(!digest.empty()&&(!nativeRecoveryHex(digest)||!nativeRecoveryHex(operation)))||!access_.eligible||!access_.snapshot||!access_.scope||!access_.machineUuid||!access_.resolver||!access_.stopCapture)
        return fail("native_stream_profile_recovery_request_invalid");
      if(reclaimedRecovery_){
        const auto& value=request.get("reclaim");nativeStreamJournalFields(value,4);
        for(const auto* field:{"intentSha256","identitySha256","runtimeSha256"})if(!nativeRecoveryHex(nativeStreamJournalString(value.get(field))))return fail("native_stream_profile_recovery_request_invalid");
        if(!access_.reclaimProof||nativeStreamJournalU32(value.get("generation"),2)!=request.get("generation").number())return fail("native_stream_profile_recovery_request_invalid");
        reclaim_=value;
      }
      access_.stopCapture();if(!access_.eligible(true))return fail("native_stream_profile_identity_changed");
      const auto snapshot=access_.snapshot(true);const auto lease=nativeStreamProfileLease(snapshot,servicePid_,reclaimedRecovery_);
      if(snapshot.get("deviceInstanceId").string()!=request.get("deviceInstanceId").string()||lease.generation!=request.get("generation").number())return fail("native_stream_profile_lease_changed");
      const auto previous=journal_.load(request.get("deviceInstanceId").string());
      // No durable prepare means the exited worker could not have applied a
      // profile. The service also checked the protected journal after exit.
      if(!previous){if(reclaimedRecovery_||!digest.empty()||!operation.empty())return fail("native_stream_profile_recovery_required");clean_=true;return true;}
      record_=*previous;proof_=record_.console.proof;ack_=journal_.ack();
      if(!ack_||ack_->sha256!=digest||record_.key.operation!=operation||record_.key.stream!=request.get("sessionId").string()||record_.key.session!=session_)return fail("native_stream_profile_recovery_required");
      if(!reclaimedRecovery_&&(record_.resize||!sameNativeStreamLease(record_.current,lease))){
        if(!record_.resize)return fail("native_stream_profile_recovery_required");
        const auto observed=viewportEvidence(true);const auto receipt=access_.viewportCommit?access_.viewportCommit():std::nullopt;const auto group=access_.trackViewportCommit?access_.trackViewportCommit():std::nullopt;
        const bool groupMatches=group&&nativeStreamTrackCommitProof(record_,observed,group->receipt.request,group->receipt,group->runtimeBytes,group->baselineBytes,true);
        const bool singleMatches=receipt&&nativeStreamViewportCommitProof(record_,observed,receipt->first.request,receipt->first,receipt->second,true);
        if(!sameNativeStreamLease(record_.current,lease)&&(groupMatches==singleMatches))return fail("native_stream_profile_recovery_required");
        viewportRecovery_=true;if(groupMatches)trackRecoveryCommit_=group;else viewportRecoveryCommit_=receipt;record_.current=lease;
      }
      if(!identityCurrent(true))return fail("native_stream_profile_recovery_required");
      if(record_.phase==NativeStreamProfilePhase::restored){
        if(!nativeStreamProfileEvidenceMatches(record_,evidence(access_.topology.query(),true),NativeStreamProfileView::restored))return fail("native_stream_profile_recovery_required");
        clean_=true;reason_.clear();return true;
      }
      auto transaction=transactionAccess(true);
      transaction.durablePrepared=[&](const NativeCcdData& before,const NativeCcdData&){
        if(!ack_)return false;
        const auto reconciled=[&](const NativeStreamProfileRecord& held,const NativeStreamProfileEvidence& observed){
          if(reclaimedRecovery_)return access_.reclaimProof&&access_.reclaimProof(held,observed,reclaim_);
          if(!viewportRecovery_||!held.resize)return false;
          if(sameNativeStreamLease(held.current,observed.lease))return nativeStreamProfileEvidenceMatches(held,observed,NativeStreamProfileView::restorable);
          if(trackRecoveryCommit_)return nativeStreamTrackCommitProof(held,observed,trackRecoveryCommit_->receipt.request,trackRecoveryCommit_->receipt,trackRecoveryCommit_->runtimeBytes,trackRecoveryCommit_->baselineBytes,true);
          return viewportRecoveryCommit_&&nativeStreamViewportCommitProof(held,observed,viewportRecoveryCommit_->first.request,viewportRecoveryCommit_->first,viewportRecoveryCommit_->second,true);
        };
        ack_=journal_.beginRecoveryRestore(*ack_,evidence(before,true),[&](const NativeStreamProfileRecord& held){return held.key==record_.key&&request.get("workerExited").boolean();},(reclaimedRecovery_||viewportRecovery_)?NativeStreamProfileJournal::TransitionProof(reconciled):NativeStreamProfileJournal::TransitionProof{});
        if(ack_){const auto held=journal_.record();if(!held)return false;record_=*held;}return ack_.has_value();
      };
      transaction.durableComplete=[&](const NativeCcdData& observed){if(!ack_)return false;ack_=journal_.restored(*ack_,evidence(observed,true));return ack_.has_value();};
      const auto result=restoreNativeStreamPrimaryProfile(record_.profile,record_.current.targets,std::move(transaction));
      if(result.ok&&ack_){clean_=true;reason_.clear();return true;}
      if(const auto current=journal_.ack())(void)journal_.recovery(*current);return fail("native_stream_profile_recovery_required");
    }catch(...){return fail("native_stream_profile_recovery_required");}
  }
  bool active()const{return active_;}
  bool clean()const{return clean_;}
  const std::string& reason()const{return reason_;}
  DWORD entrySystemError()const{return entrySystemError_;}
  uint32_t viewportStage()const{return viewportStage_;}
 private:
  bool fail(const char* reason){reason_=reason;active_=false;return false;}
  // Keep the fresh durable ACK: discarding it left finish() with a stale
  // revision, so stream stop could never restore the suppressed console.
  bool viewportFailed(){
    if(const auto current=journal_.ack()){ack_=journal_.recovery(*current);if(ack_)if(const auto held=journal_.record())record_=*held;}
    return fail("native_stream_profile_recovery_required");
  }
  bool cancelUnchangedViewport(const NativeStreamCancelRequest& request,const NativeStreamProfileEvidence& observed){
    if(!ack_||!access_.unchangedViewport||!access_.eligible(false))return false;
    const auto receipt=access_.unchangedViewport(record_,request,observed);if(!receipt)return false;
    const auto fresh=viewportEvidence(false);
    if(!nativeStreamViewportCancelProof(record_,fresh,request,receipt->receipt,receipt->runtimeBytes))return false;
    ack_=journal_.cancelResize(*ack_,fresh,[&](const auto& held,const auto& proof){return nativeStreamViewportCancelProof(held,proof,request,receipt->receipt,receipt->runtimeBytes);});return ack_.has_value();
  }
  NativeStreamProfileEvidence viewportEvidence(bool restoring){
    if(!access_.eligible(restoring)||!access_.launchProof||!access_.launchProof(record_,recoveryWorker_))throw std::runtime_error("native_stream_profile_identity_changed");
    const auto snapshot=access_.snapshot(restoring);const auto lease=nativeStreamProfileLease(snapshot,servicePid_);if(snapshot.get("deviceInstanceId").string()!=record_.key.instance)throw std::runtime_error("native_stream_profile_identity_changed");
    const auto uuid=nativeConsoleUuid(access_.machineUuid());const auto device=access_.resolver(record_.console.device.adapter);if(!uuid||*uuid!=record_.console.proof.machineUuid||!device)throw std::runtime_error("native_stream_profile_identity_changed");auto console=record_.console;console.device=*device;if(!sameNativeConsoleBinding(console,record_.console))throw std::runtime_error("native_stream_profile_identity_changed");
    const auto topology=access_.topology.query();const auto active=validateNativeStreamProfileCcd(topology,lease.targets);NativeStreamProfileEvidence proof{record_.key,lease,access_.scope(captureNativeTopologyBaseline(active,lease.targets,"0")),console,topology};
    if(!nativeStreamProfileEvidenceMatches(record_,proof,NativeStreamProfileView::active,true)||!access_.eligible(restoring)||!sameNativeStreamLease(lease,nativeStreamProfileLease(access_.snapshot(restoring),servicePid_)))throw std::runtime_error("native_stream_profile_identity_changed");return proof;
  }
  bool identityCurrent(bool restoring){
    try{
      if(!access_.eligible(restoring)||!nativeRecoveryHex(record_.launchSha256))return false;
      if(!reclaimedRecovery_&&(!access_.launchProof||!access_.launchProof(record_,recoveryWorker_)))return false;
      const auto snapshot=access_.snapshot(restoring);const auto lease=nativeStreamProfileLease(snapshot,servicePid_,reclaimedRecovery_);
      if(snapshot.get("deviceInstanceId").string()!=record_.key.instance||(!reclaimedRecovery_&&!sameNativeStreamLease(record_.current,lease)))return false;
      const auto uuid=nativeConsoleUuid(access_.machineUuid());if(!uuid||*uuid!=record_.console.proof.machineUuid)return false;
      const auto device=access_.resolver(record_.console.device.adapter);if(!device)return false;
      auto binding=record_.console;binding.device=*device;if(!sameNativeConsoleBinding(binding,record_.console))return false;
      const auto topology=access_.topology.query();const auto scope=access_.scope(captureNativeTopologyBaseline(validateNativeStreamProfileCcd(topology,lease.targets),lease.targets,"0"));
      auto expected=record_.scope;
      const auto& console=*record_.profile.suppressedConsole;
      const bool consoleActive=std::any_of(topology.paths.begin(),topology.paths.end(),[&](const auto& path){return (path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativeStreamConsolePathMatches(path,console);});
      if(!consoleActive)expected.foreign.erase(std::remove_if(expected.foreign.begin(),expected.foreign.end(),[&](const auto& item){return nativeSameLuid(item.sourceAdapter,console.sourceAdapter)&&nativeSameLuid(item.targetAdapter,console.targetAdapter)&&item.source==console.source&&item.target==console.target;}),expected.foreign.end());
      if(!nativeRecoveryScopeMatches(expected,scope))return false;
      return !reclaimedRecovery_||(restoring&&access_.reclaimProof&&access_.reclaimProof(record_,{record_.key,lease,scope,binding,topology},reclaim_));
    }catch(...){return false;}
  }
  NativeStreamProfileEvidence evidence(const NativeCcdData& ccd,bool restoring){
    if(!identityCurrent(restoring))throw std::runtime_error("native_stream_profile_identity_changed");
    const auto snapshot=access_.snapshot(restoring);const auto lease=nativeStreamProfileLease(snapshot,servicePid_,reclaimedRecovery_);
    const auto device=access_.resolver(record_.console.device.adapter);if(!device)throw std::runtime_error("native_stream_profile_identity_changed");
    auto console=record_.console;console.device=*device;
    const auto fresh=access_.topology.query();const auto active=validateNativeStreamProfileCcd(fresh,lease.targets);
    if(nativeStreamProfileSignatures(active)!=nativeStreamProfileSignatures(validateNativeStreamProfileCcd(ccd,lease.targets)))throw std::runtime_error("native_stream_profile_topology_changed");
    // Keep QDC_ALL_PATHS availability in the journal evidence. The transaction
    // callback supplies active geometry only; restoration additionally proves
    // that the saved inactive console path still exists with its exact keys.
    return {record_.key,lease,access_.scope(captureNativeTopologyBaseline(active,lease.targets,"0")),console,fresh};
  }
  NativeStreamProfileTransactionAccess transactionAccess(bool restoring){
    auto topology=access_.topology;topology.eligible=[this,restoring]{return access_.eligible(restoring);};
    return {std::move(topology),[this,restoring]{return identityCurrent(restoring);},{},{}};
  }
  DWORD servicePid_,session_;NativeStreamProfileJournal journal_;NativeStreamProfileWorkerAccess access_;
  uint32_t viewportStage_=0;
  bool viewportRecovery_=false;std::optional<std::pair<NativeStreamViewportCommit,std::string>> viewportRecoveryCommit_;
  std::optional<NativeTopologyBaseline> trackBefore_;
  std::optional<NativeStreamTrackCommitEvidence> trackRecoveryCommit_;
  NativeConsoleModelProof proof_;NativeStreamProfileRecord record_;std::optional<NativeStreamProfileAck> ack_;
  Json reclaim_;bool attempted_=false,active_=false,clean_=false,recoveryWorker_=false,reclaimedRecovery_=false,unforwardedResize_=false;DWORD entrySystemError_=0;std::string reason_="native_stream_profile_pending";
};

// Interactive-worker completion of a profile-bound single viewport attempt.
// A failed profile completion never resumes media. It asks the service to
// latch DRIVER recovery only when the native attempt may have reached the
// service mutation boundary. An attempt that never did left the exact lease
// untouched: latching it only made the profile restore precondition (a
// verified, non-recovery lease) unsatisfiable until service restart. The
// profile journal and service reservation still block every new display
// transaction until the console profile is durably restored.
inline void completeNativeProfileViewport(WorkerViewportResult& result,NativeStreamProfileWorker& profile,const StreamResize& request,bool driverChangePossible){
  // A successful native result always reports driverChangePossible=true.
  const bool changed=driverChangePossible||result.ok;
  if(profile.completeViewport(request,changed))return;
  if(result.ok){result.reason="viewport_recovery_required";result.nativeStage=9;}
  result.ok=false;result.display.active=false;result.display.viewportProvisioning=false;
  if(changed){result.display.recoveryRequired=true;result.display.configurationPending=true;result.display.error="native_topology_recovery_required";}
  else result.display.error="native_stream_profile_recovery_required";
}
// The service keeps its pipe reader alive while waiting. A worker ACK is not
// process-exit evidence, and a process exit alone is not a restoration ACK.
class NativeStreamStopFence {
 public:
  void reset(std::string session,bool profile){std::lock_guard lock(mutex_);session_=std::move(session);profile_=profile;ack_=restored_=finished_=exited_=false;}
  bool acknowledge(const Json& value){
    std::lock_guard lock(mutex_);const auto* fields=std::get_if<Json::Object>(&value.value);
    if(!fields||fields->size()!=4||value.get("type").string()!="worker.session.stopped"||value.get("sessionId").string()!=session_||
       !std::holds_alternative<bool>(value.get("profileRequired").value)||!std::holds_alternative<bool>(value.get("profileRestored").value)||value.get("profileRequired").boolean()!=profile_)return false;
    ack_=true;restored_=value.get("profileRestored").boolean();condition_.notify_all();return true;
  }
  void finished(bool exited){std::lock_guard lock(mutex_);finished_=true;exited_=exited;condition_.notify_all();}
  bool wait(std::chrono::milliseconds timeout){std::unique_lock lock(mutex_);return condition_.wait_for(lock,timeout,[&]{return ack_||finished_;});}
  bool released()const{std::lock_guard lock(mutex_);return profile_&&ack_&&restored_&&finished_&&exited_;}
 private:
  mutable std::mutex mutex_;std::condition_variable condition_;std::string session_;bool profile_=false,ack_=false,restored_=false,finished_=false,exited_=false;
};
inline std::string nativeStreamStoppedPacket(const std::string& session,bool required,bool restored){
  return "{\"type\":\"worker.session.stopped\",\"sessionId\":"+jsonQuote(session)+",\"profileRequired\":"+(required?"true":"false")+",\"profileRestored\":"+(restored?"true":"false")+'}';
}
struct NativeStreamProfileRecoveryBinding {
  std::string stream,operation,instance;DWORD session=UINT32_MAX;uint32_t generation=0;
  bool operator==(const NativeStreamProfileRecoveryBinding&)const=default;
};
// Guarded by the service's short state mutex. Locked sessions consume no retry;
// only three eligible attempts may run, and each still needs fresh journal and
// Windows identity proof. An unobserved recovery worker exit is terminal.
class NativeStreamProfileRetryGate {
 public:
  bool arm(const NativeStreamProfileRecoveryBinding& binding,bool originalExited){
    if(!originalExited||!nativeStreamJournalId(binding.stream)||!nativeRecoveryHex(binding.operation)||!nativeDisplayInstanceId(binding.instance)||!binding.session||binding.session==UINT32_MAX||!binding.generation)return false;
    if(binding_)return *binding_==binding;
    binding_=binding;attempts_=0;next_=0;busy_=blocked_=false;return true;
  }
  std::optional<NativeStreamProfileRecoveryBinding> begin(DWORD session,bool eligible,int64_t now){
    if(!binding_||busy_||blocked_||attempts_>=3||!eligible||session!=binding_->session||now<0||now<next_)return {};
    ++attempts_;busy_=true;return binding_;
  }
  bool complete(const NativeStreamProfileRecoveryBinding& binding,bool quiescent,bool restored,int64_t now){
    if(!binding_||*binding_!=binding||!busy_)return false;
    busy_=false;if(restored&&quiescent){binding_.reset();return true;}
    if(!quiescent||now<0||now>INT64_MAX-15000)blocked_=true;
    else next_=now+(attempts_==1?5000:15000);
    return false;
  }
  bool pending()const{return binding_.has_value();}
  // An unlock/logon/console connect of the bound session is a fresh user
  // action: grant another three-attempt budget with the same backoff. A
  // running attempt or an unobserved recovery-worker exit is never rearmed.
  bool rearm(DWORD session){if(!binding_||busy_||blocked_||session!=binding_->session)return false;attempts_=0;next_=0;return true;}
 private:
  std::optional<NativeStreamProfileRecoveryBinding> binding_;unsigned attempts_=0;int64_t next_=0;bool busy_=false,blocked_=false;
};
// Service teardown can outlast the ordinary five-second lease snapshot. Refresh
// only the already-held lease while its worker restores; never reopen admission.
class NativeStreamProfileCleanupHeartbeat {
 public:
  ~NativeStreamProfileCleanupHeartbeat(){stop();}
  bool start(std::function<bool()> refresh,std::chrono::milliseconds lifetime=std::chrono::seconds(45),std::chrono::milliseconds period=std::chrono::milliseconds(500))noexcept{
    try{
      if(thread_.joinable()||!refresh||lifetime.count()<=0||lifetime>std::chrono::seconds(45)||period.count()<=0||period>lifetime)return false;
      stopped_=false;const auto until=std::chrono::steady_clock::now()+lifetime;
      (void)refresh();thread_=std::thread([this,refresh=std::move(refresh),until,period]{
        std::unique_lock lock(mutex_);
        while(!stopped_&&std::chrono::steady_clock::now()<until){
          if(condition_.wait_for(lock,period,[&]{return stopped_;}))break;
          if(std::chrono::steady_clock::now()>=until)break;
          lock.unlock();try{(void)refresh();}catch(...){ }lock.lock();
        }
      });return true;
    }catch(...){return false;}
  }
  void stop()noexcept{try{{std::lock_guard lock(mutex_);stopped_=true;}condition_.notify_all();if(thread_.joinable())thread_.join();}catch(...){ }}
 private:
  std::mutex mutex_;std::condition_variable condition_;std::thread thread_;bool stopped_=true;
};
} // namespace host
