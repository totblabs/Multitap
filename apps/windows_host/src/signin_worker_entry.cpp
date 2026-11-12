#include "signin_worker_entry.hpp"
#include "signin_owned_release.hpp"
#ifndef MULTITAP_SIGNIN_ENTRY_CORE_ONLY
#include "agent.hpp" // RemoteInput only; Agent is never constructed.
#endif
#include "direct_input_validation.hpp"
#include "text_input.hpp"
#include "gpu_h264_policy.hpp"
#include "pipe_packet.hpp"
#include <bcrypt.h>
#include <charconv>
#include <map>

namespace host::signin::entry {
namespace {
[[noreturn]] void invalid(){throw std::runtime_error("Invalid private sign-in entry control");}
void fields(const Json& value,std::initializer_list<const char*> names){
  const auto* object=std::get_if<Json::Object>(&value.value);
  if(!object||object->size()!=names.size())invalid();
  for(const auto* name:names)if(!object->contains(name))invalid();
}
uint32_t number(const Json& value,uint32_t low,uint32_t high){
  const auto* n=std::get_if<double>(&value.value);
  if(!n||!std::isfinite(*n)||*n<low||*n>high||std::floor(*n)!=*n)invalid();
  return uint32_t(*n);
}
uint64_t integer(const Json& value){
  const auto* text=std::get_if<std::string>(&value.value);uint64_t out=0;
  if(!text||text->empty()||text->size()>20||(text->size()>1&&text->front()=='0'))invalid();
  const auto result=std::from_chars(text->data(),text->data()+text->size(),out);
  if(result.ec!=std::errc{}||result.ptr!=text->data()+text->size())invalid();return out;
}
template<size_t N>std::string hex(const std::array<uint8_t,N>& value){
  constexpr char digits[]="0123456789abcdef";std::string out;out.reserve(N*2);
  for(auto byte:value){out+=digits[byte>>4];out+=digits[byte&15];}return out;
}
template<size_t N>std::array<uint8_t,N> unhex(const Json& value){
  const auto* text=std::get_if<std::string>(&value.value);std::array<uint8_t,N> out{};
  if(!text||text->size()!=N*2)invalid();
  const auto digit=[](char c)->uint8_t{if(c>='0'&&c<='9')return uint8_t(c-'0');if(c>='a'&&c<='f')return uint8_t(c-'a'+10);invalid();};
  for(size_t i=0;i<N;++i)out[i]=uint8_t((digit((*text)[i*2])<<4)|digit((*text)[i*2+1]));
  if(!nonzero(out))invalid();return out;
}
std::string session(const StreamId& value){
  auto text=hex(value);text.insert(20,"-");text.insert(16,"-");text.insert(12,"-");text.insert(8,"-");return text;
}
Json bindingObject(const channel::Init& init){return JsonParser::parse(channel::initPacket(init)).get("binding");}
void checkBinding(const Json& value,const channel::Init& init){if(jsonObject(value)!=jsonObject(bindingObject(init)))invalid();}
std::string prefix(const char* type,const channel::Init& init){return "{\"type\":"+jsonQuote(type)+",\"binding\":"+jsonObject(bindingObject(init));}
Start parseStart(const Json& value,const channel::Init& init){
  fields(value,{"type","binding","transitionId","firstFrameId","scopeGeneration","fps","bitrate"});
  if(value.get("type").string()!="signin.worker.start-v1")invalid();checkBinding(value.get("binding"),init);
  Start out;out.transition=unhex<16>(value.get("transitionId"));out.firstFrame=unhex<16>(value.get("firstFrameId"));
  out.scopeGeneration=integer(value.get("scopeGeneration"));out.fps=number(value.get("fps"),1,120);
  out.bitrate=number(value.get("bitrate"),encoderMinimumBitrate,encoderMaximumBitrate);
  if(!out.scopeGeneration)invalid();return out;
}
FirstFrame parseGrant(const Json& value,const channel::Init& init,const char* type){
  fields(value,{"type","binding","transitionId","firstFrameId","sourceSequence","ptsUs","payloadSHA256"});
  if(value.get("type").string()!=type)invalid();checkBinding(value.get("binding"),init);
  FirstFrame out;out.transition=unhex<16>(value.get("transitionId"));out.firstFrame=unhex<16>(value.get("firstFrameId"));
  out.sequence=integer(value.get("sourceSequence"));out.ptsUs=integer(value.get("ptsUs"));out.payloadSha256=unhex<32>(value.get("payloadSHA256"));
  out.width=init.target.width;out.height=init.target.height;if(!out.sequence||!out.ptsUs)invalid();return out;
}
std::string correlation(const FirstFrame& frame){
  return ",\"transitionId\":"+jsonQuote(hex(frame.transition))+",\"firstFrameId\":"+jsonQuote(hex(frame.firstFrame))+
    ",\"sourceSequence\":"+jsonQuote(std::to_string(frame.sequence))+",\"ptsUs\":"+jsonQuote(std::to_string(frame.ptsUs))+
    ",\"payloadSHA256\":"+jsonQuote(hex(frame.payloadSha256));
}
bool ordinary(const Json& input,const std::string& expected){
  const auto* object=std::get_if<Json::Object>(&input.value);if(!object)return false;
  if(object->contains("sessionId")&&input.get("sessionId").string()!=expected)return false;
  if(input.get("type").string()=="input.reset"||input.get("type").string()=="input.fence"){
    for(const auto& [name,value]:*object){(void)value;if(name!="type"&&name!="sessionId")return false;}return true;
  }
  if(input.get("event").string()=="text")return bool(textInputUnits(input));
  const auto event=input.get("event").string();
  for(const auto& [name,value]:*object){
    (void)value;
    if(name=="type"||name=="event"||name=="sessionId")continue;
    if(event=="key"&&(name=="vk"||name=="scan"||name=="down"||name=="extended"))continue;
    if((event=="move"||event=="button"||event=="wheel")&&(name=="x"||name=="y"))continue;
    if(event=="button"&&(name=="button"||name=="down"))continue;
    if(event=="wheel"&&name=="delta")continue;
    return false;
  }
  return validDirectInput(input,false);
}
Json parseInput(const Json& packet,const channel::Init& init,const Start& start){
  fields(packet,{"type","binding","phase"});if(packet.get("type").string()!="signin.worker.input-v1")invalid();
  checkBinding(packet.get("binding"),init);const auto& phase=packet.get("phase");
  fields(phase,{"type","protocol","sessionId","transitionId","workerGeneration","mediaScope","input"});
  const auto& scope=phase.get("mediaScope");fields(scope,{"kind","epoch"});
  if(phase.get("type").string()!="input.phase"||phase.get("protocol").string()!="signin-stream-v1"||
     phase.get("sessionId").string()!=session(init.binding.stream)||unhex<16>(phase.get("transitionId"))!=start.transition||
     number(phase.get("workerGeneration"),1,UINT32_MAX)!=init.binding.generation||scope.get("kind").string()!="composite"||
     number(scope.get("epoch"),1,UINT16_MAX)!=init.binding.mediaEpoch||!ordinary(phase.get("input"),session(init.binding.stream)))invalid();
  return phase.get("input");
}
bool same(const FirstFrame& a,const FirstFrame& b){return a==b;}
Cause fromNative(const native::Status& status){
  if(status.outcome==native::Outcome::handoffRequired)return Cause::handoff;
  if(status.outcome==native::Outcome::unsupported)return Cause::unsupported;
  return Cause::refused;
}
#ifndef MULTITAP_SIGNIN_ENTRY_CORE_ONLY
uint64_t nowUs(){return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());}
#endif
}
Nonce payloadDigest(std::span<const uint8_t> bytes){
  if(bytes.empty()||bytes.size()>encoderMaximumFrameBytes)invalid();
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;Nonce result{};
  const auto close=[&]{if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);};
  try{
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0||
       BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0||
       BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),0)<0||
       BCryptFinishHash(hash,result.data(),ULONG(result.size()),0)<0)invalid();
    close();return result;
  }catch(...){close();throw;}
}
std::string startPacket(const channel::Init& init,const Start& start){
  const auto packet=prefix("signin.worker.start-v1",init)+",\"transitionId\":"+jsonQuote(hex(start.transition))+
    ",\"firstFrameId\":"+jsonQuote(hex(start.firstFrame))+",\"scopeGeneration\":"+jsonQuote(std::to_string(start.scopeGeneration))+
    ",\"fps\":"+std::to_string(start.fps)+",\"bitrate\":"+std::to_string(start.bitrate)+'}';
  (void)parseStart(JsonParser::parse(packet),init);return packet;
}
std::string grantPacket(const channel::Init& init,const FirstFrame& frame,Grant grant){
  if(grant!=Grant::publish&&grant!=Grant::decoded)invalid();
  const auto* type=grant==Grant::publish?"signin.worker.publish-v1":"signin.worker.decoded-v1";
  const auto packet=prefix(type,init)+correlation(frame)+'}';(void)parseGrant(JsonParser::parse(packet),init,type);return packet;
}
std::string inputPacket(const channel::Init& init,const Json& phase){return prefix("signin.worker.input-v1",init)+",\"phase\":"+jsonObject(phase)+'}';}
std::string stopPacket(const channel::Init& init){return prefix("signin.worker.stop-v1",init)+'}';}
std::string cleanupPacket(const channel::Init& init,const Exit& result){
  const bool known=result.releasePending<=UINT32_MAX;
  const auto state=!known?"unknown":result.releasePending?"pending":"complete";
  return prefix("signin.worker.cleanup-v1",init)+",\"release\":{\"protocol\":\"signin-owned-input-release-v1\",\"state\":"+jsonQuote(state)+",\"pendingCount\":"+(known?std::to_string(result.releasePending):"null")+"}}";
}
bool cleanupComplete(const Json& value,const channel::Init& init){
  fields(value,{"type","binding","release"});if(value.get("type").string()!="signin.worker.cleanup-v1")invalid();checkBinding(value.get("binding"),init);
  const auto& proof=value.get("release");fields(proof,{"protocol","state","pendingCount"});if(proof.get("protocol").string()!="signin-owned-input-release-v1")invalid();
  const auto state=proof.get("state").string();
  if(state=="unknown"){if(!std::holds_alternative<std::nullptr_t>(proof.get("pendingCount").value))invalid();return false;}
  const auto count=number(proof.get("pendingCount"),0,UINT32_MAX);
  if((state=="complete"&&count!=0)||(state=="pending"&&count==0)||(state!="complete"&&state!="pending"))invalid();
  return state=="complete";
}
FirstFrame parseFirstFrame(const Json& value,const channel::Init& init){
  fields(value,{"type","binding","transitionId","firstFrameId","sourceSequence","ptsUs","payloadSHA256","width","height","ownedOutputMask","idr","sps","pps","profile"});
  if(value.get("type").string()!="signin.worker.frame-v1")invalid();checkBinding(value.get("binding"),init);
  FirstFrame out;out.transition=unhex<16>(value.get("transitionId"));out.firstFrame=unhex<16>(value.get("firstFrameId"));
  out.sequence=integer(value.get("sourceSequence"));out.ptsUs=integer(value.get("ptsUs"));out.payloadSha256=unhex<32>(value.get("payloadSHA256"));
  out.width=number(value.get("width"),16,8192);out.height=number(value.get("height"),16,8192);
  if(!out.sequence||!out.ptsUs||out.width!=init.target.width||out.height!=init.target.height||number(value.get("ownedOutputMask"),1,1)!=1||
     number(value.get("profile"),66,66)!=66)invalid();
  for(const char* name:{"idr","sps","pps"})if(!std::holds_alternative<bool>(value.get(name).value)||!value.get(name).boolean())invalid();return out;
}
bool Deferred::push(const Json& value)noexcept{
  try{
    const auto size=jsonObject(value).size();
    if(poisoned_||size>8192||queue_.size()>=32||size>128*1024-bytes_){poisoned_=true;return false;}
    queue_.emplace_back(value,size);bytes_+=size;return true;
  }catch(...){poisoned_=true;return false;}
}
bool Deferred::pop(Json& value){if(queue_.empty())return false;value=std::move(queue_.front().first);bytes_-=queue_.front().second;queue_.pop_front();return true;}
bool Deferred::pendingFence()const{
  if(poisoned_)return true;
  return std::any_of(queue_.begin(),queue_.end(),[](const auto& packet){return packet.first.get("type").string()!="signin.worker.input-v1";});
}
struct Loop::Impl {
  channel::Init init;Deferred& deferred;Pipeline& pipeline;Output& output;Now clock;
  uint64_t begun=0,deadline=0,lastNow=0,nextCapture=0,lastCapture=0,lastPts=0,lastSubmitted=0;Exit result;Stage operation=Stage::authority;
  std::optional<Start> start;std::optional<FirstFrame> first;
  std::map<uint64_t,uint64_t> submitted;std::deque<Frame> buffered;std::vector<Frame> encodedWaiting;size_t bufferedBytes=0;
  bool opened=false,publishGranted=false,published=false,decoded=false,closed=false;
  Impl(channel::Init value,Deferred& q,Pipeline& p,Output& o,Now now,uint64_t beginning)
    :init(std::move(value)),deferred(q),pipeline(p),output(o),clock(std::move(now)),begun(beginning),lastNow(beginning){
    (void)channel::initPacket(init);
    if(!clock||!beginning||beginning>UINT64_MAX-15000000)invalid();deadline=beginning+15000000;
  }
  uint64_t time(){const auto value=clock();if(value<lastNow)invalid();lastNow=value;return value;}
  bool budget(){return decoded||time()<deadline;}
  bool delivery(){
    if(closed||!decoded||deferred.pendingFence())return false;
    const auto status=pipeline.current();return status&&!closed&&decoded&&!deferred.pendingFence();
  }
  Exit finish(Cause cause,Stage stage,uint32_t error=0){
    if(closed)return result;closed=true;decoded=false;published=false;
    result={cause,stage,error,0};buffered.clear();encodedWaiting.clear();bufferedBytes=0;submitted.clear();
    try{result.releasePending=pipeline.release([&]{return bool(pipeline.current());});}catch(...){result.releasePending=SIZE_MAX;}
    pipeline.close();return result;
  }
  enum class Sent { yes, deferred, closed };
  Sent send(Frame& frame){
    const auto fresh=pipeline.current();
    if(!fresh){finish(fromNative(fresh),Stage::authority,fresh.error);return Sent::closed;}
    if(!budget()){finish(Cause::deadline,Stage::publish);return Sent::closed;}
    if(deferred.poisoned()){finish(Cause::queue,Stage::publish);return Sent::closed;}
    if(deferred.pendingFence())return Sent::deferred;
    if(!output.media(std::move(frame),init.binding.mediaEpoch)){finish(Cause::transport,Stage::publish);return Sent::closed;}
    return Sent::yes;
  }
  bool flush(){
    if(!publishGranted)return true;
    while(!buffered.empty()){
      const auto size=buffered.front().bytes.size();
      const auto sent=send(buffered.front());
      if(sent==Sent::closed)return false;
      if(sent==Sent::deferred)return true;
      buffered.pop_front();bufferedBytes-=size;published=true;
    }
    return true;
  }
  bool control(const Json& value){
    const auto type=value.get("type").string();
    if(type=="signin.worker.stop-v1"){
      fields(value,{"type","binding"});checkBinding(value.get("binding"),init);finish(Cause::stopped,Stage::cleanup);return false;
    }
    if(type=="signin.worker.start-v1"){
      operation=Stage::start;
      if(start)invalid();start=parseStart(value,init);const auto status=pipeline.open(*start);
      if(!status){finish(fromNative(status),Stage::start,status.error);return false;}opened=true;return true;
    }
    if(type=="signin.worker.publish-v1"){
      operation=Stage::publish;
      const auto granted=parseGrant(value,init,"signin.worker.publish-v1");
      if(!first||publishGranted||published||!same(granted,*first)||buffered.empty())invalid();publishGranted=true;return true;
    }
    if(type=="signin.worker.decoded-v1"){
      operation=Stage::decoded;
      const auto granted=parseGrant(value,init,"signin.worker.decoded-v1");
      if(!first||!published||decoded||!same(granted,*first))invalid();
      if(!budget()){finish(Cause::deadline,Stage::decoded);return false;}
      decoded=true;return true;
    }
    if(type=="signin.worker.input-v1"){
      operation=Stage::input;
      if(!start)invalid();const auto input=parseInput(value,init,*start);
      // An authenticated ordering barrier does not produce input or readiness.
      // Its original public scope was checked above; it never extends budget.
      if(input.get("type").string()=="input.fence")return true;
      if(!decoded)invalid();
      if(!delivery()){finish(Cause::refused,Stage::input);return false;}
      if(!pipeline.input(input,[&]{return delivery();})){finish(Cause::input,Stage::input);return false;}return true;
    }
    invalid();
  }
  bool frames(std::vector<Frame>& frames){
    if(frames.size()>32)invalid();
    for(auto& frame:frames){
      const auto source=submitted.find(frame.ptsUs);
      const auto inspected=gpu::inspectAnnexB(frame.bytes,encoderMaximumFrameBytes);
      if(source==submitted.end()||!inspected.valid||frame.ptsUs<=lastPts||frame.key!=inspected.idr)invalid();
      if(frame.key&&(!inspected.sps||!inspected.pps||inspected.spsProfile!=66))invalid();
      const auto sequence=source->second;submitted.erase(source);lastPts=frame.ptsUs;
      if(!first){
        if(!start||!frame.key)invalid();
        first=FirstFrame{start->transition,start->firstFrame,sequence,frame.ptsUs,payloadDigest(frame.bytes),init.target.width,init.target.height};
        const auto proof=prefix("signin.worker.frame-v1",init)+correlation(*first)+",\"width\":"+std::to_string(first->width)+
          ",\"height\":"+std::to_string(first->height)+",\"ownedOutputMask\":1,\"idr\":true,\"sps\":true,\"pps\":true,\"profile\":66}";
        if(!output.text(proof)){finish(Cause::transport,Stage::encode);return false;}
      }
      if(buffered.size()>=32||frame.bytes.size()>4*1024*1024-bufferedBytes){finish(Cause::queue,Stage::publish);return false;}
      bufferedBytes+=frame.bytes.size();buffered.push_back(std::move(frame));
    }
    return true;
  }
  Exit tick(){
    if(closed)return result;
    try{
      if(deferred.poisoned())return finish(Cause::queue,Stage::authority);
      if(!budget())return finish(Cause::deadline,first?Stage::decoded:Stage::start);
      const auto permit=pipeline.current();if(!permit)return finish(fromNative(permit),Stage::authority,permit.error);
      Json message;while(deferred.pop(message)){if(!control(message))return result;if(!budget())return finish(Cause::deadline,Stage::start);}
      if(!budget())return finish(Cause::deadline,first?Stage::decoded:Stage::start);
      if(!encodedWaiting.empty()){
        operation=Stage::encode;if(!frames(encodedWaiting))return result;encodedWaiting.clear();
      }
      operation=Stage::publish;if(!flush())return result;
      // A new control observed by a per-output authority query is dispatched
      // by the next outer loop. Do not capture/encode past a queued fence.
      if(!buffered.empty()||deferred.pendingFence())return result;
      if(!start||!opened||(!published&&first))return result;
      const auto now=time();if(now<nextCapture)return result;
      operation=Stage::capture;
      if(now>UINT64_MAX-1000000/start->fps)invalid();
      nextCapture=now+1000000/start->fps;
      native::BgraFrame picture;const auto captured=pipeline.capture(picture,10);
      if(captured.outcome==native::Outcome::noFrame)return result;
      if(!captured)return finish(fromNative(captured),Stage::capture,captured.error);
      if(!budget())return finish(Cause::deadline,Stage::capture);
      if(picture.width!=init.target.width||picture.height!=init.target.height||!picture.sequence||picture.sequence<=lastCapture||
         !native::validFrameShape(picture.width,picture.height,picture.width*4,picture.pixels.size()))invalid();
      lastCapture=picture.sequence;
      const auto fresh=pipeline.current();if(!fresh)return finish(fromNative(fresh),Stage::authority,fresh.error);
      if(deferred.pendingFence())return result;
      const auto at=time();if(at<begun||at-begun==UINT64_MAX||submitted.size()>=32)invalid();
      if(lastSubmitted==UINT64_MAX)invalid();
      const uint64_t pts=std::max(lastSubmitted+1,at-begun+1);if(!pts||!submitted.emplace(pts,picture.sequence).second)invalid();lastSubmitted=pts;
      operation=Stage::encode;
      if(!pipeline.encode(picture,pts,encodedWaiting))return finish(Cause::encoder,Stage::encode);
      size_t retained=0;
      if(encodedWaiting.size()>32)invalid();
      for(const auto& frame:encodedWaiting){if(frame.bytes.size()>4*1024*1024-retained)invalid();retained+=frame.bytes.size();}
      if(!budget())return finish(Cause::deadline,Stage::encode);
      const auto after=pipeline.current();if(!after)return finish(fromNative(after),Stage::authority,after.error);
      if(deferred.pendingFence())return result;
      if(!frames(encodedWaiting))return result;encodedWaiting.clear();
      operation=Stage::publish;if(!flush())return result;
    }catch(...){return finish(Cause::protocol,operation);}
    return result;
  }
};
Loop::Loop(channel::Init value,Deferred& deferred,Pipeline& pipeline,Output& output,Now now,uint64_t began)
 :impl_(std::make_unique<Impl>(std::move(value),deferred,pipeline,output,std::move(now),began)){}
