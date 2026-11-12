#pragma once
#include "operational_log.hpp"
#include "encoder_gpu_failure.hpp"
#include "native_activation_diagnostic.hpp"
#include "native_recovery_trace.hpp"
#include <algorithm>
#include <deque>
#include <mutex>
namespace host {
// Rows of the event log a broker acknowledged (host.diagnostics.ack accepted).
// A report carries rows the broker has not had first, so a broker that keeps a
// rolling history gets every event over successive reports instead of the same
// newest eight; the rows of a report wait in `sent_` until its ACK. Bounded to
// more than the 192-row selection window. Memory only, history only.
class HostDiagnosticDelivery {
 public:
  std::set<std::string> delivered() const {std::lock_guard lock(mutex_);return {order_.begin(),order_.end()};}
  void sent(std::vector<std::string> rows,bool backlog){std::lock_guard lock(mutex_);sent_=std::move(rows);backlog_=backlog;}
  // True when the last report left rows behind: the next one may follow in 5 s.
  bool acknowledged(bool accepted){
    std::lock_guard lock(mutex_);
    if(accepted)for(auto& row:sent_)if(std::find(order_.begin(),order_.end(),row)==order_.end()){order_.push_back(std::move(row));if(order_.size()>256)order_.pop_front();}
    sent_.clear();return accepted&&backlog_;
  }
 private:
  mutable std::mutex mutex_;std::deque<std::string> order_;std::vector<std::string> sent_;bool backlog_=false;
};
inline std::string hostDiagnosticReason(const std::string& reason){
  static const std::set<std::string> allowed{"","no_interactive_session","interactive_console_required","administrator_required","reboot_required","viewport_probe_pending","viewport_probe_failed","viewport_probe_timeout","viewport_recovery_required","owned_virtual_display_not_ready","native_display_not_ready","native_display_provider_unavailable","native_display_snapshot_invalid","native_display_configuration_untrusted","native_display_topology_mismatch","native_display_change_pending","native_topology_not_ready","native_topology_recovery_required"};
  return allowed.count(reason)?reason:"viewport_probe_failed";
}
// Historical actual CPU-fallback evidence only; never readiness/current-GPU
// authority. Reuse the existing numeric keys understood by released brokers.
inline bool hostDiagnosticGpuFailureEvent(const Json& row){
 if(row.get("event").string()!="hostAction")return false;const auto& values=row.get("values");
 const auto* ok=std::get_if<bool>(&values.get("ok").value);const auto* active=std::get_if<bool>(&values.get("active").value);if(!ok||*ok||!active||!*active)return false;
 const auto number=[&](const char* key,uint32_t& target){const auto* n=std::get_if<double>(&values.get(key).value);if(!n||!std::isfinite(*n)||*n<0||*n>4294967295.0||std::floor(*n)!=*n)return false;target=uint32_t(*n);return true;};
 EncoderGpuFailure failure;uint32_t action=0,code=0,cpu=0,track=0,epoch=0;
 return number("action",action)&&action==255&&number("stage",failure.site)&&number("win32",failure.nativeStatus)&&number("format",failure.vendorId)&&number("cause",failure.deviceId)&&
   number("error",code)&&code>=32&&code<=44&&number("fallback",cpu)&&(cpu==1||cpu==2)&&number("items",track)&&track<=1&&number("expected",epoch)&&epoch<=65535&&encoderGpuFailureDetailKnown(failure);
}
// Unexpected relay-end history only. Match the fixed probe vocabulary, never
// infer process state from an exit code or use this as readiness authority.
inline bool hostDiagnosticWorkerRelayFailureEvent(const Json& row){
 if(row.get("event").string()!="hostAction")return false;const auto& values=row.get("values");
 const auto* fields=std::get_if<Json::Object>(&values.value);const auto* ok=std::get_if<bool>(&values.get("ok").value);if(!fields||!ok||*ok)return false;
 const auto number=[&](const char* key,uint32_t& target){const auto* n=std::get_if<double>(&values.get(key).value);if(!n||!std::isfinite(*n)||*n<0||*n>4294967295.0||std::floor(*n)!=*n)return false;target=uint32_t(*n);return true;};
 uint32_t action=0,stage=0,cause=0,code=0;
 if(!number("action",action)||action!=260||!number("stage",stage)||stage>5||!number("cause",cause)||cause>13)return false;
 const bool hasRunning=fields->contains("running"),hasError=fields->contains("error"),hasWin32=fields->contains("win32");
 const auto* running=std::get_if<bool>(&values.get("running").value);
 if(stage==1)return running&&*running&&!hasError&&!hasWin32;
 if(stage==2)return running&&!*running&&number("error",code)&&!hasWin32;
 if(stage==3)return !hasRunning&&!hasError&&(!hasWin32||number("win32",code));
 if(stage==4)return running&&!*running&&!hasError&&number("win32",code);
 return !hasRunning&&!hasError&&!hasWin32;
}
// hostAction 270 (udp_probe.hpp), one per grant, so ordinary events must not push
// it out before a report leaves. History only; a session never depends on it.
inline bool hostDiagnosticUdpProbeEvent(const Json& row){
 if(row.get("event").string()!="hostAction")return false;const auto& values=row.get("values");
 const auto* ok=std::get_if<bool>(&values.get("ok").value);const auto* action=std::get_if<double>(&values.get("action").value);const auto* stage=std::get_if<double>(&values.get("stage").value);const auto* relay=std::get_if<double>(&values.get("error").value);
 return ok&&action&&stage&&relay&&*action==270&&*stage>=1&&*stage<=4&&std::floor(*stage)==*stage&&*relay>=0&&*relay<8&&std::floor(*relay)==*relay&&*ok==(*stage==1);
}
// hostAction 271 (udp_media_path.hpp) leaving UDP (stage 2, with its cause) or
// holding through a dark network (stage 3): the newest one, as a phone session's
// fallbacks were pushed out by its own later probes and switches.
inline bool hostDiagnosticUdpPathEvent(const Json& row){
 if(row.get("event").string()!="hostAction")return false;const auto& values=row.get("values");
 const auto* action=std::get_if<double>(&values.get("action").value);const auto* stage=std::get_if<double>(&values.get("stage").value);
 return action&&stage&&*action==271&&(*stage==2||*stage==3);
}
// hostAction 272 (broker_route.hpp): whether the channel took the Broker's inside
// address. It comes right before routine observations, so keep the newest one.
inline bool hostDiagnosticBrokerRouteEvent(const Json& row){
 if(row.get("event").string()!="hostAction")return false;const auto& values=row.get("values");
 const auto* ok=std::get_if<bool>(&values.get("ok").value);const auto* action=std::get_if<double>(&values.get("action").value);const auto* stage=std::get_if<double>(&values.get("stage").value);
 return ok&&action&&stage&&*action==272&&(*stage==1||*stage==2)&&*ok==(*stage==1);
}
// hostAction 273 (display_adapter_inventory.hpp): the newest inventory change or
// cleanup round, so a VM's leftover adapters stay visible next to its routine events.
inline bool hostDiagnosticDisplayAdapterEvent(const Json& row){
 if(row.get("event").string()!="hostAction")return false;const auto& values=row.get("values");
 const auto* action=std::get_if<double>(&values.get("action").value);const auto* stage=std::get_if<double>(&values.get("stage").value);
 return action&&stage&&*action==273&&(*stage==0||*stage==1||*stage==5);
}
// hostAction 250 stage 31 (usb_core.hpp): the newest game-mouse summary, whether the VM read it and how
// many reports came. It has a place of its own: the other kept events can leave no room for any USB event.
inline bool hostDiagnosticVirtualMouseEvent(const Json& row){
 if(row.get("event").string()!="hostAction")return false;const auto& values=row.get("values");
 const auto* action=std::get_if<double>(&values.get("action").value);const auto* stage=std::get_if<double>(&values.get("stage").value);
 return action&&stage&&*action==250&&*stage==31;
}
// hostAction 274 (boot_timeline.hpp): the newest boot-to-ready timeline. One per
// service run, so a day of routine events must not push it out of the report.
inline bool hostDiagnosticBootEvent(const Json& row){
 if(row.get("event").string()!="hostAction")return false;
 const auto* action=std::get_if<double>(&row.get("values").get("action").value);
 return action&&*action==274;
}
// hostAction 275 (process_exit_record.hpp): the newest unexpected end of the
// service or a worker. Rare, and the first thing to read after one, so a day of
// routine events must not push it out of the report.
inline bool hostDiagnosticProcessExitEvent(const Json& row){
 if(row.get("event").string()!="hostAction")return false;
 const auto* action=std::get_if<double>(&row.get("values").get("action").value);
 return action&&*action==275;
}
// Optional observation only: no device identity or readiness authority. An
// invalid vector is omitted in full rather than presenting a partial topology.
inline std::string hostDiagnosticMonitors(const DisplayState& display,bool observed){
  if(!observed||!display.owned||display.monitors.size()>2)return "";
  std::set<std::string> ids;unsigned primaries=0;
  std::string result="[";
  for(const auto& monitor:display.monitors){
    if((monitor.id!="0"&&monitor.id!="1")||!ids.insert(monitor.id).second||
       !monitor.width||monitor.width>16384||!monitor.height||monitor.height>16384||
       monitor.left<-32768||monitor.left>32768||monitor.top<-32768||monitor.top>32768||
       monitor.primary!=(monitor.left==0&&monitor.top==0)||
       (monitor.primary&&++primaries>1))return "";
    if(result.size()>1)result+=',';
    result+="{\"id\":"+jsonQuote(monitor.id)+",\"width\":"+std::to_string(monitor.width)+",\"height\":"+std::to_string(monitor.height)+",\"left\":"+std::to_string(monitor.left)+",\"top\":"+std::to_string(monitor.top)+",\"primary\":"+(monitor.primary?"true":"false")+'}';
  }
  return ",\"monitors\":"+result+']';
}
inline std::string hostDiagnosticConsoleRecovery(const Json& value){
  const auto* fields=std::get_if<Json::Object>(&value.value);if(!fields||fields->size()!=12)return "";
  static const std::set<std::string> reasons{"","native_provider_required","reboot_required","virtual_device_not_ready","windows_session_unknown","windows_session_locked","windows_identity_required","windows_identity_mismatch","windows_login_required","windows_session_ambiguous","rdp_session_active","rdp_session_disconnected","console_unavailable","console_in_use","operation_cancelled","console_busy","console_timeout","console_transfer_failed","console_transfer_pending","console_binding_changed","worker_failed","administrator_required"};
  if(!std::holds_alternative<std::string>(value.get("reason").value)||!reasons.count(value.get("reason").string()))return "";
  std::string out=",\"consoleRecovery\":{\"reason\":"+jsonQuote(value.get("reason").string());
  for(const auto* key:{"ready","eligible","complete","configuredIdentity","consoleIdentityKnown","consoleOccupied"}){const auto* bit=std::get_if<bool>(&value.get(key).value);if(!bit)return "";out+=","+jsonQuote(key)+":"+(*bit?"true":"false");}
  for(const auto* key:{"activeRdpSessions","disconnectedRdpSessions","disconnectedConsoleSessions","unknownSessions","lockedSessions"}){const auto number=value.get(key).number(-1);if(!std::holds_alternative<double>(value.get(key).value)||!std::isfinite(number)||number<0||number>64||std::floor(number)!=number)return "";out+=","+jsonQuote(key)+":"+std::to_string(unsigned(number));}return out+'}';
}
// Optional native profile-recovery explanation (native_recovery_trace.hpp).
// Re-encoded through the compiled vocabularies; any deviation drops it whole.
inline std::string hostDiagnosticRecovery(const Json& value){
  try{
    const auto* fields=std::get_if<Json::Object>(&value.value);if(!fields)return "";
    std::string out=",\"recovery\":{";bool first=true;const auto add=[&](const std::string& key,const std::string& encoded){if(!first)out+=',';first=false;out+=jsonQuote(key)+':'+encoded;};
    const auto code=[&](const char* key,const std::set<std::string>& codes){const auto* text=std::get_if<std::string>(&value.get(key).value);if(!text||!codes.count(*text))throw std::runtime_error("invalid");add(key,jsonQuote(*text));};
    for(size_t i=0;i<size_t(NativeRecoveryField::count);++i){const auto field=NativeRecoveryField(i);code(nativeRecoveryFieldName(field),nativeRecoveryCodes(field));}
    const auto& records=value.get("records");const auto* recordFields=std::get_if<Json::Object>(&records.value);if(!recordFields||recordFields->size()!=nativeRecoveryRecordNames.size())return "";
    std::string encoded="{";for(size_t i=0;i<nativeRecoveryRecordNames.size();++i){const auto* text=std::get_if<std::string>(&records.get(nativeRecoveryRecordNames[i]).value);if(!text||!nativeRecoveryRecordStates().count(*text))return "";if(i)encoded+=',';encoded+=jsonQuote(nativeRecoveryRecordNames[i])+':'+jsonQuote(*text);}add("records",encoded+'}');
    code("profilePhase",nativeRecoveryProfilePhases());code("runtimePhase",nativeRecoveryRuntimePhases());code("launchService",nativeRecoveryProcessStates());code("launchWorker",nativeRecoveryProcessStates());code("logon",nativeRecoveryLogonStates());code("retirementPhase",nativeRecoveryMarkerPhases());code("expiredConsolePhase",nativeRecoveryMarkerPhases());
    for(const auto* key:{"profileResize","profileStopRequested","profileLaunchBound","runtimeCheckpoint","runtimeMatchesProfile","launchMatchesProfile","identityMatchesLaunch"}){const auto& item=value.get(key);if(std::holds_alternative<std::nullptr_t>(item.value)&&fields->contains(key))add(key,"null");else if(const auto* bit=std::get_if<bool>(&item.value))add(key,*bit?"true":"false");else return "";}
    const auto attempts=value.get("attempts").number(-1);if(!std::holds_alternative<double>(value.get("attempts").value)||!std::isfinite(attempts)||attempts<0||attempts>255||std::floor(attempts)!=attempts)return "";add("attempts",std::to_string(unsigned(attempts)));
    const auto* exhausted=std::get_if<bool>(&value.get("budgetExhausted").value);if(!exhausted)return "";add("budgetExhausted",*exhausted?"true":"false");
    const auto& age=value.get("lastAttemptAgeMs");if(std::holds_alternative<std::nullptr_t>(age.value)&&fields->contains("lastAttemptAgeMs"))add("lastAttemptAgeMs","null");
    else{const auto* number=std::get_if<double>(&age.value);if(!number||!std::isfinite(*number)||*number<0||*number>4294967295.0||std::floor(*number)!=*number)return "";add("lastAttemptAgeMs",std::to_string(uint64_t(*number)));}
    return out+'}';
  }catch(...){return "";}
}
// devices-v1 (optional, top level): display-adapters-v1 counts and the USB
// controller owner, re-encoded through fixed vocabularies; anything else drops
// the whole object. Counts only, never instance IDs or device names.
inline std::string hostDiagnosticDevices(const std::string& encoded){
  try{
    if(encoded.empty())return "";
    const auto value=JsonParser::parse(encoded);if(value.get("schema").number()!=1)return "";
    static const std::set<std::string> owners{"none","started","stopped","unmarked","duplicates","foreign","multiple","unknown"};
    const auto owner=value.get("usbController").string();if(!owners.count(owner))return "";
    const auto& adapters=value.get("displayAdapters");const auto* read=std::get_if<bool>(&adapters.get("read").value);if(!read)return "";
    std::string out=",\"devices\":{\"schema\":1,\"displayAdapters\":{\"read\":"+std::string(*read?"true":"false");
    if(*read){
      for(const auto* key:{"current","legacy","duplicate","unproven","foreign"}){const auto n=adapters.get(key).number(-1);if(!std::holds_alternative<double>(adapters.get(key).value)||n<0||n>64||std::floor(n)!=n)return "";out+=","+jsonQuote(key)+":"+std::to_string(unsigned(n));}
      const auto* started=std::get_if<bool>(&adapters.get("currentStarted").value);if(!started)return "";out+=std::string(",\"currentStarted\":")+(*started?"true":"false");
      static const std::set<std::string> blockers{"","inventory_unavailable","current_not_ready","display_state_pending","busy"},cleanups{"none","removed","pending_restart","failed","refused"};
      const auto blocker=adapters.get("blocker").string(),cleanupState=adapters.get("cleanup").string();if(!blockers.count(blocker)||!cleanups.count(cleanupState))return "";
      out+=",\"blocker\":"+jsonQuote(blocker)+",\"cleanup\":"+jsonQuote(cleanupState);
    }
    return out+"},\"usbController\":"+jsonQuote(owner)+'}';
  }catch(...){return "";}
}
inline std::string buildHostDiagnosticReport(const DisplayState& display,bool interactive,bool console,bool capture,const std::string& reason,const Json& native,const Json& events,bool observed=true,int64_t observationAgeMs=0,const Json& consoleRecovery={},const Json& recovery={},const std::string& devices={},HostDiagnosticDelivery* delivery=nullptr){
  const auto bit=[](bool value){return value?"true":"false";};
  std::string detail="{\"available\":false}";
  if(native.get("schema").number()==1){
    detail="{\"available\":true";
    for(const auto* key:{"installed","started","leaseActive","activationVerified","configurationPending","recoveryRequired","countControlAvailable"})detail+=","+jsonQuote(key)+":"+bit(native.get(key).boolean());
    const auto phase=native.get("phase").string(),failure=native.get("failureReason").string();
    static const std::set<std::string> phases{"empty","starting","stable","changing","stopped","recovery"};
    static const std::set<std::string> failures{"","runtime_recovery_required","transport_open_failed","controller_start_failed","heartbeat_failed","native_stream_profile_recovery_required","internal_failure"};
    detail+=",\"phase\":"+jsonQuote(phases.count(phase)?phase:"unknown")+",\"failureReason\":"+jsonQuote(failures.count(failure)?failure:"internal_failure");
    if(std::holds_alternative<bool>(native.get("activationCurrent").value))try{
      detail+=",\"activation\":"+nativeActivationDiagnosticJson(parseNativeActivationDiagnostic(native.get("activation")))+",\"activationCurrent\":"+bit(native.get("activationCurrent").boolean());
    }catch(...){ }
    detail+=hostDiagnosticRecovery(recovery);
    detail+='}';
  }
  const std::string observation="\"observed\":"+std::string(bit(observed))+",\"observationAgeMs\":"+(observed?std::to_string(std::clamp<int64_t>(observationAgeMs,0,4294967295LL)):"null")+',';
  const auto head="{\"protocol\":\"host-diagnostics-v1\",\"source\":\"host-service\",\"version\":\"" MULTITAP_HOST_VERSION_UTF8 "\",\"collectedAt\":"+jsonQuote(hostLogTime())+",\"interactiveSession\":"+bit(interactive)+",\"consoleSession\":"+bit(console)+",\"captureAvailable\":"+bit(capture)+
    ",\"display\":{"+observation+"\"installed\":"+bit(display.installed)+",\"deviceReady\":"+bit(display.deviceReady)+",\"owned\":"+bit(display.owned)+",\"activeDisplay\":"+bit(display.active)+",\"configurationPending\":"+bit(display.configurationPending)+",\"recoveryRequired\":"+bit(display.recoveryRequired)+",\"rebootRequired\":"+bit(display.rebootRequired)+",\"remoteSession\":"+bit(display.remoteSession)+",\"monitorCount\":"+std::to_string(display.monitors.size())+",\"width\":"+std::to_string(display.width)+",\"height\":"+std::to_string(display.height)+",\"reason\":"+jsonQuote(hostDiagnosticReason(reason))+hostDiagnosticMonitors(display,observed)+"},\"native\":"+detail+hostDiagnosticConsoleRecovery(consoleRecovery)+hostDiagnosticDevices(devices)+",\"events\":";
  // History only, never readiness authority. Keep two distinct clipboard
  // failures, the latest valid channel close, then admission/restore causes.
  // Coalesce recovery/catalog maintenance, but retain the worker-exit wait and
  // both preparation layers separately. Remaining slots prefer the newest
  // unexpected process end, boot timeline, valid GPU failure, unexpected relay end, UDP probe result or Broker route result,
  // at most one of each, in event order before ordinary observations within the same eight-event budget.
  // Original timestamps/order and the broker's 6144-byte bound remain.
  const size_t budget=head.size()+2<6144?std::min<size_t>(4000,6144-head.size()-2):0;
  std::map<size_t,std::string> kept;size_t used=2;bool backlog=false;
  if(const auto* array=std::get_if<Json::Array>(&events.value)){
    const size_t first=array->size()>192?array->size()-192:0;
    const auto delivered=delivery?delivery->delivered():std::set<std::string>{};
    bool fresh=false;
    const auto admit=[&](size_t index){
      if(kept.size()>=8||kept.count(index))return false;
      const auto safe=safeHostLogRecord((*array)[index]);if(safe.empty()||(fresh&&delivered.count(safe)))return false;
      const size_t cost=safe.size()+(kept.empty()?0:1);if(used+cost>budget)return false;
      used+=cost;kept.emplace(index,safe);return true;
    };
    const auto select=[&]{
    std::set<std::string> failures;
    for(size_t i=array->size();i>first&&failures.size()<2;--i){
      const auto& row=(*array)[i-1];const auto& values=row.get("values");const auto stage=values.get("stage").number(-1);
      const auto* ok=std::get_if<bool>(&values.get("ok").value);
      if(row.get("event").string()!="hostAction"||values.get("action").number()!=253||!ok||*ok||!(stage==5||stage==6||stage==7||stage==9))continue;
      const auto key=std::to_string(stage)+":"+std::to_string(values.get("error").number())+":"+std::to_string(values.get("cause").number())+":"+std::to_string(values.get("win32").number());
      if(!failures.count(key)&&admit(i-1))failures.insert(key);
    }
    // A recent channel close must survive successful observations and older
    // display failures. Fixed history only: it cannot change current readiness.
    for(size_t i=array->size();i>first;--i){
      const auto& row=(*array)[i-1];const auto& values=row.get("values");
      const auto* action=std::get_if<double>(&values.get("action").value);
      const auto* stage=std::get_if<double>(&values.get("stage").value);
      const auto* error=std::get_if<double>(&values.get("error").value);
      const auto* ok=std::get_if<bool>(&values.get("ok").value);
      const auto& cause=values.get("cause");const auto* closeCode=std::get_if<double>(&cause.value);
      if(row.get("event").string()!="hostAction"||!action||*action!=241||!ok||*ok||
         !stage||!std::isfinite(*stage)||std::floor(*stage)!=*stage||*stage<1||*stage>6||
         !error||!std::isfinite(*error)||std::floor(*error)!=*error||*error<0||*error>UINT32_MAX||
         (!std::holds_alternative<std::nullptr_t>(cause.value)&&(!closeCode||!std::isfinite(*closeCode)||std::floor(*closeCode)!=*closeCode||*closeCode<1000||*closeCode>4999)))continue;
      if(admit(i-1))break;
    }
    std::set<unsigned> admission;
    // A fixed worker failure precedes the generic terminal stage 50. Keep the
    // cause in category 3 instead of letting that later label overwrite it.
    // Stage 240 is a recovered encoder fallback, so it is not a fatal cause.
    // This remains timestamped history, never current-session/readiness proof.
    for(size_t i=array->size();i>first;--i){
      const auto& row=(*array)[i-1];const auto& values=row.get("values");
      const auto* ok=std::get_if<bool>(&values.get("ok").value);
      const auto* stage=std::get_if<double>(&values.get("stage").value);
      const auto* error=std::get_if<double>(&values.get("error").value);
      if(row.get("event").string()!="hostAction"||values.get("action").number()!=200||!ok||*ok||
         !stage||!std::isfinite(*stage)||std::floor(*stage)!=*stage||*stage<201||*stage>239||
         !error||!std::isfinite(*error)||std::floor(*error)!=*error||*error<0||*error>UINT32_MAX)continue;
      if(admit(i-1)){admission.insert(3);break;}
    }
    for(size_t i=array->size();i>first&&admission.size()<5;--i){
      const auto& row=(*array)[i-1];const auto& values=row.get("values");
      const auto* ok=std::get_if<bool>(&values.get("ok").value);
      if(row.get("event").string()!="hostAction"||!ok||*ok)continue;
      const auto action=values.get("action").number(-1),stage=values.get("stage").number(-1);
      const unsigned category=action==220&&stage==3?1:action==220&&(stage==4||stage==5)?2:action==200?3:action==210?4:action==214?5:0;
      if(category&&!admission.count(category)&&admit(i-1))admission.insert(category);
    }
    // Existing clipboard/admission priorities stay first. With one slot the
    // newer valid failure wins; with room for two retain both distinct kinds.
    bool gpuFailure=false,relayFailure=false,udpProbe=false,brokerRoute=false,udpPath=false,adapters=false,virtualMouse=false,boot=false,processExit=false;
    for(size_t i=array->size();i>first&&kept.size()<8;--i){
      const auto& row=(*array)[i-1];
      if(!processExit&&hostDiagnosticProcessExitEvent(row)){if(admit(i-1))processExit=true;}
      else if(!boot&&hostDiagnosticBootEvent(row)){if(admit(i-1))boot=true;}
      else if(!virtualMouse&&hostDiagnosticVirtualMouseEvent(row)){if(admit(i-1))virtualMouse=true;}
      else if(!gpuFailure&&hostDiagnosticGpuFailureEvent(row)){if(admit(i-1))gpuFailure=true;}
      else if(!relayFailure&&hostDiagnosticWorkerRelayFailureEvent(row)){if(admit(i-1))relayFailure=true;}
      else if(!udpProbe&&hostDiagnosticUdpProbeEvent(row)){if(admit(i-1))udpProbe=true;}
      else if(!brokerRoute&&hostDiagnosticBrokerRouteEvent(row)){if(admit(i-1))brokerRoute=true;}
      else if(!udpPath&&hostDiagnosticUdpPathEvent(row)){if(admit(i-1))udpPath=true;}
      else if(!adapters&&hostDiagnosticDisplayAdapterEvent(row)){if(admit(i-1))adapters=true;}
      if(gpuFailure&&relayFailure&&udpProbe&&brokerRoute&&udpPath&&adapters&&virtualMouse&&boot&&processExit)break;
    }
    for(size_t i=array->size();i>first&&kept.size()<8;--i){
      const auto& row=(*array)[i-1];
      if(hostDiagnosticProcessExitEvent(row)||hostDiagnosticBootEvent(row)||hostDiagnosticVirtualMouseEvent(row)||hostDiagnosticGpuFailureEvent(row)||hostDiagnosticWorkerRelayFailureEvent(row)||hostDiagnosticUdpProbeEvent(row)||hostDiagnosticBrokerRouteEvent(row)||hostDiagnosticUdpPathEvent(row)||hostDiagnosticDisplayAdapterEvent(row))continue;
      admit(i-1);
    }
    };
    // Rows the broker has not had go first, by the same priorities; then the
    // usual selection fills what is left, so a quiet log reports as before.
    if(delivery){fresh=true;select();fresh=false;}
    select();
    if(delivery)for(size_t i=first;i<array->size()&&!backlog;++i)if(!kept.count(i)){const auto safe=safeHostLogRecord((*array)[i]);backlog=!safe.empty()&&!delivered.count(safe);}
  }
  std::string log="[";std::vector<std::string> rows;for(const auto& [index,row]:kept){(void)index;if(log.size()>1)log+=',';log+=row;rows.push_back(row);}log+=']';
  if(delivery)delivery->sent(std::move(rows),backlog);
  return head+log+'}';
}
}
