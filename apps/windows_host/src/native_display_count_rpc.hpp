#pragma once
#include "native_display_change_rpc.hpp"
#include "native_display_worker_rpc.hpp"
namespace host {
struct NativeCountContext {
  std::optional<NativeCountRequest> request;int64_t deadline=0;
  bool applied=false,authorized=false,console=false,unlocked=false;
  std::optional<NativeRecoveryScope> scope;
  std::function<int64_t()> now=streamMonotonicMillis;
};
inline NativeViewportDispatch dispatchNativeCount(const Json& value,const NativeCountContext& context,NativeDisplayService* service){
  const auto phase=value.get("type").string();
  const bool prepare=phase=="worker.native.count.prepare",rollback=phase=="worker.native.count.rollback",commit=phase=="worker.native.count.commit",restored=phase=="worker.native.count.restored";
  if(!prepare&&!rollback&&!commit&&!restored)return {phase.rfind("worker.native.count.",0)==0,{}};
  if(!context.request)return {true,{}};
  const auto reply=[&](const NativeViewportReply& status,const char* reason){return "{\"type\":\"worker.native.count.reply\",\"phase\":"+jsonQuote(phase)+','+nativeCountFields(*context.request)+",\"accepted\":"+(status.accepted?"true":"false")+",\"driverChangeAttempted\":"+(status.driverChangeAttempted?"true":"false")+",\"pending\":"+(status.pending?"true":"false")+",\"recoveryRequired\":"+(status.recoveryRequired?"true":"false")+",\"snapshot\":"+(status.snapshot.empty()?"null":status.snapshot)+",\"reason\":"+jsonQuote(reason)+'}';};
  try{
    const auto request=nativeCountRequest(value);const bool cleanup=rollback||restored;
    if(!nativeSameCount(request,*context.request)||!service||!context.applied||(!cleanup&&(!context.authorized||context.now()>=context.deadline))||(!rollback&&(!context.console||!context.unlocked||!context.scope)))return {true,reply({},"native_change_not_authorized")};
    const auto key=value.get("leaseKey").string();
    if(!rollback&&(key.empty()||key.size()>2048))return {true,reply({},"invalid_native_change")};
    NativeViewportReply result;
    if(prepare)result=service->prepareCount(request,key,context.deadline,value.get("baseline").string(),context.scope);
    else if(rollback)result=service->rollbackCount(request);
    else{
      if(!std::holds_alternative<bool>(value.get("windowsVerified").value)||!value.get("windowsVerified").boolean())return {true,reply({},"native_windows_verification_required")};
      result=service->finishCount(request,key,restored,true,context.scope);
    }
    return {true,reply(result,result.accepted?"":"native_change_rejected")};
  }catch(...){return {true,reply({},"invalid_native_change")};}
}
inline NativeWorkerViewportReply nativeWorkerCountRpc(const NativeCountRequest& request,const std::string& phase,const std::string& key,bool verified,const std::string& baseline,const StreamResizeBudget& budget,NativeWorkerRpcAccess access){
  NativeWorkerViewportReply result;
  const bool prepare=phase=="worker.native.count.prepare",rollback=phase=="worker.native.count.rollback",commit=phase=="worker.native.count.commit",restored=phase=="worker.native.count.restored";
  if((!prepare&&!rollback&&!commit&&!restored)||!nativeCountValid(request)||(!rollback&&(key.empty()||key.size()>2048))){result.reason="invalid_native_change";return result;}
  try{
    budget.remaining(1);
    const auto message="{\"type\":"+jsonQuote(phase)+','+nativeCountFields(request)+",\"leaseKey\":"+jsonQuote(key)+",\"windowsVerified\":"+(verified?"true":"false")+(prepare?",\"baseline\":"+jsonQuote(baseline):"")+'}';
    if(message.size()>65536){result.reason="invalid_native_change";return result;}
    result.requestStarted=true;result.recoveryRequired=true;if(!access.write(message))return result;
    const auto received=access.read(budget.remaining(15000));if(!received)return result;const auto& value=*received;
    if(value.get("type").string()!="worker.native.count.reply"||value.get("phase").string()!=phase||!nativeSameCount(request,nativeCountRequest(value)))return result;
    for(const auto* name:{"accepted","driverChangeAttempted","pending","recoveryRequired"})if(!std::holds_alternative<bool>(value.get(name).value))return result;
    if(value.get("accepted").boolean()){
      const auto& snapshot=value.get("snapshot");const auto targets=nativeDisplayTargets(snapshot);(void)nativeDisplayLeaseKey(snapshot);
      if(value.get("recoveryRequired").boolean()||!std::holds_alternative<bool>(snapshot.get("configurationPending").value)||snapshot.get("configurationPending").boolean()!=value.get("pending").boolean()||value.get("pending").boolean()!=(prepare||rollback)||((prepare||commit)&&targets.size()!=size_t(request.count)))return result;
    }
    result.received=true;result.accepted=value.get("accepted").boolean();result.driverChangeAttempted=value.get("driverChangeAttempted").boolean();result.pending=value.get("pending").boolean();result.recoveryRequired=value.get("recoveryRequired").boolean();result.snapshot=value.get("snapshot");result.reason=result.accepted?"":"native_change_rejected";return result;
  }catch(...){return result;}
}
}
