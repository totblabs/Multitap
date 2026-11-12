#pragma once
#include "native_display_service.hpp"
namespace host {
struct NativeViewportContext {
  std::optional<StreamResize> pending;
  int64_t deadline=0;
  bool authorized=false,console=false,unlocked=false,stopped=true;
  std::optional<NativeRecoveryScope> scope;
  std::function<int64_t()> now=streamMonotonicMillis;
};
struct NativeViewportDispatch {bool handled=false;std::string response;};
inline NativeViewportDispatch dispatchNativeViewport(const Json& value,const NativeViewportContext& context,NativeDisplayService* service){
  const auto type=value.get("type").string();
  const bool prepare=type=="worker.native.resize.prepare",rollback=type=="worker.native.resize.rollback",commit=type=="worker.native.resize.commit",restored=type=="worker.native.resize.restored",unchanged=type=="worker.native.resize.unchanged";
  if(!prepare&&!rollback&&!commit&&!restored&&!unchanged)return {type.rfind("worker.native.",0)==0,{}};
  NativeViewportDispatch result;result.handled=true;
  if(!context.pending)return result;
  const auto reply=[&](const NativeViewportReply& status,const char* reason){
    return "{\"type\":\"worker.native.resize.reply\",\"phase\":"+jsonQuote(type)+','+nativeResizeFields(*context.pending)+",\"accepted\":"+(status.accepted?"true":"false")+",\"driverChangeAttempted\":"+(status.driverChangeAttempted?"true":"false")+",\"pending\":"+(status.pending?"true":"false")+",\"recoveryRequired\":"+(status.recoveryRequired?"true":"false")+",\"snapshot\":"+(status.snapshot.empty()?"null":status.snapshot)+",\"reason\":"+jsonQuote(reason)+'}';
  };
  try{
    const auto request=streamResizeRequest(value);
    if(unchanged)nativeStreamJournalFields(value,12);
    if(!nativeSameResize(request,*context.pending)||!service||!context.authorized||context.stopped||(!rollback&&(context.now()>=context.deadline||!context.console||!context.unlocked||!context.scope))){result.response=reply({},"native_change_not_authorized");return result;}
    const auto key=value.get("leaseKey").string();
    if(!rollback&&(!std::holds_alternative<std::string>(value.get("leaseKey").value)||key.empty()||key.size()>2048)){result.response=reply({},"invalid_native_change");return result;}
    NativeViewportReply status;
    if(prepare)status=service->prepareViewport(request,key,context.deadline,value.get("baseline").string(),context.scope);
    else if(rollback)status=service->rollbackViewport(request);
    else{
      if(!std::holds_alternative<bool>(value.get("windowsVerified").value)||!value.get("windowsVerified").boolean()){result.response=reply({},"native_windows_verification_required");return result;}
      status=unchanged?service->confirmUnchangedViewport(request,key,context.deadline,value.get("baseline").string(),context.scope):service->finishViewport(request,key,restored,true,context.scope);
    }
    result.response=reply(status,status.accepted?"":"native_change_rejected");return result;
  }catch(...){result.response=reply({},"invalid_native_change");return result;}
}
}
