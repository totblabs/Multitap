#pragma once
#include "native_display_count_rpc.hpp"
#include "native_display_baseline_codec.hpp"
namespace host {
struct NativeWorkerCountAccess {
  std::function<bool()> eligible;
  std::function<DisplayState()> probe;
  std::function<std::vector<CaptureMonitor>()> observed;
  std::function<Json()> snapshot;
  NativeTopologyAccess topology;
  std::function<NativeRecoveryScope(const NativeTopologyBaseline&)> recoveryScope;
  std::function<NativeWorkerViewportReply(const std::string&,const std::string&,bool,const std::string&,const StreamResizeBudget&)> exchange;
};
struct NativeWorkerCountResult {bool ok=false,rollbackAttempted=false,rollbackSucceeded=false;DisplayState display;std::string reason="display_configuration_failed";uint32_t failureStage=0;DWORD systemError=0;};
inline NativeWorkerCountResult applyNativeWorkerCount(const NativeCountRequest& request,NativeWorkerCountAccess access,const StreamResizeBudget& budget){
  NativeWorkerCountResult result;std::optional<NativeTopologyBaseline> baseline;DisplayState before;std::vector<CaptureMonitor> observedBefore;std::string key;bool mayHaveChanged=false;uint32_t phase=1;
  auto mutationBudget=budget;mutationBudget.deadline-=10000;
  const auto eligible=[&]{try{budget.remaining(1);return access.eligible()&&access.topology.eligible();}catch(...){return false;}};
  const auto lease=[&](bool pending){const auto snapshot=access.snapshot();if(!eligible()||nativeDisplayLeaseKey(snapshot)!=key||!std::holds_alternative<bool>(snapshot.get("configurationPending").value)||snapshot.get("configurationPending").boolean()!=pending)throw std::runtime_error("native_display_change_pending");return snapshot;};
  const auto foreign=[](const std::vector<CaptureMonitor>& actual,const DisplayState& owned){std::multiset<std::tuple<std::wstring,LONG,LONG,LONG,LONG,bool>> values;for(const auto& monitor:actual)if(std::none_of(owned.monitors.begin(),owned.monitors.end(),[&](const auto& value){return value.name==monitor.name;}))values.emplace(monitor.identity,monitor.bounds.left,monitor.bounds.top,monitor.bounds.right,monitor.bounds.bottom,monitor.primary);return values;};
  const auto verified=[&](bool pending){
    const auto state=lease(pending);const auto targets=nativeDisplayTargets(state);auto display=access.probe();(void)lease(pending);
    if(display.configurationPending!=pending||display.remoteSession||display.recoveryRequired||display.rebootRequired||!baseline)throw std::runtime_error("native_display_not_ready");
    auto view=display;view.configurationPending=false;if(!view.ready()||view.monitors.size()!=targets.size())throw std::runtime_error("native_display_not_ready");
    const auto ccd=access.topology.query();const auto plan=planNativeCountTopologyTransition(*baseline,ccd,targets);
    if(!nativeTopologyMatches(plan,nativePlanForeign(plan),ccd))throw std::runtime_error("native_display_topology_mismatch");
    const auto observed=access.observed();if(foreign(observedBefore,before)!=foreign(observed,view))throw std::runtime_error("native_display_topology_mismatch");
    for(const auto& target:targets){const auto monitor=std::find_if(view.monitors.begin(),view.monitors.end(),[&](const auto& value){return value.id==target.id&&value.width==target.width&&value.height==target.height;});const auto placement=std::find_if(plan.placements.begin(),plan.placements.end(),[&](const auto& value){return value.id==target.id;});if(monitor==view.monitors.end()||placement==plan.placements.end()||monitor->primary!=placement->primary||selectOwnedCapture(view,target.id,observed).monitors.size()!=1)throw std::runtime_error("native_display_topology_mismatch");}
    (void)lease(pending);return display;
  };
  const auto activate=[&](const StreamResizeBudget& phaseBudget){auto topology=access.topology;topology.eligible=[&]{try{(void)lease(true);return true;}catch(...){return false;}};const auto targets=nativeDisplayTargets(lease(true));const auto planner=[&](const NativeCcdData& ccd){return planNativeCountTopologyTransition(*baseline,ccd,targets);};const auto activated=activateNativeTopology(targets,topology,phaseBudget.remaining(8000),{},&*baseline,{},planner);if(!activated.ok){if(!result.failureStage)result.systemError=activated.systemError;throw std::runtime_error("native_display_topology_mismatch");}};
  try{
    mutationBudget.remaining(1);if(!nativeCountValid(request)||!eligible())throw std::runtime_error("native_count_unavailable");
    before=access.probe();result.display=before;const auto current=access.snapshot();key=nativeDisplayLeaseKey(current);
    if(!before.ready()||before.remoteSession||!before.monitorControlSupported||current.get("configurationPending").boolean())throw std::runtime_error("native_count_unavailable");
    phase=2;observedBefore=access.observed();for(const auto& monitor:before.monitors)if(selectOwnedCapture(before,monitor.id,observedBefore).monitors.size()!=1)throw std::runtime_error("native_display_topology_mismatch");
    if(before.monitors.size()==size_t(request.count)){result.display=access.probe();(void)lease(false);if(!result.display.ready()||result.display.monitors.size()!=size_t(request.count))throw std::runtime_error("native_count_unavailable");const auto observed=access.observed();for(const auto& monitor:result.display.monitors)if(selectOwnedCapture(result.display,monitor.id,observed).monitors.size()!=1)throw std::runtime_error("native_display_topology_mismatch");result.ok=true;result.reason.clear();return result;}
    const auto targets=nativeDisplayTargets(current);const auto ccd=access.topology.query();baseline=captureNativeTopologyBaseline(ccd,targets,request.count==1?"1":"0");
    if(!nativeTopologyMatches(planNativeTopology(ccd,targets,baseline->placements),ccd,ccd))throw std::runtime_error("native_display_topology_mismatch");
    const auto checkpoint=nativeRecoveryBaselineBytes({current.get("deviceInstanceId").string(),DWORD(current.get("generation").number()),*baseline,access.recoveryScope(*baseline)});
    if(nativeActiveSignatures(access.topology.query())!=nativeActiveSignatures(baseline->ccd))throw std::runtime_error("native_display_topology_mismatch");(void)lease(false);
    phase=3;mayHaveChanged=true;const auto prepared=access.exchange("worker.native.count.prepare",key,false,checkpoint,mutationBudget);
    mayHaveChanged=prepared.requestStarted&&(!prepared.received||prepared.driverChangeAttempted||prepared.pending||prepared.recoveryRequired);
    if(!prepared.received||!prepared.accepted||!prepared.pending||prepared.recoveryRequired)throw std::runtime_error("native_count_rejected");
    key=nativeDisplayLeaseKey(prepared.snapshot);phase=4;activate(mutationBudget);phase=5;(void)verified(true);
    phase=6;const auto committed=access.exchange("worker.native.count.commit",key,true,"",mutationBudget);
    if(!committed.received||!committed.accepted||committed.pending||committed.recoveryRequired||nativeDisplayLeaseKey(committed.snapshot)!=key)throw std::runtime_error("native_count_rejected");
    phase=7;result.display=verified(false);result.ok=true;result.reason.clear();return result;
  }catch(const NativeTopologyWindowsError& error){result.failureStage=phase;result.systemError=error.code;
  }catch(...){result.failureStage=phase;}
  if(mayHaveChanged&&baseline){
    result.rollbackAttempted=true;
    try{
      const auto restored=access.exchange("worker.native.count.rollback","",false,"",budget);
      if(!restored.received||!restored.accepted||!restored.pending||restored.recoveryRequired)throw std::runtime_error("native_count_rejected");
      key=nativeDisplayLeaseKey(restored.snapshot);const auto targets=nativeDisplayTargets(lease(true));
      if(targets.size()!=baseline->targets.size())throw std::runtime_error("native_display_topology_mismatch");
      for(size_t i=0;i<targets.size();++i)if(targets[i].id!=baseline->targets[i].id||targets[i].width!=baseline->targets[i].width||targets[i].height!=baseline->targets[i].height)throw std::runtime_error("native_display_topology_mismatch");
      activate(budget);(void)verified(true);const auto completed=access.exchange("worker.native.count.restored",key,true,"",budget);
      if(!completed.received||!completed.accepted||completed.pending||completed.recoveryRequired||nativeDisplayLeaseKey(completed.snapshot)!=key)throw std::runtime_error("native_count_rejected");
      result.display=verified(false);result.rollbackSucceeded=true;
    }catch(...){ }
  }
  if(mayHaveChanged&&!result.rollbackSucceeded){result.display.active=false;result.display.viewportProvisioning=false;result.display.monitorControlSupported=false;result.display.configurationPending=true;result.display.recoveryRequired=true;result.display.error="native_topology_recovery_required";}
  return result;
}
}
