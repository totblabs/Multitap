#pragma once
#include "native_display_worker_rpc.hpp"
#include "native_display_baseline_codec.hpp"
#include "native_stream_viewport_console.hpp"
#include "native_activation_diagnostic.hpp"
namespace host {
struct NativeWorkerViewportAccess {
  WorkerViewportAccess viewport;
  std::function<Json()> snapshot;
  NativeTopologyAccess topology;
  std::function<NativeWorkerViewportReply(const std::string&,const std::string&,bool,const std::string&)> exchange;
  std::function<NativeRecoveryScope(const NativeTopologyBaseline&)> recoveryScope;
  std::function<std::optional<NativeConsoleBinding>(const NativeTopologyBaseline&)> console;
  std::function<bool(const NativeConsoleBinding&)> consoleCurrent;
};
// This coordinator runs only in the authenticated interactive worker. Pending
// service snapshots remain unready everywhere else. The local staged view is
// admitted solely after fresh CCD and capture verification for that exact lease.
inline WorkerViewportResult applyNativeWorkerViewport(const Json& current,const StreamResize& request,const std::string& logicalMonitor,NativeWorkerViewportAccess access,const StreamResizeBudget& budget){
  WorkerViewportResult result;std::optional<NativeTopologyBaseline> baseline;std::optional<DisplayState> before;
  std::vector<CaptureMonitor> observedBefore;std::string key;bool mayHaveChanged=false,staged=false,rollbackAttempted=false,rollbackSucceeded=false,consoleReturned=false;uint32_t phase=1,failureStage=0,failureReason=0;DWORD failureError=0;
  std::optional<NativeConsoleBinding> console;
  const auto eligible=[&]{try{budget.remaining(1);return access.viewport.interactive()&&access.viewport.privileged()&&access.topology.eligible()&&(!console||(access.consoleCurrent&&access.consoleCurrent(*console)));}catch(...){return false;}};
  const auto plan=[&](const NativeCcdData& ccd,std::span<const NativeDisplayTarget> targets){if(!baseline)throw std::runtime_error("native_topology_baseline_required");if(console){if(!eligible())throw std::runtime_error("native_stream_console_profile_changed");auto value=planNativeStreamViewportConsole(*baseline,ccd,targets,*console);for(const auto& path:ccd.paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativeSameLuid(path.sourceInfo.adapterId,console->path.sourceAdapter)&&nativeSameLuid(path.targetInfo.adapterId,console->path.targetAdapter)&&path.sourceInfo.id==console->path.source&&path.targetInfo.id==console->path.target)consoleReturned=true;return value;}return planNativeTopologyTransition(*baseline,ccd,targets);};
  const auto lease=[&](const std::string& expected,bool pending){const auto state=access.snapshot();if(!eligible()||nativeDisplayLeaseKey(state)!=expected||!std::holds_alternative<bool>(state.get("configurationPending").value)||state.get("configurationPending").boolean()!=pending)throw std::runtime_error("native_display_change_pending");return state;};
  const auto topology=[&](const std::string& expected){auto io=access.topology;io.eligible=[&,expected]{try{(void)lease(expected,true);return true;}catch(...){return false;}};return io;};
  // Capture-level foreign outputs: identity, size and primary. Their exact
  // desktop position (including an accepted automatic adjacent re-layout) is
  // verified against the plan at the CCD level on every attempt.
  const auto foreign=[](const std::vector<CaptureMonitor>& actual,const DisplayState& owned){return nativeForeignCaptureSet(actual,owned);};
  const auto verified=[&](DWORD timeout,const std::string& expected,bool pending){
    if(!baseline||!before)throw std::runtime_error("native_display_not_ready");
    const auto until=access.topology.now()+std::min<DWORD>(timeout,1500);
    for(;;){
      const auto snapshot=lease(expected,pending);const auto targets=nativeDisplayTargets(snapshot);
      // Prove actual CCD geometry and foreign preservation on every attempt.
      // These failures are terminal; a GDI observation retry cannot authorize
      // a different lease, logon or an independently edited display topology.
      const auto ccd=access.topology.query();const auto expectedPlan=plan(ccd,targets);
      if(!nativeTopologyMatches(expectedPlan,nativePlanForeign(expectedPlan),ccd))throw std::runtime_error("native_display_topology_mismatch");
      try{
        auto display=access.viewport.probe(budget.remaining(timeout));budget.remaining(1);(void)lease(expected,pending);
        if(display.configurationPending!=pending||display.remoteSession||display.recoveryRequired||display.rebootRequired)throw std::runtime_error("native_display_not_ready");
        auto view=display;view.configurationPending=false;if(!view.ready()||view.monitors.size()!=targets.size())throw std::runtime_error("native_display_not_ready");
        const auto observed=access.viewport.observed();if(foreign(observedBefore,*before)!=foreign(observed,view))throw std::runtime_error("native_display_topology_mismatch");
        for(const auto& target:targets){const auto actual=std::find_if(view.monitors.begin(),view.monitors.end(),[&](const auto& value){return value.id==target.id&&value.width==target.width&&value.height==target.height;});const auto previous=std::find_if(before->monitors.begin(),before->monitors.end(),[&](const auto& value){return value.id==target.id;});if(actual==view.monitors.end()||previous==before->monitors.end()||actual->primary!=previous->primary||selectOwnedCapture(view,target.id,observed).monitors.size()!=1)throw std::runtime_error("native_display_topology_mismatch");}
        (void)lease(expected,pending);if(pending){view.viewportProvisioning=true;view.error.clear();}return view;
      }catch(...){
        // The display/capture APIs may still observe the previous source just
        // after CCD accepts the new SDK target. Wait only for fresh evidence;
        // do not repeat SDK/CCD mutation or extend the transaction deadline.
        if(!eligible()||access.topology.now()>=until)throw;
        access.topology.pause();
      }
    }
  };
  auto local=access.viewport;
  local.probe=[&](DWORD timeout){if(staged)return verified(timeout,key,true);auto display=access.viewport.probe(timeout);const auto state=access.snapshot();if(!eligible()||state.get("configurationPending").boolean()||state.get("recoveryRequired").boolean())return display;(void)nativeDisplayLeaseKey(state);if(display.ready()&&!display.remoteSession)display.viewportProvisioning=true;return display;};
  local.apply=[&](const std::wstring& device,int width,int height,DWORD timeout){
    DisplayHelperOutput applied;
    try{
      // Phase1/2 are read-only. Every rejection records one fixed site code
      // (nativeReason) so an automatic diagnostic names the failed check.
      using R=NativeViewportReason;
      const auto reject=[&](R reason)->void{failureReason=nativeViewportReasonCode(reason);throw std::runtime_error("native_viewport_rejected");};
      const auto step=[&](R reason,auto&& body){try{return body();}catch(const NativeTopologyWindowsError&){if(!failureReason)failureReason=nativeTopologyReasonCode("native_topology_query_failed");throw;}catch(...){if(!failureReason)failureReason=nativeViewportReasonCode(reason);throw;}};
      try{budget.remaining(1);}catch(...){reject(R::budgetExpired);}
      if(!eligible())reject(R::workerIneligible);
      if(width!=request.width||height!=request.height)reject(R::requestMismatch);
      (void)device; // The owned output is re-resolved by its stable owned id.
      std::string checkpoint;
      // A fresh observation can lag one CCD/GDI update behind the outer probe.
      // Retry the whole read-only phase within a short bounded window; no SDK
      // prepare, CCD apply or recovery latch can happen before phase 3.
      const auto retryUntil=access.topology.now()+1500;
      for(unsigned attempt=0;;++attempt){
        failureReason=0;console.reset();baseline.reset();before.reset();phase=1;
        try{
          const auto snapshot=step(R::snapshotUnavailable,[&]{return access.snapshot();});
          key=step(R::snapshotUnavailable,[&]{return nativeDisplayLeaseKey(snapshot);});
          if(!std::holds_alternative<bool>(snapshot.get("configurationPending").value)||snapshot.get("configurationPending").boolean())reject(R::snapshotPending);
          phase=2;
          before=step(R::probeFailed,[&]{return access.viewport.probe(budget.remaining(streamResizeProbeTimeoutMs));});
          observedBefore=step(R::probeFailed,[&]{return access.viewport.observed();});
          if(before->remoteSession)reject(R::remoteSession);
          if(!before->ready())reject(R::displayNotReady);
          const auto source=std::find_if(before->monitors.begin(),before->monitors.end(),[&](const auto& value){return value.id==request.monitor;});
          if(source==before->monitors.end())reject(R::ownedOutputMissing);
          if(step(R::captureSelectionInvalid,[&]{return selectOwnedCapture(*before,request.monitor,observedBefore).monitors.size();})!=1)reject(R::captureSelectionInvalid);
          const auto ccd=step(R::baselineInvalid,[&]{return access.topology.query();});
          const auto targets=step(R::snapshotTargetsInvalid,[&]{return nativeDisplayTargets(snapshot);});
          baseline=step(R::baselineInvalid,[&]{return captureNativeTopologyBaseline(ccd,targets,request.monitor);});
          if(access.console){
            console=step(R::consoleUnavailable,[&]{return access.console(*baseline);});
            if(!console)reject(R::consoleUnavailable);
            if(!access.consoleCurrent||!access.consoleCurrent(*console)){failureReason=nativeTopologyReasonCode("native_stream_console_profile_changed");throw std::runtime_error("native_stream_console_profile_changed");}
          }
          std::string mismatch;
          const auto current=[&]{try{return planNativeTopology(ccd,targets,baseline->placements);}catch(const std::exception& error){failureReason=nativeTopologyReasonCode(error.what());throw;}}();
          if(!nativeTopologyMatches(current,ccd,ccd,&mismatch)){failureReason=nativeTopologyReasonCode(mismatch.empty()?"native_topology_failed":mismatch);throw std::runtime_error("native_display_topology_mismatch");}
          step(R::leaseChanged,[&]{if(!eligible())reject(R::workerIneligible);(void)lease(key,false);});
          const auto scope=step(R::recoveryScopeUnavailable,[&]{return access.recoveryScope(*baseline);});
          checkpoint=step(R::checkpointInvalid,[&]{return nativeRecoveryBaselineBytes({snapshot.get("deviceInstanceId").string(),DWORD(snapshot.get("generation").number()),*baseline,scope});});
          if(step(R::baselineInvalid,[&]{return nativeActiveSignatures(access.topology.query())!=nativeActiveSignatures(baseline->ccd);})){failureReason=nativeTopologyReasonCode("native_topology_changed");throw std::runtime_error("native_display_topology_mismatch");}
          step(R::leaseChanged,[&]{if(!eligible())reject(R::workerIneligible);(void)lease(key,false);});
          break;
        }catch(...){
          // Eligibility, cancellation, request and console-profile changes are
          // final. Other observations may settle; never extend the deadline.
          const bool final=failureReason==nativeViewportReasonCode(R::workerIneligible)||failureReason==nativeViewportReasonCode(R::budgetExpired)||failureReason==nativeTopologyReasonCode("native_stream_console_profile_changed");
          bool retry=!final&&attempt<3&&access.topology.now()<retryUntil;
          if(retry)try{budget.remaining(1);retry=eligible();}catch(...){retry=false;}
          if(!retry){if(!failureReason)failureReason=255;throw;}
          for(int pause=0;pause<5&&access.topology.now()<retryUntil;++pause)access.topology.pause();
        }
      }
      // Mark uncertainty before the call: a throwing transport may already
      // have delivered the complete prepare to the service.
      phase=3;mayHaveChanged=true;const auto prepared=access.exchange("worker.native.resize.prepare",key,false,checkpoint);
      mayHaveChanged=prepared.requestStarted&&(!prepared.received||prepared.driverChangeAttempted||prepared.pending||prepared.recoveryRequired);
      if(!prepared.received||!prepared.accepted||!prepared.pending||prepared.recoveryRequired)throw std::runtime_error("native_change_rejected");
      key=nativeDisplayLeaseKey(prepared.snapshot);const auto pending=lease(key,true);
      phase=4;const auto pendingTargets=nativeDisplayTargets(pending);const auto activated=activateNativeTopology(pendingTargets,topology(key),budget.remaining(std::min<DWORD>(timeout,10000),streamResizeApplyReserveMs),{},&*baseline,{},[&](const NativeCcdData& fresh){return plan(fresh,pendingTargets);});
      if(!activated.ok){if(!failureStage){failureError=activated.systemError;failureReason=nativeTopologyReasonCode(activated.reason);}throw std::runtime_error("native_display_topology_mismatch");}
      phase=5;staged=true;const auto display=verified(budget.remaining(streamResizeProbeTimeoutMs),key,true);const auto target=std::find_if(display.monitors.begin(),display.monitors.end(),[&](const auto& value){return value.id==request.monitor;});if(target==display.monitors.end())throw std::runtime_error("native_display_not_ready");
      applied.exitCode=0;applied.value=JsonParser::parse("{\"viewportApplied\":true,\"foreignGeometryVerified\":true,\"configurationPending\":false,\"recoveryRequired\":false,\"width\":"+std::to_string(width)+",\"height\":"+std::to_string(height)+",\"refreshRate\":60,\"targetMonitorId\":"+jsonQuote(target->id)+",\"targetDisplayDeviceName\":"+jsonQuote(utf8(target->name))+'}');return applied;
    }catch(const NativeTopologyWindowsError& error){if(!failureStage){failureStage=phase;failureError=error.code;}}catch(...){if(!failureStage)failureStage=phase;}
    staged=false;applied.value=JsonParser::parse("{\"viewportApplied\":false,\"configurationPending\":"+std::string(mayHaveChanged?"true":"false")+",\"recoveryRequired\":"+(mayHaveChanged?"true":"false")+'}');return applied;
  };
  result=applyWorkerViewport(current,request,logicalMonitor,local,budget);
  if(result.ok){
    try{
      phase=5;(void)verified(budget.remaining(streamResizeProbeTimeoutMs),key,true);
      phase=6;const auto committed=access.exchange("worker.native.resize.commit",key,true,"");
      if(!committed.received||!committed.accepted||committed.pending||committed.recoveryRequired||nativeDisplayLeaseKey(committed.snapshot)!=key)throw std::runtime_error("native_change_rejected");
      phase=7;result.display=verified(budget.remaining(streamResizeProbeTimeoutMs),key,false);auto command=result.command;phase=8;attachFreshCapture(command,result.display,access.viewport.observed());result.command=std::move(command);result.consoleReturned=consoleReturned;return result;
    }catch(const NativeTopologyWindowsError& error){failureStage=phase;failureError=error.code;result.ok=false;result.reason="display_change_failed";
    }catch(...){failureStage=phase;result.ok=false;result.reason="display_change_failed";}
  }
  staged=false;
  if(mayHaveChanged&&baseline&&before){
    rollbackAttempted=true;
    try{
      const auto restored=access.exchange("worker.native.resize.rollback","",false,"");
      if(!restored.received||!restored.accepted||!restored.pending||restored.recoveryRequired)throw std::runtime_error("native_change_rejected");
      key=nativeDisplayLeaseKey(restored.snapshot);const auto state=lease(key,true);const auto targets=nativeDisplayTargets(state);
      if(targets.size()!=baseline->targets.size())throw std::runtime_error("native_display_topology_mismatch");
      for(size_t i=0;i<targets.size();++i)if(targets[i].id!=baseline->targets[i].id||targets[i].width!=baseline->targets[i].width||targets[i].height!=baseline->targets[i].height)throw std::runtime_error("native_display_topology_mismatch");
      if(!activateNativeTopology(targets,topology(key),budget.remaining(10000,streamResizeProbeTimeoutMs+1000),{},&*baseline,{},[&](const NativeCcdData& fresh){return plan(fresh,targets);}).ok)throw std::runtime_error("native_display_topology_mismatch");
      (void)verified(budget.remaining(streamResizeProbeTimeoutMs),key,true);
      const auto completed=access.exchange("worker.native.resize.restored",key,true,"");
      if(!completed.received||!completed.accepted||completed.pending||completed.recoveryRequired||nativeDisplayLeaseKey(completed.snapshot)!=key)throw std::runtime_error("native_change_rejected");
      result.display=verified(budget.remaining(streamResizeProbeTimeoutMs),key,false);rollbackSucceeded=true;
    }catch(...){result.display.active=false;result.display.viewportProvisioning=false;result.display.configurationPending=true;result.display.recoveryRequired=true;result.display.error="native_topology_recovery_required";}
  }
  if(!std::holds_alternative<Json::Object>(result.helper.value))result.helper.value=Json::Object{};
  auto& helper=std::get<Json::Object>(result.helper.value);helper["rollbackAttempted"].value=rollbackAttempted;helper["rollbackSucceeded"].value=rollbackSucceeded;
  // Worker-local fact for the profile consumer: false only when no prepare RPC
  // could have reached the service mutation boundary (phase<3), or the service
  // answered that it changed nothing. Never forwarded as readiness.
  helper["driverChangePossible"].value=mayHaveChanged;
  if(rollbackSucceeded){helper["configurationPending"].value=false;helper["recoveryRequired"].value=false;}
  if(mayHaveChanged&&!rollbackSucceeded){result.display.active=false;result.display.viewportProvisioning=false;result.display.recoveryRequired=true;helper["recoveryRequired"].value=true;}
  result.nativeStage=failureStage?failureStage:phase;result.nativeError=failureError;result.nativeReason=result.ok?0:failureReason;result.consoleReturned=consoleReturned;
  result.unchanged=!result.ok&&!mayHaveChanged&&!rollbackAttempted&&result.nativeStage<=2;
  // Worker-local: the SDK change was rolled back and the original geometry
  // re-verified. Advisory; the service re-checks its own lease targets.
  result.restored=!result.ok&&rollbackAttempted&&rollbackSucceeded;return result;
}
}
