#pragma once
// Display manager v2 state file over the private worker pipe.
//
// The saved display state (display-v2.state) lives in the Host's protected
// folder, SYSTEM and Administrators only. An interactive worker that opened
// that file itself would have to be an administrator's: with a standard
// Windows user signed in nothing could stream, and a state a stream had left
// behind would never be restored. Changing Windows'
// display configuration is not an administrator's right; holding that file is.
// So the service holds it, and a worker that is not an administrator asks for
// four fixed operations on that one file:
//
//   load     the saved state, or that none exists
//   save     a new state, replacing exactly the one this worker last saw
//   remove   the state this worker last saw (after it was restored)
//   discard  a state the service itself finds unreadable
//
// What a request can and cannot do (the worker runs as the signed-in user, so
// that user can make any request the worker can):
//   - No path, name or handle travels. The file is the service's fixed path.
//   - `save` carries a display state and nothing else: it must parse and
//     re-encode to the same bytes (parseDisplayV2State), fit the pipe, and name
//     the owned outputs the service holds now or the ones already saved. Its
//     content is a Windows display configuration, which that user may set at
//     the console anyway; the service never interprets it as anything else.
//   - `save` and `remove` name the SHA-256 of the file they replace. A request
//     made against an older file (a repeated or delayed one) is refused.
//   - `discard` works only on a file the service cannot read or parse.
//   - `load` never returns file bytes as they are: only a state that parsed,
//     re-encoded, with the label of the stream that wrote it replaced by a
//     constant (it may be another person's stream).
//   - Every operation needs the pipe's worker to be the one display operation
//     the service is running in the console session (`reader`); `save` and
//     `remove` also need that session unlocked (`writer`).
// An administrator's worker keeps opening the file itself, as before.
#include "display_v2_rpc.hpp"

