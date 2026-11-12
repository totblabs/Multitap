#include "service_worker_drain.hpp"
#include "signin_preparation.hpp"
#include "pipe_packet.hpp"
#include <sddl.h>
#include <userenv.h>
#include <charconv>
#include <limits>

namespace host::signin::preparation {
namespace {
[[noreturn]] void invalid(){throw std::runtime_error("Invalid private preparation");}
void fields(const Json& value,std::initializer_list<const char*> names){
  const auto* object=std::get_if<Json::Object>(&value.value);
  if(!object||object->size()!=names.size())invalid();
  for(const char* name:names)if(!object->count(name))invalid();
}
uint32_t number(const Json& value,uint32_t low=0,uint32_t high=UINT32_MAX){
  const auto* v=std::get_if<double>(&value.value);
  if(!v||!std::isfinite(*v)||*v<low||*v>high||std::floor(*v)!=*v)invalid();
  return uint32_t(*v);
}
int32_t signedNumber(const Json& value){
  const auto* v=std::get_if<double>(&value.value);
  if(!v||!std::isfinite(*v)||*v<INT32_MIN||*v>INT32_MAX||std::floor(*v)!=*v)invalid();
  return int32_t(*v);
}
uint64_t integer(const Json& value){
  const auto* v=std::get_if<std::string>(&value.value);uint64_t out=0;
  if(!v||v->empty()||v->size()>20||(v->size()>1&&v->front()=='0'))invalid();
  const auto r=std::from_chars(v->data(),v->data()+v->size(),out);
  if(r.ec!=std::errc{}||r.ptr!=v->data()+v->size())invalid();return out;
}
bool boolean(const Json& value){const auto* v=std::get_if<bool>(&value.value);if(!v)invalid();return *v;}
bool sha(const std::string& value){return value.size()==64&&std::all_of(value.begin(),value.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
template<size_t N>std::string hex(const std::array<uint8_t,N>& value){constexpr char digits[]="0123456789abcdef";std::string out;out.reserve(N*2);for(auto v:value){out+=digits[v>>4];out+=digits[v&15];}return out;}
Nonce unhex(const Json& value){
  const auto text=value.string();if(!sha(text))invalid();Nonce out{};
  const auto digit=[](char c){return uint8_t(c<='9'?c-'0':c-'a'+10);};
  for(size_t i=0;i<out.size();++i)out[i]=uint8_t((digit(text[i*2])<<4)|digit(text[i*2+1]));
  if(!nonzero(out))invalid();return out;
}
StreamId stream(const Json& value){
  const auto text=value.string();if(text.size()!=32)invalid();
  const auto expanded=JsonParser::parse(jsonQuote(text+text));const auto bytes=unhex(expanded);StreamId out{};std::copy_n(bytes.begin(),16,out.begin());return out;
}
std::string processPacket(Process p){return "{\"pid\":"+std::to_string(p.pid)+",\"created\":"+jsonQuote(std::to_string(p.created))+'}';}
Process process(const Json& value){fields(value,{"pid","created"});Process out{number(value.get("pid"),1,UINT32_MAX-1),integer(value.get("created"))};if(!valid(out))invalid();return out;}
std::string bindingPacket(const PreparationBinding& b){
  return "{\"service\":"+processPacket(b.service)+",\"worker\":"+processPacket(b.worker)+",\"operator\":"+processPacket(b.operatorProcess)+
    ",\"operation\":"+jsonQuote(hex(b.operation))+",\"stream\":"+jsonQuote(hex(b.stream))+",\"console\":"+std::to_string(b.consoleSession)+'}';
}
PreparationBinding binding(const Json& value){
  fields(value,{"service","worker","operator","operation","stream","console"});
  return {process(value.get("service")),process(value.get("worker")),process(value.get("operator")),unhex(value.get("operation")),stream(value.get("stream")),number(value.get("console"),1,UINT32_MAX-1)};
}
std::string targetPacket(const native::OwnedTarget& t){
  return "{\"adapterLow\":"+std::to_string(t.adapterLow)+",\"adapterHigh\":"+std::to_string(t.adapterHigh)+",\"source\":"+std::to_string(t.sourceId)+
    ",\"target\":"+std::to_string(t.targetId)+",\"width\":"+std::to_string(t.width)+",\"height\":"+std::to_string(t.height)+
    ",\"left\":"+std::to_string(t.left)+",\"top\":"+std::to_string(t.top)+",\"mask\":1}";
}
native::OwnedTarget target(const Json& value){
  fields(value,{"adapterLow","adapterHigh","source","target","width","height","left","top","mask"});
  native::OwnedTarget out{number(value.get("adapterLow")),signedNumber(value.get("adapterHigh")),number(value.get("source")),number(value.get("target")),
    number(value.get("width"),16,8192),number(value.get("height"),16,8192),signedNumber(value.get("left")),signedNumber(value.get("top")),uint8_t(number(value.get("mask"),1,1))};
  if((!out.adapterLow&&!out.adapterHigh)||out.left||out.top||!ownedOutputSize(out.width,out.height))invalid();return out;
}
bool leaseValid(const LeaseProof& proof,const PreparationAdmission& a){
  return valid(proof.lease)&&proof.lease.instance==a.lease.instance&&proof.lease.generation>=a.lease.generation&&proof.targets.size()==1&&
    proof.targets[0].id=="0"&&(proof.targets[0].adapter.LowPart||proof.targets[0].adapter.HighPart)&&ownedOutputSize(proof.targets[0].width,proof.targets[0].height);
}
std::string leasePacket(const LeaseProof& p){
  const auto& t=p.targets.at(0);
  return "{\"instance\":"+jsonQuote(p.lease.instance)+",\"generation\":"+std::to_string(p.lease.generation)+",\"id\":\"0\",\"adapterLow\":"+
    std::to_string(t.adapter.LowPart)+",\"adapterHigh\":"+std::to_string(t.adapter.HighPart)+",\"target\":"+std::to_string(t.target)+
    ",\"width\":"+std::to_string(t.width)+",\"height\":"+std::to_string(t.height)+'}';
}
LeaseProof lease(const Json& value,const PreparationAdmission& a){
  fields(value,{"instance","generation","id","adapterLow","adapterHigh","target","width","height"});
  LeaseProof out{{value.get("instance").string(),number(value.get("generation"),1)},{{value.get("id").string(),
    {number(value.get("adapterLow")),signedNumber(value.get("adapterHigh"))},number(value.get("target")),number(value.get("width")),number(value.get("height"))}}};
  if(!leaseValid(out,a))invalid();return out;
}
std::string baselinePacket(const std::optional<BaselineProof>& p){
  if(!p)return "{\"present\":false,\"bytes\":\"\",\"sha256\":\"\"}";
  return "{\"present\":true,\"bytes\":"+jsonQuote(p->bytes)+",\"sha256\":"+jsonQuote(p->sha256)+'}';
}
void baselineValid(const BaselineProof& p){
  if(p.bytes.empty()||p.bytes.size()>65536||!sha(p.sha256)||nativeBaselineDigest(p.bytes)!=p.sha256||
      displayV2StateBytes(parseDisplayV2State(p.bytes))!=p.bytes)invalid();
}
std::optional<BaselineProof> baseline(const Json& value){
  fields(value,{"present","bytes","sha256"});
  if(!boolean(value.get("present"))){if(!value.get("bytes").string().empty()||!value.get("sha256").string().empty())invalid();return {};}
  BaselineProof p{value.get("bytes").string(),value.get("sha256").string()};baselineValid(p);return p;
}
std::string session(const PreparationAdmission& a){
  const auto text=hex(a.stream);return text.substr(0,8)+'-'+text.substr(8,4)+'-'+text.substr(12,4)+'-'+text.substr(16,4)+'-'+text.substr(20);
}
bool operationTime(const PreparationAdmission& a,Operation op,uint64_t now){
  if(validAdmission(a,now))return true;
  return op==Operation::cleanup&&validAdmission(a,a.issuedAt)&&now>=a.expiresAt&&now-a.expiresAt<=12000&&now-a.issuedAt<=90000;
}
bool consoleAllowed(uint32_t sessionId){
  const auto facts=native::observeConsole(sessionId);const auto now=GetTickCount64();
  return facts.valid&&facts.consoleProtocol&&facts.session==sessionId&&facts.observedAt<=now&&now-facts.observedAt<=1000&&
    (facts.windows==WindowsState::locked||facts.windows==WindowsState::noUser);
}
native::Status ready(){return {native::Outcome::ready,native::Step::none,0};}
[[noreturn]] void nativeFail(native::Step step,DWORD error=0){throw native::NativeFailure{{native::Outcome::refused,step,error}};}
native::OwnedTarget bindTarget(const NativeCcdData& data,std::span<const NativeDisplayTarget> owned){
  const auto active=displayV2ActiveCcd(data);
  if(owned.size()!=1||owned[0].id!="0"||active.paths.size()!=1)invalid();
  const auto& p=active.paths[0];const auto& t=owned[0];
  if(!displayV2Active(p)||!nativeSameTarget(p,t)||!nativeSameLuid(p.sourceInfo.adapterId,t.adapter)||
      p.targetInfo.rotation!=DISPLAYCONFIG_ROTATION_IDENTITY)invalid();
  const auto& source=nativeCcdSource(active,p);
  if(source.width!=t.width||source.height!=t.height||source.position.x||source.position.y)invalid();
  return {t.adapter.LowPart,t.adapter.HighPart,p.sourceInfo.id,t.target,t.width,t.height,0,0,1};
}
std::string requestPacket(const PreparationBinding& b,uint64_t serial,const char* op,Operation mode,const std::string& payload){
  return "{\"type\":\"signin.prepare.request-v1\",\"protocol\":"+jsonQuote(protocol)+",\"binding\":"+bindingPacket(b)+
    ",\"serial\":"+jsonQuote(std::to_string(serial))+",\"operation\":"+jsonQuote(op)+",\"mode\":"+jsonQuote(mode==Operation::prepare?"prepare":"cleanup")+",\"payload\":"+payload+'}';
}
struct Proxy {
  const PreparationAdmission& admission;const PreparationBinding& binding;Exchange exchange;
  std::function<uint64_t()> now;uint64_t serial=0;Operation mode=Operation::prepare;
  std::optional<BaselineProof> sealed;uint32_t highestGeneration=0;
  Json call(const char* op,const std::string& payload="{}"){
    if(!exchange||!now||serial==UINT64_MAX||!operationTime(admission,mode,now()))invalid();
    const auto sequence=++serial;const auto response=exchange(JsonParser::parse(requestPacket(binding,sequence,op,mode,payload)));
    fields(response,{"type","protocol","binding","serial","operation","ok","payload"});
    if(response.get("type").string()!="signin.prepare.result-v1"||response.get("protocol").string()!=protocol||
        preparation::binding(response.get("binding"))!=binding||integer(response.get("serial"))!=sequence||
        response.get("operation").string()!=op||!boolean(response.get("ok"))||!operationTime(admission,mode,now()))invalid();
    return response.get("payload");
  }
  LeaseProof acceptLease(const Json& value){
    auto proof=lease(value,admission);if(proof.lease.generation<highestGeneration)invalid();highestGeneration=proof.lease.generation;return proof;
  }
  LeaseProof current(){return acceptLease(call("lease.read"));}
  std::optional<std::string> load(){
    const auto value=baseline(call("original.read"));
    if(sealed&&(!value||value->sha256!=sealed->sha256||value->bytes!=sealed->bytes))invalid();
    if(!sealed&&!admission.expectedOriginalSha256.empty()&&(!value||value->sha256!=admission.expectedOriginalSha256))invalid();
    return value?std::optional<std::string>(value->bytes):std::nullopt;
  }
  bool save(const std::string& bytes){
    BaselineProof proposed{bytes,nativeBaselineDigest(bytes)};baselineValid(proposed);
    const auto saved=baseline(call("original.save","{\"bytes\":"+jsonQuote(bytes)+",\"expectedSha256\":"+jsonQuote(admission.expectedOriginalSha256)+'}'));
    if(!saved||saved->bytes!=bytes||saved->sha256!=proposed.sha256)invalid();sealed=*saved;return true;
  }
};
}

bool validAdmission(const PreparationAdmission& a,uint64_t now){
  const bool diagnostic=a.purpose==Purpose::privateDiagnostic&&a.service!=a.operatorProcess&&
    a.serviceInstance.empty()&&!a.grantRevision&&!a.eligibilityRevision&&!a.brokerDeadline;
  const bool broker=a.purpose==Purpose::brokerStream&&a.service==a.operatorProcess&&
    a.serviceInstance.size()==32&&std::any_of(a.serviceInstance.begin(),a.serviceInstance.end(),[](char c){return c!='0';})&&
    std::all_of(a.serviceInstance.begin(),a.serviceInstance.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');})&&
    a.grantRevision&&a.eligibilityRevision&&a.brokerDeadline>a.issuedAt&&a.brokerDeadline-a.issuedAt<=1000;
  // brokerDeadline is a one-shot launch deadline consumed by BrokerAuthority.
  // After exact child binding, the live service grant plus preparation expiry
  // govern every forward RPC; a replay cannot create a second authority/child.
  return (diagnostic||broker)&&valid(a.service)&&valid(a.operatorProcess)&&valid(a.lease)&&
    nonzero(a.stream)&&nonzero(a.operation)&&nonzero(a.installedImageSha256)&&a.consoleSession&&a.consoleSession!=UINT32_MAX&&ownedOutputSize(a.width,a.height)&&
    a.width<=8192&&a.height<=8192&&a.issuedAt&&a.issuedAt<=UINT64_MAX-90000&&a.issuedAt<=now&&a.expiresAt>a.issuedAt&&a.expiresAt-a.issuedAt<=90000&&now<a.expiresAt&&
    (a.expectedOriginalSha256.empty()||sha(a.expectedOriginalSha256));
}
bool sameBinding(const PreparationBinding& b,const PreparationAdmission& a,const Process& child){
  return b.service==a.service&&b.operatorProcess==a.operatorProcess&&b.operation==a.operation&&b.stream==a.stream&&b.consoleSession==a.consoleSession&&
    valid(child)&&b.worker==child&&child!=a.service&&child!=a.operatorProcess;
}
std::optional<Command> parseCommand(int argc,const wchar_t* const* argv){
  if(argc!=6||!argv||!argv[0]||!argv[1]||!argv[2]||!argv[3]||!argv[4]||!argv[5]||
      std::wstring(argv[1])!=L"--signin-preparer-v1"||std::wstring(argv[2])!=L"--pipe"||std::wstring(argv[4])!=L"--service-pid")return {};
  const std::wstring pid(argv[5]);uint64_t parsed=0;
  if(pid.empty()||pid.size()>10||pid.front()==L'0')return {};
  for(auto c:pid){if(c<L'0'||c>L'9')return {};parsed=parsed*10+uint64_t(c-L'0');if(parsed>=UINT32_MAX)return {};}
  const auto prefix=L"\\\\.\\pipe\\MultitapHost.SigninPrepare."+pid+L".";const std::wstring pipe(argv[3]);
  if(!pipe.starts_with(prefix)||pipe.size()!=prefix.size()+64)return {};
  const auto nonce=pipe.substr(prefix.size());
  if(std::all_of(nonce.begin(),nonce.end(),[](wchar_t c){return c==L'0';})||!std::all_of(nonce.begin(),nonce.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}))return {};
  return Command{pipe,uint32_t(parsed)};
}
std::string initPacket(const PreparationAdmission& a,const PreparationBinding& b,uint64_t serviceQuery){
  if(!validAdmission(a,a.issuedAt)||!sameBinding(b,a,b.worker)||!serviceQuery)invalid();
  const auto purpose=a.purpose==Purpose::privateDiagnostic?"private-diagnostic":"broker-stream";
  std::string result="{\"type\":\"signin.prepare.init-v1\",\"protocol\":"+jsonQuote(protocol)+",\"purpose\":"+jsonQuote(purpose)+",\"binding\":"+bindingPacket(b)+
    ",\"instance\":"+jsonQuote(a.lease.instance)+",\"leaseGeneration\":"+std::to_string(a.lease.generation)+",\"imageSha256\":"+jsonQuote(hex(a.installedImageSha256))+
    ",\"width\":"+std::to_string(a.width)+",\"height\":"+std::to_string(a.height)+",\"issuedAt\":"+jsonQuote(std::to_string(a.issuedAt))+
    ",\"expiresAt\":"+jsonQuote(std::to_string(a.expiresAt))+",\"expectedOriginalSha256\":"+jsonQuote(a.expectedOriginalSha256)+",\"serviceQuery\":"+jsonQuote(std::to_string(serviceQuery));
  if(a.purpose==Purpose::brokerStream)result+=",\"serviceInstance\":"+jsonQuote(a.serviceInstance)+
    ",\"grantRevision\":"+std::to_string(a.grantRevision)+",\"eligibilityRevision\":"+std::to_string(a.eligibilityRevision)+
    ",\"brokerDeadline\":"+jsonQuote(std::to_string(a.brokerDeadline));
  return result+'}';
}
ParsedInit parseInitPacket(const Json& init,uint64_t now){
  const auto purpose=init.get("purpose").string();
  if(purpose=="private-diagnostic")fields(init,{"type","protocol","purpose","binding","instance","leaseGeneration","imageSha256","width","height","issuedAt","expiresAt","expectedOriginalSha256","serviceQuery"});
  else if(purpose=="broker-stream")fields(init,{"type","protocol","purpose","binding","instance","leaseGeneration","imageSha256","width","height","issuedAt","expiresAt","expectedOriginalSha256","serviceQuery","serviceInstance","grantRevision","eligibilityRevision","brokerDeadline"});
  else invalid();
  if(init.get("type").string()!="signin.prepare.init-v1"||init.get("protocol").string()!=protocol)invalid();
  ParsedInit result;result.binding=binding(init.get("binding"));auto& a=result.admission;const auto& b=result.binding;
  a.purpose=purpose=="broker-stream"?Purpose::brokerStream:Purpose::privateDiagnostic;
  a.service=b.service;a.operatorProcess=b.operatorProcess;a.operation=b.operation;a.stream=b.stream;a.consoleSession=b.consoleSession;
  a.lease={init.get("instance").string(),number(init.get("leaseGeneration"),1)};a.installedImageSha256=unhex(init.get("imageSha256"));
  a.width=number(init.get("width"));a.height=number(init.get("height"));a.issuedAt=integer(init.get("issuedAt"));a.expiresAt=integer(init.get("expiresAt"));a.expectedOriginalSha256=init.get("expectedOriginalSha256").string();
  if(a.purpose==Purpose::brokerStream){a.serviceInstance=init.get("serviceInstance").string();a.grantRevision=number(init.get("grantRevision"),1);a.eligibilityRevision=number(init.get("eligibilityRevision"),1);a.brokerDeadline=integer(init.get("brokerDeadline"));}
  result.serviceQuery=integer(init.get("serviceQuery"));
  if(!result.serviceQuery||result.serviceQuery>=UINTPTR_MAX||!validAdmission(a,now)||!sameBinding(b,a,b.worker))invalid();
  return result;
}
std::string receiptPacket(const Receipt& r){
  const char* result=r.outcome==Outcome::prepared?"prepared":r.outcome==Outcome::restorePending?"restore-pending":r.outcome==Outcome::failed?"failed":"refused";
  return "{\"type\":\"signin.prepare.finished-v1\",\"protocol\":"+jsonQuote(protocol)+",\"binding\":"+bindingPacket(r.binding)+",\"outcome\":"+jsonQuote(result)+
    ",\"stage\":"+std::to_string(r.stage)+",\"error\":"+std::to_string(r.error)+",\"originalSha256\":"+jsonQuote(r.originalSha256)+",\"target\":"+
    (r.target.mask==1?targetPacket(r.target):"null")+",\"originalRetained\":"+(r.originalRetained?"true":"false")+
    ",\"fullOriginalLayoutObserved\":"+(r.fullOriginalLayoutObserved?"true":"false")+'}';
}
Receipt parseReceipt(const Json& value){
  fields(value,{"type","protocol","binding","outcome","stage","error","originalSha256","target","originalRetained","fullOriginalLayoutObserved"});
  if(value.get("type").string()!="signin.prepare.finished-v1"||value.get("protocol").string()!=protocol)invalid();
  Receipt r;r.binding=binding(value.get("binding"));const auto outcome=value.get("outcome").string();
  if(outcome=="prepared")r.outcome=Outcome::prepared;else if(outcome=="restore-pending")r.outcome=Outcome::restorePending;else if(outcome=="failed")r.outcome=Outcome::failed;else if(outcome!="refused")invalid();
  r.stage=number(value.get("stage"));r.error=number(value.get("error"));r.originalSha256=value.get("originalSha256").string();
  if(!r.originalSha256.empty()&&!sha(r.originalSha256))invalid();
  if(!std::holds_alternative<std::nullptr_t>(value.get("target").value))r.target=target(value.get("target"));
  r.originalRetained=boolean(value.get("originalRetained"));r.fullOriginalLayoutObserved=boolean(value.get("fullOriginalLayoutObserved"));
  if(r.outcome==Outcome::prepared&&(!sha(r.originalSha256)||r.target.mask!=1||r.stage||r.error))invalid();return r;
}
std::string answer(const Json& request,const PreparationAdmission& a,const PreparationBinding& b,Authority& authority,uint64_t& lastSerial,uint64_t now){
  fields(request,{"type","protocol","binding","serial","operation","mode","payload"});
  if(request.get("type").string()!="signin.prepare.request-v1"||request.get("protocol").string()!=protocol||binding(request.get("binding"))!=b||!sameBinding(b,a,b.worker))invalid();
  const auto serial=integer(request.get("serial"));if(!serial||lastSerial==UINT64_MAX||serial!=lastSerial+1)invalid();lastSerial=serial;
  const auto modeText=request.get("mode").string();if(modeText!="prepare"&&modeText!="cleanup")invalid();const auto mode=modeText=="prepare"?Operation::prepare:Operation::cleanup;
  const auto op=request.get("operation").string();const auto& payload=request.get("payload");
  bool ok=false;std::string result="{}";
  try{
    if(!operationTime(a,mode,now))invalid();const auto current=authority.current(a,b,mode);if(!current||!leaseValid(*current,a))invalid();
    if(op=="lease.read"){fields(payload,{});result=leasePacket(*current);ok=true;}
    else if(op=="original.read"){
      fields(payload,{});const auto original=authority.original(a,b,mode);if(!original)invalid();
      if(original->bytes.empty()&&original->sha256.empty())result=baselinePacket(std::nullopt);
      else{baselineValid(*original);result=baselinePacket(original);}ok=true;
    }
    else if(op=="original.save"){
      fields(payload,{"bytes","expectedSha256"});if(mode!=Operation::prepare||payload.get("expectedSha256").string()!=a.expectedOriginalSha256)invalid();
      BaselineProof proposed{payload.get("bytes").string(),{}};proposed.sha256=nativeBaselineDigest(proposed.bytes);baselineValid(proposed);
      const auto parsed=parseDisplayV2State(proposed.bytes);
      if(parsed.session!=session(a)||parsed.owned.size()!=1||parsed.owned[0].id!="0"||
          !nativeSameLuid(parsed.owned[0].adapter,current->targets[0].adapter)||parsed.owned[0].target!=current->targets[0].target||
          parsed.owned[0].width!=current->targets[0].width||parsed.owned[0].height!=current->targets[0].height)invalid();
      const auto saved=authority.saveOriginal(a,b,proposed.bytes,a.expectedOriginalSha256);
      if(!saved)invalid();baselineValid(*saved);if(saved->sha256!=proposed.sha256||saved->bytes!=proposed.bytes)invalid();
      result=baselinePacket(saved);ok=true;
    }else if(op=="size"){
      fields(payload,{"width","height"});const auto w=number(payload.get("width")),h=number(payload.get("height"));
      const auto original=authority.original(a,b,mode);if(!original)invalid();baselineValid(*original);
      auto saved=parseDisplayV2State(original->bytes);if(saved.owned.size()!=1||saved.owned[0].id!="0")invalid();
      uint32_t allowedW=a.width,allowedH=a.height;
      if(mode==Operation::cleanup){allowedW=saved.owned[0].width;allowedH=saved.owned[0].height;for(const auto& p:saved.ccd.paths)if(displayV2Active(p)&&nativeSameTarget(p,saved.owned[0])){const auto& s=nativeCcdSource(saved.ccd,p);allowedW=s.width;allowedH=s.height;break;}}
      if(w!=allowedW||h!=allowedH)invalid();const auto resized=authority.size(a,b,w,h,mode);
      if(!resized||!leaseValid(*resized,a)||resized->targets[0].width!=w||resized->targets[0].height!=h)invalid();result=leasePacket(*resized);ok=true;
    }else if(op=="retain"){
      if(mode!=Operation::prepare)invalid();const auto receipt=parseReceipt(payload);const auto original=authority.original(a,b,mode);
      if(receipt.binding!=b||receipt.outcome!=Outcome::prepared||!original||receipt.originalSha256!=original->sha256||receipt.target.mask!=1)invalid();
      baselineValid(*original);const auto& t=current->targets[0];
      if(t.adapter.LowPart!=receipt.target.adapterLow||t.adapter.HighPart!=receipt.target.adapterHigh||t.target!=receipt.target.targetId||t.width!=receipt.target.width||t.height!=receipt.target.height)invalid();
      ok=authority.retain(a,b,receipt);result="{}";
    }else invalid();
    const auto after=authority.current(a,b,mode);if(!after||!leaseValid(*after,a))ok=false;
  }catch(...){ok=false;result="{}";}
  return "{\"type\":\"signin.prepare.result-v1\",\"protocol\":"+jsonQuote(protocol)+",\"binding\":"+bindingPacket(b)+",\"serial\":"+jsonQuote(std::to_string(serial))+
    ",\"operation\":"+jsonQuote(op)+",\"ok\":"+(ok?"true":"false")+",\"payload\":"+result+'}';
}

Receipt prepareDisplay(const PreparationAdmission& a,const PreparationBinding& b,DisplayV2Access local,Exchange exchange,std::function<uint64_t()> now){
  Receipt receipt;receipt.binding=b;
  if(!now||!validAdmission(a,now())||!sameBinding(b,a,b.worker)||!exchange||!local.query||!local.configure||!local.eligible)return receipt;
  Proxy proxy{a,b,std::move(exchange),now};const auto localEligible=local.eligible;const auto localQuery=local.query;
  const auto localConfigure=local.configure;bool mayHaveChanged=false;
  std::unique_ptr<DisplaySessionV2> display;
  try{
    const auto initial=proxy.current();(void)initial;const auto existing=proxy.load();
    if(a.expectedOriginalSha256.empty()&&existing)invalid();
    local.owned=[&]{return proxy.current().targets;};local.restoreOwned=local.owned;
    local.resize=[&](const std::string& id,DWORD width,DWORD height)->std::optional<std::vector<NativeDisplayTarget>>{
      if(id!="0")return {};mayHaveChanged=true;
      return proxy.acceptLease(proxy.call("size","{\"width\":"+std::to_string(width)+",\"height\":"+std::to_string(height)+'}')).targets;
    };
    local.configure=[&](const NativeCcdData& plan,UINT32 flags){if(flags&SDC_APPLY)mayHaveChanged=true;return localConfigure(plan,flags);};
    local.sizes={};local.hidden=[](LUID){return true;};
    local.store.load=[&]{return proxy.load();};local.store.save=[&](const std::string& bytes){return proxy.save(bytes);};
    local.store.remove=[] {return false;};local.store.discard=[] {return false;};
    local.eligible=[&]{if(!localEligible()||!operationTime(a,proxy.mode,now()))return false;try{(void)proxy.current();return true;}catch(...){return false;}};
    display=std::make_unique<DisplaySessionV2>(std::move(local));
    const auto prepared=display->start(session(a),a.width,a.height,false,a.expiresAt,[&]{
      if(!localEligible())return false;try{receipt.target=bindTarget(localQuery(),proxy.current().targets);return true;}catch(...){return false;}
    });
    receipt.stage=prepared.stage;receipt.error=prepared.error;
    const auto original=proxy.load();if(original){receipt.originalSha256=nativeBaselineDigest(*original);receipt.originalRetained=true;}
    if(prepared.ok&&!receipt.originalSha256.empty()){
      receipt.outcome=Outcome::prepared;receipt.stage=0;receipt.error=0;
      if(display->retainForHandoff(receipt.originalSha256,[&]{(void)proxy.call("retain",receiptPacket(receipt));return true;}))return receipt;
    }
  }catch(...){if(!receipt.stage)receipt.stage=displayV2Stage::internal;}
  proxy.mode=Operation::cleanup;
  try{
    if(display&&mayHaveChanged){const auto restored=display->restore();receipt.stage=restored.stage;receipt.error=restored.error;}
    const auto original=proxy.load();receipt.originalRetained=original.has_value();
    if(original){
      receipt.originalSha256=nativeBaselineDigest(*original);const auto saved=parseDisplayV2State(*original);const auto owned=proxy.current().targets;const auto fresh=localQuery();
      std::vector<DisplayV2Expected> expected;const auto plan=planDisplayV2Restore(saved,fresh,owned,&expected);
      receipt.fullOriginalLayoutObserved=displayV2RestoreTargetsMatch(fresh,expected)&&displayV2SameForeign(displayV2Foreign(plan,owned,std::nullopt),displayV2Foreign(fresh,owned,std::nullopt));
    }
  }catch(...){receipt.fullOriginalLayoutObserved=false;}
  receipt.outcome=receipt.originalRetained?Outcome::restorePending:Outcome::failed;return receipt;
}

OwnedPreparationChild::~OwnedPreparationChild(){(void)stopAndWait(0);}
bool OwnedPreparationChild::connect(uint32_t milliseconds){
  if(!pipe_||!process()||milliseconds>5000)return false;
  const auto peer=[&]{try{ULONG pid=0;return GetNamedPipeClientProcessId(pipe_.get(),&pid)&&pid==binding_.worker.pid&&native::processIdentity(process())==binding_.worker&&WaitForSingleObject(process(),0)==WAIT_TIMEOUT;}catch(...){return false;}};
  native::Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event)return false;OVERLAPPED operation{};operation.hEvent=event.get();
  if(ConnectNamedPipe(pipe_.get(),&operation))return peer();const DWORD error=GetLastError();if(error==ERROR_PIPE_CONNECTED)return peer();if(error!=ERROR_IO_PENDING)return false;
  const HANDLE handles[]{event.get(),process()};const auto waited=WaitForMultipleObjects(2,handles,FALSE,milliseconds);DWORD ignored=0;
  if(waited!=WAIT_OBJECT_0){CancelIoEx(pipe_.get(),&operation);GetOverlappedResult(pipe_.get(),&operation,&ignored,TRUE);return false;}
  return GetOverlappedResult(pipe_.get(),&operation,&ignored,FALSE)&&peer();
}
bool OwnedPreparationChild::stopAndWait(uint32_t milliseconds){
  if(milliseconds>5000)return false;pipe_.reset();
  if(!process()||WaitForSingleObject(process(),milliseconds)==WAIT_OBJECT_0){job_.reset();return true;}
  job_.reset();
  if(process()&&!jobAssigned_&&WaitForSingleObject(process(),0)==WAIT_TIMEOUT)(void)TerminateProcess(process(),1);
  return WaitForSingleObject(process(),0)==WAIT_OBJECT_0;
}
native::Handle OwnedPreparationChild::takeUnexited(){
  if(!process()||WaitForSingleObject(process(),0)!=WAIT_TIMEOUT)return {};HANDLE copy=nullptr;
  if(!DuplicateHandle(GetCurrentProcess(),process(),GetCurrentProcess(),&copy,PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,0))nativeFail(native::Step::childIdentity,GetLastError());return native::Handle(copy);
}
native::Status launch(const PreparationAdmission& a,Authority& authority,OwnedPreparationChild& child){
  try{
    if(child.process()||child.pipe_||child.job_||!validAdmission(a,GetTickCount64()))nativeFail(native::Step::admission);
    auto physicalCreation=service_workers::reserveCurrent(service_workers::Role::userWorker,true);
    native::Handle physicalJob(physicalCreation.createTrackingJob());
    child.retainedProcess_=physicalCreation.observer();
    PreparationBinding before{a.service,{},a.operatorProcess,a.operation,a.stream,a.consoleSession};
    const auto proof=authority.current(a,before,Operation::prepare);if(!proof||!leaseValid(*proof,a)||!consoleAllowed(a.consoleSession))nativeFail(native::Step::lease);
    native::OwnSystemLaunchMaterial material;const auto admitted=native::ownSystemLaunchMaterial(a.service,a.consoleSession,a.installedImageSha256,material);if(!admitted)return admitted;
    PSECURITY_DESCRIPTOR descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)",SDDL_REVISION_1,&descriptor,nullptr))nativeFail(native::Step::pipe,GetLastError());
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    const auto nonce=hex(a.operation);child.pipeName_=L"\\\\.\\pipe\\MultitapHost.SigninPrepare."+std::to_wstring(a.service.pid)+L"."+std::wstring(nonce.begin(),nonce.end());
    child.pipe_=native::Handle(CreateNamedPipeW(child.pipeName_.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,262144,262144,0,&security));const auto pipeError=GetLastError();LocalFree(descriptor);if(!child.pipe_)nativeFail(native::Step::pipe,pipeError);
    child.job_=native::Handle(CreateJobObjectW(nullptr,nullptr));if(!child.job_)nativeFail(native::Step::isolateProcess,GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(child.job_.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)))nativeFail(native::Step::isolateProcess,GetLastError());
    struct Environment{void* value=nullptr;~Environment(){if(value)DestroyEnvironmentBlock(value);}}environment;
    if(!CreateEnvironmentBlock(&environment.value,material.token.get(),FALSE))nativeFail(native::Step::createProcess,GetLastError());
    const auto finalProof=authority.current(a,before,Operation::prepare);
    if(!validAdmission(a,GetTickCount64())||!finalProof||!leaseValid(*finalProof,a)||!consoleAllowed(a.consoleSession))nativeFail(native::Step::session);
    std::wstring command=L"\""+material.image+L"\" --signin-preparer-v1 --pipe "+child.pipeName_+L" --service-pid "+std::to_wstring(a.service.pid);
    service_workers::JobListStartup physicalStartup(physicalJob.get(),child.job_.get());auto& startup=physicalStartup.startup.StartupInfo;wchar_t desktop[]=L"winsta0\\Winlogon";startup.lpDesktop=desktop;PROCESS_INFORMATION created{};
    const auto slash=material.image.find_last_of(L"\\/");if(slash==std::wstring::npos)nativeFail(native::Step::serviceImage);
    physicalCreation.requireLaunchAllowed();
    if(!CreateProcessAsUserW(material.token.get(),material.image.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,environment.value,material.image.substr(0,slash).c_str(),&startup,&created))nativeFail(native::Step::createProcess,GetLastError());
    child.process_=native::Handle(created.hProcess);native::Handle thread(created.hThread);child.imageLock_=std::move(material.imageLock);child.jobAssigned_=true;
    HANDLE physicalOriginal=child.process_.release();
    try{physicalCreation.retain(physicalOriginal);}catch(...){child.process_.reset(physicalOriginal);const auto known=child.retainedProcess_.identity();if(known.known)child.binding_={a.service,{known.pid,known.birth},a.operatorProcess,a.operation,a.stream,a.consoleSession};throw;}
    child.process_.reset(physicalOriginal);
    child.binding_={a.service,native::processIdentity(child.process()),a.operatorProcess,a.operation,a.stream,a.consoleSession};
    std::wstring childImage(32768,L'\0');DWORD imageSize=DWORD(childImage.size());
    if(!QueryFullProcessImageNameW(child.process(),0,childImage.data(),&imageSize)||!imageSize||imageSize>=childImage.size())nativeFail(native::Step::childIdentity,GetLastError());
    childImage.resize(imageSize);
    if(!sameBinding(child.binding_,a,child.binding_.worker)||WaitForSingleObject(child.process(),0)!=WAIT_TIMEOUT||
        CompareStringOrdinal(childImage.c_str(),int(childImage.size()),material.image.c_str(),int(material.image.size()),TRUE)!=CSTR_EQUAL)nativeFail(native::Step::childIdentity);
    HANDLE delegated=nullptr;if(!DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),child.process(),&delegated,PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,0))nativeFail(native::Step::binding,GetLastError());
    child.delegatedServiceQuery_=uint64_t(reinterpret_cast<uintptr_t>(delegated));
    if(ResumeThread(thread.get())==DWORD(-1))nativeFail(native::Step::createProcess,GetLastError());return ready();
  }catch(const native::NativeFailure& failure){return failure.status;}
  catch(...){return {native::Outcome::failed,native::Step::createProcess,0};}
}
int workerCommand(int argc,const wchar_t* const* argv)noexcept{
  try{
    const auto command=parseCommand(argc,argv);if(!command||command->servicePid==GetCurrentProcessId())return ERROR_INVALID_PARAMETER;
    if(!WaitNamedPipeW(command->pipe.c_str(),5000))return ERROR_PIPE_NOT_CONNECTED;
    native::Handle pipe(CreateFileW(command->pipe.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));if(!pipe)return ERROR_PIPE_NOT_CONNECTED;
    ULONG server=0;if(!GetNamedPipeServerProcessId(pipe.get(),&server)||server!=command->servicePid)return ERROR_ACCESS_DENIED;
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))return ERROR_INVALID_STATE;
    std::atomic<bool> stopped=false;std::mutex writing;bool text=false;std::vector<uint8_t> bytes;
    const std::atomic<int64_t> initDeadline{streamMonotonicMillis()+5000};
    if(!readPipePacket(pipe.get(),text,bytes,stopped,std::chrono::milliseconds(5000),&initDeadline)||!text||bytes.size()>8192)return ERROR_INVALID_DATA;
    const auto init=JsonParser::parse(std::string(bytes.begin(),bytes.end()));
    const auto parsed=parseInitPacket(init,GetTickCount64());const auto& b=parsed.binding;const auto& a=parsed.admission;
    if(!validAdmission(a,GetTickCount64())||!sameBinding(b,a,native::processIdentity(GetCurrentProcess()))||b.service.pid!=command->servicePid)return ERROR_ACCESS_DENIED;
    const auto raw=parsed.serviceQuery;
    const HANDLE held=reinterpret_cast<HANDLE>(uintptr_t(raw));native::Handle imageLock;
    const auto peer=native::verifyInstalledParentPeer(b.service,held,pipe.get(),a.installedImageSha256,imageLock);if(!peer)return ERROR_ACCESS_DENIED;
    native::Handle service(held);
    const auto localEligible=[&]{
      try{
        if(native::processIdentity(service.get())!=b.service||WaitForSingleObject(service.get(),0)!=WAIT_TIMEOUT||!consoleAllowed(a.consoleSession))return false;
        ULONG actual=0;if(!GetNamedPipeServerProcessId(pipe.get(),&actual)||actual!=b.service.pid)return false;
        const auto facts=native::observeWorker();return facts.process==b.worker&&facts.session==a.consoleSession&&facts.localSystem&&facts.stationWinSta0&&facts.threadWinlogon&&facts.inputWinlogon;
      }catch(...){return false;}
    };
    if(!localEligible())return ERROR_ACCESS_DENIED;
    DisplayV2Access access;access.query=[]{return queryNativeCcd();};access.configure=windowsNativeCcdConfigure();access.eligible=localEligible;
    access.poll=[] {Sleep(displayV2StreamPollMs);};access.now=[] {return GetTickCount64();};
    const auto exchange=[&](const Json& request){
      if(!localEligible())invalid();const auto packet=jsonObject(request);if(packet.size()>262144)invalid();
      const auto mode=request.get("mode").string();const uint64_t until=mode=="cleanup"?std::min(a.expiresAt+12000,a.issuedAt+90000):a.expiresAt;
      const std::atomic<int64_t> deadline{int64_t(std::min<uint64_t>(until,GetTickCount64()+15000))};
      if(!pipePacket(pipe.get(),true,{packet.begin(),packet.end()},stopped,writing,&deadline))invalid();
      bool isText=false;std::vector<uint8_t> response;
      if(!readPipePacket(pipe.get(),isText,response,stopped,std::chrono::milliseconds(15000),&deadline)||!isText||response.size()>262144||!localEligible())invalid();
      return JsonParser::parse(std::string(response.begin(),response.end()));
    };
    const auto result=prepareDisplay(a,b,std::move(access),exchange,[]{return GetTickCount64();});
    const auto packet=receiptPacket(result);const std::atomic<int64_t> end{streamMonotonicMillis()+1000};
    (void)pipePacket(pipe.get(),true,{packet.begin(),packet.end()},stopped,writing,&end);
    return result.outcome==Outcome::prepared?0:ERROR_NOT_READY;
  }catch(...){return ERROR_INVALID_DATA;}
}
} // namespace host::signin::preparation