Loop::~Loop(){if(impl_)(void)impl_->finish(Cause::stopped,Stage::cleanup);}
Exit Loop::tick(){return impl_->tick();}Exit Loop::stop(){return impl_->finish(Cause::stopped,Stage::cleanup);}
bool Loop::active()const{return impl_&&!impl_->closed&&impl_->decoded&&!impl_->deferred.pendingFence();}
std::optional<FirstFrame> Loop::firstFrame()const{return impl_->first;}

#ifndef MULTITAP_SIGNIN_ENTRY_CORE_ONLY
namespace {
// All of these real adapters are reachable only through run(), never through
// the injected Loop tests. No output/controller/profile is created here.
RECT exactBounds(const native::OwnedTarget& target){
  const auto sameAdapter=[&](LUID value){return value.LowPart==target.adapterLow&&value.HighPart==target.adapterHigh;};
  for(unsigned retry=0;retry<3;++retry){
    UINT32 paths=0,modes=0;const auto flags=QDC_ONLY_ACTIVE_PATHS|QDC_VIRTUAL_MODE_AWARE;
    if(GetDisplayConfigBufferSizes(flags,&paths,&modes)!=ERROR_SUCCESS||paths!=1||modes>256)return {};
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    const auto queried=QueryDisplayConfig(flags,&paths,p.data(),&modes,m.data(),nullptr);
    if(queried==ERROR_INSUFFICIENT_BUFFER)continue;
    if(queried!=ERROR_SUCCESS||paths!=1||!(p[0].flags&DISPLAYCONFIG_PATH_ACTIVE)||!p[0].targetInfo.targetAvailable||
       !sameAdapter(p[0].sourceInfo.adapterId)||!sameAdapter(p[0].targetInfo.adapterId)||p[0].sourceInfo.id!=target.sourceId||
       p[0].targetInfo.id!=target.targetId||p[0].targetInfo.rotation!=DISPLAYCONFIG_ROTATION_IDENTITY)return {};
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};name.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(name),p[0].sourceInfo.adapterId,p[0].sourceInfo.id};
    if(DisplayConfigGetDeviceInfo(&name.header)!=ERROR_SUCCESS||!name.viewGdiDeviceName[0])return {};
    struct Match{const native::OwnedTarget* target;const wchar_t* name;unsigned count=0;RECT bounds{};} match{&target,name.viewGdiDeviceName};
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM parameter)->BOOL{
      auto& found=*reinterpret_cast<Match*>(parameter);MONITORINFOEXW info{};info.cbSize=sizeof(info);
      if(!GetMonitorInfoW(monitor,reinterpret_cast<MONITORINFO*>(&info))||_wcsicmp(info.szDevice,found.name)!=0)return TRUE;
      const auto& t=*found.target;
      if(info.rcMonitor.left==t.left&&info.rcMonitor.top==t.top&&int64_t(info.rcMonitor.right)-t.left==t.width&&int64_t(info.rcMonitor.bottom)-t.top==t.height){++found.count;found.bounds=info.rcMonitor;}
      return TRUE;
    },reinterpret_cast<LPARAM>(&match));
    const auto virtualBounds=remotePointerDesktopBounds();
    if(match.count==1&&EqualRect(&match.bounds,&virtualBounds))return match.bounds;return {};
  }
  return {};
}
class NativePipeline final:public Pipeline {
 public:
  NativePipeline(const native::Permit& permit,native::OwnedTarget target):permit_(permit),target_(target),
    input_([this](INPUT& input)->UINT{return gate_&&(*gate_)()&&geometry()&&(!releaseOnly_||ownedReleaseShape(input))?SendInput(1,&input,sizeof(input)):0;},false,[this]{return exactBounds(target_);},{},[this](std::array<INPUT,2>& batch)->RemoteInput::EnterBatchResult{
      if(releaseOnly_||!gate_||!(*gate_)()||!geometry())return {};
      const auto accepted=SendInput(2,batch.data(),sizeof(INPUT));
      // Recheck the real Permit/native scope after the one atomic insertion.
      // A changed desktop closes this worker; never send a second UP there.
      const bool current=gate_&&(*gate_)()&&geometry();return {accepted,current};
    }){}
  native::Status current()override{return permit_.current();}
  native::Status open(const Start& start)override{
    try{
      if(!geometry())return {native::Outcome::refused,native::Step::output,0};
      runtime_=std::make_unique<Runtime>();capture_=std::make_unique<native::CaptureDxgi>(permit_,target_);
      nv12_.resize(size_t(target_.width)*target_.height*3/2);
      encoder_=std::make_unique<Encoder>(int(target_.width),int(target_.height),int(start.fps),int(start.bitrate),EncoderTuning{},EncoderBackend::automatic,
        (uint64_t(uint32_t(target_.adapterHigh))<<32)|target_.adapterLow);
      input_.area(exactBounds(target_));return {native::Outcome::ready,native::Step::none,0};
    }catch(const native::NativeFailure& failure){return failure.status;}catch(...){return {native::Outcome::failed,native::Step::binding,0};}
  }
  native::Status capture(native::BgraFrame& frame,uint32_t timeout)override{
    if(!capture_||!geometry())return {native::Outcome::handoffRequired,native::Step::output,0};
    const auto captured=capture_->next(frame,timeout);
    if(captured&&!geometry()){frame={};return {native::Outcome::handoffRequired,native::Step::output,0};}return captured;
  }
  bool encode(const native::BgraFrame& frame,uint64_t pts,std::vector<Frame>& frames)override{
    if(!encoder_||frame.width!=target_.width||frame.height!=target_.height)return false;
    try{bgraToNv12(frame.pixels.data(),nv12_.data(),int(frame.width),int(frame.height));encoder_->encode(nv12_.data(),pts,[&](Frame output){frames.push_back(std::move(output));});return true;}catch(...){return false;}
  }
  bool input(const Json& value,const Gate& gate)override{
    struct Clear{const Gate*& at;~Clear(){at=nullptr;}} clear{gate_};gate_=&gate;
    if(value.get("type").string()=="input.reset"){input_.reset();return input_.pending()==0;}
    return value.get("event").string()=="text"?input_.text(value):input_.apply(value);
  }
  size_t release(const Gate&)override{
    Gate releaseGate=[this]{return bool(permit_.currentForRelease());};
    struct Clear{NativePipeline& self;~Clear(){self.gate_=nullptr;self.releaseOnly_=false;}} clear{*this};
    releaseOnly_=true;gate_=&releaseGate;
    // reset() supplies only records this exact worker injected successfully.
    // The independent release Permit still requires the SAME Winlogon desktop.
    input_.reset();return input_.pending();
  }
  void close()noexcept override{
    gate_=nullptr;
    if(encoder_)try{encoder_->finish([](Frame){});}catch(...){}
    encoder_.reset();capture_.reset();nv12_.clear();runtime_.reset();
  }
 private:
  bool geometry(){const auto bounds=exactBounds(target_);return int64_t(bounds.right)-bounds.left==target_.width&&int64_t(bounds.bottom)-bounds.top==target_.height;}
  const native::Permit& permit_;native::OwnedTarget target_;const Gate* gate_=nullptr;bool releaseOnly_=false;RemoteInput input_;
  std::unique_ptr<Runtime> runtime_;std::unique_ptr<native::CaptureDxgi> capture_;std::unique_ptr<Encoder> encoder_;std::vector<uint8_t> nv12_;
};
class PipeOutput final:public Output {
 public:
  PipeOutput(HANDLE pipe,const std::atomic<bool>& stopped,std::mutex& writing):pipe_(pipe),stopped_(stopped),writing_(writing){}
  bool text(const std::string& value)override{return pipePacket(pipe_,true,{value.begin(),value.end()},stopped_,writing_);}
  bool media(Frame frame,uint16_t epoch)override{
    auto bytes=mediaPacket(1,frame.ptsUs,frame.bytes,frame.key);bytes[2]=uint8_t(epoch);bytes[3]=uint8_t(epoch>>8);
    return pipePacket(pipe_,false,std::move(bytes),stopped_,writing_);
  }
 private:HANDLE pipe_;const std::atomic<bool>& stopped_;std::mutex& writing_;
};
}
Exit run(HANDLE pipe,uint32_t expectedServicePid,const std::atomic<bool>& stopped){
  const auto begun=nowUs();
  try{
    ULONG peer=0;if(!GetNamedPipeServerProcessId(pipe,&peer)||peer!=expectedServicePid||!peer)return {Cause::refused,Stage::bootstrap,0,0};
    const std::atomic<int64_t> deadline{streamMonotonicMillis()+15000};
    bool text=false;std::vector<uint8_t> bytes;
    if(!readPipePacket(pipe,text,bytes,stopped,std::chrono::milliseconds(15000),&deadline)||!text||bytes.size()>8192)return {Cause::transport,Stage::bootstrap,0,0};
    const auto init=channel::parseInit(JsonParser::parse(std::string(bytes.begin(),bytes.end())));
    auto service=channel::acceptInitService(init,pipe,expectedServicePid);
    Deferred deferred;std::mutex writing;
    channel::WorkerLease authority(pipe,init.binding,init.target,stopped,writing,[&](const Json& value){return deferred.push(value);});
    native::Status status;auto permit=native::authorizeWorker(init.binding,init.context,service.get(),pipe,{&init.target,1},authority,status);
    if(!permit)return {fromNative(status),Stage::authority,status.error,0};
    NativePipeline pipeline(*permit,init.target);PipeOutput output(pipe,stopped,writing);
    Loop loop(init,deferred,pipeline,output,nowUs,begun);
    const auto finish=[&](Exit result){(void)output.text(cleanupPacket(init,result));return result;};
    for(;;){if(stopped)return finish(loop.stop());const auto result=loop.tick();if(result.cause!=Cause::running)return finish(result);Sleep(1);}
  }catch(...){return {Cause::protocol,Stage::bootstrap,0,0};}
}
#endif
}
