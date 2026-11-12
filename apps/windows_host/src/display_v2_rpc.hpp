#pragma once
#include "display_v2.hpp"
#include "owned_output_size.hpp"
#include <array>
#include <deque>

// Display manager v2 glue that is independent of the service/worker classes:
// the host config switch, the private worker->service SDK size RPC and the
// one-time retirement of v1 stream-profile journals. Broker/client wire
// messages are unchanged; these packets never leave the private worker pipe.
namespace host {
// ------------------------------------------------------------ config switch
// `displayManager` in settings.json (host config), else native-display.json.
// "v1" keeps the previous transaction model; anything else (default) is v2.
inline bool displayManagerV2FromConfig(const Json& settings,const Json& native){
  for(const auto* config:{&settings,&native}){
    const auto* text=std::get_if<std::string>(&config->get("displayManager").value);if(!text)continue;
    if(*text=="v1")return false;if(*text=="v2")return true;
  }
  return true;
}
inline bool displayManagerV2Selected(){
  Json settings,native;
  try{settings=readPublicConfig();}catch(...){ }
  try{native=nativeDisplayConfig();}catch(...){ }
  return displayManagerV2FromConfig(settings,native);
}
inline std::filesystem::path displayV2StatePath(){return machineDirectory()/L"private"/L"display-v2.state";}
inline bool displayV2StatePresent(const std::filesystem::path& path=displayV2StatePath()){return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;}

// ------------------------------------------- packets during a private RPC
// A worker waiting for a private RPC reply (SDK size/sizes/count, v1 viewport
// phases) reads whatever the service writes to its pipe next. Session traffic
// keeps arriving meanwhile: Session0 forwards the client's lossless opt-in
// (`stream.lossless`) for every `lt1-` ping, about once a second for the
// whole stream, and a track tunnel forwards the pings themselves and the
// client's ready acknowledgements. Abandoning the RPC for one of these would
// fail a resize whose SDK call overlapped a ping as display_change_failed and
// end the stream (rapid window/fullscreen changes run into it). They are kept,
// in order, and handled by the worker's ordinary loop after the change, like
// before it.
struct WorkerPipeBacklog {
  static constexpr size_t limit=256;
  std::deque<Json> items;
  uint32_t deferred=0;   // diagnostics: packets kept during RPC waits
  // Only session traffic the ordinary loop already handles. Anything else is
  // still a protocol violation that ends the RPC (and the worker).
  // worker.secure.ready / worker.secure.failed: the service's answer to this worker's secure-desktop helper
  // request (secure_desktop_service.hpp) may cross a resize; it must never end the RPC, and the handles a ready
  // carries are taken by the ordinary loop afterwards.
  static bool deferrable(const std::string& type){
    for(const auto* allowed:{"stream.lossless","ping","stream.tracks.ready","stream.bitrate","stream.resize","stream.tracks.resize","display.scale","worker.secure.ready","worker.secure.failed"})if(type==allowed)return true;
    return false;
  }
  bool defer(const Json& value){
    const auto type=value.get("type").string();
    if(!deferrable(type))return false;
    ++deferred;
    // The opt-in is a state ("on":false pauses tiles while 게임 마우스 holds the
    // mouse): one pending copy, the latest, is enough.
    if(type=="stream.lossless")if(const auto kept=std::find_if(items.begin(),items.end(),[](const Json& item){return item.get("type").string()=="stream.lossless";});kept!=items.end()){*kept=value;return true;}
    if(items.size()>=limit)return false;
    items.push_back(value);return true;
  }
  std::optional<Json> next(){if(items.empty())return {};auto value=std::move(items.front());items.pop_front();return value;}
};
// A reply to an earlier RPC whose own wait ended (timeout, or the previous
// worker of a pipe): never the answer to this request, never a failure.
inline bool displayV2StaleReply(const Json& value,const std::string& nonce){
  const auto type=value.get("type").string();
  // worker.v2.state.result: the state file RPC of a worker that is not an administrator (display_v2_state_rpc.hpp).
  return (type=="worker.v2.display.size.result"||type=="worker.v2.display.sizes.result"||type=="worker.v2.display.count.result"||type=="worker.v2.state.result")&&value.get("nonce").string()!=nonce;
}

// ----------------------------------------------- worker -> service size RPC
struct DisplayV2SizeRequest {std::string nonce,monitor;DWORD width=0,height=0;};
inline bool displayV2Nonce(const std::string& value){return value.size()==32&&value.find_first_not_of("0123456789abcdef")==std::string::npos;}
inline std::string displayV2SizeRequestBytes(const DisplayV2SizeRequest& request){
  return "{\"type\":\"worker.v2.display.size\",\"nonce\":"+jsonQuote(request.nonce)+",\"monitor\":"+jsonQuote(request.monitor)+",\"width\":"+std::to_string(request.width)+",\"height\":"+std::to_string(request.height)+'}';
}
inline std::optional<DisplayV2SizeRequest> parseDisplayV2SizeRequest(const Json& value){
  const auto* fields=std::get_if<Json::Object>(&value.value);
  if(!fields||fields->size()!=5||value.get("type").string()!="worker.v2.display.size")return {};
  DisplayV2SizeRequest request{value.get("nonce").string(),value.get("monitor").string()};
  const auto width=value.get("width").number(-1),height=value.get("height").number(-1);
  if(!displayV2Nonce(request.nonce)||(request.monitor!="0"&&request.monitor!="1")||!nativeDisplayInteger(width,320,ownedOutputMaxSide)||!nativeDisplayInteger(height,180,ownedOutputMaxSide)||!ownedOutputSize(uint64_t(width),uint64_t(height)))return {};
  request.width=DWORD(width);request.height=DWORD(height);return request;
}
inline std::string displayV2SizeReplyBytes(const std::string& nonce,const std::optional<std::string>& snapshot){
  return "{\"type\":\"worker.v2.display.size.result\",\"nonce\":"+jsonQuote(nonce)+",\"ok\":"+(snapshot?"true":"false")+",\"snapshot\":"+(snapshot?*snapshot:std::string("null"))+'}';
}
// ----------------------------------------------- worker -> service sizes RPC
// Several owned outputs' sizes in one SDK reconfiguration (a dual-output
// stream's start, resize and restore). Positions never travel; the worker
// places the outputs in Windows itself.
struct DisplayV2SizesRequest {std::string nonce;std::vector<DisplayV2Output> outputs;};
inline std::string displayV2SizesRequestBytes(const DisplayV2SizesRequest& request){
  std::string outputs="[";
  for(const auto& o:request.outputs){if(outputs.size()>1)outputs+=',';outputs+="{\"id\":"+jsonQuote(o.id)+",\"width\":"+std::to_string(o.width)+",\"height\":"+std::to_string(o.height)+'}';}
  return "{\"type\":\"worker.v2.display.sizes\",\"nonce\":"+jsonQuote(request.nonce)+",\"outputs\":"+outputs+"]}";
}
inline std::optional<DisplayV2SizesRequest> parseDisplayV2SizesRequest(const Json& value){
  const auto* fields=std::get_if<Json::Object>(&value.value);
  if(!fields||fields->size()!=3||value.get("type").string()!="worker.v2.display.sizes")return {};
  DisplayV2SizesRequest request{value.get("nonce").string()};const auto* rows=std::get_if<Json::Array>(&value.get("outputs").value);
  if(!displayV2Nonce(request.nonce)||!rows||rows->empty()||rows->size()>2)return {};
  for(const auto& row:*rows){
    const auto* item=std::get_if<Json::Object>(&row.value);if(!item||item->size()!=3)return {};
    DisplayV2Output output{row.get("id").string()};const auto width=row.get("width").number(-1),height=row.get("height").number(-1);
    if((output.id!="0"&&output.id!="1")||!nativeDisplayInteger(width,320,ownedOutputMaxSide)||!nativeDisplayInteger(height,180,ownedOutputMaxSide)||!ownedOutputSize(uint64_t(width),uint64_t(height)))return {};
    if(std::any_of(request.outputs.begin(),request.outputs.end(),[&](const auto& o){return o.id==output.id;}))return {};
    output.width=DWORD(width);output.height=DWORD(height);request.outputs.push_back(std::move(output));
  }
  return request;
}
inline std::string displayV2SizesReplyBytes(const std::string& nonce,const std::optional<std::string>& snapshot){
  return "{\"type\":\"worker.v2.display.sizes.result\",\"nonce\":"+jsonQuote(nonce)+",\"ok\":"+(snapshot?"true":"false")+",\"snapshot\":"+(snapshot?*snapshot:std::string("null"))+'}';
}
// ----------------------------------------------- worker -> service count RPC
// Owned output count (1 or 2), used only by the display-operation worker of a
// client "virtual monitor count" request. Streams and recovery never get it.
struct DisplayV2CountRequest {std::string nonce;int count=0;};
inline std::string displayV2CountRequestBytes(const DisplayV2CountRequest& request){
  return "{\"type\":\"worker.v2.display.count\",\"nonce\":"+jsonQuote(request.nonce)+",\"count\":"+std::to_string(request.count)+'}';
}
inline std::optional<DisplayV2CountRequest> parseDisplayV2CountRequest(const Json& value){
  const auto* fields=std::get_if<Json::Object>(&value.value);
  if(!fields||fields->size()!=3||value.get("type").string()!="worker.v2.display.count")return {};
  DisplayV2CountRequest request{value.get("nonce").string()};const auto count=value.get("count").number(-1);
  if(!displayV2Nonce(request.nonce)||!nativeDisplayInteger(count,1,2))return {};
  request.count=int(count);return request;
}
inline std::string displayV2CountReplyBytes(const std::string& nonce,const std::optional<std::string>& snapshot){
  return "{\"type\":\"worker.v2.display.count.result\",\"nonce\":"+jsonQuote(nonce)+",\"ok\":"+(snapshot?"true":"false")+",\"snapshot\":"+(snapshot?*snapshot:std::string("null"))+'}';
}
// The display operation's own requested count may be applied only while the
// operation is current (applied, not cancelled, before its deadline, on the
// unlocked console). Any other count is its previous count being put back
// after a failure; that must stay possible after a cancellation or a lock.
inline bool displayV2CountAuthorized(int requested,int operationCount,bool applied,bool cancelled,bool current,bool console,bool unlocked){
  if(!applied||(requested!=1&&requested!=2)||(operationCount!=1&&operationCount!=2))return false;
  if(requested!=operationCount)return true;
  return !cancelled&&current&&console&&unlocked;
}
// Private worker -> service count diagnostics: fixed values only. Stage and
// error travel in the existing nativeStage/nativeError fields of
// worker.display.result (v2 stage; displayV2ForeignCode at stage 8); the
// "v2" object (displayV2CountDiagnosticFields) adds the rest.
struct DisplayV2CountDiagnostic {
  bool ok=false;uint32_t stage=0,error=0,foreignCount=0;
  bool consoleIdentified=false,displaysReturned=false,restored=false,stateKept=false;
};
inline DisplayV2CountDiagnostic displayV2CountDiagnostic(const DisplayV2Result& result){
  DisplayV2CountDiagnostic value;value.ok=result.ok;
  value.stage=result.ok?0:std::min<uint32_t>(result.stage?result.stage:displayV2Stage::internal,displayV2Stage::internal);
  value.error=result.ok?0:value.stage==displayV2Stage::foreign?result.foreignDifference:uint32_t(result.error);
  value.foreignCount=std::min<uint32_t>(result.foreignBefore,64);value.consoleIdentified=result.consoleIdentified;value.displaysReturned=result.displaysReturned;
  value.restored=result.restore&&(result.restore->outcome==DisplayV2Outcome::restored||result.restore->outcome==DisplayV2Outcome::consoleOnly);
  value.stateKept=result.stateKept;return value;
}
inline std::string displayV2CountDiagnosticFields(const DisplayV2CountDiagnostic& value){
  const auto flag=[](bool b){return b?"true":"false";};
  return ",\"v2\":{\"foreignCount\":"+std::to_string(value.foreignCount)+",\"consoleIdentified\":"+flag(value.consoleIdentified)+",\"displaysReturned\":"+flag(value.displaysReturned)+",\"restored\":"+flag(value.restored)+",\"stateKept\":"+flag(value.stateKept)+'}';
}
inline std::optional<DisplayV2CountDiagnostic> parseDisplayV2CountDiagnostic(const Json& response){
  const auto& v2=response.get("v2");const auto* fields=std::get_if<Json::Object>(&v2.value);
  if(!fields||fields->size()!=5||!std::holds_alternative<bool>(response.get("ok").value))return {};
  DisplayV2CountDiagnostic value;value.ok=response.get("ok").boolean();
  const auto stage=response.get("nativeStage").number(-1),error=response.get("nativeError").number(-1),count=v2.get("foreignCount").number(-1);
  if(!nativeDisplayInteger(stage,0,displayV2Stage::internal)||!nativeDisplayInteger(error,0,UINT32_MAX)||!nativeDisplayInteger(count,0,64)||(value.ok&&(stage||error)))return {};
  for(const auto* name:{"consoleIdentified","displaysReturned","restored","stateKept"})if(!std::holds_alternative<bool>(v2.get(name).value))return {};
  value.stage=uint32_t(stage);value.error=uint32_t(error);value.foreignCount=uint32_t(count);
  value.consoleIdentified=v2.get("consoleIdentified").boolean();value.displaysReturned=v2.get("displaysReturned").boolean();value.restored=v2.get("restored").boolean();value.stateKept=v2.get("stateKept").boolean();
  return value;
}
struct DisplayV2Dispatch {bool handled=false;std::string response;};
// Service side. `authorized` = a v2 stream, v2 recovery or v2 count worker owns
// this pipe. Count requests additionally need `count` (only the display
// operation passes it); without it they are answered with ok:false. Sizes
// requests likewise need `sizes` (stream and recovery pipes).
using DisplayV2SizesCall=std::function<std::optional<std::string>(std::span<const DisplayV2Output>)>;
inline DisplayV2Dispatch dispatchDisplayV2(const Json& value,bool authorized,const std::function<std::optional<std::string>(const std::string&,DWORD,DWORD)>& resize,const std::function<std::optional<std::string>(int)>& count={},const DisplayV2SizesCall& sizes={}){
  const auto type=value.get("type").string();
  if(type=="worker.v2.display.sizes"){
    DisplayV2Dispatch result;result.handled=true;
    const auto request=parseDisplayV2SizesRequest(value);if(!request)return result;
    std::optional<std::string> snapshot;
    if(authorized&&sizes)try{snapshot=sizes(request->outputs);}catch(...){snapshot.reset();}
    result.response=displayV2SizesReplyBytes(request->nonce,snapshot);return result;
  }
  if(type=="worker.v2.display.count"){
    DisplayV2Dispatch result;result.handled=true;
    const auto request=parseDisplayV2CountRequest(value);if(!request)return result;
    std::optional<std::string> snapshot;
    if(authorized&&count)try{snapshot=count(request->count);}catch(...){snapshot.reset();}
    result.response=displayV2CountReplyBytes(request->nonce,snapshot);return result;
  }
  if(type!="worker.v2.display.size")return {};
  DisplayV2Dispatch result;result.handled=true;
  const auto request=parseDisplayV2SizeRequest(value);if(!request)return result;
  std::optional<std::string> snapshot;
  if(authorized&&resize)try{snapshot=resize(request->monitor,request->width,request->height);}catch(...){snapshot.reset();}
  result.response=displayV2SizeReplyBytes(request->nonce,snapshot);return result;
}
// Worker side. Returns the owned targets after the change, proving the
// requested output has the requested size in the service's own lease.
inline std::optional<std::vector<NativeDisplayTarget>> displayV2SizeRpc(const DisplayV2SizeRequest& request,const std::function<bool(const std::string&)>& write,const std::function<std::optional<Json>(DWORD)>& read,const std::function<bool(const Json&)>& interleave,DWORD timeoutMs,const std::function<uint64_t()>& now=[]{return GetTickCount64();}){
  try{
    if(!displayV2Nonce(request.nonce)||!write||!read||!write(displayV2SizeRequestBytes(request)))return {};
    const auto deadline=now()+timeoutMs;
    for(;;){
      const auto current=now();if(current>=deadline)return {};
      const auto value=read(DWORD(std::min<uint64_t>(deadline-current,15000)));if(!value)return {};
      if(displayV2StaleReply(*value,request.nonce))continue;
      if(value->get("type").string()!="worker.v2.display.size.result"){if(interleave&&interleave(*value))continue;return {};}
      if(value->get("nonce").string()!=request.nonce||!std::holds_alternative<bool>(value->get("ok").value)||!value->get("ok").boolean())return {};
      auto targets=nativeDisplayTargets(value->get("snapshot"));
      const auto changed=std::find_if(targets.begin(),targets.end(),[&](const auto& target){return target.id==request.monitor;});
      if(changed==targets.end()||changed->width!=request.width||changed->height!=request.height)return {};
      return targets;
    }
  }catch(...){return {};}
}
// Worker side. Returns the owned targets after the change, proving every
// requested output has its requested size in the service's own lease.
inline std::optional<std::vector<NativeDisplayTarget>> displayV2SizesRpc(const DisplayV2SizesRequest& request,const std::function<bool(const std::string&)>& write,const std::function<std::optional<Json>(DWORD)>& read,const std::function<bool(const Json&)>& interleave,DWORD timeoutMs,const std::function<uint64_t()>& now=[]{return GetTickCount64();}){
  try{
    if(!displayV2Nonce(request.nonce)||!write||!read||!parseDisplayV2SizesRequest(JsonParser::parse(displayV2SizesRequestBytes(request)))||!write(displayV2SizesRequestBytes(request)))return {};
    const auto deadline=now()+timeoutMs;
    for(;;){
      const auto current=now();if(current>=deadline)return {};
      const auto value=read(DWORD(std::min<uint64_t>(deadline-current,15000)));if(!value)return {};
      if(displayV2StaleReply(*value,request.nonce))continue;
      if(value->get("type").string()!="worker.v2.display.sizes.result"){if(interleave&&interleave(*value))continue;return {};}
      if(value->get("nonce").string()!=request.nonce||!std::holds_alternative<bool>(value->get("ok").value)||!value->get("ok").boolean())return {};
      auto targets=nativeDisplayTargets(value->get("snapshot"));
      for(const auto& output:request.outputs){
        const auto changed=std::find_if(targets.begin(),targets.end(),[&](const auto& target){return target.id==output.id;});
        if(changed==targets.end()||changed->width!=output.width||changed->height!=output.height)return {};
      }
      return targets;
    }
  }catch(...){return {};}
}
// Worker side. Returns the owned targets after the change: exactly `count`
// outputs with public IDs "0" (and "1"), from the service's own lease.
inline std::optional<std::vector<NativeDisplayTarget>> displayV2CountRpc(const DisplayV2CountRequest& request,const std::function<bool(const std::string&)>& write,const std::function<std::optional<Json>(DWORD)>& read,const std::function<bool(const Json&)>& interleave,DWORD timeoutMs,const std::function<uint64_t()>& now=[]{return GetTickCount64();}){
  try{
    if(!displayV2Nonce(request.nonce)||(request.count!=1&&request.count!=2)||!write||!read||!write(displayV2CountRequestBytes(request)))return {};
    const auto deadline=now()+timeoutMs;
    for(;;){
      const auto current=now();if(current>=deadline)return {};
      const auto value=read(DWORD(std::min<uint64_t>(deadline-current,15000)));if(!value)return {};
      if(displayV2StaleReply(*value,request.nonce))continue;
      if(value->get("type").string()!="worker.v2.display.count.result"){if(interleave&&interleave(*value))continue;return {};}
      if(value->get("nonce").string()!=request.nonce||!std::holds_alternative<bool>(value->get("ok").value)||!value->get("ok").boolean())return {};
      auto targets=nativeDisplayTargets(value->get("snapshot"));
      if(targets.size()!=size_t(request.count))return {};
      for(int i=0;i<request.count;++i)if(std::count_if(targets.begin(),targets.end(),[&](const auto& t){return t.id==std::to_string(i);})!=1)return {};
      return targets;
    }
  }catch(...){return {};}
}

// ------------------------------------------------ v1 journal retirement
// v2 never reads these. They are moved (bytes preserved) into an archive so a
// later switch back to v1 does not replay a stale transaction either. When the
// v1 profile still had a console suppressed, its pre-stream topology becomes
// the v2 state, so the ordinary v2 recovery brings that console back.
inline constexpr std::array<const wchar_t*,6> displayV1JournalFiles{L"native-stream-profile.json",L"native-stream-launch.json",L"native-stream-retirement.json",L"native-stream-expired-console.json",L"native-stream-viewport-commit.json",L"native-stream-reclaim.json"};
struct DisplayV1Migration {bool found=false,archived=false,stateCreated=false;};
inline DisplayV1Migration migrateDisplayV1Leftovers(const std::filesystem::path& directory,const DisplayV2Store& store,const std::function<std::optional<DisplayV2State>(const std::string&)>& extract,const std::function<bool(const std::filesystem::path&)>& trusted,const std::wstring& stamp){
  DisplayV1Migration result;
  std::vector<std::filesystem::path> present;
  for(const auto* name:displayV1JournalFiles){const auto path=directory/name;const auto attributes=GetFileAttributesW(path.c_str());if(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))present.push_back(path);}
  if(present.empty())return result;
  result.found=true;
  try{
    const auto profile=directory/displayV1JournalFiles[0];
    if(extract&&GetFileAttributesW(profile.c_str())!=INVALID_FILE_ATTRIBUTES&&!store.load())
      if(const auto bytes=readNativeJournalFile(profile,trusted,65536))
        if(const auto state=extract(*bytes))result.stateCreated=store.save(displayV2StateBytes(*state));
  }catch(...){result.stateCreated=false;}
  try{
    if(stamp.empty()||stamp.size()>64||stamp.find_first_not_of(L"0123456789abcdefABCDEF-")!=std::wstring::npos)return result;
    const auto root=directory/L"display-v1-archive",target=root/stamp;
    if(!CreateDirectoryW(root.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return result;
    if(!trusted(root)||!CreateDirectoryW(target.c_str(),nullptr)||!trusted(target))return result;
    bool moved=true;
    for(const auto& path:present)moved=MoveFileExW(path.c_str(),(target/path.filename()).c_str(),MOVEFILE_WRITE_THROUGH)&&moved;
    result.archived=moved;
  }catch(...){ }
  return result;
}
}