namespace host {
// The largest canonical state (64 paths, 256 modes) is about 42 KiB; a pipe
// packet is at most 64 KiB on both sides.
inline constexpr size_t displayV2StatePipeLimit=49152;
inline constexpr const char* displayV2StateForeignLabel="restricted";
enum class DisplayV2StateOp {load,save,remove,discard};
struct DisplayV2StateRequest {
  std::string nonce;DisplayV2StateOp op=DisplayV2StateOp::load;
  std::optional<std::string> expected;   // save/remove: digest of the file being replaced; none = no file
  std::string state;                     // save: the new state
};
struct DisplayV2StateReply {
  bool ok=false;
  std::optional<std::string> sha256;     // load: the file's digest; save: the new file's
  std::optional<std::string> state;      // load: the state, when one exists
};
inline bool displayV2StateDigest(const std::string& value){return value.size()==64&&value.find_first_not_of("0123456789abcdef")==std::string::npos;}
inline const char* displayV2StateOpName(DisplayV2StateOp op){
  return op==DisplayV2StateOp::load?"load":op==DisplayV2StateOp::save?"save":op==DisplayV2StateOp::remove?"remove":"discard";
}
inline std::string displayV2StateRequestBytes(const DisplayV2StateRequest& request){
  std::string bytes="{\"type\":\"worker.v2.state\",\"nonce\":"+jsonQuote(request.nonce)+",\"op\":"+jsonQuote(displayV2StateOpName(request.op));
  if(request.op==DisplayV2StateOp::save||request.op==DisplayV2StateOp::remove)bytes+=",\"expected\":"+(request.expected?jsonQuote(*request.expected):std::string("null"));
  if(request.op==DisplayV2StateOp::save)bytes+=",\"state\":"+jsonQuote(request.state);
  return bytes+'}';
}
// Exact shapes only: load/discard 3 fields, remove 4, save 5.
inline std::optional<DisplayV2StateRequest> parseDisplayV2StateRequest(const Json& value){
  const auto* fields=std::get_if<Json::Object>(&value.value);
  if(!fields||value.get("type").string()!="worker.v2.state")return {};
  DisplayV2StateRequest request;request.nonce=value.get("nonce").string();
  const auto* op=std::get_if<std::string>(&value.get("op").value);
  if(!displayV2Nonce(request.nonce)||!op)return {};
  size_t expectedFields=3;
  if(*op=="load")request.op=DisplayV2StateOp::load;
  else if(*op=="discard")request.op=DisplayV2StateOp::discard;
  else if(*op=="remove"){request.op=DisplayV2StateOp::remove;expectedFields=4;}
  else if(*op=="save"){request.op=DisplayV2StateOp::save;expectedFields=5;}
  else return {};
  if(fields->size()!=expectedFields)return {};
  if(expectedFields>=4){
    const auto& expected=value.get("expected");
    if(const auto* text=std::get_if<std::string>(&expected.value)){if(!displayV2StateDigest(*text))return {};request.expected=*text;}
    else if(!std::holds_alternative<std::nullptr_t>(expected.value))return {};
  }
  if(expectedFields==5){
    const auto* state=std::get_if<std::string>(&value.get("state").value);
    if(!state||state->empty()||state->size()>displayV2StatePipeLimit)return {};
    request.state=*state;
  }
  return request;
}
inline std::string displayV2StateReplyBytes(const std::string& nonce,const DisplayV2StateReply& reply){
  return "{\"type\":\"worker.v2.state.result\",\"nonce\":"+jsonQuote(nonce)+",\"ok\":"+(reply.ok?"true":"false")+
    ",\"sha256\":"+(reply.ok&&reply.sha256?jsonQuote(*reply.sha256):std::string("null"))+",\"state\":"+(reply.ok&&reply.state?jsonQuote(*reply.state):std::string("null"))+'}';
}
inline std::optional<DisplayV2StateReply> parseDisplayV2StateReply(const Json& value,const std::string& nonce){
  const auto* fields=std::get_if<Json::Object>(&value.value);
  if(!fields||fields->size()!=5||value.get("type").string()!="worker.v2.state.result"||value.get("nonce").string()!=nonce||!std::holds_alternative<bool>(value.get("ok").value))return {};
  DisplayV2StateReply reply;reply.ok=value.get("ok").boolean();
  if(const auto* sha=std::get_if<std::string>(&value.get("sha256").value)){if(!displayV2StateDigest(*sha))return {};reply.sha256=*sha;}
  else if(!std::holds_alternative<std::nullptr_t>(value.get("sha256").value))return {};
  if(const auto* state=std::get_if<std::string>(&value.get("state").value)){if(state->empty()||state->size()>displayV2StatePipeLimit)return {};reply.state=*state;}
  else if(!std::holds_alternative<std::nullptr_t>(value.get("state").value))return {};
  if(!reply.ok&&(reply.sha256||reply.state))return {};
  return reply;
}
inline bool displayV2StateSameOwned(const std::vector<NativeDisplayTarget>& a,const std::vector<NativeDisplayTarget>& b){
  if(a.size()!=b.size())return false;
  return std::all_of(a.begin(),a.end(),[&](const auto& one){return std::count_if(b.begin(),b.end(),[&](const auto& two){
    return one.id==two.id&&nativeSameLuid(one.adapter,two.adapter)&&one.target==two.target&&one.width==two.width&&one.height==two.height;})==1;});
}

// ------------------------------------------------------------ service side
struct DisplayV2StateService {
  DisplayV2Store store;                                       // the protected file
  std::function<bool()> reader;                               // this pipe's worker is the running v2 operation of the console session
  std::function<bool()> writer;                               // and that session is unlocked
  std::function<std::vector<NativeDisplayTarget>()> owned;    // the owned outputs the service holds now
};
// A request that is not exactly one of the four shapes gets no answer, like a
// malformed size request; every well-formed one is answered, refused or not.
inline DisplayV2Dispatch dispatchDisplayV2State(const Json& value,const DisplayV2StateService& service){
  if(value.get("type").string()!="worker.v2.state")return {};
  DisplayV2Dispatch result;result.handled=true;
  const auto request=parseDisplayV2StateRequest(value);if(!request)return result;
  DisplayV2StateReply reply;
  const auto answer=[&]{result.response=displayV2StateReplyBytes(request->nonce,reply);return result;};
  try{
    if(!service.reader||!service.reader()||!service.store.load||!service.store.save||!service.store.remove)return answer();
    // The file as it is now. Unreadable covers a file that cannot be opened,
    // is not trusted, or does not parse and re-encode to itself.
    std::optional<std::string> bytes;std::optional<DisplayV2State> current;bool unreadable=false;
    try{bytes=service.store.load();if(bytes)current=parseDisplayV2State(*bytes);}catch(...){unreadable=true;current.reset();}
    const auto digest=bytes&&!unreadable?std::optional<std::string>(nativeBaselineDigest(*bytes)):std::nullopt;
    switch(request->op){
      case DisplayV2StateOp::load:{
        if(unreadable)return answer();
        if(current){auto shown=*current;shown.session=displayV2StateForeignLabel;const auto text=displayV2StateBytes(shown);if(text.size()>displayV2StatePipeLimit)return answer();reply.sha256=digest;reply.state=text;}
        reply.ok=true;return answer();
      }
      case DisplayV2StateOp::discard:{
        if(!unreadable)return answer();
        service.store.discard();reply.ok=true;return answer();
      }
      case DisplayV2StateOp::save:{
        if(unreadable||!service.writer||!service.writer()||request->expected!=digest||!service.owned)return answer();
        const auto next=parseDisplayV2State(request->state);
        if(!displayV2StateSameOwned(next.owned,service.owned())&&!(current&&displayV2StateSameOwned(next.owned,current->owned)))return answer();
        if(!service.store.save(request->state))return answer();
        reply.sha256=nativeBaselineDigest(request->state);reply.ok=true;return answer();
      }
      case DisplayV2StateOp::remove:{
        if(unreadable||!service.writer||!service.writer()||request->expected!=digest)return answer();
        reply.ok=!bytes||service.store.remove();return answer();
      }
    }
  }catch(...){reply=DisplayV2StateReply{};}
  return answer();
}

// ------------------------------------------------------------- worker side
// The same DisplayV2Store the file gives, asked of the service. `load` throws
// when the service refuses or does not answer, which DisplaySessionV2 treats
// like an unreadable file: it asks for a discard (refused unless the service
// agrees the file is unreadable) and changes nothing it cannot put back.
inline DisplayV2Store displayV2PipeStore(std::function<bool(const std::string&)> write,std::function<std::optional<Json>(DWORD)> read,std::function<bool(const Json&)> interleave,std::function<DWORD()> timeoutMs,std::function<std::string()> nonce,std::function<uint64_t()> now=[]{return GetTickCount64();}){
  const auto seen=std::make_shared<std::optional<std::string>>();   // digest of the file as this store last saw it
  const auto call=[write=std::move(write),read=std::move(read),interleave=std::move(interleave),timeoutMs=std::move(timeoutMs),nonce=std::move(nonce),now=std::move(now)](DisplayV2StateRequest request)->std::optional<DisplayV2StateReply>{
    try{
      request.nonce=nonce();
      const auto bytes=displayV2StateRequestBytes(request);
      if(!displayV2Nonce(request.nonce)||!write||!read||bytes.size()>65536||!write(bytes))return {};
      const auto deadline=now()+timeoutMs();
      for(;;){
        const auto current=now();if(current>=deadline)return {};
        const auto value=read(DWORD(std::min<uint64_t>(deadline-current,15000)));if(!value)return {};
        if(displayV2StaleReply(*value,request.nonce))continue;
        if(value->get("type").string()!="worker.v2.state.result"){if(interleave&&interleave(*value))continue;return {};}
        return parseDisplayV2StateReply(*value,request.nonce);
      }
    }catch(...){return {};}
  };
  DisplayV2Store store;
  store.load=[call,seen]()->std::optional<std::string>{
    const auto reply=call({{},DisplayV2StateOp::load});
    if(!reply||!reply->ok||reply->sha256.has_value()!=reply->state.has_value())throw std::runtime_error("display_v2_state_unavailable");
    *seen=reply->sha256;return reply->state;
  };
  store.save=[call,seen](const std::string& bytes){
    if(bytes.empty()||bytes.size()>displayV2StatePipeLimit)return false;
    const auto reply=call({{},DisplayV2StateOp::save,*seen,bytes});
    if(!reply||!reply->ok||!reply->sha256)return false;
    *seen=reply->sha256;return true;
  };
  store.remove=[call,seen]{
    const auto reply=call({{},DisplayV2StateOp::remove,*seen});
    if(!reply||!reply->ok)return false;
    seen->reset();return true;
  };
  store.discard=[call,seen]{const auto reply=call({{},DisplayV2StateOp::discard});if(reply&&reply->ok)seen->reset();};
  return store;
}
}
