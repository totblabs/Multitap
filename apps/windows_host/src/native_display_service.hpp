#pragma once
#include "machine.hpp"
#include "owned_output_size.hpp"
#include "native_display_change.hpp"
#include "native_display_count.hpp"
#include "native_track_change.hpp"
#include "native_display_journal.hpp"
#include "native_display_baseline_journal.hpp"
#include "native_stream_launch.hpp"
#include "native_display_identity.hpp"
#include "native_stream_reclaim.hpp"
#include "native_stream_retirement.hpp"
#include "native_stream_track_commit.hpp"
#include "native_stream_expired_integration.hpp"
#include "native_recovery_reset.hpp"
#include "boot_timeline.hpp"
#include "native_display_files.hpp"
#include "signal_cache.hpp"
namespace host {
// In-process proof from the existing controller. Driver capabilities and this
// object never travel over the broker connection. CCD geometry and the saved
// original topology still need their independent worker/baseline proofs.
struct NativeOwnedStreamLeaseProof {
  std::string stream,instance;
  NativeStreamProcessIdentity service;
  uint32_t generation=0;
  std::vector<NativeDisplayTarget> targets;
};
// Private preparation proves only the existing SDK owner, never Windows
// activation/capture readiness. In particular a cold service may be starting.
struct NativePreparationLeaseProof {
  std::string stream,instance;
  NativeStreamProcessIdentity service;
  uint32_t generation=0;
  std::vector<NativeDisplayTarget> targets;
  std::string phase;
};
// Restoration of an already held private obligation is never forward/capture
// authority. This type is local to the service and has no peer wire form.
struct NativePreparationCleanupLeaseProof {
  std::string stream,instance;
  NativeStreamProcessIdentity service;
  uint32_t generation=0;
  std::vector<NativeDisplayTarget> targets;
  std::string phase;
};
// NativeDisplayService::parkForConsole: nothing changed / the owned outputs are
// gone and stay gone while held / the stop went to recovery and nothing is held.
enum class NativeConsolePark {refused,parked,failed};
}
#ifdef MULTITAP_NATIVE_DISPLAY
#include "runtime.hpp"
#include "windows_endpoint.hpp"
#include "native_render_adapter.hpp"
namespace host {
// This backend is selected only by an administrator-protected provider marker.
// Merely compiling it never installs a device or changes certificate stores.
class NativeDisplayService {
  static NativeDisplayIdentitySnapshot identity(){
    prepareMachineDirectory();return NativeDisplayIdentityStore(nativeDisplayIdentityFileAccess(machineDirectory()/L"private"/L"native-display.dpapi")).getOrCreate();
  }
 public:
  NativeDisplayService()=default;
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
  // Test executables inject an actual SDK controller over a memory transport.
  // The production host does not compile this constructor or the overrides.
  NativeDisplayService(std::function<std::unique_ptr<multitap::vdisplay::Controller>()> factory,std::vector<multitap::vdisplay::Output> outputs,std::function<Json()> config,std::function<void(const std::string&)> publish,NativeRuntimeJournalAccess journal,NativeRuntimeJournalAccess baseline,std::function<bool()> quiescent={},std::function<uint64_t()> recoveryClock={},std::function<bool()> profilePending={})
      :runtime_(std::move(journal)),baseline_(std::move(baseline)),configFixture_(std::move(config)),publishFixture_(std::move(publish)),factoryFixture_(std::move(factory)),quiescentFixture_(std::move(quiescent)),recoveryClockFixture_(std::move(recoveryClock)),profilePendingFixture_(std::move(profilePending)){
    for(size_t i=0;i<outputs.size();++i)publicIdentities_[i]=outputs[i].identity;
  }
  void testProfileViewportStores(NativeRuntimeJournalAccess profiles,NativeRuntimeJournalAccess receipts){std::lock_guard lock(mutex_);profileStoreFixture_=std::move(profiles);viewportReceiptFixture_=std::move(receipts);}
  void testSnapshotPublisher(std::function<void(const std::string&)> publish,bool forceNextWrite=false){
    std::lock_guard lock(mutex_);publishFixture_=std::move(publish);
    if(forceNextWrite)publication_.failed();
  }
  // The render preference the fixture controller path applies at each cold
  // start, and whether a hardware render adapter is "found" for the re-arm.
  // Set before the first tick.
  void testRenderPreference(std::function<NativeRenderPreference()> prefer,std::function<bool()> hardware){std::lock_guard lock(mutex_);renderPreferenceFixture_=std::move(prefer);renderHardwareFixture_=std::move(hardware);}
#endif
  // Render adapter re-arm (native_render_adapter.hpp, hostAction 282). A worker
  // found its capture on WARP although the hardware preference had been
  // accepted. The first such report per service lifetime is latched here and
  // tick() performs it once the display is idle; every later report is
  // refused. Only flags change under the mutex; nothing happens to the
  // reporting stream.
  bool requestRenderRearm()noexcept{
    try{
      std::lock_guard lock(mutex_);
      if(renderRearmUsed_){recordHostEvent("hostAction",{{"action",nativeRenderRearmAction},{"stage",nativeRenderRearmSkipped},{"ok",false},{"error",nativeRenderRearmAlreadyUsed}},true);return false;}
      renderRearmUsed_=true;
      if(!renderPreferenceAttempted_||std::string(renderPreference_.status)!="accepted"){recordHostEvent("hostAction",{{"action",nativeRenderRearmAction},{"stage",nativeRenderRearmSkipped},{"ok",false},{"error",nativeRenderRearmPreferenceNotAccepted}});return false;}
      renderRearmPending_=true;renderRearmSkip_=0;
      recordHostEvent("hostAction",{{"action",nativeRenderRearmAction},{"stage",nativeRenderRearmRequested},{"ok",true}});return true;
    }catch(...){return false;}
  }
  // Whether nothing outside this object holds the virtual display (no stream,
  // console, recovery or unexited worker; ServiceBridge::renderRearmIdle). Set
  // once before the first tick by the service loop; tick() calls it without
  // this object's mutex, only while a re-arm waits. Absent: never re-armed.
  void setRenderRearmIdle(std::function<bool()> idle){std::lock_guard lock(mutex_);renderRearmIdle_=std::move(idle);}
  // Console park (qemu_console_route.hpp). With nobody at an unlocked
  // administrator's console session no worker can change Windows' display
  // configuration, and the Proxmox console showed a QEMU output that Windows
  // had switched off. The SDK owner needs no session: it stops its controller
  // exactly like an orderly service stop (stopControllerLocked: the owned
  // outputs leave, Windows falls back to the displays that remain) and stays
  // stopped for `holdMs`, which the bridge's console hold renews. When the hold
  // ends, or is not renewed in time, the next tick is the ordinary cold start.
  // Fail closed: display manager v2 only, and a reservation, a pending change,
  // any recovery or a lease that is not this controller's leaves the display as
  // it is (refused). A failed SDK stop is "recovery", as it is at a service
  // stop: the outputs may be gone, nothing is held, and the ordinary recovery
  // start follows (failed).
  NativeConsolePark parkForConsole(uint64_t holdMs)noexcept{
    try{
      std::lock_guard lock(mutex_);
      if(!holdMs)return NativeConsolePark::refused;
      if(consolePark_&&!controller_){consoleParkUntil_=recoveryNow()+holdMs;return NativeConsolePark::parked;}
      if(!v2_||!controller_||!runtimeLoaded_||preparation_||!streamProfileOwner_.empty()||pending_||countPending_||activationRecovery_||viewportRecovery_)return NativeConsolePark::refused;
      const auto state=controller_->state();if(!state.leaseActive||state.recoveryRequired||!controller_->leaseCurrent())return NativeConsolePark::refused;
      // As in the render re-arm: the preference belongs to the controller being
      // stopped; the cold start applies a fresh one before any output exists.
      // A re-arm that was waiting wanted exactly this stop and cold start.
      renderPreferenceAttempted_=false;renderPreference_=NativeRenderPreference{};renderRearmPending_=false;
      stopControllerLocked();
      if(viewportRecovery_)return NativeConsolePark::failed;
      consolePark_=true;consoleParkUntil_=recoveryNow()+holdMs;return NativeConsolePark::parked;
    }catch(...){return NativeConsolePark::failed;}
  }
  // False once the park has ended or run out: the outputs are coming back.
  bool renewConsolePark(uint64_t holdMs)noexcept{
    try{std::lock_guard lock(mutex_);if(!consolePark_||!holdMs||recoveryNow()>=consoleParkUntil_)return false;consoleParkUntil_=recoveryNow()+holdMs;return true;}catch(...){return false;}
  }
  void endConsolePark()noexcept{try{std::lock_guard lock(mutex_);consolePark_=false;consoleParkUntil_=0;}catch(...){ }}
  bool consoleParked()noexcept{try{std::lock_guard lock(mutex_);return consolePark_&&recoveryNow()<consoleParkUntil_;}catch(...){return false;}}
  // Display manager v2 (docs/display-manager-v2.md). Set once at service start.
  // v2 never reads the v1 stream-profile journals, tolerates a stale v1
  // baseline binding and self-heals a live-lease recovery latch. Windows
  // topology restoration is owned by the v2 state file, not by this class.
  void setDisplayManagerV2(bool value){std::lock_guard lock(mutex_);v2_=value;}
  bool displayManagerV2(){std::lock_guard lock(mutex_);return v2_;}
  // v2: change one owned output's size (SDK target mode). Returns the published
  // lease snapshot after the change, or nothing when the SDK refused it. The
  // worker then proves the Windows side itself; no checkpoint/journal is needed.
  std::optional<std::string> setOwnedSizeV2(const std::string& monitor,DWORD width,DWORD height){
    const NativeRuntimeOutput one{monitor,width,height};return setOwnedSizesV2({&one,1});
  }
  // v2: several owned outputs' sizes in one SDK reconfiguration (a dual-output
  // stream's start/resize/restore), so Windows sees one set of target
  // recreations instead of one per output. Same contract as setOwnedSizeV2.
  std::optional<std::string> setOwnedSizesV2(std::span<const NativeRuntimeOutput> sizes){
    std::lock_guard lock(mutex_);
    try{
      if(preparation_||!v2_||pending_||countPending_||!controller_||!runtimeLoaded_||sizes.empty()||sizes.size()>2)return {};
      auto desired=outputs_;bool seen[2]{};
      for(const auto& size:sizes){
        const size_t index=size.id=="0"?0:size.id=="1"?1:SIZE_MAX;if(index>=outputs_.size()||seen[index])return {};seen[index]=true;
        if(!ownedOutputSize(size.width,size.height)||!multitap::vdisplay::ownedOutputModeSupported(size.width,size.height))return {};
        desired[index].width=size.width;desired[index].height=size.height;
      }
      const auto before=controller_->state();if(!before.leaseActive||before.recoveryRequired)return {};
      if(desired!=outputs_&&!applyOutputsLocked(desired)){
        // The controller rolls back to the previous outputs when it can.
        const auto after=controller_->state();
        if(!after.leaseActive||after.recoveryRequired){diagnosticFailure_="runtime_recovery_required";viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);snapshot(false,true);return {};}
        committed_=publicOutputsLocked();persistRuntimeLocked("stable",committed_,generation_);snapshot(true,false);return {};
      }
      committed_=publicOutputsLocked();recoveryHash_.clear();recoveryBaseline_.reset();
      if(!persistRuntimeLocked("stable",committed_,generation_)||!snapshot(true,false))return {};
      return stateJsonLocked(true,false);
    }catch(...){return {};}
  }
  // v2: owned output count 1 <-> 2 (client "virtual monitor count"). Adds
  // output 1 at output 0's size (same as v1) or removes it; the surviving
  // output 0 is not touched. The runtime record is committed "stable" with
  // the actual SDK outputs, so a service restart, the next stream and the
  // client's displayed count all follow it. Windows topology (snapshot,
  // verification, restore) is the worker's job; a failure puts the previous
  // count back through this same call. No checkpoint/journal, no latch.
  std::optional<std::string> setOwnedCountV2(int count){
    std::lock_guard lock(mutex_);
    try{
      if(preparation_||!v2_||pending_||countPending_||!controller_||!runtimeLoaded_||outputs_.empty()||(count!=1&&count!=2))return {};
      if(!publicIdentities_[0]||!publicIdentities_[1]||publicIdentities_[0]==publicIdentities_[1]||outputs_.front().identity!=publicIdentities_[0])return {};
      const auto before=controller_->state();if(!before.leaseActive||before.recoveryRequired)return {};
      auto desired=outputs_;
      if(count==1)desired.resize(1);
      else if(desired.size()==1)desired.push_back({publicIdentities_[1],outputs_[0].width,outputs_[0].height});
      if(desired!=outputs_&&!applyOutputsLocked(desired)){
        const auto after=controller_->state();
        if(!after.leaseActive||after.recoveryRequired){diagnosticFailure_="runtime_recovery_required";viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);snapshot(false,true);return {};}
        committed_=publicOutputsLocked();persistRuntimeLocked("stable",committed_,generation_);snapshot(true,false);return {};
      }
      committed_=publicOutputsLocked();recoveryHash_.clear();recoveryBaseline_.reset();
      if(!persistRuntimeLocked("stable",committed_,generation_)||!snapshot(true,false))return {};
      return stateJsonLocked(true,false);
    }catch(...){return {};}
  }
  bool reserveStreamProfile(const std::string& stream,const std::string& expectedLease){
    std::lock_guard lock(mutex_);
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
    if(!factoryFixture_)
#endif
    try{const auto directory=machineDirectory()/L"private";const auto previous=nativeStreamRetirementMarkerAccess(directory/L"native-stream-retirement.json").load();if(previous&&parseNativeStreamRetirement(*previous).phase=="retired"&&!nativeArchiveRetiredExpiredConsole(directory))return false;}catch(...){return false;}
    if(preparation_||!nativeStreamJournalId(stream)||profileBlockedLocked()||pending_||countPending_||activationRecovery_||viewportRecovery_||!controller_||!controller_->state().leaseActive||!runtime_.record()||runtime_.record()->phase!="stable")return false;
    try{if(nativeDisplayLeaseKey(JsonParser::parse(stateJsonLocked(true,false)))!=expectedLease)return false;streamProfileOwner_=stream;if(snapshot(true,false))return true;}catch(...){ }
    streamProfileOwner_.clear();return false;
  }
  bool releaseStreamProfile(const std::string& stream){
    std::lock_guard lock(mutex_);if(preparation_||streamProfileOwner_!=stream)return false;
    // A lost ACK never erases durable recovery. Releasing the process hold
    // leaves the journal guard active until verified restoration is persisted.
    streamProfileOwner_.clear();const bool pending=profileFilePendingLocked();
    return snapshot(controller_&&controller_->state().leaseActive,pending)&&!pending;
  }
  std::optional<std::string> saveStreamProfileLaunch(NativeStreamLaunchReceipt value){
    std::lock_guard lock(mutex_);
    try{
      if(streamProfileOwner_!=value.key.stream||value.key.instance!=instance_||value.entry.generation!=generation_||value.service.pid!=GetCurrentProcessId()||value.service!=nativeStreamProcessIdentity(GetCurrentProcess())||pending_||countPending_||activationRecovery_||viewportRecovery_||!controller_||!runtime_.record()||runtime_.record()->phase!="stable")return {};
      const auto worker=observeNativeStreamProcess(value.worker);if(worker.state!=NativeStreamProcessState::running)return {};
      const auto state=controller_->state();if(!state.leaseActive||state.recoveryRequired||state.targets.empty()||state.targets.size()>2||state.targets.size()!=outputs_.size())return {};
      value.entry.servicePid=value.service.pid;value.entry.targets.clear();value.outputs.clear();value.lease=0;
      for(size_t i=0;i<state.targets.size();++i){const auto& target=state.targets[i];const auto& output=outputs_[i];
        if(target.display_id!=output.identity||output.identity!=publicIdentities_[i]||(value.lease&&value.lease!=target.lease_id))return {};
        value.lease=target.lease_id;value.outputs.push_back({target.display_id,target.connector_index});
        value.entry.targets.push_back({std::to_string(i),{target.os_adapter_luid.low_part,target.os_adapter_luid.high_part},target.target_id,output.width,output.height});
      }
      return saveNativeStreamLaunch(nativeStreamLaunchFileAccess(machineDirectory()/L"private"/L"native-stream-launch.json"),nativeStreamProfileFileAccess(machineDirectory()/L"private"/L"native-stream-profile.json"),value,[](const auto& process){return observeNativeStreamProcess(process).state==NativeStreamProcessState::gone;});
    }catch(...){return {};}
  }
  bool releaseUnpreparedStreamProfile(const NativeStreamProfileKey& key,uint32_t generation,bool startDispatched,bool originalExited){
    std::lock_guard lock(mutex_);
    if(streamProfileOwner_!=key.stream||key.instance!=instance_||generation!=generation_)return false;
    if(!nativeStreamUnpreparedLaunchRetirable(nativeStreamProfileFileAccess(machineDirectory()/L"private"/L"native-stream-profile.json"),nativeStreamLaunchFileAccess(machineDirectory()/L"private"/L"native-stream-launch.json"),key,generation,nativeStreamProcessIdentity(GetCurrentProcess()),startDispatched,originalExited,[](const auto& process){return observeNativeStreamProcess(process).state;}))return false;
    streamProfileOwner_.clear();return snapshot(controller_&&controller_->state().leaseActive,false);
  }
  bool streamProfilePending(){std::lock_guard lock(mutex_);return profileBlockedLocked();}
  bool hasStreamProfileReservation(){std::lock_guard lock(mutex_);return !streamProfileOwner_.empty();}
  bool reservePreparation(const std::string& stream,const std::string& expectedLease){
    std::lock_guard lock(mutex_);
    try{
      if(preparation_||!v2_||!nativeStreamJournalId(stream)||!streamProfileOwner_.empty()||
          pending_||countPending_||activationRecovery_||viewportRecovery_||!controller_||!runtimeLoaded_||
          !runtime_.record()||(runtime_.record()->phase!="starting"&&runtime_.record()->phase!="stable")||
          outputs_.size()!=1||!runtime_.verifiedBytes()||!controller_->leaseCurrent())return false;
      const auto snapshotValue=JsonParser::parse(stateJsonLocked(true,false));
      if(!nativeConfigMatchesLease(configuration(),snapshotValue)||nativeDisplayLeaseKey(snapshotValue)!=expectedLease||
          nativeDisplayTargets(snapshotValue).size()!=1)return false;
      Preparation held{stream,runtime_.record()->phase};std::string owner=stream;
      preparation_.emplace(std::move(held));streamProfileOwner_=std::move(owner);
      if(snapshot(true,false))return true;
      preparation_.reset();streamProfileOwner_.clear();return false;
    }catch(...){return false;}
  }
  std::optional<NativePreparationLeaseProof> preparationLease(const std::string& stream,const std::string& instance,uint32_t generation){
    std::lock_guard lock(mutex_);return preparationProofLocked(stream,instance,generation);
  }
  std::optional<NativePreparationLeaseProof> setPreparationOriginalBoundSize(const std::string& stream,
      const std::string& instance,uint32_t generation,DWORD width,DWORD height){
    std::lock_guard lock(mutex_);
    if(!preparationProofLocked(stream,instance,generation)||!preparationSizeLocked(width,height,false))return {};
    return preparationProofLocked(stream,instance,generation_);
  }
  std::optional<NativePreparationCleanupLeaseProof> preparationCleanupLease(const std::string& stream,
      const std::string& instance,uint32_t generation){
    std::lock_guard lock(mutex_);return preparationCleanupProofLocked(stream,instance,generation);
  }
  // Only parent original-bound cleanup authorities call this path. The parent
  // separately holds the sealed original and admits its exact saved CCD mode.
  std::optional<NativePreparationCleanupLeaseProof> restorePreparationOriginalBoundSize(const std::string& stream,
      const std::string& instance,uint32_t generation,DWORD width,DWORD height){
    std::lock_guard lock(mutex_);
    if(!preparationCleanupProofLocked(stream,instance,generation)||!preparationSizeLocked(width,height,true))return {};
    return preparationCleanupProofLocked(stream,instance,generation_);
  }
  bool refreshPreparationLease(const std::string& stream)noexcept{
    std::lock_guard lock(mutex_);
    try{
      if(!preparationProofLocked(stream,instance_,generation_)||!controller_->heartbeat())return false;
      return snapshot(true,false);
    }catch(...){return false;}
  }
  bool refreshPreparationCleanupLease(const std::string& stream)noexcept{
    std::lock_guard lock(mutex_);
    try{
      if(!preparationCleanupProofLocked(stream,instance_,generation_)||!controller_->heartbeat())return false;
      // This return proves only renewal of the same owned lease. A failed
      // public publication cannot drop it while cleanup still needs it, and
      // never clears the private commit obligation or advertises readiness.
      (void)snapshot(true,viewportRecovery_);return true;
    }catch(...){return false;}
  }
  bool releasePreparation(const std::string& stream){
    std::lock_guard lock(mutex_);
    try{
      if(!preparationCleanupProofLocked(stream,instance_,generation_)||preparation_->commitPending||viewportRecovery_)return false;
      auto held=std::move(*preparation_);auto owner=std::move(streamProfileOwner_);preparation_.reset();streamProfileOwner_.clear();
      if(snapshot(true,false))return true;
      // Keep this exact purpose/owner on a failed publication, so the service
      // can retry cleanup without freeing another operation to take the lease.
      held.forwardFailed=true;preparation_.emplace(std::move(held));streamProfileOwner_=std::move(owner);return false;
    }catch(...){return false;}
  }
  bool streamStopClean(const std::string& instance){
    std::lock_guard lock(mutex_);
    try{
      if(instance.empty()||instance_!=instance||profileBlockedLocked()||pending_||countPending_||activationRecovery_||viewportRecovery_||
         !controller_||!runtime_.record()||runtime_.record()->phase!="stable"||!controller_->leaseCurrent())return false;
      const auto config=configuration(),state=JsonParser::parse(stateJsonLocked(controller_->state().leaseActive,false));
      return config.get("deviceInstanceId").string()==instance&&nativeConfigMatchesLease(config,state)&&
        state.get("servicePid").number()==GetCurrentProcessId()&&state.get("leaseActive").boolean()&&
        !state.get("recoveryRequired").boolean()&&!state.get("configurationPending").boolean();
    }catch(...){return false;}
  }
  bool expiredStreamProfileQuiescent(const NativeStreamProfileRecord& profile){
    std::lock_guard lock(mutex_);
    const auto deny=[](const char* code){nativeRecoveryNote(NativeRecoveryField::quiescent,code);return false;};
    try{
      if(!selected())return deny("not_selected");if(controller_)return deny("controller_active");if(!streamProfileOwner_.empty())return deny("reservation_held");if(pending_||countPending_)return deny("change_pending");if(configuration().get("deviceInstanceId").string()!=profile.key.instance)return deny("instance_mismatch");
      const auto directory=machineDirectory()/L"private";const auto launchBytes=nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json").load(),profileBytes=nativeStreamProfileFileAccess(directory/L"native-stream-profile.json").load();if(!launchBytes||!profileBytes)return deny("records_missing");if(*profileBytes!=nativeStreamProfileRecordBytes(profile))return deny("profile_changed");
      std::string rejection;const auto resolved=nativeExpiredResolvedSource(profile,parseNativeStreamLaunch(*launchBytes),nativeExpiredPrivateSources(directory),&rejection);nativeRecoveryNote(NativeRecoveryField::ledger,rejection);if(!resolved)return deny("ledger_unresolved");
      auto wire=multitap::vdisplay::openOwnedInstance(wide(profile.key.instance));if(!wire)return deny("transport_unavailable");multitap::vdisplay::api::ControlClient client(*wire);
      if(!nativeExpiredSdkAbsence(client,resolved->ledger,resolved->privateDisplays))return deny("sdk_present");
      nativeRecoveryNote(NativeRecoveryField::quiescent,"ok");return true;
    }catch(...){return deny("failed");}
  }
  bool reserveExpiredStreamProfile(const NativeStreamProfileRecord& profile,const NativeStreamProcessIdentity& worker,const std::function<NativeStreamProcessState(const NativeStreamProcessIdentity&)>& processes,const std::function<bool(const NativeRecoveryScope&)>& current){
    std::lock_guard lock(mutex_);
    try{
      if(!selected()||controller_||!streamProfileOwner_.empty()||pending_||countPending_||!processes||!current||!current(profile.scope)||configuration().get("deviceInstanceId").string()!=profile.key.instance)return false;
      const auto directory=machineDirectory()/L"private";const auto prior=nativeStreamExpiredConsoleFileAccess(directory/L"native-stream-expired-console.json").load();if(prior){const auto old=parseNativeStreamExpiredConsoleRecord(*prior);if(old.profileSha256!=nativeBaselineDigest(nativeStreamProfileRecordBytes(profile))&&!nativeArchiveRetiredExpiredConsole(directory))return false;}
      const auto profileBytes=nativeStreamProfileFileAccess(directory/L"native-stream-profile.json").load(),launchBytes=nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json").load();if(!profileBytes||!launchBytes||*profileBytes!=nativeStreamProfileRecordBytes(profile))return false;const auto launch=parseNativeStreamLaunch(*launchBytes);const auto executor=NativeStreamExpiredConsoleExecutor{nativeStreamProcessIdentity(GetCurrentProcess()),worker};
      if(!nativeStreamLaunchMatches(launch,profile)||processes(launch.service)!=NativeStreamProcessState::gone||processes(launch.worker)!=NativeStreamProcessState::gone||processes(executor.service)!=NativeStreamProcessState::running||processes(executor.worker)!=NativeStreamProcessState::running)return false;
      const auto resolved=nativeExpiredResolvedSource(profile,launch,nativeExpiredPrivateSources(directory));if(!resolved)return false;auto wire=multitap::vdisplay::openOwnedInstance(wide(profile.key.instance));if(!wire)return false;multitap::vdisplay::api::ControlClient client(*wire);if(!nativeExpiredSdkAbsence(client,resolved->ledger,resolved->privateDisplays)||!current(profile.scope))return false;
      expiredProfileExecutor_=executor;expiredProfileReservation_=nativeBaselineDigest(utf8(randomName())+profile.key.operation);streamProfileOwner_=profile.key.stream;snapshot(false,true);return true;
    }catch(...){return false;}
  }
  std::optional<NativeStreamExpiredConsoleAuthorityProof> expiredStreamProfileAuthority(const NativeStreamExpiredConsoleRecord& record,const std::function<NativeStreamProcessState(const NativeStreamProcessIdentity&)>& processes,const std::function<bool(const NativeRecoveryScope&)>& current){
    std::lock_guard lock(mutex_);
    try{
      if(!expiredProfileExecutor_||expiredProfileReservation_.empty()||controller_||pending_||countPending_||!processes||!current||streamProfileOwner_.empty())return {};
      const auto directory=machineDirectory()/L"private";const auto profileBytes=nativeStreamProfileFileAccess(directory/L"native-stream-profile.json").load(),launchBytes=nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json").load();if(!profileBytes||!launchBytes||record.profileSha256!=nativeBaselineDigest(*profileBytes)||record.launchSha256!=nativeBaselineDigest(*launchBytes))return {};const auto profile=parseNativeStreamProfileRecord(*profileBytes);const auto launch=parseNativeStreamLaunch(*launchBytes);if(profile.key.stream!=streamProfileOwner_||!nativeStreamLaunchMatches(launch,profile)||!current(profile.scope))return {};
      const auto resolved=nativeExpiredResolvedSource(profile,launch,nativeExpiredPrivateSources(directory));if(!resolved||record.ledgerSha256!=nativeBaselineDigest(nativeExpiredLedgerBytes(resolved->ledger,resolved->snapshots)))return {};
      std::optional<NativeStreamExpiredConsoleExecutor> previous;const auto markerBytes=nativeStreamExpiredConsoleFileAccess(directory/L"native-stream-expired-console.json").load();if(markerBytes){const auto marker=parseNativeStreamExpiredConsoleRecord(*markerBytes);if(!nativeExpiredRecordPinsEqual(marker,record))return {};if(marker.executor!=*expiredProfileExecutor_)previous=marker.executor;}else if(record.revision!=1||record.phase!="prepared"||record.executor!=*expiredProfileExecutor_)return {};
      if(record.executor!=*expiredProfileExecutor_&&(!previous||record.executor!=*previous))return {};
      NativeStreamExpiredConsoleAuthorityProof proof;proof.operation=record.operation;proof.profileSha256=record.profileSha256;proof.launchSha256=record.launchSha256;proof.reservation=expiredProfileReservation_;proof.executor=*expiredProfileExecutor_;proof.session=profile.scope.session;proof.logon=profile.scope.logon;proof.tick=GetTickCount64();
      std::vector<NativeStreamProcessIdentity> identities={launch.service,launch.worker,proof.executor.service,proof.executor.worker};if(previous){identities.push_back(previous->service);identities.push_back(previous->worker);}
      for(const auto& identity:identities)if(std::none_of(proof.processes.begin(),proof.processes.end(),[&](const auto& p){return p.identity==identity;}))proof.processes.push_back({identity,processes(identity)});
      if(processes(launch.service)!=NativeStreamProcessState::gone||processes(launch.worker)!=NativeStreamProcessState::gone||processes(proof.executor.service)!=NativeStreamProcessState::running||processes(proof.executor.worker)!=NativeStreamProcessState::running)return {};
      auto wire=multitap::vdisplay::openOwnedInstance(wide(profile.key.instance));if(!wire)return {};multitap::vdisplay::api::ControlClient client(*wire);const auto absent=nativeExpiredSdkAbsence(client,resolved->ledger,resolved->privateDisplays);if(!absent||!current(profile.scope))return {};proof.leases=absent->leases;proof.displays=absent->displays;return proof;
    }catch(...){return {};}
  }
  // Administrator record reset (see native_recovery_reset.hpp). Preflight and
  // commit each run under this service's mutex; the caller holds the bridge
  // locks between them so no session/recovery can start in the gap.
  NativeResetPreflight resetPreflight(){std::lock_guard lock(mutex_);try{return resetPreflightLocked();}catch(...){return {};}}
  NativeResetCommit resetCommit(){
    std::lock_guard lock(mutex_);NativeResetCommit result;
    try{
      const auto pre=resetPreflightLocked();
      if(pre.pending!="none"){result.steps.push_back({"pending",pre.pending,false});return result;}
      if(pre.sdk!="absent"){result.steps.push_back({"sdk",pre.sdk,false});return result;}
      const auto instance=configuration().get("deviceInstanceId").string();
      // Keep the last committed output sizes and a monotonic generation so a
      // fresh lease key can never repeat an earlier activation/readiness key.
      std::optional<NativeRuntimeRecord> old;try{auto previous=runtime_.reopened();old=previous.load(instance);}catch(...){old.reset();}
      auto io=resetIoLocked();const auto code=io.move(result.moved);const bool moved=code=="moved"||code=="nothing_to_move";
      result.steps.push_back({"archive",code,moved});
      if(!moved){diagnosticFailure_="native_stream_profile_recovery_required";snapshot(false,true);return result;}
      try{auto fresh=runtime_.reopened();if(old&&old->generation<UINT32_MAX-1&&!fresh.load(instance))(void)fresh.store({instance,"stopped",old->generation+1,old->outputs,""});}catch(...){ }
      runtime_=runtime_.reopened();baseline_=baseline_.reopened();runtimeLoaded_=false;committed_.clear();generation_=0;instance_.clear();
      viewportRecovery_=activationRecovery_=activationWorkerExited_=false;recoveryHash_.clear();recoveryBaseline_.reset();reclaimCommit_.reset();diagnosticFailure_.clear();
      streamProfileOwner_.clear();expiredProfileExecutor_.reset();expiredProfileReservation_.clear();recoveryNextAttempt_=0;recoveryBackoff_=1000;outputs_.clear();activationDiagnostic_.reset();activationDiagnosticGeneration_=0;client_.reset();transport_.reset();
      result.steps.push_back({"reinitialize","reinitialized",true});result.ok=true;(void)snapshot(false,false);
    }catch(...){result.steps.push_back({"reinitialize","failed",false});}
    return result;
  }
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
  void testResetIo(NativeResetIo io){std::lock_guard lock(mutex_);resetIoFixture_=std::move(io);}
