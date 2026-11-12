#pragma once
#include "native_display_change_rpc.hpp"
#include "native_display_worker_rpc.hpp"
namespace host {
struct NativeTrackViewportContext {
  std::optional<NativeTrackResize> pending;
  int64_t deadline=0;
  bool authorized=false,console=false,unlocked=false,stopped=true;
  std::optional<NativeRecoveryScope> scope;
  std::function<int64_t()> now=streamMonotonicMillis;
};
inline NativeViewportDispatch dispatchNativeTrackViewport(const Json& value,const NativeTrackViewportContext& context,NativeDisplayService* service){
  const auto type=value.get("type").string();
  const bool prepare=type=="worker.native.tracks.resize.prepare",rollback=type=="worker.native.tracks.resize.rollback",commit=type=="worker.native.tracks.resize.commit",restored=type=="worker.native.tracks.resize.restored",unchanged=type=="worker.native.tracks.resize.unchanged";
  if(!prepare&&!rollback&&!commit&&!restored&&!unchanged)return {type.rfind("worker.native.tracks.",0)==0,{}};
  NativeViewportDispatch result;result.handled=true;if(!context.pending)return result;
  const auto reply=[&](const NativeViewportReply& status,const char* reason){return "{\"type\":\"worker.native.tracks.resize.reply\",\"phase\":"+jsonQuote(type)+','+nativeTrackResizeFields(*context.pending)+",\"accepted\":"+(status.accepted?"true":"false")+",\"driverChangeAttempted\":"+(status.driverChangeAttempted?"true":"false")+",\"pending\":"+(status.pending?"true":"false")+",\"recoveryRequired\":"+(status.recoveryRequired?"true":"false")+",\"snapshot\":"+(status.snapshot.empty()?"null":status.snapshot)+",\"reason\":"+jsonQuote(reason)+'}';};
  try{
    const auto request=nativeTrackResizeRequest(value);if(unchanged)nativeStreamJournalFields(value,10);
    if(!nativeSameTrackResize(request,*context.pending)||!service||!context.authorized||context.stopped||(!rollback&&(context.now()>=context.deadline||!context.console||!context.unlocked||!context.scope))){result.response=reply({},"native_change_not_authorized");return result;}
    const auto key=value.get("leaseKey").string();if(!rollback&&(!std::holds_alternative<std::string>(value.get("leaseKey").value)||key.empty()||key.size()>2048)){result.response=reply({},"invalid_native_change");return result;}
    NativeViewportReply status;
    if(prepare)status=service->prepareTrackViewport(request,key,context.deadline,value.get("baseline").string(),context.scope);
    else if(rollback)status=service->rollbackTrackViewport(request);
    else{
      if(!std::holds_alternative<bool>(value.get("windowsVerified").value)||!value.get("windowsVerified").boolean()){result.response=reply({},"native_windows_verification_required");return result;}
      status=unchanged?service->confirmUnchangedViewport(request,key,context.deadline,value.get("baseline").string(),context.scope):service->finishTrackViewport(request,key,restored,true,context.scope);
    }
    result.response=reply(status,status.accepted?"":"native_change_rejected");return result;
  }catch(...){result.response=reply({},"invalid_native_change");return result;}
}
inline NativeWorkerViewportReply nativeWorkerTrackViewportRpc(const NativeTrackResize& request,const std::string& phase,const std::string& key,bool windowsVerified,const StreamResizeBudget& budget,NativeWorkerRpcAccess access,const std::string& baseline={}){
  NativeWorkerViewportReply result;
  const bool prepare=phase=="worker.native.tracks.resize.prepare",rollback=phase=="worker.native.tracks.resize.rollback",commit=phase=="worker.native.tracks.resize.commit",restored=phase=="worker.native.tracks.resize.restored",unchanged=phase=="worker.native.tracks.resize.unchanged";
  if((!prepare&&!rollback&&!commit&&!restored&&!unchanged)||!nativeTrackResizeValid(request)||(!rollback&&(key.empty()||key.size()>2048))){result.reason="invalid_native_change";return result;}
  try{
    budget.remaining(1);const auto message="{\"type\":"+jsonQuote(phase)+','+nativeTrackResizeFields(request)+",\"leaseKey\":"+jsonQuote(key)+",\"windowsVerified\":"+(windowsVerified?"true":"false")+((prepare||unchanged)?",\"baseline\":"+jsonQuote(baseline):"")+'}';if(message.size()>65536){result.reason="invalid_native_change";return result;}
    result.requestStarted=true;result.recoveryRequired=true;if(!access.write(message))return result;
    for(;;){
      const auto received=access.read(budget.remaining(15000));if(!received)return result;const auto& value=*received;
      if(value.get("type").string()!="worker.native.tracks.resize.reply"){if(access.interleave&&access.interleave(value))continue;return result;}
      if(value.get("phase").string()!=phase||!nativeSameTrackResize(request,nativeTrackResizeRequest(value)))return result;
      for(const auto* field:{"accepted","driverChangeAttempted","pending","recoveryRequired"})if(!std::holds_alternative<bool>(value.get(field).value))return result;
      const bool accepted=value.get("accepted").boolean(),pending=value.get("pending").boolean();
      if(accepted){
        const auto& snapshot=value.get("snapshot");const auto targets=nativeDisplayTargets(snapshot);(void)nativeDisplayLeaseKey(snapshot);if(unchanged&&(value.get("driverChangeAttempted").boolean()||nativeDisplayLeaseKey(snapshot)!=key))return result;
        if(value.get("recoveryRequired").boolean()||!std::holds_alternative<bool>(snapshot.get("configurationPending").value)||snapshot.get("configurationPending").boolean()!=pending||pending!=(prepare||rollback)||targets.size()!=request.outputs.size())return result;
        for(const auto& output:request.outputs){const auto found=std::find_if(targets.begin(),targets.end(),[&](const auto& target){return target.id==output.id;});if(found==targets.end()||((prepare||commit)&&(found->width!=output.width||found->height!=output.height)))return result;}
      }
      result.received=true;result.accepted=accepted;result.pending=pending;result.driverChangeAttempted=value.get("driverChangeAttempted").boolean();result.recoveryRequired=value.get("recoveryRequired").boolean();result.snapshot=value.get("snapshot");result.reason=accepted?"":"native_change_rejected";return result;
    }
  }catch(...){return result;}
}
}
