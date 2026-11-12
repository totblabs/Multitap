#pragma once
#include "stream_console_recovery.hpp"
#include "stream_resize.hpp"
#include "windows_error.hpp"

namespace host {
struct StreamConsoleWorkerAccess {
  std::function<bool(const std::string&)> write;
  std::function<std::optional<Json>(DWORD)> read;
  std::function<StreamConsoleBinding()> binding;
  std::function<ConsoleTransferObservation()> observe;
  std::function<bool(DWORD,DWORD)> connect;
  std::function<int64_t()> now=streamMonotonicMillis;
  std::function<void()> pause=[] {Sleep(100);};
  std::function<bool()> cancelled=[] {return false;};
};
// This command is accepted only on the service's authenticated private worker
// pipe. The external broker never supplies Windows session IDs or identity.
inline void prepareStreamConsoleWorker(const Json& request,StreamConsoleWorkerAccess access){
  const auto key=displayOperationKey(request);const auto expected=parseStreamConsoleBinding(request.get("sourceBinding"));
  const auto target=request.get("consoleSessionId").number();const auto deadline=request.get("workerDeadlineMs").number();
  if(!std::isfinite(target)||target<1||target>=UINT32_MAX||std::floor(target)!=target||DWORD(target)==expected.session||!std::isfinite(deadline)||std::floor(deadline)!=deadline||deadline<1||deadline>9007199254740991.0)throw std::runtime_error("windows_session_unknown");
  const auto current=[&]{return !access.cancelled()&&access.now()<deadline;};
  const auto observation=[&]{
    if(!current())throw std::runtime_error("operation_cancelled");
    if(!sameStreamConsoleBinding(expected,access.binding()))throw std::runtime_error("console_binding_changed");
    const auto value=access.observe();
    if(value.source!=expected.session||(value.console!=DWORD(target)&&!(value.console==expected.session&&value.protocol==0&&value.sourceActive)))throw std::runtime_error("console_binding_changed");
    return value;
  };
  bool prepared=false,attempted=false;DWORD systemError=0;
  const auto reply=[&](const char* type,bool ok,const std::string& reason){return access.write("{\"type\":"+jsonQuote(type)+','+displayOperationFields(key)+",\"ok\":"+(ok?"true":"false")+",\"attempted\":"+(attempted?"true":"false")+",\"reason\":"+jsonQuote(reason.empty()?"":safeStreamConsoleReason(reason))+",\"systemError\":"+std::to_string(systemError)+'}');};
  try{
    if(deadline-access.now()>20000||!current()){reply("worker.console.prepared",false,"console_timeout");return;}
    const auto first=observation();
    if(const auto reason=consoleTransferReason(first,true);*reason){reply("worker.console.prepared",false,std::string(reason)=="administrator_required"?"administrator_required":"console_binding_changed");return;}
    if(!reply("worker.console.prepared",true,""))return;prepared=true;
    const auto remaining=std::min<int64_t>(5000,int64_t(deadline)-access.now());if(remaining<=0)return;
    const auto command=access.read(DWORD(remaining));if(!command||!key.matches(*command)||command->get("type").string()!="worker.console.apply"||!current())return;
    const auto result=transferOwnConsole({observation,[&](DWORD source,DWORD console){if(!current())throw std::runtime_error("operation_cancelled");if(!sameStreamConsoleBinding(expected,access.binding()))throw std::runtime_error("console_binding_changed");attempted=true;return access.connect(source,console);},access.pause},expected.session,DWORD(target),true);
    reply("worker.console.result",result.completed,result.completed?"":"console_transfer_pending");
  }catch(const WindowsOperationError& error){systemError=error.systemError();reply(prepared?"worker.console.result":"worker.console.prepared",false,"console_transfer_failed");}
  catch(const std::exception& error){const std::string reason=error.what();reply(prepared?"worker.console.result":"worker.console.prepared",false,reason=="console_transfer_rejected"?"console_transfer_failed":safeStreamConsoleReason(reason));}
  catch(...){reply(prepared?"worker.console.result":"worker.console.prepared",false,"worker_failed");}
}
}
