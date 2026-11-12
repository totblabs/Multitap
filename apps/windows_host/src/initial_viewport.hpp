#pragma once
#include "stream_resize.hpp"
#include "native_display_worker_viewport.hpp"

namespace host {
// Initial preparation is a separate private phase. No Agent/capture starts in
// this module. The service arms correlation first, the worker performs the real
// native transaction, then the service freshly binds any console-profile journal
// and launch receipt to the NEW generation before dispatching session.start.
inline bool hasInitialViewport(const Json& command) {
  const auto* object=std::get_if<Json::Object>(&command.value);
  return object&&object->contains("initialViewport");
}
inline bool initialViewportJsonEqual(const Json& a,const Json& b,int depth=0) {
  if(depth>16||a.value.index()!=b.value.index())return false;
  if(std::holds_alternative<std::nullptr_t>(a.value))return true;
  if(const auto* value=std::get_if<bool>(&a.value))return *value==std::get<bool>(b.value);
  if(const auto* value=std::get_if<double>(&a.value))return std::isfinite(*value)&&*value==std::get<double>(b.value);
  if(const auto* value=std::get_if<std::string>(&a.value))return *value==std::get<std::string>(b.value);
  if(const auto* values=std::get_if<Json::Object>(&a.value)){const auto& other=std::get<Json::Object>(b.value);if(values->size()!=other.size())return false;for(const auto& [key,value]:*values){const auto item=other.find(key);if(item==other.end()||!initialViewportJsonEqual(value,item->second,depth+1))return false;}return true;}
  const auto& values=std::get<Json::Array>(a.value);const auto& other=std::get<Json::Array>(b.value);if(values.size()!=other.size())return false;for(size_t i=0;i<values.size();++i)if(!initialViewportJsonEqual(values[i],other[i],depth+1))return false;return true;
}
inline bool initialViewportPhase2Matches(const Json& prepared,const Json& start) {
  try{
    if(prepared.get("type").string()!="session.start"||prepared.get("epoch").number(-1)!=1||prepared.get("monitor").string()!="0"||prepared.get("resizeProtocol").string()!="epoch-v1"||hasInitialViewport(prepared)||hasInitialViewport(start))return false;
    auto actual=start;auto& fields=std::get<Json::Object>(actual.value);const auto& expected=std::get<Json::Object>(prepared.value);
    if(!std::holds_alternative<std::nullptr_t>(prepared.get("consoleProfile").value)){
      for(const auto* key:{"profileOperation","profileLaunchSha256"}){
        const auto digest=start.get(key).string();if(expected.contains(key)||digest.size()!=64||digest.find_first_not_of("0123456789abcdef")!=std::string::npos)return false;fields.erase(key);
      }
    }
    return initialViewportJsonEqual(prepared,actual);
  }catch(...){return false;}
}
inline std::optional<StreamResize> initialViewportRequest(const Json& command,bool allowDeferredProfile=false) {
  (void)viewportEncoderLimits(command);
  (void)streamSizeRequest(command);
  if(!hasInitialViewport(command))return {};
  const auto& value=command.get("initialViewport");const auto* fields=std::get_if<Json::Object>(&value.value);
  static const std::set<std::string> names{"protocol","sessionId","requestId","resizeId","monitor","width","height","fps","epoch"};
  if(!fields||fields->size()!=9||!std::all_of(fields->begin(),fields->end(),[](const auto& field){return names.contains(field.first);})||value.get("protocol").string()!="epoch-v1")throw std::runtime_error("initial_viewport_invalid");
  const auto request=streamResizeRequest(value);const auto& object=std::get<Json::Object>(command.value);
  if(!streamSizeV2(command)&&!legacyStreamOutputSize(uint64_t(request.width),uint64_t(request.height)))throw std::runtime_error("initial_viewport_invalid");
  const auto& profile=command.get("consoleProfile");const bool hasProfile=!std::holds_alternative<std::nullptr_t>(profile.value);
  const auto& media=command.get("mediaProtocol");
  if(command.get("type").string()!="session.start"||command.get("mode").string()!="stream"||command.get("resizeProtocol").string()!="epoch-v1"||command.get("epoch").number(-1)!=0||request.epoch!=1||request.monitor!="0"||command.get("monitor").string()!=request.monitor||command.get("sessionId").string()!=request.sessionId||command.get("fps").number(-1)!=request.fps||(!std::holds_alternative<std::nullptr_t>(media.value)&&(!std::holds_alternative<std::string>(media.value)||!media.string().empty()))||object.contains("tracks")||object.contains("initialTracks")||object.contains("profileOperation")||object.contains("profileLaunchSha256")||object.contains("workerDeadlineMs")||(hasProfile&&(!allowDeferredProfile||!std::holds_alternative<Json::Object>(profile.value))))throw std::runtime_error("initial_viewport_invalid");
  // stream-size-v1: the box is two per-side ceilings up to 3840 each.
  const bool v2=command.get("streamSizeProtocol").string()=="stream-size-v2";
  const int boxHeight=v2?int(ownedOutputMaxSide):(object.contains("streamSizeProtocol")?3840:2160);
  for(const auto& [key,maximum]:std::array<std::pair<const char*,int>,2>{{{"width",v2?int(ownedOutputMaxSide):3840},{"height",boxHeight}}}){
    const auto limit=command.get(key).number(-1);if(!std::isfinite(limit)||limit<(v2?16:128)||limit>maximum||std::floor(limit)!=limit||int(limit)%2)throw std::runtime_error("initial_viewport_invalid");
  }
  return request;
}
inline bool initialOwnedViewportReady(const DisplayState& display,bool nativeSelected,bool epochMedia,bool profilePending) {
  return nativeSelected&&epochMedia&&!profilePending&&display.ready()&&!display.remoteSession&&display.viewportProvisioning&&display.monitors.size()==1&&display.monitors.front().id=="0";
}
inline bool initialViewportDisplayMatches(const DisplayState& display,const StreamResize& request) {
  return display.ready()&&!display.remoteSession&&display.monitors.size()==1&&display.monitors.front().id==request.monitor&&display.monitors.front().width==DWORD(request.width)&&display.monitors.front().height==DWORD(request.height);
}
inline bool initialViewportPreparedCommandMatches(const Json& original,const Json& prepared,const DisplayState& display,const StreamResize& request) {
  try{
    if(!initialViewportDisplayMatches(display,request)||hasInitialViewport(prepared)||!request.matches(prepared))return false;
    auto expected=original;auto& fields=std::get<Json::Object>(expected.value);const auto* preparedFields=std::get_if<Json::Object>(&prepared.value);if(!preparedFields)return false;fields.erase("initialViewport");
    // Authentication/quality/profile fields must survive unchanged. These are
    // the only worker-owned deltas; even an extra field rejects the entire ACK.
    for(const auto* key:{"requestId","resizeId","epoch","captureLeft","captureTop","captureWidth","captureHeight","displayDeviceName","captureDisplays"}){const auto item=preparedFields->find(key);if(item==preparedFields->end())return false;fields[key]=item->second;}
    if(!initialViewportJsonEqual(expected,prepared)||prepared.get("captureWidth").number(-1)!=request.width||prepared.get("captureHeight").number(-1)!=request.height)return false;
    const auto& monitor=display.monitors.front();
    if(prepared.get("captureLeft").number(-65536)!=monitor.left||prepared.get("captureTop").number(-65536)!=monitor.top||prepared.get("displayDeviceName").string()!=utf8(monitor.name))return false;
    const RECT bounds{monitor.left,monitor.top,monitor.left+LONG(monitor.width),monitor.top+LONG(monitor.height)};const auto selected=parseCaptureSelection(prepared,bounds);validateCaptureSelection(selected,selected.monitors);
    return selected.monitors.size()==1&&selected.monitors.front().name==monitor.name&&sameRect(selected.monitors.front().bounds,bounds);
  }catch(...){return false;}
}
inline WorkerViewportResult applyInitialNativeViewport(const Json& command,NativeWorkerViewportAccess access,const StreamResizeBudget& budget,bool supported,bool profileReserved=false,bool allowDeferredProfile=false) {
  WorkerViewportResult result;result.reason="unsupported_display_mode";result.nativeStage=1;result.unchanged=true;
  bool coordinated=false;
  try {
    const auto parsed=initialViewportRequest(command,allowDeferredProfile);if(!parsed||!supported||profileReserved)return result;const auto request=*parsed;
    budget.remaining(1);if(!access.viewport.interactive()||!access.viewport.privileged()||!access.topology.eligible())return result;
    result.display=access.viewport.probe(budget.remaining(streamResizeProbeTimeoutMs));
    if(!result.display.ready()||result.display.remoteSession||result.display.monitors.size()!=1||result.display.monitors.front().id!="0"){result.reason="monitor_not_available";return result;}
    auto current=command;const auto selection=attachFreshCapture(current,result.display,access.viewport.observed());if(selection.monitors.size()!=1){result.reason="monitor_not_available";return result;}
    // Actual lease reserve + durable CCD checkpoint + SDK/Windows apply +
    // fresh capture verification + commit, retaining the existing rollback.
    coordinated=true;result=applyNativeWorkerViewport(current,request,request.monitor,access,budget);if(!result.ok)return result;result.unchanged=false;
    budget.remaining(1);if(!access.viewport.interactive()||!access.viewport.privileged()||!access.topology.eligible())throw std::runtime_error("initial_viewport_identity_changed");
    result.display=access.viewport.probe(budget.remaining(streamResizeProbeTimeoutMs));if(!initialViewportDisplayMatches(result.display,request))throw std::runtime_error("initial_viewport_unverified");
    const auto fresh=attachFreshCapture(result.command,result.display,access.viewport.observed());
    if(fresh.monitors.size()!=1||fresh.bounds.right-fresh.bounds.left!=request.width||fresh.bounds.bottom-fresh.bounds.top!=request.height)throw std::runtime_error("initial_viewport_unverified");
    // Physical source size is independent of the encoding ceiling.
    streamField(result.command,"width",command.get("width").number());streamField(result.command,"height",command.get("height").number());std::get<Json::Object>(result.command.value).erase("initialViewport");
    if(!initialViewportPreparedCommandMatches(command,result.command,result.display,request))throw std::runtime_error("initial_viewport_unverified");
    budget.remaining(1);result.reason.clear();return result;
  }catch(...){result.ok=false;result.reason="resize_failed";if(!result.nativeStage)result.nativeStage=9;if(coordinated)result.unchanged=false;result.display.active=false;result.display.viewportProvisioning=false;return result;}
}

// A worker that failed the initial viewport in its read-only phases reports
// unchanged:true. The released broker/client require the requested source size
// for this session, so the attempt itself still fails. The service admits a
// fallback only when its OWN protected lease is the exact start lease (no SDK
// reconfigure: every change advances the generation) with nothing pending.
struct InitialViewportFallback {DWORD session=0xffffffff;std::string lease;uint32_t stage=0,reason=0;};
// Two admissible worker outcomes:
//  - unchanged (read-only phase <=2): the service's lease key must equal the
//    start lease exactly (no generation advanced, nothing pending/recovering);
//  - restored (phase >=3 with a verified rollback): the service's CURRENT
//    owned targets must equal the start targets (same id/adapter/size;
//    the rollback legitimately advanced the generation), nothing pending or
//    recovering. The latch then binds to the current lease.
inline std::optional<InitialViewportFallback> initialViewportFallbackAdmitted(const Json& value,DWORD session,const std::string& startLease,const Json& currentSnapshot,const Json& startSnapshot={}){
  try{
    const auto* fields=std::get_if<Json::Object>(&value.value);if(!fields||value.get("type").string()!="worker.initial.result")return {};
    const auto* ok=std::get_if<bool>(&value.get("ok").value);const auto* unchanged=std::get_if<bool>(&value.get("unchanged").value);const auto* restored=std::get_if<bool>(&value.get("restored").value);
    if(!ok||*ok||startLease.empty()||!session||session==0xffffffff)return {};
    const bool same=unchanged&&*unchanged,rolledBack=!same&&restored&&*restored;if(!same&&!rolledBack)return {};
    const auto stage=value.get("nativeStage").number(-1),reason=fields->contains("nativeReason")?value.get("nativeReason").number(-1):0.0;
    if(!nativeDisplayInteger(stage,0,same?2:9)||!nativeDisplayInteger(reason,0,255)||(reason>nativeViewportReasonMax&&reason!=255))return {};
    if(!std::holds_alternative<bool>(currentSnapshot.get("configurationPending").value)||currentSnapshot.get("configurationPending").boolean()||currentSnapshot.get("recoveryRequired").boolean())return {};
    const auto lease=nativeDisplayLeaseKey(currentSnapshot);
    if(same){if(lease!=startLease)return {};}
    else{
      if(nativeDisplayLeaseKey(startSnapshot)!=startLease||currentSnapshot.get("deviceInstanceId").string()!=startSnapshot.get("deviceInstanceId").string())return {};
      const auto now=nativeDisplayTargets(currentSnapshot),before=nativeDisplayTargets(startSnapshot);if(now.size()!=before.size())return {};
      // A re-created SDK target may receive a new Windows target id; the owned
      // adapter, output ids and restored sizes must match the start exactly.
      for(size_t i=0;i<now.size();++i)if(now[i].id!=before[i].id||!nativeSameLuid(now[i].adapter,before[i].adapter)||now[i].width!=before[i].width||now[i].height!=before[i].height)return {};
    }
    return InitialViewportFallback{session,lease,uint32_t(stage),uint32_t(reason)};
  }catch(...){return {};}
}
// While armed for the same interactive session and exact lease, the host stops
// advertising initial/profile viewport and dynamic resolution, so the next
// connection uses the plain current-geometry stream that released brokers and
// clients already accept. A new lease (any SDK change), logon or service start
// clears it; nothing here changes a display or grants readiness.
class InitialViewportFallbackLatch {
 public:
  void arm(const InitialViewportFallback& value){std::lock_guard lock(mutex_);value_=value;}
  void clear(){std::lock_guard lock(mutex_);value_.reset();}
  bool armed()const{std::lock_guard lock(mutex_);return value_.has_value();}
  std::optional<InitialViewportFallback> current()const{std::lock_guard lock(mutex_);return value_;}
  bool active(DWORD session,const std::string& lease){std::lock_guard lock(mutex_);if(!value_)return false;if(value_->session==session&&value_->lease==lease&&!lease.empty())return true;value_.reset();return false;}
 private:
  mutable std::mutex mutex_;std::optional<InitialViewportFallback> value_;
};
// Service-only, under its resize mutex. Arm BEFORE worker.initial.prepare.
// Successful acceptWorker permits the later profile reserve/start, never marks
// media ready. Only matching initial stream.info commits the stream epoch.
class InitialViewportCompletion {
 public:
  bool arm(const Json& command,bool supported,bool profileReserved=false,bool allowDeferredProfile=false) {
    if(armed_||!supported||profileReserved)return false;
    try{const auto request=initialViewportRequest(command,allowDeferredProfile);if(!request)return false;request_=*request;original_=command;maximumWidth_=int(command.get("width").number());maximumHeight_=int(command.get("height").number());armed_=true;return true;}catch(...){return false;}
  }
  const std::optional<StreamResize>& request()const{return request_;}
  const std::optional<Json>& preparedCommand()const{return prepared_;}
  bool workerVerified()const{return workerVerified_;}
  bool complete()const{return complete_;}
  bool pending()const{return armed_&&!complete_;}
  bool acceptWorker(const Json& value,bool eligible,const std::function<bool(const DisplayState&)>& publish) {
    try{
      if(!pending()||workerVerified_||!eligible||!publish||value.get("type").string()!="worker.initial.result"||!request_->matches(value)||!std::holds_alternative<bool>(value.get("ok").value)||!value.get("ok").boolean()||!std::holds_alternative<bool>(value.get("desktopAvailable").value)||!value.get("desktopAvailable").boolean())return false;
      const auto display=parseDisplayState(value.get("display"));const auto& command=value.get("command");if(!initialViewportPreparedCommandMatches(original_,command,display,*request_)||!publish(display))return false;prepared_=command;workerVerified_=true;return true;
    }catch(...){return false;}
  }
  bool acceptMetadata(const Json& value,const DisplayState& committed,bool eligible) {
    if(!pending()||!workerVerified_||!eligible||value.get("type").string()!="stream.info"||!request_->matches(value)||!initialViewportDisplayMatches(committed,*request_)||value.get("codec").string()!="h264"||value.get("sourceWidth").number(-1)!=request_->width||value.get("sourceHeight").number(-1)!=request_->height||value.get("fpsTarget").number(-1)!=request_->fps)return false;
    for(const auto& [name,maximum]:std::array<std::pair<const char*,int>,2>{{{"width",maximumWidth_},{"height",maximumHeight_}}}){const auto valueNumber=value.get(name).number(-1);if(!std::isfinite(valueNumber)||valueNumber<16||valueNumber>maximum||std::floor(valueNumber)!=valueNumber||int(valueNumber)%2)return false;}
    complete_=true;return true;
  }
 private:
  std::optional<StreamResize> request_;Json original_;std::optional<Json> prepared_;int maximumWidth_=0,maximumHeight_=0;bool armed_=false,workerVerified_=false,complete_=false;
};
}
