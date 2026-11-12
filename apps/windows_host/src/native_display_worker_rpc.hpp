#pragma once
#include "native_display_change.hpp"
namespace host {
struct NativeWorkerRpcAccess {
  std::function<bool(const std::string&)> write;
  std::function<std::optional<Json>(DWORD)> read;
  std::function<bool(const Json&)> interleave;
};
struct NativeWorkerViewportReply {
  bool requestStarted=false,received=false,accepted=false,driverChangeAttempted=false,pending=false,recoveryRequired=false;
  Json snapshot;
  std::string reason="native_change_transport_failed";
};
inline NativeWorkerViewportReply nativeWorkerViewportRpc(const StreamResize& request,const std::string& phase,const std::string& key,bool windowsVerified,const StreamResizeBudget& budget,NativeWorkerRpcAccess access,const std::string& baseline={}){
  NativeWorkerViewportReply result;
  const bool prepare=phase=="worker.native.resize.prepare",rollback=phase=="worker.native.resize.rollback",commit=phase=="worker.native.resize.commit",restored=phase=="worker.native.resize.restored",unchanged=phase=="worker.native.resize.unchanged";
  if((!prepare&&!rollback&&!commit&&!restored&&!unchanged)||!nativeResizeValid(request)||(!rollback&&(key.empty()||key.size()>2048))){result.reason="invalid_native_change";return result;}
  try{
    budget.remaining(1);
    const auto message="{\"type\":"+jsonQuote(phase)+','+nativeResizeFields(request)+",\"leaseKey\":"+jsonQuote(key)+",\"windowsVerified\":"+(windowsVerified?"true":"false")+((prepare||unchanged)?",\"baseline\":"+jsonQuote(baseline):"")+'}';if(message.size()>65536){result.reason="invalid_native_change";return result;}
    // Even a failed write can leave a complete request at the peer. Do not
    // infer unchanged state or automatically repeat a prepare from that error.
    result.requestStarted=true;result.recoveryRequired=true;
    if(!access.write(message))return result;
    for(;;){
      const auto received=access.read(budget.remaining(15000));if(!received)return result;
      const auto& value=*received;
      if(value.get("type").string()!="worker.native.resize.reply"){
        if(access.interleave&&access.interleave(value))continue;return result;
      }
      if(value.get("phase").string()!=phase||!nativeSameResize(request,streamResizeRequest(value)))return result;
      for(const auto name:{"accepted","driverChangeAttempted","pending","recoveryRequired"})if(!std::holds_alternative<bool>(value.get(name).value))return result;
      const bool accepted=value.get("accepted").boolean(),pending=value.get("pending").boolean();
      if(accepted){
        const auto& snapshot=value.get("snapshot");const auto targets=nativeDisplayTargets(snapshot);(void)nativeDisplayLeaseKey(snapshot);
        if(unchanged&&(value.get("driverChangeAttempted").boolean()||nativeDisplayLeaseKey(snapshot)!=key))return result;
        if(value.get("recoveryRequired").boolean()||!std::holds_alternative<bool>(snapshot.get("configurationPending").value)||snapshot.get("configurationPending").boolean()!=pending||pending!=(prepare||rollback))return result;
        if(prepare||commit){const auto target=std::find_if(targets.begin(),targets.end(),[&](const auto& item){return item.id==request.monitor;});if(target==targets.end()||target->width!=DWORD(request.width)||target->height!=DWORD(request.height))return result;}
      }
      result.received=true;result.accepted=accepted;result.pending=pending;result.driverChangeAttempted=value.get("driverChangeAttempted").boolean();result.recoveryRequired=value.get("recoveryRequired").boolean();result.snapshot=value.get("snapshot");result.reason=accepted?"":"native_change_rejected";return result;
    }
  }catch(...){return result;}
}
}
