#include "service_worker_drain.hpp"
#include "signin_restoration.hpp"
#include "pipe_packet.hpp"
#include <sddl.h>
#include <userenv.h>
#include <charconv>

namespace host::signin::restoration {
namespace {
[[noreturn]]void invalid(){throw std::runtime_error("Invalid private restoration");}
void fields(const Json& v,std::initializer_list<const char*> names){const auto* p=std::get_if<Json::Object>(&v.value);if(!p||p->size()!=names.size())invalid();for(const char* n:names)if(!p->count(n))invalid();}
uint32_t number(const Json& v,uint32_t low=0,uint32_t high=UINT32_MAX){const auto* p=std::get_if<double>(&v.value);if(!p||!std::isfinite(*p)||*p<low||*p>high||std::floor(*p)!=*p)invalid();return uint32_t(*p);}
int32_t signedNumber(const Json& v){const auto* p=std::get_if<double>(&v.value);if(!p||!std::isfinite(*p)||*p<INT32_MIN||*p>INT32_MAX||std::floor(*p)!=*p)invalid();return int32_t(*p);}
uint64_t integer(const Json& v){const auto* p=std::get_if<std::string>(&v.value);uint64_t out=0;if(!p||p->empty()||p->size()>20||(p->size()>1&&p->front()=='0'))invalid();const auto r=std::from_chars(p->data(),p->data()+p->size(),out);if(r.ec!=std::errc{}||r.ptr!=p->data()+p->size())invalid();return out;}
bool boolean(const Json& v){const auto* p=std::get_if<bool>(&v.value);if(!p)invalid();return *p;}
bool sha(const std::string& v){return v.size()==64&&std::all_of(v.begin(),v.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
template<size_t N>std::string hex(const std::array<uint8_t,N>& v){constexpr char d[]="0123456789abcdef";std::string out;out.reserve(N*2);for(auto b:v){out+=d[b>>4];out+=d[b&15];}return out;}
template<size_t N>std::array<uint8_t,N> unhex(const Json& v){const auto text=v.string();if(text.size()!=N*2)invalid();const auto digit=[](char c)->uint8_t{if(c>='0'&&c<='9')return uint8_t(c-'0');if(c>='a'&&c<='f')return uint8_t(c-'a'+10);invalid();};std::array<uint8_t,N> out{};for(size_t i=0;i<N;++i)out[i]=uint8_t((digit(text[i*2])<<4)|digit(text[i*2+1]));if(!nonzero(out))invalid();return out;}
std::string session(const StreamId& v){const auto h=hex(v);return h.substr(0,8)+'-'+h.substr(8,4)+'-'+h.substr(12,4)+'-'+h.substr(16,4)+'-'+h.substr(20);}
std::string processPacket(Process p){return "{\"pid\":"+std::to_string(p.pid)+",\"created\":"+jsonQuote(std::to_string(p.created))+'}';}
Process process(const Json& v){fields(v,{"pid","created"});Process p{number(v.get("pid"),1,UINT32_MAX-1),integer(v.get("created"))};if(!valid(p))invalid();return p;}
std::string bindingPacket(const RestorationBinding& b){return "{\"service\":"+processPacket(b.service)+",\"worker\":"+processPacket(b.worker)+",\"stream\":"+jsonQuote(hex(b.stream))+",\"operation\":"+jsonQuote(hex(b.operation))+",\"console\":"+std::to_string(b.consoleSession)+'}';}
RestorationBinding binding(const Json& v){fields(v,{"service","worker","stream","operation","console"});return{process(v.get("service")),process(v.get("worker")),unhex<16>(v.get("stream")),unhex<32>(v.get("operation")),number(v.get("console"),1,UINT32_MAX-1)};}
bool sameTargets(std::span<const NativeDisplayTarget> a,std::span<const NativeDisplayTarget> b){if(a.size()!=1||b.size()!=1)return false;return a[0].id==b[0].id&&nativeSameLuid(a[0].adapter,b[0].adapter)&&a[0].target==b[0].target&&a[0].width==b[0].width&&a[0].height==b[0].height;}
bool leaseValid(const LeaseProof& p,const RestorationAdmission& a){return valid(p.lease)&&p.lease.instance==a.lease.instance&&p.lease.generation>=a.lease.generation&&p.targets.size()==1&&p.targets[0].id=="0"&&(p.targets[0].adapter.LowPart||p.targets[0].adapter.HighPart)&&ownedOutputSize(p.targets[0].width,p.targets[0].height);}
std::string leasePacket(const LeaseProof& p){const auto& t=p.targets.at(0);return "{\"instance\":"+jsonQuote(p.lease.instance)+",\"generation\":"+std::to_string(p.lease.generation)+",\"id\":\"0\",\"adapterLow\":"+std::to_string(t.adapter.LowPart)+",\"adapterHigh\":"+std::to_string(t.adapter.HighPart)+",\"target\":"+std::to_string(t.target)+",\"width\":"+std::to_string(t.width)+",\"height\":"+std::to_string(t.height)+'}';}
LeaseProof lease(const Json& v,const RestorationAdmission& a){fields(v,{"instance","generation","id","adapterLow","adapterHigh","target","width","height"});LeaseProof p{{v.get("instance").string(),number(v.get("generation"),1)},{{v.get("id").string(),{number(v.get("adapterLow")),signedNumber(v.get("adapterHigh"))},number(v.get("target")),number(v.get("width")),number(v.get("height"))}}};if(!leaseValid(p,a))invalid();return p;}
void originalValid(const OriginalProof& p,const RestorationAdmission& a){if(p.bytes.empty()||p.bytes.size()>65536||p.sha256!=a.originalSha256||nativeBaselineDigest(p.bytes)!=p.sha256)invalid();const auto state=parseDisplayV2State(p.bytes);if(state.session!=session(a.stream)||state.owned.size()!=1||state.owned[0].id!="0")invalid();}
std::pair<uint32_t,uint32_t> originalSize(const OriginalProof& p){const auto saved=parseDisplayV2State(p.bytes);if(saved.owned.size()!=1||saved.owned[0].id!="0")invalid();const auto& target=saved.owned[0];for(const auto& path:saved.ccd.paths)if(displayV2Active(path)&&nativeSameTarget(path,target)){const auto& source=nativeCcdSource(saved.ccd,path);return{source.width,source.height};}return{target.width,target.height};}
std::string originalPacket(const OriginalProof& p){return "{\"bytes\":"+jsonQuote(p.bytes)+",\"sha256\":"+jsonQuote(p.sha256)+'}';}
OriginalProof original(const Json& v,const RestorationAdmission& a){fields(v,{"bytes","sha256"});OriginalProof p{v.get("bytes").string(),v.get("sha256").string()};originalValid(p,a);return p;}
bool consoleAllowed(uint32_t id){const auto v=native::observeConsole(id);return cleanupDesktop(v,id,GetTickCount64()).has_value();}
native::Status ready(){return{native::Outcome::ready,native::Step::none,0};}
[[noreturn]]void nativeFail(native::Step step,DWORD error=0){throw native::NativeFailure{{native::Outcome::refused,step,error}};}
std::string requestPacket(const RestorationBinding& b,uint64_t serial,const char* op){return "{\"type\":\"signin.restore.request-v1\",\"protocol\":"+jsonQuote(protocol)+",\"binding\":"+bindingPacket(b)+",\"serial\":"+jsonQuote(std::to_string(serial))+",\"operation\":"+jsonQuote(op)+",\"payload\":{}}";}
struct Proxy {
  const RestorationAdmission& admission;const RestorationBinding& binding;Exchange exchange;std::function<uint64_t()> now;
  uint64_t serial=0;uint32_t generation=0;
  Json call(const char* op){
    if(!exchange||!now||!validAdmission(admission,now())||serial==UINT64_MAX)invalid();const auto sequence=++serial;
    const auto response=exchange(JsonParser::parse(requestPacket(binding,sequence,op)));
    fields(response,{"type","protocol","binding","serial","operation","ok","payload"});
    if(response.get("type").string()!="signin.restore.result-v1"||response.get("protocol").string()!=protocol||
        restoration::binding(response.get("binding"))!=binding||integer(response.get("serial"))!=sequence||response.get("operation").string()!=op||
        !boolean(response.get("ok"))||!validAdmission(admission,now()))invalid();return response.get("payload");
  }
  LeaseProof accept(const Json& value){auto p=lease(value,admission);if(p.lease.generation<generation)invalid();generation=p.lease.generation;return p;}
  LeaseProof current(){return accept(call("lease.read"));}
  OriginalProof original(){return restoration::original(call("original.read"),admission);}
};
}
std::optional<CleanupDesktop> cleanupDesktop(const native::ConsoleFacts& observed,uint32_t session,uint64_t now){
  if(!session||session==UINT32_MAX||!observed.valid||!observed.consoleProtocol||observed.session!=session||
      !observed.observedAt||observed.observedAt>now||now-observed.observedAt>1000)return {};
  if(observed.windows==WindowsState::noUser){if(!observed.signedInSid.empty())return {};return CleanupDesktop::winlogon;}
  if(observed.windows==WindowsState::locked)return CleanupDesktop::winlogon;
  if(observed.windows==WindowsState::unlocked)return CleanupDesktop::defaultDesktop;
  return {};
}
bool eligibleCleanupWorker(const RestorationBinding& binding,const native::ConsoleFacts& console,
    const native::WorkerFacts& worker,uint64_t now){
  const auto desktop=cleanupDesktop(console,binding.consoleSession,now);
  if(!desktop||!valid(binding.worker)||worker.process!=binding.worker||worker.session!=binding.consoleSession||
      !worker.localSystem||!worker.stationWinSta0)return false;
  if(*desktop==CleanupDesktop::winlogon)return worker.threadWinlogon&&worker.inputWinlogon&&!worker.threadDefault&&!worker.inputDefault;
  return worker.threadDefault&&worker.inputDefault&&!worker.threadWinlogon&&!worker.inputWinlogon;
}
// No predecessors: a leftover original whose stream worker already ended with
// its Windows session (signed out inside the stream). The service admits that
// only with nobody signed in (SigninBridge::Impl::restoreLeftover).
bool validAdmission(const RestorationAdmission& a,uint64_t now){
  if(a.purpose!=Purpose::sealedOriginalCleanup||!valid(a.service)||!nonzero(a.stream)||!valid(a.lease)||!nonzero(a.operation)||!nonzero(a.installedImageSha256)||
      !sha(a.originalSha256)||!a.consoleSession||a.consoleSession==UINT32_MAX||!a.issuedAt||a.issuedAt>UINT64_MAX-60000||a.issuedAt>now||
      a.expiresAt<=a.issuedAt||a.expiresAt-a.issuedAt>60000||now>=a.expiresAt||a.predecessors.size()>4)return false;
  for(size_t i=0;i<a.predecessors.size();++i){if(!valid(a.predecessors[i])||a.predecessors[i]==a.service)return false;for(size_t j=0;j<i;++j)if(a.predecessors[i]==a.predecessors[j])return false;}return true;
}
bool sameBinding(const RestorationBinding& b,const RestorationAdmission& a,Process child){return b.service==a.service&&b.stream==a.stream&&b.operation==a.operation&&b.consoleSession==a.consoleSession&&valid(child)&&b.worker==child&&child!=a.service&&std::find(a.predecessors.begin(),a.predecessors.end(),child)==a.predecessors.end();}
std::optional<Command> parseCommand(int argc,const wchar_t* const* argv){
  if(argc!=6||!argv||!argv[0]||!argv[1]||!argv[2]||!argv[3]||!argv[4]||!argv[5]||std::wstring(argv[1])!=L"--signin-restorer-v1"||std::wstring(argv[2])!=L"--pipe"||std::wstring(argv[4])!=L"--service-pid")return{};
  const std::wstring pid(argv[5]);uint64_t parsed=0;if(pid.empty()||pid.size()>10||pid.front()==L'0')return{};for(auto c:pid){if(c<L'0'||c>L'9')return{};parsed=parsed*10+uint64_t(c-L'0');if(parsed>=UINT32_MAX)return{};}
  const std::wstring pipe(argv[3]),prefix=L"\\\\.\\pipe\\MultitapHost.SigninRestore."+pid+L".";if(!pipe.starts_with(prefix)||pipe.size()!=prefix.size()+64)return{};
  const auto nonce=pipe.substr(prefix.size());if(std::all_of(nonce.begin(),nonce.end(),[](wchar_t c){return c==L'0';})||!std::all_of(nonce.begin(),nonce.end(),[](wchar_t c){return(c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}))return{};return Command{pipe,uint32_t(parsed)};
}
std::string initPacket(const RestorationAdmission& a,const RestorationBinding& b,uint64_t query){
  if(!sameBinding(b,a,b.worker)||!query)invalid();std::string previous="[";for(const auto& p:a.predecessors){if(previous.size()>1)previous+=',';previous+=processPacket(p);}previous+=']';
  return "{\"type\":\"signin.restore.init-v1\",\"protocol\":"+jsonQuote(protocol)+",\"purpose\":\"sealed-original-cleanup\",\"binding\":"+bindingPacket(b)+
    ",\"instance\":"+jsonQuote(a.lease.instance)+",\"leaseGeneration\":"+std::to_string(a.lease.generation)+",\"imageSha256\":"+jsonQuote(hex(a.installedImageSha256))+
    ",\"originalSha256\":"+jsonQuote(a.originalSha256)+",\"predecessors\":"+previous+",\"issuedAt\":"+jsonQuote(std::to_string(a.issuedAt))+
    ",\"expiresAt\":"+jsonQuote(std::to_string(a.expiresAt))+",\"serviceQuery\":"+jsonQuote(std::to_string(query))+'}';
}
std::string answer(const Json& request,const RestorationAdmission& a,const RestorationBinding& b,Authority& authority,uint64_t& lastSerial,uint64_t now){
  fields(request,{"type","protocol","binding","serial","operation","payload"});
  if(request.get("type").string()!="signin.restore.request-v1"||request.get("protocol").string()!=protocol||binding(request.get("binding"))!=b||!sameBinding(b,a,b.worker))invalid();
  const auto serial=integer(request.get("serial"));if(!serial||lastSerial==UINT64_MAX||serial!=lastSerial+1)invalid();lastSerial=serial;
  const auto op=request.get("operation").string();fields(request.get("payload"),{});bool ok=false;std::string result="{}";
  try{
    if(!validAdmission(a,now))invalid();const auto current=authority.current(a,b);if(!current||!leaseValid(*current,a))invalid();
    const auto saved=authority.original(a,b);if(!saved)invalid();originalValid(*saved,a);
    if(op=="lease.read"){result=leasePacket(*current);ok=true;}
    else if(op=="original.read"){result=originalPacket(*saved);ok=true;}
    else if(op=="original.size"){
      const auto resized=authority.sizeOriginal(a,b);const auto wanted=originalSize(*saved);
      if(!resized||!leaseValid(*resized,a)||resized->targets[0].width!=wanted.first||resized->targets[0].height!=wanted.second)invalid();result=leasePacket(*resized);ok=true;
    }else invalid();
    const auto after=authority.current(a,b);const auto retained=authority.original(a,b);
    if(!after||!leaseValid(*after,a)||!retained||retained->bytes!=saved->bytes||retained->sha256!=saved->sha256)ok=false;
  }catch(...){ok=false;result="{}";}
  return "{\"type\":\"signin.restore.result-v1\",\"protocol\":"+jsonQuote(protocol)+",\"binding\":"+bindingPacket(b)+",\"serial\":"+jsonQuote(std::to_string(serial))+
    ",\"operation\":"+jsonQuote(op)+",\"ok\":"+(ok?"true":"false")+",\"payload\":"+result+'}';
}
bool originalLayoutMatches(const std::string& originalBytes,const std::string& observedState){
  try{
    const auto saved=parseDisplayV2State(originalBytes),observed=parseDisplayV2State(observedState);
    if(saved.session!=observed.session||saved.owned.size()!=1||observed.owned.size()!=1||saved.owned[0].id!="0"||observed.owned[0].id!="0")return false;
    std::vector<DisplayV2Expected> expected;const auto plan=planDisplayV2Restore(saved,observed.ccd,observed.owned,&expected);
    if(!displayV2RestoreTargetsMatch(observed.ccd,expected)||!displayV2SameForeign(displayV2Foreign(plan,observed.owned,std::nullopt),displayV2Foreign(observed.ccd,observed.owned,std::nullopt)))return false;
    const auto active=[](const NativeCcdData& data){return std::count_if(data.paths.begin(),data.paths.end(),[](const auto& p){return displayV2Active(p);});};
    if(active(plan)!=active(observed.ccd))return false;
    // SDK public id remaps the owned target only. Its original active state,
    // source geometry/format, refresh and rotation must also really return.
    const auto find=[](const NativeCcdData& ccd,const NativeDisplayTarget& t){return std::find_if(ccd.paths.begin(),ccd.paths.end(),[&](const auto& p){return displayV2Active(p)&&nativeSameTarget(p,t);});};
    const auto before=find(saved.ccd,saved.owned[0]),after=find(observed.ccd,observed.owned[0]);
    if((before==saved.ccd.paths.end())!=(after==observed.ccd.paths.end()))return false;
    if(before!=saved.ccd.paths.end()){
      const auto& a=nativeCcdSource(saved.ccd,*before);const auto& b=nativeCcdSource(observed.ccd,*after);
      if(a.width!=b.width||a.height!=b.height||a.position.x!=b.position.x||a.position.y!=b.position.y||a.pixelFormat!=b.pixelFormat||before->targetInfo.rotation!=after->targetInfo.rotation)return false;
      const auto& x=before->targetInfo.refreshRate;const auto& y=after->targetInfo.refreshRate;
      if(!x.Denominator||!y.Denominator){if(x.Numerator!=y.Numerator||x.Denominator!=y.Denominator)return false;}
      else if(uint64_t(x.Numerator)*y.Denominator!=uint64_t(y.Numerator)*x.Denominator)return false;
    }
    if(saved.console&&displayV2ConsoleActive(saved.ccd,*saved.console)&&!displayV2ConsoleActive(observed.ccd,*saved.console))return false;
    return true;
  }catch(...){return false;}
}
std::string receiptPacket(const Receipt& r){
  const char* outcome=r.outcome==Outcome::topologyRestoredOriginalHeld?"topology-restored-original-held":r.outcome==Outcome::failed?"failed":"refused";
  return "{\"type\":\"signin.restore.finished-v1\",\"protocol\":"+jsonQuote(protocol)+",\"binding\":"+bindingPacket(r.binding)+",\"outcome\":"+jsonQuote(outcome)+
    ",\"underlyingOutcome\":"+std::to_string(uint32_t(r.underlyingOutcome))+",\"stage\":"+std::to_string(r.stage)+",\"error\":"+std::to_string(r.error)+",\"fallback\":"+std::to_string(r.fallback)+
    ",\"originalSha256\":"+jsonQuote(r.originalSha256)+",\"observedInstance\":"+jsonQuote(r.observedLease.instance)+",\"observedGeneration\":"+std::to_string(r.observedLease.generation)+
    ",\"observedAt\":"+jsonQuote(std::to_string(r.observedAt))+",\"observedState\":"+jsonQuote(r.observedState)+'}';
}
Receipt parseReceipt(const Json& v){
  fields(v,{"type","protocol","binding","outcome","underlyingOutcome","stage","error","fallback","originalSha256","observedInstance","observedGeneration","observedAt","observedState"});
  if(v.get("type").string()!="signin.restore.finished-v1"||v.get("protocol").string()!=protocol)invalid();
  Receipt r;r.binding=binding(v.get("binding"));const auto out=v.get("outcome").string();
  if(out=="topology-restored-original-held")r.outcome=Outcome::topologyRestoredOriginalHeld;else if(out=="failed")r.outcome=Outcome::failed;else if(out!="refused")invalid();
  r.underlyingOutcome=DisplayV2Outcome(number(v.get("underlyingOutcome"),0,3));r.stage=number(v.get("stage"));r.error=number(v.get("error"));r.fallback=number(v.get("fallback"));
  r.originalSha256=v.get("originalSha256").string();r.observedLease={v.get("observedInstance").string(),number(v.get("observedGeneration"))};r.observedAt=integer(v.get("observedAt"));r.observedState=v.get("observedState").string();
  if(!sha(r.originalSha256)||r.observedState.size()>65536)invalid();
  if(r.outcome==Outcome::topologyRestoredOriginalHeld&&(!valid(r.observedLease)||!r.observedAt||r.observedState.empty()||r.underlyingOutcome!=DisplayV2Outcome::failed||r.stage!=displayV2Stage::state||r.error||r.fallback>1))invalid();
  return r;
}
bool verifiedReceipt(const RestorationAdmission& a,const RestorationBinding& b,const OriginalProof& original,const LeaseProof& actual,const Receipt& r,uint64_t now){
  try{
    if(!sameBinding(b,a,b.worker)||r.binding!=b||r.outcome!=Outcome::topologyRestoredOriginalHeld||r.underlyingOutcome!=DisplayV2Outcome::failed||
        r.stage!=displayV2Stage::state||r.error||r.fallback>1||!r.observedAt||r.observedAt>now||now-r.observedAt>2000||!validAdmission(a,r.observedAt)||
        r.originalSha256!=a.originalSha256||r.observedLease!=actual.lease||!leaseValid(actual,a))return false;
    originalValid(original,a);const auto observed=parseDisplayV2State(r.observedState);
    return observed.session==session(a.stream)&&sameTargets(observed.owned,actual.targets)&&originalLayoutMatches(original.bytes,r.observedState);
  }catch(...){return false;}
}
Receipt restoreDisplay(const RestorationAdmission& a,const RestorationBinding& b,DisplayV2Access local,Exchange exchange,std::function<uint64_t()> now){
  Receipt result;result.binding=b;result.originalSha256=a.originalSha256;
  if(!now||!exchange||!validAdmission(a,now())||!sameBinding(b,a,b.worker)||!local.query||!local.configure||!local.eligible)return result;
  const auto localEligible=local.eligible;const auto query=local.query;Proxy proxy{a,b,std::move(exchange),now};
  try{
    const auto original=proxy.original();const auto wanted=originalSize(original);(void)proxy.current();
    local.owned=[&]{return proxy.current().targets;};local.restoreOwned=local.owned;
    local.resize=[&](const std::string& id,DWORD width,DWORD height)->std::optional<std::vector<NativeDisplayTarget>>{
      if(id!="0"||width!=wanted.first||height!=wanted.second)return{};return proxy.accept(proxy.call("original.size")).targets;
    };
    local.sizes={};local.count={};local.store.load=[&]()->std::optional<std::string>{return proxy.original().bytes;};
    local.store.save=[](const std::string&){return false;};local.store.remove=[] {return false;};local.store.discard=[]{};
    local.eligible=[&]{if(!localEligible()||!validAdmission(a,now()))return false;try{(void)proxy.current();return true;}catch(...){return false;}};
    DisplaySessionV2 display(std::move(local));const uint64_t at=now();if(at>=a.expiresAt)invalid();
    const auto restored=display.restore(std::min<uint64_t>(12000,a.expiresAt-at));
    result.underlyingOutcome=restored.outcome;result.stage=restored.stage;result.error=restored.error;result.fallback=restored.fallback;
    if(!localEligible())invalid();const auto finalLease=proxy.current();const auto observed=displayV2ActiveCcd(query());
    if(!localEligible()||proxy.original().bytes!=original.bytes)invalid();
    result.observedState=displayV2StateBytes({session(a.stream),observed,finalLease.targets,parseDisplayV2State(original.bytes).console});
    result.observedLease=finalLease.lease;result.observedAt=now();result.outcome=Outcome::failed;
    if(result.underlyingOutcome==DisplayV2Outcome::failed&&result.stage==displayV2Stage::state&&!result.error&&result.fallback<=1&&
        originalLayoutMatches(original.bytes,result.observedState))result.outcome=Outcome::topologyRestoredOriginalHeld;
  }catch(...){result.outcome=Outcome::failed;if(!result.stage)result.stage=displayV2Stage::internal;}
  return result;
}

OwnedRestorationChild::~OwnedRestorationChild(){(void)stopAndWait(0);}
bool OwnedRestorationChild::connect(uint32_t milliseconds){
  if(!pipe_||!process()||milliseconds>5000)return false;
  const auto peer=[&]{try{ULONG pid=0;return GetNamedPipeClientProcessId(pipe_.get(),&pid)&&pid==binding_.worker.pid&&native::processIdentity(process())==binding_.worker&&WaitForSingleObject(process(),0)==WAIT_TIMEOUT;}catch(...){return false;}};
  native::Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event)return false;OVERLAPPED op{};op.hEvent=event.get();
  if(ConnectNamedPipe(pipe_.get(),&op))return peer();const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)return peer();if(error!=ERROR_IO_PENDING)return false;
  const HANDLE handles[]{event.get(),process()};const auto waited=WaitForMultipleObjects(2,handles,FALSE,milliseconds);DWORD ignored=0;
  if(waited!=WAIT_OBJECT_0){CancelIoEx(pipe_.get(),&op);GetOverlappedResult(pipe_.get(),&op,&ignored,TRUE);return false;}return GetOverlappedResult(pipe_.get(),&op,&ignored,FALSE)&&peer();
}
bool OwnedRestorationChild::stopAndWait(uint32_t milliseconds){
  if(milliseconds>5000)return false;pipe_.reset();if(!process()||WaitForSingleObject(process(),milliseconds)==WAIT_OBJECT_0){job_.reset();return true;}
  job_.reset();if(process()&&!jobAssigned_&&WaitForSingleObject(process(),0)==WAIT_TIMEOUT)(void)TerminateProcess(process(),1);return WaitForSingleObject(process(),0)==WAIT_OBJECT_0;
}
native::Handle OwnedRestorationChild::takeUnexited(){
  if(!process()||WaitForSingleObject(process(),0)!=WAIT_TIMEOUT)return{};HANDLE copy=nullptr;
  if(!DuplicateHandle(GetCurrentProcess(),process(),GetCurrentProcess(),&copy,PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,0))nativeFail(native::Step::childIdentity,GetLastError());return native::Handle(copy);
}
native::Status launch(const RestorationAdmission& a,Authority& authority,OwnedRestorationChild& child,const service_workers::CleanupPermit* cleanupOwner){
  try{
    if(child.process()||child.pipe_||child.job_||!validAdmission(a,GetTickCount64()))nativeFail(native::Step::admission);
    auto physicalCreation=cleanupOwner?cleanupOwner->reserveRestoration():service_workers::reserveCurrent(service_workers::Role::signinRestoration,true);
    native::Handle physicalJob(physicalCreation.createTrackingJob());
    child.retainedProcess_=physicalCreation.observer();
    RestorationBinding before{a.service,{},a.stream,a.operation,a.consoleSession};const auto proof=authority.current(a,before);const auto saved=authority.original(a,before);
    if(!proof||!leaseValid(*proof,a)||!saved||!consoleAllowed(a.consoleSession))nativeFail(native::Step::lease);originalValid(*saved,a);
    native::OwnSystemLaunchMaterial material;const auto admitted=native::ownSystemRestorationLaunchMaterial(a.service,a.consoleSession,a.installedImageSha256,material);if(!admitted)return admitted;
    PSECURITY_DESCRIPTOR descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)",SDDL_REVISION_1,&descriptor,nullptr))nativeFail(native::Step::pipe,GetLastError());
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};const auto nonce=hex(a.operation);
    child.pipeName_=L"\\\\.\\pipe\\MultitapHost.SigninRestore."+std::to_wstring(a.service.pid)+L"."+std::wstring(nonce.begin(),nonce.end());
    child.pipe_=native::Handle(CreateNamedPipeW(child.pipeName_.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,262144,262144,0,&security));const auto pipeError=GetLastError();LocalFree(descriptor);if(!child.pipe_)nativeFail(native::Step::pipe,pipeError);
    child.job_=native::Handle(CreateJobObjectW(nullptr,nullptr));if(!child.job_)nativeFail(native::Step::isolateProcess,GetLastError());JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(child.job_.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)))nativeFail(native::Step::isolateProcess,GetLastError());
    struct Environment{void* value=nullptr;~Environment(){if(value)DestroyEnvironmentBlock(value);}}environment;if(!CreateEnvironmentBlock(&environment.value,material.token.get(),FALSE))nativeFail(native::Step::createProcess,GetLastError());
    const auto finalProof=authority.current(a,before);const auto originalAgain=authority.original(a,before);
    const auto finalConsole=native::observeConsole(a.consoleSession);const auto desktopKind=cleanupDesktop(finalConsole,a.consoleSession,GetTickCount64());
    if(!validAdmission(a,GetTickCount64())||!finalProof||!leaseValid(*finalProof,a)||!originalAgain||originalAgain->bytes!=saved->bytes||originalAgain->sha256!=saved->sha256||!desktopKind)nativeFail(native::Step::lease);
    std::wstring command=L"\""+material.image+L"\" --signin-restorer-v1 --pipe "+child.pipeName_+L" --service-pid "+std::to_wstring(a.service.pid);
    service_workers::JobListStartup physicalStartup(physicalJob.get(),child.job_.get());auto& startup=physicalStartup.startup.StartupInfo;
    std::wstring desktop=*desktopKind==CleanupDesktop::winlogon?L"winsta0\\Winlogon":L"winsta0\\Default";
    startup.lpDesktop=desktop.data();PROCESS_INFORMATION created{};
    const auto slash=material.image.find_last_of(L"\\/");if(slash==std::wstring::npos)nativeFail(native::Step::serviceImage);
    physicalCreation.requireLaunchAllowed();
    if(!CreateProcessAsUserW(material.token.get(),material.image.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,environment.value,material.image.substr(0,slash).c_str(),&startup,&created))nativeFail(native::Step::createProcess,GetLastError());
    child.process_=native::Handle(created.hProcess);native::Handle thread(created.hThread);child.imageLock_=std::move(material.imageLock);child.jobAssigned_=true;
    HANDLE physicalOriginal=child.process_.release();
    try{physicalCreation.retain(physicalOriginal);}catch(...){child.process_.reset(physicalOriginal);const auto known=child.retainedProcess_.identity();if(known.known)child.binding_={a.service,{known.pid,known.birth},a.stream,a.operation,a.consoleSession};throw;}
    child.process_.reset(physicalOriginal);
    child.binding_={a.service,native::processIdentity(child.process()),a.stream,a.operation,a.consoleSession};
    std::wstring image(32768,L'\0');DWORD length=DWORD(image.size());if(!QueryFullProcessImageNameW(child.process(),0,image.data(),&length)||!length||length>=image.size())nativeFail(native::Step::childIdentity,GetLastError());image.resize(length);
    if(!sameBinding(child.binding_,a,child.binding_.worker)||WaitForSingleObject(child.process(),0)!=WAIT_TIMEOUT||CompareStringOrdinal(image.c_str(),int(image.size()),material.image.c_str(),int(material.image.size()),TRUE)!=CSTR_EQUAL)nativeFail(native::Step::childIdentity);
    HANDLE query=nullptr;if(!DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),child.process(),&query,PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,0))nativeFail(native::Step::binding,GetLastError());child.delegatedServiceQuery_=uint64_t(reinterpret_cast<uintptr_t>(query));
    if(ResumeThread(thread.get())==DWORD(-1))nativeFail(native::Step::createProcess,GetLastError());return ready();
  }catch(const native::NativeFailure& failure){return failure.status;}catch(...){return{native::Outcome::failed,native::Step::createProcess,0};}
}
int workerCommand(int argc,const wchar_t* const* argv)noexcept{
  try{
    const auto command=parseCommand(argc,argv);if(!command||command->servicePid==GetCurrentProcessId())return ERROR_INVALID_PARAMETER;if(!WaitNamedPipeW(command->pipe.c_str(),5000))return ERROR_PIPE_NOT_CONNECTED;
    native::Handle pipe(CreateFileW(command->pipe.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));if(!pipe)return ERROR_PIPE_NOT_CONNECTED;
    ULONG server=0;if(!GetNamedPipeServerProcessId(pipe.get(),&server)||server!=command->servicePid)return ERROR_ACCESS_DENIED;
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);if(!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))return ERROR_INVALID_STATE;
    std::atomic<bool> stopped=false;std::mutex writing;bool text=false;std::vector<uint8_t> bytes;const std::atomic<int64_t> initial{streamMonotonicMillis()+5000};
    if(!readPipePacket(pipe.get(),text,bytes,stopped,std::chrono::milliseconds(5000),&initial)||!text||bytes.size()>8192)return ERROR_INVALID_DATA;
    const auto init=JsonParser::parse(std::string(bytes.begin(),bytes.end()));fields(init,{"type","protocol","purpose","binding","instance","leaseGeneration","imageSha256","originalSha256","predecessors","issuedAt","expiresAt","serviceQuery"});
    if(init.get("type").string()!="signin.restore.init-v1"||init.get("protocol").string()!=protocol||init.get("purpose").string()!="sealed-original-cleanup")return ERROR_INVALID_DATA;
    const auto b=binding(init.get("binding"));RestorationAdmission a;a.service=b.service;a.stream=b.stream;a.operation=b.operation;a.consoleSession=b.consoleSession;
    a.lease={init.get("instance").string(),number(init.get("leaseGeneration"),1)};a.installedImageSha256=unhex<32>(init.get("imageSha256"));a.originalSha256=init.get("originalSha256").string();a.issuedAt=integer(init.get("issuedAt"));a.expiresAt=integer(init.get("expiresAt"));
    const auto* previous=std::get_if<Json::Array>(&init.get("predecessors").value);if(!previous||previous->size()>4)return ERROR_INVALID_DATA;for(const auto& p:*previous)a.predecessors.push_back(process(p));
    if(!validAdmission(a,GetTickCount64())||!sameBinding(b,a,native::processIdentity(GetCurrentProcess()))||b.service.pid!=command->servicePid)return ERROR_ACCESS_DENIED;
    const auto raw=integer(init.get("serviceQuery"));if(!raw||raw>UINTPTR_MAX)return ERROR_INVALID_DATA;const HANDLE held=reinterpret_cast<HANDLE>(uintptr_t(raw));native::Handle imageLock;
    const auto peer=native::verifyRestorationParentPeer(b.service,held,pipe.get(),a.installedImageSha256,imageLock);if(!peer)return ERROR_ACCESS_DENIED;native::Handle service(held);
    const auto localEligible=[&]{try{
      if(!native::verifyRestorationParentCurrent(b.service,service.get(),pipe.get()))return false;
      const auto console=native::observeConsole(a.consoleSession);const auto worker=native::observeWorker();
      return eligibleCleanupWorker(b,console,worker,GetTickCount64());
    }catch(...){return false;}};
    if(!localEligible())return ERROR_ACCESS_DENIED;DisplayV2Access access;access.query=[]{return queryNativeCcd();};access.configure=windowsNativeCcdConfigure();access.eligible=localEligible;access.now=[]{return GetTickCount64();};access.poll=[]{Sleep(displayV2StreamPollMs);};
    const auto exchange=[&](const Json& request){
      if(!localEligible())invalid();const auto packet=jsonObject(request);if(packet.size()>262144)invalid();const std::atomic<int64_t> deadline{int64_t(std::min<uint64_t>(a.expiresAt,GetTickCount64()+15000))};
      if(!pipePacket(pipe.get(),true,{packet.begin(),packet.end()},stopped,writing,&deadline))invalid();bool isText=false;std::vector<uint8_t> response;
      if(!readPipePacket(pipe.get(),isText,response,stopped,std::chrono::milliseconds(15000),&deadline)||!isText||response.size()>262144||!localEligible())invalid();return JsonParser::parse(std::string(response.begin(),response.end()));
    };
    const auto result=restoreDisplay(a,b,std::move(access),exchange,[]{return GetTickCount64();});const auto packet=receiptPacket(result);const std::atomic<int64_t> end{streamMonotonicMillis()+1000};
    (void)pipePacket(pipe.get(),true,{packet.begin(),packet.end()},stopped,writing,&end);return result.outcome==Outcome::topologyRestoredOriginalHeld?0:ERROR_NOT_READY;
  }catch(...){return ERROR_INVALID_DATA;}
}
} // namespace host::signin::restoration