#endif
  bool releaseExpiredStreamProfile(const NativeStreamProfileKey& key,bool workerExited){
    std::lock_guard lock(mutex_);if(!workerExited||!expiredProfileExecutor_||streamProfileOwner_!=key.stream||controller_)return false;expiredProfileExecutor_.reset();expiredProfileReservation_.clear();streamProfileOwner_.clear();snapshot(false,true);return true;
  }
  std::optional<NativeStreamReclaimCommit> reclaimStreamProfile(const NativeStreamProfileRecord& profile,const NativeStreamProcessIdentity& worker,const NativeStreamProfileEvidence& observed,
      const std::function<NativeStreamProcessState(const NativeStreamProcessIdentity&)>& processes,const std::function<bool(const NativeRecoveryScope&)>& identityCurrent){
    std::lock_guard lock(mutex_);
    try{
      if(!selected()||controller_||!streamProfileOwner_.empty()||pending_||countPending_||!processes||!identityCurrent)return {};
      const auto config=configuration();if(config.get("deviceInstanceId").string()!=profile.key.instance||!loadRuntimeLocked(config)||!runtime_.record()||runtime_.record()->phase!="stable"||viewportRecovery_||activationRecovery_)return {};
      const auto directory=machineDirectory()/L"private";
      const auto profiles=nativeStreamProfileFileAccess(directory/L"native-stream-profile.json"),launches=nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json");
      const auto profileBytes=profiles.load(),launchBytes=launches.load();if(!profileBytes||!launchBytes||*profileBytes!=nativeStreamProfileRecordBytes(profile))return {};
      const auto launch=parseNativeStreamLaunch(*launchBytes);const auto service=nativeStreamProcessIdentity(GetCurrentProcess());
      if(!nativeStreamLaunchMatches(launch,profile)||launch.service==service||profile.phase==NativeStreamProfilePhase::restored||profile.resize||profile.resizeSequence||!identityCurrent(profile.scope))return {};
      NativeStreamReclaimJournal journal(nativeStreamReclaimFileAccess(directory/L"native-stream-reclaim.json"),launches,profiles,nativeRuntimeFileAccess(directory/L"native-display-runtime.json"),nativeDisplayIdentityFileAccess(directory/L"native-display.dpapi"));
      NativeStreamReclaimAuthority authority;authority.process=processes;authority.display=[observed]()->std::optional<NativeStreamProfileEvidence>{return observed;};authority.now=[]{return GetTickCount64();};
      authority.identity=[&](const NativeStreamReclaimSource& source){const auto fresh=configuration();return selected()&&fresh.get("deviceInstanceId").string()==profile.key.instance&&source.profile.key==profile.key&&profiles.load()==profileBytes&&launches.load()==launchBytes&&identityCurrent(source.profile.scope);};
      auto connection=multitap::vdisplay::openOwnedInstance(wide(profile.key.instance));if(!connection)return {};
      transport_=std::move(connection);client_=std::make_unique<multitap::vdisplay::api::ControlClient>(*transport_);
      const auto owner=NativeDisplayIdentityStore(nativeDisplayIdentityFileAccess(directory/L"native-display.dpapi")).existing();
      uint64_t replacement=0;winCheck(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&replacement),sizeof(replacement),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"Create recovery lease");replacement|=0x6000000000000000ull;if(replacement==launch.lease)return {};
      const auto intent=journal.prepare(nativeBaselineDigest(utf8(randomName())+profile.key.stream),replacement,service,worker,authority);if(!intent)return {};
      if(journal.reconcile(*client_,owner.value.owner,*intent,authority).location!=multitap::vdisplay::LiveLeaseLocation::replacement)return {};
      // The controller becomes service-owned BEFORE adopt/commit. Never let a
      // temporary local destructor release a reconciled lease on a lost ACK.
      controller_=std::make_unique<multitap::vdisplay::Controller>(*client_,replacement,owner.value.owner);
      streamProfileOwner_=profile.key.stream;outputs_.clear();for(size_t i=0;i<launch.outputs.size();++i){publicIdentities_[i]=launch.outputs[i].display;outputs_.push_back({launch.outputs[i].display,launch.entry.targets[i].width,launch.entry.targets[i].height});}
      if(!journal.adopt(*controller_,*intent,authority)){diagnosticFailure_="native_stream_profile_recovery_required";viewportRecovery_=true;snapshot(false,true);return {};}
      const auto committed=journal.commit(*client_,*intent,authority);if(!committed){diagnosticFailure_="native_stream_profile_recovery_required";viewportRecovery_=true;snapshot(true,true);return {};}
      runtime_=NativeRuntimeJournal(nativeRuntimeFileAccess(directory/L"native-display-runtime.json"));const auto current=runtime_.load(profile.key.instance);
      if(!current||current->phase!="recovery"||current->generation!=committed->generation){viewportRecovery_=true;snapshot(true,true);return {};}
      committed_=current->outputs;generation_=current->generation;runtimeLoaded_=true;viewportRecovery_=activationRecovery_=false;reclaimCommit_=*committed;
      if(!snapshot(true,false))return {};return committed;
    }catch(...){diagnosticFailure_="native_stream_profile_recovery_required";if(controller_)viewportRecovery_=true;snapshot(false,true);return {};}
  }
  bool completeReclaimedStreamProfile(const NativeStreamProfileKey& key,const NativeStreamReclaimCommit& commit,bool workerExited){
    std::lock_guard lock(mutex_);
    try{
      if(!workerExited||!reclaimCommit_||nativeStreamReclaimCommitBytes(*reclaimCommit_)!=nativeStreamReclaimCommitBytes(commit)||streamProfileOwner_!=key.stream||instance_!=key.instance||generation_!=commit.generation||!controller_||!controller_->leaseCurrent()||viewportRecovery_||activationRecovery_||pending_||countPending_)return false;
      const auto directory=machineDirectory()/L"private";const auto profileBytes=nativeStreamProfileFileAccess(directory/L"native-stream-profile.json").load(),intentBytes=nativeStreamReclaimFileAccess(directory/L"native-stream-reclaim.json").load(),identityBytes=nativeDisplayIdentityFileAccess(directory/L"native-display.dpapi").load(),runtimeBytes=nativeRuntimeFileAccess(directory/L"native-display-runtime.json").load();
      if(!profileBytes||!intentBytes||!identityBytes||!runtimeBytes||nativeBaselineDigest(*intentBytes)!=commit.intentSha256||nativeBaselineDigest(*identityBytes)!=commit.identitySha256||nativeBaselineDigest(*runtimeBytes)!=commit.runtimeSha256)return false;
      const auto profile=parseNativeStreamProfileRecord(*profileBytes);const auto intent=parseNativeStreamReclaim(*intentBytes);const auto service=nativeStreamProcessIdentity(GetCurrentProcess());
      const auto lease=nativeStreamProfileLeaseFromStateLocked();
      if(profile.key!=key||profile.phase!=NativeStreamProfilePhase::restored||!sameNativeStreamLease(profile.current,lease)||intent.key!=key||intent.service!=service||intent.generation!=generation_||observeNativeStreamProcess(intent.worker).state!=NativeStreamProcessState::gone)return false;
      if(!persistRuntimeLocked("stable",committed_,generation_))return false;
      streamProfileOwner_.clear();if(snapshot(true,false)){reclaimCommit_.reset();diagnosticFailure_.clear();return true;}
      streamProfileOwner_=key.stream;persistRuntimeLocked("recovery",committed_,generation_);snapshot(true,true);return false;
    }catch(...){return false;}
  }
  bool retireStreamProfile(const NativeStreamProfileRecord& profile,const NativeStreamProcessIdentity& worker,const NativeStreamRetirementObservation& observed,
      const std::function<NativeStreamProcessState(const NativeStreamProcessIdentity&)>& processes,const std::function<bool(const NativeRecoveryScope&)>& identityCurrent){
    std::lock_guard lock(mutex_);
    const auto deny=[](const char* code){nativeRecoveryNote(NativeRecoveryField::retirement,code);return false;};
    try{
      if(!selected())return deny("not_selected");if(controller_)return deny("controller_active");
      if((!streamProfileOwner_.empty()&&(!expiredProfileExecutor_||streamProfileOwner_!=profile.key.stream||expiredProfileExecutor_->worker!=worker)))return deny("reservation_conflict");
      if(pending_||countPending_)return deny("change_pending");if(!processes||!identityCurrent)return deny("failed");
      const auto config=configuration();if(config.get("deviceInstanceId").string()!=profile.key.instance)return deny("instance_mismatch");
      auto connection=multitap::vdisplay::openOwnedInstance(wide(profile.key.instance));if(!connection)return deny("transport_unavailable");multitap::vdisplay::api::ControlClient client(*connection);
      auto io=nativeStreamRetirementFileAccess(machineDirectory()/L"private");NativeStreamRetirementJournal retirement(io);
      NativeStreamRetirementAuthority authority;authority.process=processes;authority.display=[observed]()->std::optional<NativeStreamRetirementObservation>{return observed;};authority.now=[]{return GetTickCount64();};
      authority.identity=[&](const NativeStreamRetirementRecord& record){const auto fresh=configuration();return selected()&&record.key==profile.key&&fresh.get("deviceInstanceId").string()==profile.key.instance&&identityCurrent(record.fresh);};
      authority.expiredConsole=[&](const NativeStreamProfileRecord& held,const NativeStreamRetirementObservation& fresh)->std::optional<NativeStreamRetirementExpiredConsoleProof>{
        const auto directory=machineDirectory()/L"private";const auto marker=nativeStreamExpiredConsoleFileAccess(directory/L"native-stream-expired-console.json").load();if(!marker)return {};auto launch=nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json").load();if(!launch){const auto pending=io.marker.load();if(!pending)return {};const auto record=parseNativeStreamRetirement(*pending);launch=io.archive(record.operation,1);}if(!launch)return {};return nativeExpiredRetirementCompletion(held,fresh,*launch,*marker);
      };
      authority.leasesAbsent=[&](auto leases,auto displays){return nativeStreamRetirementLeasesAbsent(client,leases,displays);};
      std::optional<NativeStreamRetirementRecord> prepared;const auto previous=io.marker.load();
      if(previous){const auto held=parseNativeStreamRetirement(*previous);if(held.phase=="prepared"){if(held.key!=profile.key)return deny("marker_conflict");if(held.service==nativeStreamProcessIdentity(GetCurrentProcess())&&held.worker==worker)prepared=held;else prepared=retirement.handoff(nativeStreamProcessIdentity(GetCurrentProcess()),worker,authority);if(!prepared)return deny("handoff_rejected");}}
      if(!prepared){uint64_t replacement=0;winCheck(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&replacement),sizeof(replacement),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"Create fresh display lease");replacement|=0x6000000000000000ull;prepared=retirement.prepare(nativeBaselineDigest(utf8(randomName())+profile.key.operation),replacement,nativeStreamProcessIdentity(GetCurrentProcess()),worker,authority);}
      if(!prepared)return deny(retirement.rejection().empty()?"failed":retirement.rejection().c_str());
      if(!retirement.commit(*prepared,authority))return deny(retirement.rejection().empty()?"commit_failed":retirement.rejection().c_str());
      // No old geometry was applied. Fresh ordinary activation must prove the
      // new lease/outputs before the host may advertise capture readiness.
      runtime_=NativeRuntimeJournal(nativeRuntimeFileAccess(machineDirectory()/L"private"/L"native-display-runtime.json"));const auto current=runtime_.load(profile.key.instance);if(!current||current->phase!="stopped"||current->generation!=prepared->generation)return deny("runtime_unverified");
      instance_=profile.key.instance;runtimeLoaded_=true;generation_=current->generation;committed_=current->outputs;viewportRecovery_=activationRecovery_=false;recoveryHash_.clear();recoveryBaseline_.reset();reclaimCommit_.reset();diagnosticFailure_.clear();streamProfileOwner_=profile.key.stream;nativeRecoveryNote(NativeRecoveryField::retirement,"ok");return snapshot(false,false);
    }catch(...){diagnosticFailure_="native_stream_profile_recovery_required";return deny("failed");}
  }
  bool completeRetiredStreamProfile(const NativeStreamProfileKey& key,bool workerExited){
    std::lock_guard lock(mutex_);
    try{
      if(!workerExited||controller_||streamProfileOwner_!=key.stream||instance_!=key.instance||!runtime_.record()||runtime_.record()->phase!="stopped"||viewportRecovery_||activationRecovery_)return false;
      const auto directory=machineDirectory()/L"private";const auto bytes=nativeStreamRetirementMarkerAccess(directory/L"native-stream-retirement.json").load();if(!bytes)return false;const auto record=parseNativeStreamRetirement(*bytes);
      if(record.phase!="retired"||record.key!=key||record.service!=nativeStreamProcessIdentity(GetCurrentProcess())||record.generation!=generation_||observeNativeStreamProcess(record.worker).state!=NativeStreamProcessState::gone||nativeStreamProfileFileAccess(directory/L"native-stream-profile.json").load()||nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json").load())return false;
      if(!nativeArchiveRetiredExpiredConsole(directory))return false;
      streamProfileOwner_.clear();if(snapshot(false,false)){expiredProfileExecutor_.reset();expiredProfileReservation_.clear();return true;}streamProfileOwner_=key.stream;return false;
    }catch(...){return false;}
  }
  bool refreshStreamProfileLease()noexcept{
    std::lock_guard lock(mutex_);
    if(streamProfileOwner_.empty()||!controller_||!controller_->state().leaseActive||pending_||countPending_||activationRecovery_||viewportRecovery_)return false;
    try{
      if(!nativeConfigMatchesLease(configuration(),JsonParser::parse(stateJsonLocked(true,false))))return false;
      if(!controller_->heartbeat()){diagnosticFailure_="heartbeat_failed";viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);snapshot(false,true);return false;}
      return snapshot(true,false);
    }catch(...){return false;}
  }
  // Service teardown only: keep the already held v2/v1 controller alive until
  // its worker joins. Never tick, reopen, resize, reclaim or create a lease.
  bool refreshOwnedCleanupLease()noexcept{
    std::lock_guard lock(mutex_);
    try{
      if((!v2_&&streamProfileOwner_.empty())||!controller_||!controller_->state().leaseActive||pending_||countPending_||activationRecovery_||viewportRecovery_)return false;
      const auto& record=runtime_.record();
      if(!record||record->instance!=instance_||record->phase!="stable"||record->generation!=generation_||record->outputs!=committed_||!runtime_.verifiedBytes())return false;
      if(!nativeConfigMatchesLease(configuration(),JsonParser::parse(stateJsonLocked(true,false))))return false;
      if(!controller_->heartbeat()){diagnosticFailure_="heartbeat_failed";viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);snapshot(false,true);return false;}
      return snapshot(true,false);
    }catch(...){return false;}
  }
  ~NativeDisplayService(){shutdown();}
  Json diagnosticSnapshot(){
    std::lock_guard lock(mutex_);
    auto value=JsonParser::parse(stateJsonLocked(controller_&&controller_->state().leaseActive,viewportRecovery_));
    auto& object=std::get<Json::Object>(value.value);
    object["phase"].value=runtime_.record()?runtime_.record()->phase:std::string("empty");
    object["failureReason"].value=diagnosticFailure_;
    if(activationDiagnostic_){object["activation"]=JsonParser::parse(nativeActivationDiagnosticJson(*activationDiagnostic_));object["activationCurrent"].value=activationDiagnosticGeneration_==generation_;}
    return value;
  }
  void observeActivation(uint32_t generation,const NativeActivationDiagnostic& diagnostic)noexcept{
    try{std::lock_guard lock(mutex_);if(!generation||generation!=generation_)return;
      // Diagnostics do not call activationReady or change the runtime journal.
      activationDiagnostic_=parseNativeActivationDiagnostic(JsonParser::parse(nativeActivationDiagnosticJson(diagnostic)));activationDiagnosticGeneration_=generation;
    }catch(...){ }
  }
  void shutdown()noexcept{std::lock_guard lock(mutex_);stopControllerLocked();}
  // Orderly stop of the held controller (the service stop path): abandon any
  // pending change, "changing" -> SDK stop -> "stopped" in the runtime record,
  // snapshot published without a lease, then the controller, client and
  // transport are released. A failed stop or an unknown state is persisted
  // as "recovery" instead. Also the render adapter re-arm's teardown.
  void stopControllerLocked()noexcept{
    if(!controller_)return;
    if(pending_)abandonLocked();if(countPending_)abandonCountLocked();
    if(!persistRuntimeLocked("changing",committed_,generation_))viewportRecovery_=true;
    const bool stopped=controller_->stop();
    if(!stopped||activationRecovery_||viewportRecovery_||!persistRuntimeLocked("stopped",committed_,generation_)){viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);}
    snapshot(false,!stopped||viewportRecovery_);controller_.reset();client_.reset();transport_.reset();outputs_.clear();pending_.reset();countPending_.reset();
  }
  void tick() noexcept {
    // The re-arm's probes run before this mutex is taken: the idle probe reaches
    // into the bridge, whose locks other threads take before calling into this
    // object (signinOriginalRestored -> streamStopClean), and the hardware probe
    // creates a D3D11 device. Both run only while a re-arm waits.
    const auto rearm=probeRenderRearm();
    std::lock_guard lock(mutex_);
    try{
      // One marker read per pass: selection and the provider fields come from it.
      bool present=false;const auto config=configuration(&present);if(!nativeProviderSelected(config,present))return;
      if(config.get("provider").string()!="libvirtualdisplay-1.6.4"||!config.get("enabled").boolean()||!nativeDisplayInstanceId(config.get("deviceInstanceId").string()))return;
      // Console park: the controller stays stopped while the console's hold is
      // renewed; without a renewal it starts again below like after a service start.
      if(consolePark_){
        if(!controller_&&recoveryNow()<consoleParkUntil_){snapshot(false,false);return;}
        consolePark_=false;consoleParkUntil_=0;
      }
      const auto hold=streamProfileOwner_.empty()?profileHoldReasonLocked():std::string("reservation");
      nativeRecoveryNote(NativeRecoveryField::hold,hold);
      if(hold!="none"){
        if(renderRearmPending_)skipRenderRearmLocked(nativeRenderRearmDisplayHeld,false);
        // Keep the exact lease alive for restoration, without replacing outputs
        // or starting another activation transaction underneath the worker.
        if(controller_){if(!controller_->heartbeat()){diagnosticFailure_="heartbeat_failed";viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);}
          // Never publish recovery with an empty cause (fixed diagnostic codes only).
          else if(diagnosticFailure_.empty()&&(viewportRecovery_||activationRecovery_))diagnosticFailure_="runtime_recovery_required";
          else if(diagnosticFailure_.empty()&&streamProfileOwner_.empty())diagnosticFailure_="native_stream_profile_recovery_required";
          snapshot(controller_->state().leaseActive,viewportRecovery_);}
        else{diagnosticFailure_="native_stream_profile_recovery_required";snapshot(false,true);}return;
      }
      if(pending_&&streamMonotonicMillis()>=pending_->deadline)abandonLocked();
      if(countPending_&&streamMonotonicMillis()>=countPending_->deadline)abandonCountLocked();
      // The idle path: nothing holds the display. A waiting re-arm stops the
      // controller here and returns; the next pass is the ordinary cold start.
      if(renderRearmPending_&&performRenderRearmLocked(rearm))return;
      if(!controller_){
        if(!loadRuntimeLocked(config)){if(diagnosticFailure_.empty())diagnosticFailure_="runtime_recovery_required";snapshot(false,true);return;}
        const bool recovering=viewportRecovery_||activationRecovery_;
        if(recovering){
          if(diagnosticFailure_.empty())diagnosticFailure_="runtime_recovery_required";
          const auto now=recoveryNow();if(now<recoveryNextAttempt_){snapshot(false,true);return;}
          recoveryNextAttempt_=now+recoveryBackoff_;recoveryBackoff_=std::min<DWORD>(30000,recoveryBackoff_*2);
        }
        std::unique_ptr<multitap::vdisplay::Controller> controller;
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
        if(factoryFixture_){
          if(recovering&&(!quiescentFixture_||!quiescentFixture_())){snapshot(false,true);return;}
          // The same one-time cold hint as the production path below, from the fixture.
          if(!renderPreferenceAttempted_){renderPreferenceAttempted_=true;renderPreference_=renderPreferenceFixture_?renderPreferenceFixture_():NativeRenderPreference{};}
          controller=factoryFixture_();
        }else
#endif
        {
        auto connection=multitap::vdisplay::openOwnedInstance(wide(config.get("deviceInstanceId").string()));if(!connection){diagnosticFailure_="transport_open_failed";snapshot(false,true);return;}
        const auto identityState=identity();const auto& owner=identityState.value;
        publicIdentities_[0]=owner.displays[0];publicIdentities_[1]=owner.displays[1];
        transport_=std::move(connection);client_=std::make_unique<multitap::vdisplay::api::ControlClient>(*transport_);
        if(recovering&&!multitap::vdisplay::quiescentOwnedLease(*client_,owner.lease)){
          client_.reset();transport_.reset();snapshot(false,true);return;
        }
        // Never rebind a live/reclaimed controller; this is a one-time cold hint.
        if(!renderPreferenceAttempted_){renderPreferenceAttempted_=true;renderPreference_=preferNativeRenderAdapter(*client_,chooseNativeRenderHardware);}
        controller=std::make_unique<multitap::vdisplay::Controller>(*client_,owner.lease,owner.owner);
        }
        std::vector<multitap::vdisplay::Output> outputs;for(size_t i=0;i<committed_.size();++i)outputs.push_back({publicIdentities_[i],committed_[i].width,committed_[i].height});
        if(generation_==UINT32_MAX)throw std::runtime_error("native_display_generation_exhausted");
        if(!persistRuntimeLocked("starting",committed_,generation_+1)){viewportRecovery_=true;snapshot(false,true);return;}++generation_;
        const bool started=controller->start(outputs);if(!started){diagnosticFailure_="controller_start_failed";controller.reset();client_.reset();transport_.reset();viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);snapshot(false,true);return;}outputs_=outputs;controller_=std::move(controller);activationRecovery_=false;activationWorkerExited_=false;viewportRecovery_=false;
      }
      if(!controller_->heartbeat()){diagnosticFailure_="heartbeat_failed";viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);pending_.reset();controller_->stop();snapshot(false,true);controller_.reset();client_.reset();transport_.reset();return;}
      if(v2_)selfHealV2Locked();
      retryActivationLocked(config);
      if(activationRecovery_||viewportRecovery_){if(diagnosticFailure_.empty())diagnosticFailure_="runtime_recovery_required";}else diagnosticFailure_.clear();
      snapshot(true,false);
      bootTimeline().mark(BootStage::display);  // boot-ready-v1: controller running, lease alive
    }catch(...){diagnosticFailure_="internal_failure";viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);snapshot(false,true);}
  }
  bool reconfigure(std::span<const multitap::vdisplay::Output> outputs){
    std::lock_guard lock(mutex_);
    if(profileBlockedLocked())return false;
    if(pending_||countPending_||activationRecovery_||viewportRecovery_)return false;
    if(outputs.empty()||outputs.size()>2)return false;
    for(size_t i=0;i<outputs.size();++i)if(outputs[i].identity!=publicIdentities_[i])return false;
    const bool changed=std::vector<multitap::vdisplay::Output>(outputs.begin(),outputs.end())!=outputs_;const bool ok=applyOutputsLocked(outputs);if(changed){viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);}snapshot(controller_&&controller_->state().leaseActive,viewportRecovery_);return ok;
  }
  NativeViewportReply prepareViewport(const StreamResize& request,const std::string& expectedLease,int64_t deadline,const std::string& checkpoint={},const std::optional<NativeRecoveryScope>& expectedScope={}){
    std::lock_guard lock(mutex_);
    const auto profile=profileResizeRecordLocked(request);if(profileBlockedLocked()&&!profile)return replyLocked(false,false);
    if(!nativeResizeValid(request)||!multitap::vdisplay::ownedOutputModeSupported(uint32_t(request.width),uint32_t(request.height))||pending_||countPending_||activationRecovery_||viewportRecovery_||!controller_||!runtime_.record()||runtime_.record()->phase!="stable"||deadline<=streamMonotonicMillis()||deadline>streamMonotonicMillis()+streamResizeTimeoutMs)return replyLocked(false,false);
    bool attempted=false;try{
      const auto current=stateJsonLocked(true,false);if(!nativeConfigMatchesLease(configuration(),JsonParser::parse(current))||nativeDisplayLeaseKey(JsonParser::parse(current))!=expectedLease)return replyLocked(false,false);
      const size_t index=request.monitor=="0"?0:1;if(index>=outputs_.size())return replyLocked(false,false);
      const auto saved=parseNativeRecoveryBaseline(checkpoint);const auto targets=nativeDisplayTargets(JsonParser::parse(current));if(!saved.scope||!expectedScope||!nativeRecoveryLogonMatches(*saved.scope,*expectedScope)||expectedScope->tick-saved.scope->tick>streamResizeTimeoutMs||saved.instance!=instance_||saved.generation!=generation_||saved.topology.replacedMonitor!=request.monitor||saved.topology.targets.size()!=targets.size())return replyLocked(false,false);
      for(size_t i=0;i<targets.size();++i){const auto& a=saved.topology.targets[i];const auto& b=targets[i];if(a.id!=b.id||!nativeSameLuid(a.adapter,b.adapter)||a.target!=b.target||a.width!=b.width||a.height!=b.height)return replyLocked(false,false);}
      baseline_.load(instance_);if(!baseline_.store(saved)){viewportRecovery_=true;snapshot(true,true);return replyLocked(false,false);}recoveryHash_=baseline_.digest();recoveryBaseline_=saved;
      auto desired=outputs_;desired[index].width=uint32_t(request.width);desired[index].height=uint32_t(request.height);
      pending_=PendingViewport{request,outputs_,deadline,generation_,false,false};
      pending_->profile=profile;
      if(!persistRuntimeLocked("changing",committed_,generation_)){pending_.reset();viewportRecovery_=true;snapshot(true,true);return replyLocked(false,false);}
      const bool ok=applyOutputsLocked(desired,&attempted);pending_->generation=generation_;pending_->prepared=ok;
      if(!ok&&controller_->state().recoveryRequired)viewportRecovery_=true;
      if(!snapshot(controller_->state().leaseActive,viewportRecovery_)){abandonLocked();return replyLocked(false,attempted);}
      return replyLocked(ok,attempted);
    }catch(...){if(pending_)abandonLocked();return replyLocked(false,attempted);}
  }
  NativeViewportReply rollbackViewport(const StreamResize& request){
    std::lock_guard lock(mutex_);
    if(!pending_||pending_->tracks||!nativeSameResize(pending_->request,request))return replyLocked(false,false);
    return rollbackViewportLocked();
  }
  NativeViewportReply confirmUnchangedViewport(const NativeStreamCancelRequest& request,const std::string& expectedLease,int64_t deadline,const std::string& checkpoint,const std::optional<NativeRecoveryScope>& expectedScope){
    std::lock_guard lock(mutex_);
    try{
      (void)nativeStreamCancelRequestFields(request);
      const auto profile=std::visit([&](const auto& value){return profileResizeRecordLocked(value);},request);
      if(!profile||pending_||countPending_||activationRecovery_||viewportRecovery_||!controller_||!expectedScope||deadline<=streamMonotonicMillis()||deadline>streamMonotonicMillis()+streamResizeTimeoutMs||!controller_->leaseCurrent())return replyLocked(false,false);
      const auto state=JsonParser::parse(stateJsonLocked(true,false));if(!nativeConfigMatchesLease(configuration(),state)||nativeDisplayLeaseKey(state)!=expectedLease)return replyLocked(false,false);
      const auto runtimeBytes=runtime_.verifiedBytes();if(!runtimeBytes)return replyLocked(false,false);const auto runtime=parseNativeRuntimeRecord(*runtimeBytes);
      if(runtime.instance!=instance_||runtime.phase!="stable"||runtime.generation!=generation_||!runtime.baselineHash.empty()||runtime.outputs!=publicOutputsLocked())return replyLocked(false,false);
      const auto saved=parseNativeRecoveryBaseline(checkpoint);if(!saved.scope||!nativeRecoveryLogonMatches(profile->scope,*expectedScope)||expectedScope->tick<saved.scope->tick||expectedScope->tick-saved.scope->tick>streamResizeTimeoutMs)return replyLocked(false,false);
      NativeStreamViewportCancel receipt;receipt.request=request;receipt.instance=instance_;receipt.profileOperation=profile->key.operation;receipt.resizeOperation=profile->resize->operation;receipt.profileBeforeBytes=nativeStreamProfileRecordBytes(*profile);receipt.profileBeforeSha256=nativeBaselineDigest(receipt.profileBeforeBytes);receipt.baselineBytes=checkpoint;receipt.baselineSha256=nativeBaselineDigest(checkpoint);receipt.runtimeSha256=nativeBaselineDigest(*runtimeBytes);receipt.current=nativeStreamProfileLeaseFromStateLocked();receipt.scope=*saved.scope;receipt.scope.tick=expectedScope->tick;
      const auto controller=controller_->state();if(!controller.leaseActive||controller.recoveryRequired||controller.targets.size()!=outputs_.size()||controller.targets.size()!=receipt.current.targets.size())return replyLocked(false,false);
      for(size_t i=0;i<outputs_.size();++i){const auto& output=outputs_[i];const auto target=std::find_if(controller.targets.begin(),controller.targets.end(),[&](const auto& item){return item.display_id==output.identity;});const auto& published=receipt.current.targets[i];
        if(output.identity!=publicIdentities_[i]||target==controller.targets.end()||(receipt.lease&&receipt.lease!=target->lease_id)||published.id!=std::to_string(i)||published.width!=output.width||published.height!=output.height||published.adapter.LowPart!=target->os_adapter_luid.low_part||published.adapter.HighPart!=target->os_adapter_luid.high_part||published.target!=target->target_id)return replyLocked(false,false);
        receipt.lease=target->lease_id;receipt.outputs.push_back({target->display_id,target->connector_index});
      }
      auto bytes=nativeStreamViewportCancelBytes(receipt);const auto profiles=profileViewportStore();if(profiles.load()!=std::optional(receipt.profileBeforeBytes))return replyLocked(false,false);
      const auto store=profileViewportReceiptStore();const auto previous=store.load();
      if(previous)try{
        const auto old=parseNativeStreamViewportCancel(*previous);bool outputs=old.lease==receipt.lease&&old.outputs.size()==receipt.outputs.size();for(size_t i=0;outputs&&i<old.outputs.size();++i)outputs=old.outputs[i].display==receipt.outputs[i].display&&old.outputs[i].connector==receipt.outputs[i].connector;
        if(old.profileBeforeBytes==receipt.profileBeforeBytes&&nativeStreamCancelRequestFields(old.request)==nativeStreamCancelRequestFields(request)&&receipt.scope.tick<old.scope.tick)return replyLocked(false,false);
        if(outputs&&nativeStreamViewportCancelProof(*profile,{profile->key,receipt.current,receipt.scope,profile->console,saved.topology.ccd},request,old,*runtimeBytes))bytes=*previous;
      }catch(...){ }
      // No SDK configure/runtime/baseline write. A lost save ACK leaves the
      // reservation pending; recovery may inspect the exact durable receipt.
      if(previous!=std::optional(bytes)&&!store.save(previous,bytes))return replyLocked(false,false);
      if(store.load()!=std::optional(bytes)||profiles.load()!=std::optional(receipt.profileBeforeBytes)||runtime_.verifiedBytes()!=runtimeBytes||streamMonotonicMillis()>=deadline||!controller_->leaseCurrent()||nativeDisplayLeaseKey(JsonParser::parse(stateJsonLocked(true,false)))!=expectedLease||!nativeConfigMatchesLease(configuration(),JsonParser::parse(stateJsonLocked(true,false))))return replyLocked(false,false);
      return replyLocked(true,false);
    }catch(...){return replyLocked(false,false);}
  }
  NativeViewportReply finishViewport(const StreamResize& request,const std::string& expectedLease,bool rollback,bool windowsVerified,const std::optional<NativeRecoveryScope>& expectedScope={}){
    std::lock_guard lock(mutex_);
    if(!pending_||pending_->tracks||!nativeSameResize(pending_->request,request))return replyLocked(false,false);
    return finishViewportLocked(expectedLease,rollback,windowsVerified,expectedScope);
  }
  NativeViewportReply prepareTrackViewport(const NativeTrackResize& request,const std::string& expectedLease,int64_t deadline,const std::string& checkpoint,const std::optional<NativeRecoveryScope>& expectedScope){
    std::lock_guard lock(mutex_);
    if(std::any_of(request.outputs.begin(),request.outputs.end(),[](const auto& output){return !multitap::vdisplay::ownedOutputModeSupported(output.width,output.height);}))return replyLocked(false,false);
    const auto profile=profileResizeRecordLocked(request);if(profileBlockedLocked()&&!profile)return replyLocked(false,false);
    if(!nativeTrackResizeValid(request)||pending_||countPending_||activationRecovery_||viewportRecovery_||!controller_||!runtime_.record()||runtime_.record()->phase!="stable"||deadline<=streamMonotonicMillis()||deadline>streamMonotonicMillis()+streamResizeTimeoutMs)return replyLocked(false,false);
    bool attempted=false;
    try{
      const auto state=JsonParser::parse(stateJsonLocked(true,false));if(!nativeConfigMatchesLease(configuration(),state)||nativeDisplayLeaseKey(state)!=expectedLease)return replyLocked(false,false);
      const auto saved=parseNativeRecoveryBaseline(checkpoint);const auto targets=nativeDisplayTargets(state);
      if(!saved.scope||!expectedScope||!nativeRecoveryLogonMatches(*saved.scope,*expectedScope)||expectedScope->tick-saved.scope->tick>streamResizeTimeoutMs||saved.instance!=instance_||saved.generation!=generation_||saved.topology.targets.size()!=targets.size())return replyLocked(false,false);
      for(size_t i=0;i<targets.size();++i){const auto& a=saved.topology.targets[i];const auto& b=targets[i];if(a.id!=b.id||!nativeSameLuid(a.adapter,b.adapter)||a.target!=b.target||a.width!=b.width||a.height!=b.height)return replyLocked(false,false);}
      const auto group=planNativeTrackViewport(saved.topology,request.outputs);auto desired=outputs_;
      if(group.targets.size()!=desired.size())return replyLocked(false,false);
      for(size_t i=0;i<desired.size();++i){desired[i].width=group.targets[i].width;desired[i].height=group.targets[i].height;}
      baseline_.load(instance_);if(!baseline_.store(saved)){viewportRecovery_=true;snapshot(true,true);return replyLocked(false,false);}recoveryHash_=baseline_.digest();recoveryBaseline_=saved;
      StreamResize owner;owner.sessionId=request.sessionId;owner.requestId=request.requestId;owner.resizeId=request.resizeId;
      pending_=PendingViewport{owner,outputs_,deadline,generation_,false,false,request};
      pending_->profile=profile;
      if(!persistRuntimeLocked("changing",committed_,generation_)){pending_.reset();viewportRecovery_=true;snapshot(true,true);return replyLocked(false,false);}
      const bool ok=applyOutputsLocked(desired,&attempted);pending_->generation=generation_;pending_->prepared=ok;
      if(!ok&&controller_->state().recoveryRequired)viewportRecovery_=true;
      if(!snapshot(controller_->state().leaseActive,viewportRecovery_)){abandonLocked();return replyLocked(false,attempted);}
      return replyLocked(ok,attempted);
    }catch(...){if(pending_)abandonLocked();return replyLocked(false,attempted);}
  }
  NativeViewportReply rollbackTrackViewport(const NativeTrackResize& request){
    std::lock_guard lock(mutex_);if(!pending_||!pending_->tracks||!nativeSameTrackResize(*pending_->tracks,request))return replyLocked(false,false);return rollbackViewportLocked();
  }
  NativeViewportReply finishTrackViewport(const NativeTrackResize& request,const std::string& expectedLease,bool rollback,bool windowsVerified,const std::optional<NativeRecoveryScope>& expectedScope){
    std::lock_guard lock(mutex_);if(!pending_||!pending_->tracks||!nativeSameTrackResize(*pending_->tracks,request))return replyLocked(false,false);return finishViewportLocked(expectedLease,rollback,windowsVerified,expectedScope);
  }
  NativeViewportReply prepareCount(const NativeCountRequest& request,const std::string& expectedLease,int64_t deadline,const std::string& checkpoint,const std::optional<NativeRecoveryScope>& expectedScope){
    std::lock_guard lock(mutex_);
    if(profileBlockedLocked())return replyLocked(false,false);
    if(!nativeCountValid(request)||pending_||countPending_||activationRecovery_||viewportRecovery_||!controller_||!runtime_.record()||runtime_.record()->phase!="stable"||outputs_.size()==size_t(request.count)||deadline<=streamMonotonicMillis()||deadline>streamMonotonicMillis()+nativeCountTimeoutMs)return replyLocked(false,false);
    bool attempted=false;
    try{
      const auto state=JsonParser::parse(stateJsonLocked(true,false));
      if(!nativeConfigMatchesLease(configuration(),state)||nativeDisplayLeaseKey(state)!=expectedLease)return replyLocked(false,false);
      const auto saved=parseNativeRecoveryBaseline(checkpoint);const auto targets=nativeDisplayTargets(state);
      const auto replaced=request.count==1?"1":"0";
      if(!saved.scope||!expectedScope||!nativeRecoveryLogonMatches(*saved.scope,*expectedScope)||expectedScope->tick-saved.scope->tick>nativeCountTimeoutMs||saved.instance!=instance_||saved.generation!=generation_||saved.topology.replacedMonitor!=replaced||saved.topology.targets.size()!=targets.size())return replyLocked(false,false);
      for(size_t i=0;i<targets.size();++i){const auto& a=saved.topology.targets[i];const auto& b=targets[i];if(a.id!=b.id||!nativeSameLuid(a.adapter,b.adapter)||a.target!=b.target||a.width!=b.width||a.height!=b.height)return replyLocked(false,false);}
      baseline_.load(instance_);if(!baseline_.store(saved)){viewportRecovery_=true;snapshot(true,true);return replyLocked(false,false);}
      recoveryHash_=baseline_.digest();recoveryBaseline_=saved;
      auto desired=outputs_;
      if(request.count==1)desired.resize(1);
      else desired.push_back({publicIdentities_[1],outputs_[0].width,outputs_[0].height});
      countPending_=PendingCount{request,outputs_,deadline,generation_,false,false};
      if(!persistRuntimeLocked("changing",committed_,generation_)){countPending_.reset();viewportRecovery_=true;snapshot(true,true);return replyLocked(false,false);}
      const bool ok=applyOutputsLocked(desired,&attempted);countPending_->generation=generation_;countPending_->prepared=ok;
      if(!ok&&controller_->state().recoveryRequired)viewportRecovery_=true;
      if(!snapshot(controller_->state().leaseActive,viewportRecovery_)){abandonCountLocked();return replyLocked(false,attempted);}
      return replyLocked(ok,attempted);
    }catch(...){if(countPending_)abandonCountLocked();return replyLocked(false,attempted);}
  }
  NativeViewportReply rollbackCount(const NativeCountRequest& request){
    std::lock_guard lock(mutex_);
    if(!countPending_||!nativeSameCount(countPending_->request,request)||countPending_->generation!=generation_||!controller_)return replyLocked(false,false);
    if(countPending_->rollingBack){const bool valid=!viewportRecovery_&&controller_->state().leaseActive&&!controller_->state().recoveryRequired;return replyLocked(snapshot(controller_->state().leaseActive,viewportRecovery_)&&valid,false);}
    countPending_->completed=false;
    bool attempted=false;const bool ok=applyOutputsLocked(countPending_->before,&attempted);countPending_->generation=generation_;countPending_->rollingBack=true;
    if(!ok)viewportRecovery_=true;const bool published=snapshot(controller_->state().leaseActive,viewportRecovery_);return replyLocked(ok&&published,attempted);
  }
  NativeViewportReply finishCount(const NativeCountRequest& request,const std::string& expectedLease,bool rollback,bool windowsVerified,const std::optional<NativeRecoveryScope>& expectedScope){
    std::lock_guard lock(mutex_);
    if(!countPending_||!nativeSameCount(countPending_->request,request)||countPending_->generation!=generation_||countPending_->rollingBack!=rollback||(!rollback&&!countPending_->prepared)||!windowsVerified||viewportRecovery_||!controller_||!recoveryBaseline_||!recoveryBaseline_->scope||!expectedScope||!nativeRecoveryLogonMatches(*recoveryBaseline_->scope,*expectedScope)||streamMonotonicMillis()>=countPending_->deadline)return replyLocked(false,false);
    try{const auto state=JsonParser::parse(stateJsonLocked(true,false));if(!nativeConfigMatchesLease(configuration(),state)||nativeDisplayLeaseKey(state)!=expectedLease||outputs_.size()!=(rollback?countPending_->before.size():size_t(request.count)))return replyLocked(false,false);}catch(...){return replyLocked(false,false);}
    const auto completed=publicOutputsLocked();
    if(!persistRuntimeLocked("stable",completed,generation_)){viewportRecovery_=true;snapshot(true,true);return replyLocked(false,false);}
    countPending_->completed=true;
    if(!snapshot(controller_->state().leaseActive,false)){countPending_->completed=false;if(!persistRuntimeLocked("changing",committed_,generation_))viewportRecovery_=true;return replyLocked(false,false);}
    return replyLocked(true,false);
  }
  bool acknowledgeCountOperation(const NativeCountRequest& request,bool succeeded){
    std::lock_guard lock(mutex_);
    if(!nativeCountValid(request)||pending_||activationRecovery_||viewportRecovery_||!controller_||!controller_->state().leaseActive||!runtime_.record()||runtime_.record()->phase!="stable")return false;
    if(!countPending_)return !succeeded||outputs_.size()==size_t(request.count);
    if(!nativeSameCount(countPending_->request,request)||!countPending_->completed||countPending_->generation!=generation_||countPending_->rollingBack==succeeded)return false;
    const auto completed=publicOutputsLocked();
    if(!persistRuntimeLocked("stable",completed,generation_)){viewportRecovery_=true;snapshot(true,true);return false;}
    auto pending=*countPending_;countPending_.reset();
    if(!snapshot(true,false)){countPending_=std::move(pending);return false;}
    committed_=completed;recoveryHash_.clear();recoveryBaseline_.reset();return true;
  }
  void cancelCountOperation(const DisplayOperationKey& key)noexcept{
    std::lock_guard lock(mutex_);if(countPending_&&countPending_->request.operation.id==key.id&&countPending_->request.operation.nonce==key.nonce)abandonCountLocked();
  }
  void cancelViewportSession(const std::string& session,bool uncertain=false)noexcept{
    std::lock_guard lock(mutex_);if(pending_&&pending_->request.sessionId==session)abandonLocked();else if(uncertain&&!pending_&&!countPending_&&controller_){viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);snapshot(controller_->state().leaseActive,true);}
  }
  std::string viewportSnapshot(){std::lock_guard lock(mutex_);return stateJsonLocked(controller_&&controller_->state().leaseActive,false);}
  std::optional<NativeOwnedStreamLeaseProof> proveOwnedStreamLease(const std::string& stream,const std::string& instance,uint32_t generation){
    std::lock_guard lock(mutex_);
    try{
      if(preparation_||!v2_||!nativeStreamJournalId(stream)||streamProfileOwner_!=stream||instance_.empty()||instance_!=instance||
         !generation||generation_!=generation||pending_||countPending_||activationRecovery_||viewportRecovery_||
         !controller_||!runtimeLoaded_||!runtime_.record()||runtime_.record()->phase!="stable"||!controller_->leaseCurrent())return {};
      const auto snapshot=JsonParser::parse(stateJsonLocked(true,false));
      if(!nativeConfigMatchesLease(configuration(),snapshot)||!snapshot.get("leaseActive").boolean()||
         !snapshot.get("activationVerified").boolean()||snapshot.get("configurationPending").boolean()||snapshot.get("recoveryRequired").boolean())return {};
      auto targets=nativeDisplayTargets(snapshot);
      if(targets.empty()||targets.size()>2||targets.size()!=outputs_.size())return {};
      const auto service=nativeStreamProcessIdentity(GetCurrentProcess());
      if(!service.pid||!service.created||!controller_->leaseCurrent())return {};
      return NativeOwnedStreamLeaseProof{stream,instance_,service,generation_,std::move(targets)};
    }catch(...){return {};}
  }
  void activationRecovery(uint32_t generation,bool workerExited=false)noexcept{
    std::lock_guard lock(mutex_);if(!controller_||!generation||generation!=generation_)return;
    if(!activationRecovery_){
      recoveryNextAttempt_=recoveryNow()+recoveryBackoff_;
      activationWorkerExited_=workerExited;
    }else{
      // A subsequent read-only probe owns no activation worker. It must not
      // revoke the signalled-process proof from this recovery episode. The
      // proof is reset when a new episode/generation starts.
      activationWorkerExited_=activationWorkerExited_||workerExited;
    }
    activationRecovery_=true;
    if(!persistRuntimeLocked("recovery",committed_,generation_))viewportRecovery_=true;
    snapshot(controller_->state().leaseActive,true);
  }
  bool activationReady(uint32_t generation)noexcept{std::lock_guard lock(mutex_);if(preparation_||profileBlockedLocked()||!controller_||generation!=generation_||activationRecovery_||viewportRecovery_||pending_||countPending_||!controller_->state().leaseActive)return false;if(!persistRuntimeLocked("stable",committed_,generation_)||!snapshot(true,false)){viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);snapshot(true,true);return false;}recoveryHash_.clear();recoveryBaseline_.reset();recoveryNextAttempt_=0;recoveryBackoff_=1000;return true;}
 private:
  struct PendingViewport {StreamResize request;std::vector<multitap::vdisplay::Output> before;int64_t deadline=0;uint32_t generation=0;bool rollingBack=false,prepared=false;std::optional<NativeTrackResize> tracks;std::optional<NativeStreamProfileRecord> profile;};
  struct PendingCount {NativeCountRequest request;std::vector<multitap::vdisplay::Output> before;int64_t deadline=0;uint32_t generation=0;bool rollingBack=false,prepared=false,completed=false;};
  std::mutex mutex_;
  std::string streamProfileOwner_;
  struct Preparation {
    std::string stream,phase;
    bool forwardFailed=false,commitPending=false;
    uint32_t cleanupGeneration=0;
    std::optional<std::string> cleanupRuntime;
    std::optional<NativeDisplayTarget> cleanupTarget;
  };
  std::optional<Preparation> preparation_;
  std::optional<NativePreparationLeaseProof> preparationProofLocked(const std::string& stream,const std::string& instance,uint32_t generation){
    try{
      if(!preparation_||preparation_->forwardFailed||preparation_->commitPending||preparation_->stream!=stream||streamProfileOwner_!=stream||!v2_||
          instance_!=instance||!generation||generation_!=generation||pending_||countPending_||
          activationRecovery_||viewportRecovery_||!controller_||!runtimeLoaded_||!runtime_.record()||
          runtime_.record()->phase!=preparation_->phase||runtime_.record()->instance!=instance_||
          runtime_.record()->generation!=generation_||runtime_.record()->outputs!=committed_||
          publicOutputsLocked()!=committed_||outputs_.size()!=1||!runtime_.verifiedBytes()||!controller_->leaseCurrent())return {};
      const auto state=controller_->state();if(!state.leaseActive||state.recoveryRequired)return {};
      const auto value=JsonParser::parse(stateJsonLocked(true,false));
      if(!nativeConfigMatchesLease(configuration(),value)||value.get("recoveryRequired").boolean()||value.get("configurationPending").boolean())return {};
      auto targets=nativeDisplayTargets(value);if(targets.size()!=1||targets.front().id!="0"||!controller_->leaseCurrent())return {};
      return NativePreparationLeaseProof{stream,instance_,nativeStreamProcessIdentity(GetCurrentProcess()),generation_,std::move(targets),preparation_->phase};
    }catch(...){return {};}
  }
  std::optional<NativePreparationCleanupLeaseProof> preparationCleanupProofLocked(const std::string& stream,
      const std::string& instance,uint32_t generation){
    try{
      if(!preparation_||preparation_->stream!=stream||streamProfileOwner_!=stream||!v2_||instance_!=instance||
          !generation||generation_!=generation||pending_||countPending_||activationRecovery_||!controller_||
          !runtimeLoaded_||!runtime_.record()||runtime_.record()->instance!=instance_||
          runtime_.record()->generation!=generation_||outputs_.size()!=1||!controller_->leaseCurrent())return {};
      const auto durable=runtime_.verifiedBytes();if(!durable)return {};
      const auto state=controller_->state();if(!state.leaseActive||state.recoveryRequired)return {};
      const auto value=JsonParser::parse(stateJsonLocked(true,viewportRecovery_));
      if(!nativeConfigMatchesLease(configuration(),value)||value.get("configurationPending").boolean())return {};
      auto targets=nativeDisplayTargets(value);if(targets.size()!=1||targets.front().id!="0")return {};
      if(preparation_->commitPending){
        if(!viewportRecovery_||preparation_->cleanupGeneration!=generation_||!preparation_->cleanupRuntime||
            *durable!=*preparation_->cleanupRuntime||!preparation_->cleanupTarget||
            (runtime_.record()->phase!=preparation_->phase&&runtime_.record()->phase!="changing"))return {};
        const auto& a=targets.front();const auto& b=*preparation_->cleanupTarget;
        if(a.id!=b.id||!nativeSameLuid(a.adapter,b.adapter)||a.target!=b.target||a.width!=b.width||a.height!=b.height)return {};
      }else if(viewportRecovery_||runtime_.record()->phase!=preparation_->phase||
          runtime_.record()->outputs!=committed_||publicOutputsLocked()!=committed_)return {};
      if(!controller_->leaseCurrent())return {};
      return NativePreparationCleanupLeaseProof{stream,instance_,nativeStreamProcessIdentity(GetCurrentProcess()),generation_,std::move(targets),preparation_->phase};
    }catch(...){return {};}
  }
  void preparationCommitFailedLocked()noexcept{
    viewportRecovery_=true;
    if(!preparation_)return;
    preparation_->forwardFailed=true;preparation_->commitPending=false;
    preparation_->cleanupRuntime.reset();preparation_->cleanupTarget.reset();preparation_->cleanupGeneration=0;
    try{
      if(!controller_||!runtime_.record()||runtime_.record()->instance!=instance_||
          runtime_.record()->generation!=generation_||
          (runtime_.record()->phase!=preparation_->phase&&runtime_.record()->phase!="changing")||
          outputs_.size()!=1||!controller_->leaseCurrent())return;
      const auto state=controller_->state();if(!state.leaseActive||state.recoveryRequired)return;
      const auto durable=runtime_.verifiedBytes();if(!durable)return;
      const auto targets=nativeDisplayTargets(JsonParser::parse(stateJsonLocked(true,true)));if(targets.size()!=1||targets.front().id!="0")return;
      preparation_->cleanupRuntime=*durable;preparation_->cleanupTarget=targets.front();
      preparation_->cleanupGeneration=generation_;preparation_->commitPending=true;
    }catch(...){ }
  }
  bool preparationSizeLocked(DWORD width,DWORD height,bool cleanup){
    try{
      if(!ownedOutputSize(width,height)||!multitap::vdisplay::ownedOutputModeSupported(width,height))return false;
      if(cleanup)preparation_->forwardFailed=true;
      auto desired=outputs_;desired.front().width=width;desired.front().height=height;
      const bool applied=desired==outputs_||applyOutputsLocked(desired);
      const auto actual=controller_->state();
      if(!actual.leaseActive||actual.recoveryRequired||!controller_->leaseCurrent()){
        preparation_->forwardFailed=true;viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);snapshot(false,true);return false;
      }
      committed_=publicOutputsLocked();
      // Preserve the admitted starting/stable phase. Both private purposes
      // remain non-ready while this reservation exists.
      if(!persistRuntimeLocked(preparation_->phase,committed_,generation_)){preparationCommitFailedLocked();return false;}
      const bool priorRecovery=viewportRecovery_;
      // Under this same mutex, stage the cleanup publication and roll back
      // its recovery bit on failure. commitPending stays set until the actual
      // durable runtime AND snapshot writes have succeeded.
      if(cleanup&&preparation_->commitPending)viewportRecovery_=false;
      if(!snapshot(true,false)){viewportRecovery_=priorRecovery;preparationCommitFailedLocked();return false;}
      if(!applied){preparation_->forwardFailed=true;if(viewportRecovery_)preparationCommitFailedLocked();return false;}
      if(cleanup){preparation_->commitPending=false;preparation_->cleanupRuntime.reset();preparation_->cleanupTarget.reset();preparation_->cleanupGeneration=0;}
      return true;
    }catch(...){if(preparation_)preparation_->forwardFailed=true;viewportRecovery_=true;return false;}
  }
  std::optional<NativeStreamReclaimCommit> reclaimCommit_;
  NativeStreamProfileLease nativeStreamProfileLeaseFromStateLocked(){const auto value=JsonParser::parse(stateJsonLocked(controller_&&controller_->state().leaseActive,false));return {GetCurrentProcessId(),generation_,nativeDisplayTargets(value)};}
  bool v2_=false;
  // v2: a live, healthy lease with a latched viewport-recovery flag is not an
  // unknown state any more (Windows restoration is the v2 state file's job).
  // Adopt the actual SDK outputs and re-enter the ordinary activation proof.
  void selfHealV2Locked()noexcept{
    try{
      if(!viewportRecovery_||pending_||countPending_||!controller_||!controller_->leaseCurrent())return;
      const auto state=controller_->state();if(!state.leaseActive||state.recoveryRequired)return;
      committed_=publicOutputsLocked();recoveryHash_.clear();recoveryBaseline_.reset();viewportRecovery_=false;
      activationRecovery_=true;activationWorkerExited_=true;recoveryNextAttempt_=0;
      if(!persistRuntimeLocked("recovery",committed_,generation_))viewportRecovery_=true;
    }catch(...){ }
  }
  // One implementation for the decision and its fixed diagnostic code.
  std::string profileHoldReasonLocked()const noexcept{
    if(preparation_)return "reservation";
    if(v2_)return "none";
    try{
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
      if(profilePendingFixture_)return profilePendingFixture_()?"profile_not_restored":"none";
      if(factoryFixture_)return "none";
#endif
      try{const auto retirement=nativeStreamRetirementMarkerAccess(machineDirectory()/L"private"/L"native-stream-retirement.json").load();if(retirement&&parseNativeStreamRetirement(*retirement).phase!="retired")return "retirement_prepared";}catch(...){return "retirement_invalid";}
      return nativeStreamLaunchQuarantineReason(nativeStreamProfileFileAccess(machineDirectory()/L"private"/L"native-stream-profile.json"),nativeStreamLaunchFileAccess(machineDirectory()/L"private"/L"native-stream-launch.json"),nativeStreamProcessIdentity(GetCurrentProcess()),[](const auto& process){return observeNativeStreamProcess(process).state;});
    }catch(...){return "records_unreadable";}
  }
  bool profileFilePendingLocked()const noexcept{try{return profileHoldReasonLocked()!="none";}catch(...){return true;}}
  bool profileBlockedLocked()const noexcept{return !streamProfileOwner_.empty()||profileFilePendingLocked();}
  NativeRuntimeJournal runtime_{nativeRuntimeFileAccess(machineDirectory()/L"private"/L"native-display-runtime.json")};
  NativeBaselineJournal baseline_{nativeBaselineFileAccess(machineDirectory()/L"private"/L"native-display-baseline.json")};
  std::string recoveryHash_;std::optional<NativeRecoveryBaseline> recoveryBaseline_;
  bool runtimeLoaded_=false;
  std::vector<NativeRuntimeOutput> committed_;
  uint64_t publicIdentities_[2]{};
  uint32_t generation_=0;
  std::optional<NativeActivationDiagnostic> activationDiagnostic_;uint32_t activationDiagnosticGeneration_=0;
  bool activationRecovery_=false;
  bool activationWorkerExited_=false;
  bool viewportRecovery_=false;
  std::optional<PendingViewport> pending_;
  std::optional<PendingCount> countPending_;
  std::string instance_;
  std::string diagnosticFailure_;
  std::optional<NativeStreamExpiredConsoleExecutor> expiredProfileExecutor_;std::string expiredProfileReservation_;
  uint64_t recoveryNextAttempt_=0;DWORD recoveryBackoff_=1000;
  std::vector<multitap::vdisplay::Output> outputs_;
  std::unique_ptr<multitap::vdisplay::api::WindowsControlTransport> transport_;
  std::unique_ptr<multitap::vdisplay::api::ControlClient> client_;
  bool renderPreferenceAttempted_=false;NativeRenderPreference renderPreference_;
  // Render adapter re-arm: used_ latches the one request of this lifetime,
  // pending_ waits for the idle tick, skip_ is the last recorded waiting reason.
  bool renderRearmUsed_=false,renderRearmPending_=false;uint32_t renderRearmSkip_=0;
  std::function<bool()> renderRearmIdle_;
  // Console park: parked until this recoveryNow() value unless renewed or ended.
  bool consolePark_=false;uint64_t consoleParkUntil_=0;
  struct RenderRearmProbe {bool waiting=false,hasProbe=false,idle=false,hardware=false;};
  // Before the mutex, see tick(). The hardware probe runs only on an idle display.
  RenderRearmProbe probeRenderRearm()noexcept{
    RenderRearmProbe result;
    try{
      std::function<bool()> idle;
      {std::lock_guard lock(mutex_);if(!renderRearmPending_)return result;result.waiting=true;idle=renderRearmIdle_;}
      if(!idle)return result;
      result.hasProbe=true;result.idle=idle();
      if(result.idle)result.hardware=renderRearmHardware();
    }catch(...){result.idle=false;result.hardware=false;}
    return result;
  }
  bool renderRearmHardware(){
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
    if(renderHardwareFixture_)return renderHardwareFixture_();
#endif
    return chooseNativeRenderHardware().has_value();
  }
  // A permanent reason ends the request; a waiting one keeps it for a later
  // tick and is recorded once per change, so a long stream logs one row.
  void skipRenderRearmLocked(uint32_t reason,bool permanent)noexcept{
    if(permanent)renderRearmPending_=false;
    if(renderRearmSkip_==reason)return;
    renderRearmSkip_=reason;
    recordHostEvent("hostAction",{{"action",nativeRenderRearmAction},{"stage",nativeRenderRearmSkipped},{"ok",false},{"error",reason}});
  }
  // Performed exactly like an orderly service stop (stopControllerLocked):
  // "changing" -> SDK stop -> "stopped", snapshot without a lease, controller/
  // client/transport released. The next tick is then the ordinary cold start,
  // which applies the render preference before it creates any output; on VM
  // 173 that is the path (Restart-Service) measured to bring NVIDIA back.
  // A recovery-marked stop was not chosen on purpose: "recovery" means the
  // lease's state is unknown and sends the next start through quiescence
  // checks and a growing backoff meant for a crashed owner, while here the
  // owner is this process holding a healthy lease. If the SDK stop itself
  // fails, stopControllerLocked persists "recovery" as shutdown would.
  // Fail closed: every doubt leaves the display as it is.
  bool performRenderRearmLocked(const RenderRearmProbe& probe)noexcept{
    try{
      if(!probe.waiting)return false;
      if(!probe.hasProbe){skipRenderRearmLocked(nativeRenderRearmNoIdleProbe,true);return false;}
      if(!probe.idle){skipRenderRearmLocked(nativeRenderRearmStreamActive,false);return false;}
      if(std::string(renderPreference_.status)!="accepted"){skipRenderRearmLocked(nativeRenderRearmPreferenceNotAccepted,true);return false;}
      if(!controller_||pending_||countPending_||activationRecovery_||viewportRecovery_||!runtimeLoaded_||!runtime_.record()||runtime_.record()->phase!="stable"){skipRenderRearmLocked(nativeRenderRearmOperationPending,false);return false;}
      if(!controller_->leaseCurrent()){skipRenderRearmLocked(nativeRenderRearmLeaseNotCurrent,false);return false;}
      if(!probe.hardware){skipRenderRearmLocked(nativeRenderRearmNoHardwareAdapter,true);return false;}
      renderRearmPending_=false;renderRearmSkip_=0;
      // The preference belongs to the controller being stopped; the cold start
      // applies a fresh one to the new transport before any output exists.
      renderPreferenceAttempted_=false;renderPreference_=NativeRenderPreference{};
      stopControllerLocked();
      recordHostEvent("hostAction",{{"action",nativeRenderRearmAction},{"stage",nativeRenderRearmPerformed},{"ok",!viewportRecovery_}});
      return true;
    }catch(...){renderRearmPending_=false;return false;}
  }
  std::unique_ptr<multitap::vdisplay::Controller> controller_;
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
  std::function<Json()> configFixture_;std::function<void(const std::string&)> publishFixture_;std::function<std::unique_ptr<multitap::vdisplay::Controller>()> factoryFixture_;std::function<bool()> quiescentFixture_;std::function<uint64_t()> recoveryClockFixture_;std::function<bool()> profilePendingFixture_;
  NativeRuntimeJournalAccess profileStoreFixture_,viewportReceiptFixture_;
  std::function<NativeRenderPreference()> renderPreferenceFixture_;std::function<bool()> renderHardwareFixture_;
