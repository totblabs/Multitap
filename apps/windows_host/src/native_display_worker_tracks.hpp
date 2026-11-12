#pragma once
#include "native_track_change_rpc.hpp"
#include "native_display_baseline_codec.hpp"
#include "native_stream_viewport_console.hpp"
namespace host {
struct NativeWorkerTrackAccess {
  std::function<bool()> eligible;
  std::function<DisplayState()> probe;
  std::function<std::vector<CaptureMonitor>()> observed;
  std::function<Json()> snapshot;
  NativeTopologyAccess topology;
  std::function<NativeRecoveryScope(const NativeTopologyBaseline&)> recoveryScope;
  std::function<NativeWorkerViewportReply(const std::string&,const std::string&,bool,const std::string&,const StreamResizeBudget&)> exchange;
  std::function<std::optional<NativeConsoleBinding>(const NativeTopologyBaseline&)> console;
  std::function<bool(const NativeConsoleBinding&)> consoleCurrent;
};
struct NativeWorkerTrackResult {
  bool ok=false,rollbackAttempted=false,rollbackSucceeded=false;
  DisplayState display;
  Json command;
  std::string reason="display_change_failed";
};
// Called with the source encoders and pressed input already stopped. This
// coordinates the protected Session0 transaction and one exact Windows layout.
inline NativeWorkerTrackResult applyNativeWorkerTracks(const Json& current,const NativeTrackResize& request,NativeWorkerTrackAccess access,const StreamResizeBudget& budget,bool initial=false){
  NativeWorkerTrackResult result;std::optional<NativeTopologyBaseline> baseline;std::optional<NativeTrackViewportPlan> group;DisplayState before;std::vector<CaptureMonitor> observedBefore;std::string key;bool mayHaveChanged=false;
  auto mutationBudget=budget;mutationBudget.deadline-=10000;
  std::optional<NativeConsoleBinding> console;
  const auto eligible=[&]{try{budget.remaining(1);return access.eligible()&&access.topology.eligible()&&(!console||(access.consoleCurrent&&access.consoleCurrent(*console)));}catch(...){return false;}};
  const auto plan=[&](const NativeCcdData& ccd,std::span<const NativeDisplayTarget> targets,std::span<const NativeOutputPlacement> placements){if(!baseline)throw std::runtime_error("native_topology_baseline_required");if(console){if(!eligible())throw std::runtime_error("native_stream_console_profile_changed");return planNativeStreamViewportConsole(*baseline,ccd,targets,*console,placements);}return planNativeTopologyTransition(*baseline,ccd,targets,placements);};
  const auto lease=[&](bool pending){const auto state=access.snapshot();if(!eligible()||nativeDisplayLeaseKey(state)!=key||!std::holds_alternative<bool>(state.get("configurationPending").value)||state.get("configurationPending").boolean()!=pending)throw std::runtime_error("native_display_change_pending");return state;};
  const auto foreign=[](const std::vector<CaptureMonitor>& actual,const DisplayState& owned){std::multiset<std::tuple<std::wstring,LONG,LONG,LONG,LONG,bool>> values;for(const auto& monitor:actual)if(std::none_of(owned.monitors.begin(),owned.monitors.end(),[&](const auto& value){return value.name==monitor.name;}))values.emplace(monitor.identity,monitor.bounds.left,monitor.bounds.top,monitor.bounds.right,monitor.bounds.bottom,monitor.primary);return values;};
  const auto verified=[&](bool pending,bool restoring){
    const auto state=lease(pending);const auto targets=nativeDisplayTargets(state);auto display=access.probe();(void)lease(pending);
    if(display.configurationPending!=pending||display.remoteSession||display.recoveryRequired||display.rebootRequired||!baseline||!group)throw std::runtime_error("native_display_not_ready");
    auto view=display;view.configurationPending=false;if(!view.ready()||view.monitors.size()!=targets.size())throw std::runtime_error("native_display_not_ready");
    const auto& placements=restoring?baseline->placements:group->placements;const auto ccd=access.topology.query();const auto expectedPlan=plan(ccd,targets,placements);
    if(!nativeTopologyMatches(expectedPlan,nativePlanForeign(expectedPlan),ccd))throw std::runtime_error("native_display_topology_mismatch");
    const auto observed=access.observed();if(foreign(observedBefore,before)!=foreign(observed,view))throw std::runtime_error("native_display_topology_mismatch");
    for(const auto& target:targets){const auto monitor=std::find_if(view.monitors.begin(),view.monitors.end(),[&](const auto& value){return value.id==target.id&&value.width==target.width&&value.height==target.height;});const auto place=std::find_if(placements.begin(),placements.end(),[&](const auto& value){return value.id==target.id;});if(monitor==view.monitors.end()||place==placements.end()||monitor->left!=place->left||monitor->top!=place->top||monitor->primary!=place->primary||selectOwnedCapture(view,target.id,observed).monitors.size()!=1)throw std::runtime_error("native_display_topology_mismatch");}
    (void)lease(pending);return display;
  };
  const auto activate=[&](bool restoring,const StreamResizeBudget& phaseBudget){auto topology=access.topology;topology.eligible=[&]{try{(void)lease(true);return true;}catch(...){return false;}};const auto targets=nativeDisplayTargets(lease(true));const auto& placements=restoring?baseline->placements:group->placements;if(!activateNativeTopology(targets,topology,phaseBudget.remaining(8000),{},&*baseline,placements,[&](const NativeCcdData& fresh){return plan(fresh,targets,placements);}).ok)throw std::runtime_error("native_display_topology_mismatch");};
  try{
    mutationBudget.remaining(1);if(!nativeTrackResizeValid(request)||!eligible()||current.get("mode").string()!="stream"||current.get("mediaProtocol").string()!="tracks-v1"||current.get("sessionId").string()!=request.sessionId)throw std::runtime_error("invalid_track_request");
    if(initial){
      if(current.get("type").string()!="session.start"||request.topologyEpoch!=1||std::any_of(request.epochs.begin(),request.epochs.end(),[](uint32_t epoch){return epoch!=1;})||!std::holds_alternative<std::nullptr_t>(current.get("requestId").value)||!std::holds_alternative<std::nullptr_t>(current.get("resizeId").value)||!nativeSameTrackResize(nativeInitialTrackViewport(current),request))throw std::runtime_error("invalid_initial_track_request");
    }else if(!nativeTrackResizeFollows(nativeInitialTrackViewport(current),request))throw std::runtime_error("stale_topology");
    before=access.probe();result.display=before;const auto state=access.snapshot();key=nativeDisplayLeaseKey(state);if(!before.ready()||before.remoteSession||state.get("configurationPending").boolean())throw std::runtime_error("native_display_not_ready");
    observedBefore=access.observed();for(const auto& monitor:before.monitors)if(selectOwnedCapture(before,monitor.id,observedBefore).monitors.size()!=1)throw std::runtime_error("native_display_topology_mismatch");
    const auto targets=nativeDisplayTargets(state);const auto ccd=access.topology.query();const auto root=std::find_if(before.monitors.begin(),before.monitors.end(),[](const auto& value){return value.primary;});baseline=captureNativeTopologyBaseline(ccd,targets,root==before.monitors.end()?targets.front().id:root->id);group=planNativeTrackViewport(*baseline,request.outputs);
    if(access.console){console=access.console(*baseline);if(!console||!access.consoleCurrent||!access.consoleCurrent(*console))throw std::runtime_error("native_stream_console_profile_changed");}
    if(!nativeTopologyMatches(planNativeTopology(ccd,targets,baseline->placements),ccd,ccd))throw std::runtime_error("native_display_topology_mismatch");
    const auto checkpoint=nativeRecoveryBaselineBytes({state.get("deviceInstanceId").string(),DWORD(state.get("generation").number()),*baseline,access.recoveryScope(*baseline)});
    if(nativeActiveSignatures(access.topology.query())!=nativeActiveSignatures(baseline->ccd))throw std::runtime_error("native_display_topology_mismatch");(void)lease(false);
    mayHaveChanged=true;const auto prepared=access.exchange("worker.native.tracks.resize.prepare",key,false,checkpoint,mutationBudget);
    mayHaveChanged=prepared.requestStarted&&(!prepared.received||prepared.driverChangeAttempted||prepared.pending||prepared.recoveryRequired);
    if(!prepared.received||!prepared.accepted||!prepared.pending||prepared.recoveryRequired)throw std::runtime_error("native_change_rejected");
    key=nativeDisplayLeaseKey(prepared.snapshot);activate(false,mutationBudget);(void)verified(true,false);
    const auto committed=access.exchange("worker.native.tracks.resize.commit",key,true,"",mutationBudget);
    if(!committed.received||!committed.accepted||committed.pending||committed.recoveryRequired||nativeDisplayLeaseKey(committed.snapshot)!=key)throw std::runtime_error("native_change_rejected");
    result.display=verified(false,false);result.command=current;const auto fields=JsonParser::parse('{'+nativeTrackResizeFields(request)+'}');
    for(const auto* field:{"requestId","resizeId","topologyEpoch","fps","tracks"})std::get<Json::Object>(result.command.value)[field]=fields.get(field);
    if(initial){std::get<Json::Object>(result.command.value).erase("requestId");std::get<Json::Object>(result.command.value).erase("resizeId");}
    uint32_t maximumWidth=0,maximumHeight=0;for(const auto& output:request.outputs){maximumWidth=std::max(maximumWidth,output.width);maximumHeight=std::max(maximumHeight,output.height);}streamField(result.command,"maxWidth",std::max(result.command.get("maxWidth").number(),double(maximumWidth)));streamField(result.command,"maxHeight",std::max(result.command.get("maxHeight").number(),double(maximumHeight)));
    result.ok=true;result.reason.clear();return result;
  }catch(...){ }
  if(mayHaveChanged&&baseline&&group){
    result.rollbackAttempted=true;
    try{
      const auto restored=access.exchange("worker.native.tracks.resize.rollback","",false,"",budget);
      if(!restored.received||!restored.accepted||!restored.pending||restored.recoveryRequired)throw std::runtime_error("native_change_rejected");key=nativeDisplayLeaseKey(restored.snapshot);
      const auto targets=nativeDisplayTargets(lease(true));if(targets.size()!=baseline->targets.size())throw std::runtime_error("native_display_topology_mismatch");for(size_t i=0;i<targets.size();++i)if(targets[i].id!=baseline->targets[i].id||targets[i].width!=baseline->targets[i].width||targets[i].height!=baseline->targets[i].height)throw std::runtime_error("native_display_topology_mismatch");
      activate(true,budget);(void)verified(true,true);const auto completed=access.exchange("worker.native.tracks.resize.restored",key,true,"",budget);
      if(!completed.received||!completed.accepted||completed.pending||completed.recoveryRequired||nativeDisplayLeaseKey(completed.snapshot)!=key)throw std::runtime_error("native_change_rejected");result.display=verified(false,true);result.rollbackSucceeded=true;
    }catch(...){ }
  }
  if(mayHaveChanged&&!result.rollbackSucceeded){result.display.active=false;result.display.viewportProvisioning=false;result.display.monitorControlSupported=false;result.display.configurationPending=true;result.display.recoveryRequired=true;result.display.error="native_topology_recovery_required";}
  return result;
}
}
