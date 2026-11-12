#pragma once
#include "machine.hpp"
#include "display_operation.hpp"
#include "capture_selection.hpp"
#include "owned_output_size.hpp"
#include "stream_size.hpp"
#include <tuple>
#include <optional>
#include <array>

namespace host {
inline std::optional<DecoderLimits> resizeDecoderLimits(const Json& request,bool negotiatedV2){
  const auto* fields=std::get_if<Json::Object>(&request.value);
  if(!fields||!fields->contains("decoderLimits"))return {};
  if(!negotiatedV2)throw std::runtime_error("Invalid resize decoder limits");
  return streamDecoderLimits(request.get("decoderLimits"),true);
}
// Call only after the active session/request/epoch has been correlated. This
// is a candidate copy: failure/restore must retain the original command.
inline Json resizeDecoderCommand(const Json& current,const Json& request){
  const auto limits=resizeDecoderLimits(request,streamSizeV2(current));
  if(!limits)return current;
  const bool tracks=current.get("mediaProtocol").string()=="tracks-v1";
  const auto epoch=tracks?"topologyEpoch":"epoch";
  if(current.get("type").string()!="session.start"||current.get("sessionId").string().empty()||current.get("sessionId").string()!=request.get("sessionId").string()||request.get(epoch).number(-1)<=current.get(epoch).number(0))throw std::runtime_error("Resize decoder limits are not current");
  auto next=current;auto& fields=std::get<Json::Object>(next.value);
  fields["decoderLimits"]=request.get("decoderLimits");
  Json width,height;width.value=double(std::min(limits->maxWidth,int(ownedOutputMaxSide)));height.value=double(std::min(limits->maxHeight,int(ownedOutputMaxSide)));
  fields[tracks?"maxWidth":"width"]=std::move(width);fields[tracks?"maxHeight":"height"]=std::move(height);
  return next;
}
inline Json preparedResizeDecoderCommand(const Json& current,const Json& prepared,const Json& request){
  const auto limits=resizeDecoderLimits(request,streamSizeV2(current));
  if(!limits)return prepared;
  const auto candidate=resizeDecoderCommand(current,request);
  const bool tracks=current.get("mediaProtocol").string()=="tracks-v1";
  const auto epoch=tracks?"topologyEpoch":"epoch";
  if(prepared.get("type").string()!="session.start"||prepared.get("sessionId").string()!=request.get("sessionId").string()||prepared.get("requestId").string()!=request.get("requestId").string()||prepared.get("resizeId").string()!=request.get("resizeId").string()||prepared.get(epoch).number(-1)!=request.get(epoch).number(-2))throw std::runtime_error("Prepared resize decoder limits are not current");
  auto next=prepared;auto& fields=std::get<Json::Object>(next.value);
  for(const auto* key:tracks?std::array<const char*,3>{"decoderLimits","maxWidth","maxHeight"}:std::array<const char*,3>{"decoderLimits","width","height"})fields[key]=candidate.get(key);
  return next;
}
struct StreamResize {
  std::string sessionId, requestId, resizeId, monitor;
  int width = 0, height = 0, fps = 0; uint16_t epoch = 0;
  bool matches(const Json& value) const { return sessionId == value.get("sessionId").string() && requestId == value.get("requestId").string() && resizeId == value.get("resizeId").string() && epoch == value.get("epoch").number(-1); }
};
struct ViewportEncoderLimits {
  int width=0,height=0;
  bool matches(const Json& value,const StreamResize& request)const{
    const auto encoded=[&](const char* key,int maximum){const auto size=value.get(key).number(-1);return std::isfinite(size)&&std::floor(size)==size&&size>=16&&size<=maximum&&!(int(size)&1);};
    return encoded("width",std::min(width,request.width))&&encoded("height",std::min(height,request.height))&&value.get("sourceWidth").number(-1)==request.width&&value.get("sourceHeight").number(-1)==request.height&&value.get("fpsTarget").number(-1)==request.fps;
  }
};
// Candidate for this already-correlated resize only. A portrait/landscape
// decoder query can change either axis independently; the initial encoding
// box is not the ceiling for every later epoch. Keep the active box until the
// service verifies the new display and matching metadata. Legacy requests
// without a fresh negotiated decoder observation retain their existing box.
inline std::optional<ViewportEncoderLimits> resizeViewportEncoderLimits(const std::optional<ViewportEncoderLimits>& current,const Json& request,bool negotiatedV2){
  const auto decoder=resizeDecoderLimits(request,negotiatedV2);
  if(!current||!decoder)return current;
  return ViewportEncoderLimits{std::min(decoder->maxWidth,int(ownedOutputMaxSide)),std::min(decoder->maxHeight,int(ownedOutputMaxSide))};
}
inline std::optional<ViewportEncoderLimits> viewportEncoderLimits(const Json& command){
  const auto* fields=std::get_if<Json::Object>(&command.value);
  if(!fields||!fields->contains("viewportEncoderLimitsProtocol"))return {};
  if(command.get("viewportEncoderLimitsProtocol").string()!="separate-v1"||command.get("type").string()!="session.start"||command.get("mode").string()!="stream"||command.get("resizeProtocol").string()!="epoch-v1"||command.get("monitor").string()!="0"||fields->contains("mediaProtocol")||fields->contains("tracks")||fields->contains("initialTracks"))throw std::runtime_error("Invalid viewport encoder limits");
  ViewportEncoderLimits limits;
  const auto dimension=[&](const char* key,int maximum){const auto size=command.get(key).number(-1);if(!std::isfinite(size)||size<(command.get("streamSizeProtocol").string()=="stream-size-v2"?16:128)||size>maximum||std::floor(size)!=size||(int(size)&1))throw std::runtime_error("Invalid viewport encoder limits");return int(size);};
  // stream-size-v1: the box is two per-side ceilings up to 3840 each.
  const bool streamSize=fields->contains("streamSizeProtocol");
  const bool v2=command.get("streamSizeProtocol").string()=="stream-size-v2";
  limits.width=dimension("width",v2?int(ownedOutputMaxSide):3840);limits.height=dimension("height",v2?int(ownedOutputMaxSide):(streamSize?3840:2160));return limits;
}
inline StreamResize streamResizeRequest(const Json& value) {
  StreamResize request; request.sessionId=value.get("sessionId").string();request.requestId=value.get("requestId").string();request.resizeId=value.get("resizeId").string();request.monitor=value.get("monitor").string();
  // Physical workspace geometry is independent of the picture decoder box.
  // Its selected provider/CCD result is verified by the actual transaction.
  const auto dimension=[&](const char* key,int minimum,int maximum){const auto number=value.get(key).number(-1);if(number<minimum||number>maximum||std::floor(number)!=number||int(number)%2)throw std::runtime_error("Invalid viewport dimensions");return int(number);};
  request.width=dimension("width",320,int(ownedOutputMaxSide));request.height=dimension("height",180,int(ownedOutputMaxSide));const auto fps=value.get("fps").number(-1),epoch=value.get("epoch").number(-1);
  if(!displayOperationUuid(request.sessionId)||!displayOperationUuid(request.resizeId)||request.requestId.empty()||request.requestId.size()>64||request.requestId.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos||(request.monitor!="0"&&request.monitor!="1")||(fps!=30&&fps!=60)||epoch<1||epoch>65535||std::floor(epoch)!=epoch||!ownedOutputSize(uint64_t(request.width),uint64_t(request.height)))throw std::runtime_error("Invalid stream resize request");
  request.fps=int(fps);request.epoch=uint16_t(epoch);return request;
}
inline std::string streamResizeFields(const StreamResize& request) { return "\"sessionId\":"+jsonQuote(request.sessionId)+",\"requestId\":"+jsonQuote(request.requestId)+",\"resizeId\":"+jsonQuote(request.resizeId)+",\"epoch\":"+std::to_string(request.epoch); }
inline int64_t streamMonotonicMillis() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
inline constexpr DWORD streamResizeTimeoutMs = 90000;
inline constexpr DWORD streamResizeProbeTimeoutMs = 10000;
// executeDisplayHelper may spend five seconds collecting a timed-out child;
// retain another five seconds for the bounded result IPC after the post-probe.
inline constexpr DWORD streamResizeApplyReserveMs = streamResizeProbeTimeoutMs + 5000 + 5000;
struct StreamResizeBudget {
  std::function<int64_t()> now = streamMonotonicMillis;
  std::function<bool()> cancelled = [] { return false; };
  int64_t deadline = streamMonotonicMillis() + streamResizeTimeoutMs;
  DWORD remaining(DWORD maximum, DWORD reserve = 0) const {
    const auto available = deadline - now() - reserve;
    if (cancelled() || available <= 0 || !maximum)
      throw std::runtime_error("resize_failed");
    return DWORD(std::min<int64_t>(maximum, available));
  }
};
inline int64_t workerResizeDeadline(const Json& command, int64_t now) {
  const auto value = command.get("workerDeadlineMs").number(-1);
  if (!std::isfinite(value) || value != std::floor(value) || value <= double(now) ||
      value > double(now + streamResizeTimeoutMs))
    throw std::runtime_error("Invalid worker resize deadline");
  return int64_t(value);
}
inline bool viewportProvisioningReady(const Json& probe,const DisplayState& display,bool desktop,bool console,bool privileged) {
  return desktop&&console&&privileged&&display.ready()&&!display.remoteSession&&!display.configurationPending&&!display.rebootRequired&&(display.monitors.size()==1||display.monitors.size()==2)&&probe.get("viewportProvisioningSupported").boolean()&&probe.get("minWidth").number()==320&&probe.get("minHeight").number()==180&&(probe.get("viewportSizeProtocol").string()=="runtime-verified-v1"||(probe.get("maxWidth").number()==3840&&probe.get("maxHeight").number()==2160))&&probe.get("widthAlignment").number()==2&&probe.get("heightAlignment").number()==2&&probe.get("maxRefreshRate").number()==60;
}
inline void streamField(Json& command,const char* key,double value){Json field;field.value=value;std::get<Json::Object>(command.value)[key]=std::move(field);}
inline void streamField(Json& command,const char* key,const std::string& value){Json field;field.value=value;std::get<Json::Object>(command.value)[key]=std::move(field);}
inline CaptureSelection attachFreshCapture(Json& command,const DisplayState& display,const std::vector<CaptureMonitor>& observed) {
  const auto selection=selectOwnedCapture(display,command.get("monitor").string(),observed);const auto area=selection.bounds;
  streamField(command,"captureLeft",area.left);streamField(command,"captureTop",area.top);streamField(command,"captureWidth",area.right-area.left);streamField(command,"captureHeight",area.bottom-area.top);streamField(command,"displayDeviceName",selection.monitors.size()==1?utf8(selection.monitors[0].name):"");
  Json::Array selected;for(const auto& monitor:selection.monitors){Json item;item.value=Json::Object{};streamField(item,"left",monitor.bounds.left);streamField(item,"top",monitor.bounds.top);streamField(item,"right",monitor.bounds.right);streamField(item,"bottom",monitor.bounds.bottom);streamField(item,"name",utf8(monitor.name));streamField(item,"identity",utf8(monitor.identity));selected.push_back(std::move(item));}Json captures;captures.value=std::move(selected);std::get<Json::Object>(command.value)["captureDisplays"]=std::move(captures);return selection;
}
// Non-owned capture outputs by identity, size and primary (position excluded).
inline std::multiset<std::tuple<std::wstring,LONG,LONG,bool>> nativeForeignCaptureSet(const std::vector<CaptureMonitor>& actual,const DisplayState& owned){
  std::multiset<std::tuple<std::wstring,LONG,LONG,bool>> values;
  for(const auto& monitor:actual)if(std::none_of(owned.monitors.begin(),owned.monitors.end(),[&](const auto& item){return item.name==monitor.name;}))values.emplace(monitor.identity,monitor.bounds.right-monitor.bounds.left,monitor.bounds.bottom-monitor.bounds.top,monitor.primary);
  return values;
}
struct WorkerViewportAccess {
  std::function<DisplayState(DWORD)> probe;std::function<std::vector<CaptureMonitor>()> observed;
  std::function<bool()> interactive,privileged;
  std::function<DisplayHelperOutput(const std::wstring&,int,int,DWORD)> apply;
};
// unchanged: worker-local proof that no prepare reached the service mutation
// boundary (the native coordinator never left its read-only phases). Advisory
// only; the service re-verifies its own lease before relying on it.
struct WorkerViewportResult { bool ok=false;std::string reason="resize_failed";Json command,helper;DisplayState display;uint32_t nativeStage=0,nativeError=0,nativeReason=0;bool consoleReturned=false,unchanged=false,restored=false; };
inline WorkerViewportResult applyWorkerViewport(const Json& current,const StreamResize& request,const std::string& logicalMonitor,WorkerViewportAccess access,const StreamResizeBudget& budget = {}) {
  WorkerViewportResult result;
  try {
    const auto limits=viewportEncoderLimits(current);
    result.display=access.probe(budget.remaining(streamResizeProbeTimeoutMs));
    budget.remaining(1);
    if(!access.interactive()||!access.privileged()){result.reason="resize_failed";return result;}
    if(current.get("resizeProtocol").string()!="epoch-v1"||current.get("sessionId").string()!=request.sessionId||request.monitor!=logicalMonitor||request.epoch<=current.get("epoch").number(0)||!result.display.viewportProvisioning){result.reason="unsupported_display_mode";return result;}
    const auto oldDevice=wide(current.get("displayDeviceName").string());const auto source=std::find_if(result.display.monitors.begin(),result.display.monitors.end(),[&](const auto& monitor){return monitor.id==logicalMonitor&&monitor.name==oldDevice;});
    if(source==result.display.monitors.end()){result.reason="monitor_not_available";return result;}
    const auto before=result.display;const auto observedBefore=access.observed();
    auto check=current;streamField(check,"monitor",source->id);const auto selected=attachFreshCapture(check,result.display,observedBefore);if(selected.monitors.size()!=1){result.reason="monitor_not_available";return result;}
    if(!access.interactive()||!access.privileged())return result;
    const auto timeout=budget.remaining(75000,streamResizeApplyReserveMs);
    result.reason="display_change_failed";
    const auto applied=access.apply(source->name,request.width,request.height,timeout);result.helper=applied.value;
    const bool helperRecovery=applied.value.get("recoveryRequired").boolean();
    const bool helperPending=applied.value.get("configurationPending").boolean();
    const bool helperReboot=applied.value.get("rebootRequired").boolean();
    result.display.recoveryRequired=result.display.recoveryRequired||helperRecovery;
    result.display.configurationPending=result.display.configurationPending||helperPending;
    result.display.rebootRequired=result.display.rebootRequired||helperReboot;
    // No helper result substitutes for a fresh post-change topology check.
    result.display.active=false;result.display.viewportProvisioning=false;
    result.display=access.probe(budget.remaining(streamResizeProbeTimeoutMs));
    result.display.recoveryRequired=result.display.recoveryRequired||helperRecovery;
    result.display.configurationPending=result.display.configurationPending||helperPending;
    result.display.rebootRequired=result.display.rebootRequired||helperReboot;
    budget.remaining(1);
    result.reason="display_change_failed";
    if(applied.timedOut||applied.exitCode||!applied.value.get("viewportApplied").boolean()||applied.value.get("configurationPending").boolean(true)||applied.value.get("width").number()!=request.width||applied.value.get("height").number()!=request.height||applied.value.get("refreshRate").number()!=60||!access.interactive()||!result.display.ready()||result.display.monitors.size()!=before.monitors.size())return result;
    const auto target=wide(applied.value.get("targetDisplayDeviceName").string());const auto monitorId=applied.value.get("targetMonitorId").string();
    const auto found=std::find_if(result.display.monitors.begin(),result.display.monitors.end(),[&](const auto& monitor){return monitor.id==request.monitor&&monitor.id==monitorId&&monitor.name==target&&monitor.width==DWORD(request.width)&&monitor.height==DWORD(request.height);});
    if(found==result.display.monitors.end()){result.reason="monitor_not_available";return result;}
    result.command=current;streamField(result.command,"type",std::string("session.start"));streamField(result.command,"requestId",request.requestId);streamField(result.command,"resizeId",request.resizeId);streamField(result.command,"epoch",request.epoch);streamField(result.command,"width",limits?limits->width:request.width);streamField(result.command,"height",limits?limits->height:request.height);streamField(result.command,"fps",request.fps);streamField(result.command,"monitor",monitorId);
    const auto observedAfter=access.observed();const auto foreign=[&](const auto& monitors,const DisplayState& owned){std::multiset<std::tuple<std::wstring,LONG,LONG,LONG,LONG,bool>> values;for(const auto& actual:monitors)if(std::none_of(owned.monitors.begin(),owned.monitors.end(),[&](const auto& monitor){return monitor.name==actual.name;}))values.emplace(actual.identity,actual.bounds.left,actual.bounds.top,actual.bounds.right,actual.bounds.bottom,actual.primary);return values;};
    // A native apply that verified the exact expected foreign CCD geometry
    // (including Windows' automatic adjacent re-layout) is compared by
    // identity/size/primary here; the legacy helper stays position-exact.
    if(applied.value.get("foreignGeometryVerified").boolean()?nativeForeignCaptureSet(observedBefore,before)!=nativeForeignCaptureSet(observedAfter,result.display):foreign(observedBefore,before)!=foreign(observedAfter,result.display))return result;
    for(const auto& old:before.monitors){const auto now=std::find_if(result.display.monitors.begin(),result.display.monitors.end(),[&](const auto& monitor){return monitor.id==old.id;});if(now==result.display.monitors.end()||old.primary!=now->primary||(old.id!=request.monitor&&(old.width!=now->width||old.height!=now->height)))return result;
      const auto actual=selectOwnedCapture(result.display,now->id,observedAfter);if(actual.monitors.size()!=1||std::none_of(observedAfter.begin(),observedAfter.end(),[&](const auto& monitor){return monitor.name==now->name&&monitor.primary==now->primary;}))return result;
    }
    const auto fresh=attachFreshCapture(result.command,result.display,observedAfter);if(fresh.monitors.size()!=1||fresh.monitors[0].name!=target||fresh.bounds.right-fresh.bounds.left!=request.width||fresh.bounds.bottom-fresh.bounds.top!=request.height)return result;
    budget.remaining(1);result.ok=true;result.reason.clear();return result;
  } catch(...) {
    result.display.active=false;result.display.viewportProvisioning=false;
    result.display.error=budget.now()>=budget.deadline?"viewport_probe_timeout":"viewport_probe_failed";
    // Recovery observations share the same budget; never start an unbounded
    // third probe after cancellation/expiry or erase known recovery metadata.
    try{auto fresh=access.probe(budget.remaining(streamResizeProbeTimeoutMs));budget.remaining(1);
      fresh.recoveryRequired=fresh.recoveryRequired||result.display.recoveryRequired||result.helper.get("recoveryRequired").boolean();
      fresh.configurationPending=fresh.configurationPending||result.display.configurationPending||result.helper.get("configurationPending").boolean();
      fresh.rebootRequired=fresh.rebootRequired||result.display.rebootRequired||result.helper.get("rebootRequired").boolean();
      result.display=std::move(fresh);}
    catch(...){result.display.active=false;result.display.viewportProvisioning=false;}
    return result;
  }
}
}