#endif
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
  NativeResetIo resetIoFixture_;
#endif
  NativeResetPreflight resetPreflightLocked(){
    NativeResetPreflight result;
    if(pending_||countPending_){result.pending="change_pending";return result;}
    if(controller_){result.pending="controller_active";return result;}
    if(!streamProfileOwner_.empty()||expiredProfileExecutor_||reclaimCommit_){result.pending="reservation_held";return result;}
    if(!selected()||!nativeDisplayInstanceId(configuration().get("deviceInstanceId").string())){result.sdk="not_selected";return result;}
    auto io=resetIoLocked();const auto known=io.leases?io.leases():std::nullopt;if(!known){result.sdk="identity_unavailable";return result;}
    result.sdk=io.sdk?io.sdk(*known):std::string("present");return result;
  }
  NativeResetIo resetIoLocked(){
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
    if(resetIoFixture_.move||factoryFixture_)return resetIoFixture_;
#endif
    const auto directory=machineDirectory()/L"private";const auto instance=configuration().get("deviceInstanceId").string();
    NativeResetIo io;io.leases=[directory]{return nativeResetKnownLeases(directory);};
    io.sdk=[instance](const NativeResetLeaseSet& known){auto wire=multitap::vdisplay::openOwnedInstance(wide(instance));if(!wire)return std::string("transport_unavailable");multitap::vdisplay::api::ControlClient client(*wire);return nativeResetSdkCode(client,known);};
    io.move=[directory](size_t& moved){return moveNativeRecoveryRecords(directory,nativeResetStamp(),moved);};return io;
  }
  NativeRuntimeJournalAccess profileViewportStore(){
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
    if(factoryFixture_){if(profileStoreFixture_.load)return profileStoreFixture_;return {[]{return std::optional<std::string>{};},[](const auto&,const auto&){return false;}};}
#endif
    return nativeStreamProfileFileAccess(machineDirectory()/L"private"/L"native-stream-profile.json");
  }
  NativeRuntimeJournalAccess profileViewportReceiptStore(){
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
    if(factoryFixture_){if(viewportReceiptFixture_.load)return viewportReceiptFixture_;return {[]{return std::optional<std::string>{};},[](const auto&,const auto&){return false;}};}
#endif
    return nativeStreamViewportReceiptFileAccess(machineDirectory()/L"private"/L"native-stream-viewport-commit.json");
  }
  std::optional<NativeStreamProfileRecord> profileResizeRecordLocked(const StreamResize& request){
    return profileResizeRecordLocked(request.sessionId,nativeBaselineDigest(nativeResizeFields(request)));
  }
  std::optional<NativeStreamProfileRecord> profileResizeRecordLocked(const NativeTrackResize& request){
    if(!nativeTrackResizeValid(request)||request.topologyEpoch<2)return {};
    return profileResizeRecordLocked(request.sessionId,nativeBaselineDigest(nativeTrackResizeFields(request)));
  }
  std::optional<NativeStreamProfileRecord> profileResizeRecordLocked(const std::string& stream,const std::string& requestSha){
    try{
      if(streamProfileOwner_.empty()||streamProfileOwner_!=stream)return {};const auto bytes=profileViewportStore().load();if(!bytes)return {};const auto profile=parseNativeStreamProfileRecord(*bytes);
      if(profile.key.stream!=stream||profile.key.instance!=instance_||profile.phase!=NativeStreamProfilePhase::active||profile.stopRequested||!profile.resize||profile.resize->requestSha256!=requestSha||!sameNativeStreamLease(profile.current,nativeStreamProfileLeaseFromStateLocked()))return {};return profile;
    }catch(...){return {};}
  }
  bool saveProfileViewportCommitLocked(const std::vector<NativeRuntimeOutput>& completed,const NativeRecoveryScope& expectedScope){
    try{
      if(!pending_||!pending_->profile||!pending_->profile->resize||!recoveryBaseline_||!recoveryBaseline_->scope||!controller_)return false;
      const auto& profile=*pending_->profile;const auto profileBytes=nativeStreamProfileRecordBytes(profile);if(profileViewportStore().load()!=std::optional(profileBytes)||!nativeRecoveryLogonMatches(profile.scope,expectedScope))return false;
      if(pending_->tracks){
        NativeStreamTrackCommit receipt;receipt.request=*pending_->tracks;receipt.instance=instance_;receipt.profileOperation=profile.key.operation;receipt.resizeOperation=profile.resize->operation;receipt.profileBeforeSha256=nativeBaselineDigest(profileBytes);receipt.profileBeforeBytes=profileBytes;receipt.baselineSha256=recoveryHash_;receipt.before=profile.current;receipt.after=nativeStreamProfileLeaseFromStateLocked();receipt.rollback=pending_->rollingBack;receipt.scope=expectedScope;receipt.scope.foreign.clear();receipt.runtimeSha256=nativeBaselineDigest(nativeRuntimeBytes({instance_,"stable",generation_,completed,""}));
        receipt.placements=receipt.rollback?recoveryBaseline_->topology.placements:planNativeTrackViewport(recoveryBaseline_->topology,receipt.request.outputs).placements;
        const auto state=controller_->state();if(!state.leaseActive||state.recoveryRequired||state.targets.size()!=receipt.after.targets.size())return false;
        for(const auto& target:state.targets){if(receipt.lease&&receipt.lease!=target.lease_id)return false;receipt.lease=target.lease_id;receipt.outputs.push_back({target.display_id,target.connector_index});}
        if(!nativeStreamTrackCommitBaselineMatches(receipt,nativeRecoveryBaselineBytes(*recoveryBaseline_)))return false;
        const auto bytes=nativeStreamTrackCommitBytes(receipt);const auto store=profileViewportReceiptStore();const auto before=store.load();if(before!=std::optional(bytes))(void)store.save(before,bytes);return store.load()==std::optional(bytes)&&profileViewportStore().load()==std::optional(profileBytes);
      }
      NativeStreamViewportCommit receipt;receipt.request=pending_->request;receipt.instance=instance_;receipt.profileOperation=profile.key.operation;receipt.resizeOperation=profile.resize->operation;receipt.profileBeforeSha256=nativeBaselineDigest(profileBytes);receipt.profileBeforeBytes=profileBytes;receipt.baselineSha256=recoveryHash_;receipt.before=profile.current;receipt.after=nativeStreamProfileLeaseFromStateLocked();receipt.rollback=pending_->rollingBack;receipt.scope=expectedScope;receipt.scope.foreign.clear();receipt.runtimeSha256=nativeBaselineDigest(nativeRuntimeBytes({instance_,"stable",generation_,completed,""}));
      const auto state=controller_->state();if(!state.leaseActive||state.recoveryRequired||state.targets.size()!=receipt.after.targets.size())return false;
      for(const auto& target:state.targets){if(receipt.lease&&receipt.lease!=target.lease_id)return false;receipt.lease=target.lease_id;receipt.outputs.push_back({target.display_id,target.connector_index});}
      const auto bytes=nativeStreamViewportCommitBytes(receipt);const auto store=profileViewportReceiptStore();const auto before=store.load();if(before!=std::optional(bytes))(void)store.save(before,bytes);return store.load()==std::optional(bytes)&&profileViewportStore().load()==std::optional(profileBytes);
    }catch(...){return false;}
  }
  uint64_t recoveryNow()const{
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
    if(recoveryClockFixture_)return recoveryClockFixture_();
#endif
    return GetTickCount64();
  }
  void retryActivationLocked(const Json& config){
    if(profileBlockedLocked())return;
    if(!activationRecovery_||!activationWorkerExited_||viewportRecovery_||pending_||countPending_||!controller_||
       config.get("deviceInstanceId").string()!=instance_||!runtime_.record()||runtime_.record()->phase!="recovery"||
       runtime_.record()->generation!=generation_||runtime_.record()->outputs!=committed_||
       publicOutputsLocked()!=committed_||generation_==UINT32_MAX)return;
    const auto now=recoveryNow();if(now<recoveryNextAttempt_)return;
    recoveryNextAttempt_=now+recoveryBackoff_;recoveryBackoff_=std::min<DWORD>(30000,recoveryBackoff_*2);
    const auto state=controller_->state();
    if(!state.leaseActive||state.recoveryRequired||!controller_->leaseCurrent())return;
    // The previous Windows worker has exited and the actual driver still owns
    // the committed output count. Re-verify existing targets: no remove/create,
    // no lease release, no old foreign topology replay, and no readiness claim.
    if(!persistRuntimeLocked("starting",committed_,generation_+1)){viewportRecovery_=true;return;}
    ++generation_;activationRecovery_=false;activationWorkerExited_=false;
  }
  Json configuration(bool* present=nullptr){
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
    if(configFixture_){if(present)*present=true;return configFixture_();}
#endif
    return nativeDisplayConfig(present);
  }
  bool selected(){
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
    if(configFixture_)return nativeProviderSelected(configFixture_(),true);
#endif
    return nativeDisplaySelected();
  }
  bool loadRuntimeLocked(const Json& config){
    const auto instance=config.get("deviceInstanceId").string();if(runtimeLoaded_)return instance==instance_;
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
    if(!configFixture_)
#endif
    prepareMachineDirectory();
    const auto count=config.get("monitors").number(1);if(count!=1&&count!=2)throw std::runtime_error("native_display_count_invalid");
    // The installer count is an initial default. A verified durable runtime
    // record owns later client choices, including across service updates.
    const auto previous=runtime_.load(instance);instance_=instance;if(previous){generation_=previous->generation;committed_=previous->outputs;viewportRecovery_=previous->uncertain();recoveryHash_=previous->baselineHash;if(!recoveryHash_.empty()){
      // v2 restores Windows from its own state file; a stale v1 checkpoint
      // binding must never keep the controller from starting.
      const auto bound=[&]{try{recoveryBaseline_=baseline_.load(instance);if(!recoveryBaseline_||baseline_.digest()!=recoveryHash_||recoveryBaseline_->generation>generation_||recoveryBaseline_->topology.targets.size()!=committed_.size())return false;for(size_t i=0;i<committed_.size();++i)if(recoveryBaseline_->topology.targets[i].id!=committed_[i].id||recoveryBaseline_->topology.targets[i].width!=committed_[i].width||recoveryBaseline_->topology.targets[i].height!=committed_[i].height)return false;return true;}catch(...){if(!v2_)throw;return false;}}();
      if(!bound){if(!v2_)throw std::runtime_error("native_baseline_binding_invalid");recoveryHash_.clear();recoveryBaseline_.reset();}}}else for(size_t i=0;i<size_t(count);++i)committed_.push_back({std::to_string(i),1920,1080});runtimeLoaded_=true;return true;
  }
  std::vector<NativeRuntimeOutput> publicOutputsLocked()const{std::vector<NativeRuntimeOutput> result;for(size_t i=0;i<outputs_.size();++i)result.push_back({std::to_string(i),outputs_[i].width,outputs_[i].height});return result;}
  bool persistRuntimeLocked(const std::string& phase,const std::vector<NativeRuntimeOutput>& outputs,uint32_t generation)noexcept{try{return runtimeLoaded_&&runtime_.store({instance_,phase,generation,outputs,(phase=="stable"||phase=="stopped")?"":recoveryHash_});}catch(...){return false;}}
  bool applyOutputsLocked(std::span<const multitap::vdisplay::Output> desired,bool* attempted=nullptr){
    if(attempted)*attempted=false;
    if(!controller_||generation_==UINT32_MAX)return false;
    const bool changed=std::vector<multitap::vdisplay::Output>(desired.begin(),desired.end())!=outputs_;
    if(!changed)return controller_->state().leaseActive&&!controller_->state().recoveryRequired;
    // A failed reconfiguration can still recreate a target during rollback.
    // Always retire the old public generation after crossing that boundary.
    if(!persistRuntimeLocked("changing",committed_,generation_+1)){viewportRecovery_=true;return false;}++generation_;if(attempted)*attempted=true;
    const bool ok=controller_->reconfigure(desired);if(ok)outputs_.assign(desired.begin(),desired.end());return ok;
  }
  NativeViewportReply rollbackViewportLocked(){
    if(!pending_||pending_->generation!=generation_||!controller_)return replyLocked(false,false);
    if(pending_->rollingBack){const bool valid=!viewportRecovery_&&controller_->state().leaseActive&&!controller_->state().recoveryRequired;return replyLocked(snapshot(controller_->state().leaseActive,viewportRecovery_)&&valid,false);}
    bool attempted=false;const bool ok=applyOutputsLocked(pending_->before,&attempted);pending_->generation=generation_;pending_->rollingBack=true;
    if(!ok)viewportRecovery_=true;const bool published=snapshot(controller_->state().leaseActive,viewportRecovery_);return replyLocked(ok&&published,attempted);
  }
  NativeViewportReply finishViewportLocked(const std::string& expectedLease,bool rollback,bool windowsVerified,const std::optional<NativeRecoveryScope>& expectedScope){
    if(!pending_||pending_->generation!=generation_||pending_->rollingBack!=rollback||(!rollback&&!pending_->prepared)||!windowsVerified||viewportRecovery_||!controller_||!recoveryBaseline_||!recoveryBaseline_->scope||!expectedScope||!nativeRecoveryLogonMatches(*recoveryBaseline_->scope,*expectedScope)||streamMonotonicMillis()>=pending_->deadline)return replyLocked(false,false);
    try{const auto state=JsonParser::parse(stateJsonLocked(true,false));if(!nativeConfigMatchesLease(configuration(),state)||nativeDisplayLeaseKey(state)!=expectedLease)return replyLocked(false,false);}catch(...){return replyLocked(false,false);}
    // A durable stable record and the protected snapshot are both part of ACK.
    // Retain rollback ownership if final snapshot publication fails.
    const auto completed=publicOutputsLocked();if(pending_->profile&&!saveProfileViewportCommitLocked(completed,*expectedScope))return replyLocked(false,false);if(!persistRuntimeLocked("stable",completed,generation_)){viewportRecovery_=true;snapshot(true,true);return replyLocked(false,false);}
    auto pending=*pending_;pending_.reset();if(!snapshot(controller_->state().leaseActive,false)){pending_=std::move(pending);if(!persistRuntimeLocked("changing",committed_,generation_))viewportRecovery_=true;return replyLocked(false,false);}committed_=completed;recoveryHash_.clear();recoveryBaseline_.reset();return replyLocked(true,false);
  }
  void abandonLocked()noexcept{
    try{if(!pending_)return;const auto before=pending_->before;pending_.reset();const bool restored=applyOutputsLocked(before);(void)restored;viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);snapshot(controller_&&controller_->state().leaseActive,true);}catch(...){pending_.reset();viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);snapshot(false,true);}
  }
  void abandonCountLocked()noexcept{
    try{if(!countPending_)return;const auto before=countPending_->before;countPending_.reset();(void)applyOutputsLocked(before);viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);snapshot(controller_&&controller_->state().leaseActive,true);}catch(...){countPending_.reset();viewportRecovery_=true;persistRuntimeLocked("recovery",committed_,generation_);snapshot(false,true);}
  }
  NativeViewportReply replyLocked(bool accepted,bool attempted){
    NativeViewportReply result;result.accepted=accepted;result.driverChangeAttempted=attempted;result.pending=pending_.has_value()||(countPending_&&!countPending_->completed);result.recoveryRequired=activationRecovery_||viewportRecovery_||!controller_||controller_->state().recoveryRequired;
    try{result.snapshot=stateJsonLocked(controller_&&controller_->state().leaseActive,result.recoveryRequired);}catch(...){result.accepted=false;result.recoveryRequired=true;}return result;
  }
  std::string stateJsonLocked(bool active,bool recovery){
      const auto current=controller_?controller_->state():multitap::vdisplay::ControllerState{};
      active=active&&current.leaseActive;recovery=recovery||current.recoveryRequired||activationRecovery_||viewportRecovery_||(streamProfileOwner_.empty()&&profileFilePendingLocked());
      std::string targets="[";
      if(active)for(const auto& target:current.targets){
        const auto output=std::find_if(outputs_.begin(),outputs_.end(),[&](const auto& value){return value.identity==target.display_id;});
        const int publicId=target.display_id==publicIdentities_[0]?0:target.display_id==publicIdentities_[1]?1:-1;
        if(publicId<0||output==outputs_.end())throw std::runtime_error("native_display_snapshot_invalid");
        if(targets.size()>1)targets+=',';
        targets+="{\"id\":"+jsonQuote(std::to_string(publicId))+",\"width\":"+std::to_string(output->width)+",\"height\":"+std::to_string(output->height)+",\"adapterLow\":"+std::to_string(target.os_adapter_luid.low_part)+",\"adapterHigh\":"+std::to_string(target.os_adapter_luid.high_part)+",\"target\":"+std::to_string(target.target_id)+'}';
      }
      return "{\"schema\":1,\"provider\":\"libvirtualdisplay-1.6.4\",\"deviceInstanceId\":"+jsonQuote(instance_)+",\"servicePid\":"+std::to_string(GetCurrentProcessId())+",\"generation\":"+std::to_string(generation_)+",\"tick\":"+std::to_string(GetTickCount64())+",\"installed\":"+(transport_?"true":"false")+",\"started\":"+(transport_?"true":"false")+",\"leaseActive\":"+(active?"true":"false")+",\"countControlAvailable\":"+(active&&!recovery&&!profileBlockedLocked()&&!pending_&&!countPending_&&runtime_.record()&&runtime_.record()->phase=="stable"&&publicIdentities_[0]&&publicIdentities_[1]&&publicIdentities_[0]!=publicIdentities_[1]?"true":"false")+",\"activationVerified\":"+(runtime_.record()&&runtime_.record()->phase=="stable"&&!recovery&&!preparation_?"true":"false")+",\"recoveryBaselineAvailable\":"+(runtimeLoaded_&&runtime_.record()&&runtime_.record()->uncertain()&&runtime_.record()->baselineHash==recoveryHash_&&recoveryBaseline_&&!recoveryHash_.empty()?"true":"false")+",\"recoveryScopeAvailable\":"+(runtimeLoaded_&&runtime_.record()&&runtime_.record()->uncertain()&&runtime_.record()->baselineHash==recoveryHash_&&recoveryBaseline_&&recoveryBaseline_->scope&&nativeRecoveryScopeComplete(*recoveryBaseline_->scope)?"true":"false")+",\"configurationPending\":"+(pending_||(countPending_&&!countPending_->completed)?"true":"false")+",\"recoveryRequired\":"+(recovery?"true":"false")+",\"targets\":"+targets+"],\"renderPreference\":"+nativeRenderPreferenceJson(renderPreference_)+"}";
  }
  // An unchanged snapshot is only rewritten for its tick, without a flush
  // (native_display_files.hpp); the loop used to write and flush it every pass.
  // A changed one is a display change for the display probe (display_change_signals.hpp).
  NativeSnapshotPublication publication_;
  bool snapshot(bool active,bool recovery)noexcept{
    try{
      const auto now=GetTickCount64();const auto bytes=stateJsonLocked(active,recovery);
      const auto write=publication_.plan(bytes,now);if(write==NativeSnapshotWrite::none)return true;
      publication_.failed();
#ifdef MULTITAP_NATIVE_DISPLAY_TEST
      if(publishFixture_){publishFixture_(bytes);publication_.written(bytes,now);if(write==NativeSnapshotWrite::change)noteHostDisplayChange();return true;}
#endif
      if(!trustedMachineFile(machineDirectory()))return false;
      const auto path=machineDirectory()/L"native-display-state.json";if(std::filesystem::exists(path)&&!trustedMachineFile(path))return false;
      if(!writeNativeDisplaySnapshot(path,bytes,write==NativeSnapshotWrite::change))return false;
      publication_.written(bytes,now);if(write==NativeSnapshotWrite::change)noteHostDisplayChange();return true;
    }catch(...){return false;}
  }
};
}
#else
namespace host {class NativeDisplayService {
 public:
  void setDisplayManagerV2(bool){} bool displayManagerV2(){return false;}
  std::optional<std::string> setOwnedSizeV2(const std::string&,DWORD,DWORD){return {};}
  std::optional<std::string> setOwnedSizesV2(std::span<const NativeRuntimeOutput>){return {};}
  std::optional<std::string> setOwnedCountV2(int){return {};}
  bool reserveStreamProfile(const std::string&,const std::string&){return false;}bool releaseStreamProfile(const std::string&){return false;}
  bool reservePreparation(const std::string&,const std::string&){return false;}
  std::optional<NativePreparationLeaseProof> preparationLease(const std::string&,const std::string&,uint32_t){return {};}
  std::optional<NativePreparationLeaseProof> setPreparationOriginalBoundSize(const std::string&,const std::string&,uint32_t,DWORD,DWORD){return {};}
  std::optional<NativePreparationCleanupLeaseProof> preparationCleanupLease(const std::string&,const std::string&,uint32_t){return {};}
  std::optional<NativePreparationCleanupLeaseProof> restorePreparationOriginalBoundSize(const std::string&,const std::string&,uint32_t,DWORD,DWORD){return {};}
  bool refreshPreparationLease(const std::string&)noexcept{return false;}
  bool refreshPreparationCleanupLease(const std::string&)noexcept{return false;}
  bool releasePreparation(const std::string&){return false;}
  std::optional<std::string> saveStreamProfileLaunch(NativeStreamLaunchReceipt){return {};}
  bool releaseUnpreparedStreamProfile(const NativeStreamProfileKey&,uint32_t,bool,bool){return false;}
  bool streamProfilePending(){return false;}bool hasStreamProfileReservation(){return false;}
  bool refreshStreamProfileLease()noexcept{return false;}bool refreshOwnedCleanupLease()noexcept{return false;}
  void tick()noexcept{} void shutdown()noexcept{} void activationRecovery(uint32_t,bool=false)noexcept{} bool activationReady(uint32_t)noexcept{return false;}
  bool requestRenderRearm()noexcept{return false;} void setRenderRearmIdle(std::function<bool()>){}
  NativeConsolePark parkForConsole(uint64_t)noexcept{return NativeConsolePark::refused;} bool renewConsolePark(uint64_t)noexcept{return false;} void endConsolePark()noexcept{} bool consoleParked()noexcept{return false;}
  NativeViewportReply prepareViewport(const StreamResize&,const std::string&,int64_t,const std::string& = {},const std::optional<NativeRecoveryScope>& = {}){return {};}
  NativeViewportReply rollbackViewport(const StreamResize&){return {};}
  NativeViewportReply confirmUnchangedViewport(const NativeStreamCancelRequest&,const std::string&,int64_t,const std::string&,const std::optional<NativeRecoveryScope>&){return {};}
  NativeViewportReply finishViewport(const StreamResize&,const std::string&,bool,bool,const std::optional<NativeRecoveryScope>& = {}){return {};}
  NativeViewportReply prepareTrackViewport(const NativeTrackResize&,const std::string&,int64_t,const std::string&,const std::optional<NativeRecoveryScope>&){return {};}
  NativeViewportReply rollbackTrackViewport(const NativeTrackResize&){return {};}
  NativeViewportReply finishTrackViewport(const NativeTrackResize&,const std::string&,bool,bool,const std::optional<NativeRecoveryScope>&){return {};}
  NativeViewportReply prepareCount(const NativeCountRequest&,const std::string&,int64_t,const std::string&,const std::optional<NativeRecoveryScope>&){return {};}
  NativeViewportReply rollbackCount(const NativeCountRequest&){return {};}
  NativeViewportReply finishCount(const NativeCountRequest&,const std::string&,bool,bool,const std::optional<NativeRecoveryScope>&){return {};}
  bool acknowledgeCountOperation(const NativeCountRequest&,bool){return false;} void cancelCountOperation(const DisplayOperationKey&)noexcept{} void cancelViewportSession(const std::string&,bool=false)noexcept{} std::string viewportSnapshot(){return {};} bool streamStopClean(const std::string&){return false;}
  std::optional<NativeOwnedStreamLeaseProof> proveOwnedStreamLease(const std::string&,const std::string&,uint32_t){return {};}
};}
#endif
