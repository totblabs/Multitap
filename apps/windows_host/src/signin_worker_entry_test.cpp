#include "signin_worker_entry.hpp"
#include <iostream>

using namespace host;
using namespace host::signin;
using namespace host::signin::entry;
namespace {
unsigned checks=0;
void expect(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
channel::Init init(){
  channel::Init out;auto& c=out.context;
  c.stream[0]=1;c.service={100,1000};c.serviceIsLocalSystem=true;c.lease={"ROOT\\DISPLAY\\0000",4};c.leaseOwned=true;
  c.baseline[0]=2;c.nonce[0]=3;c.consoleSession=1;c.workerGeneration=7;c.mediaEpoch=5;c.ownedOutputMask=1;
  c.configuredSid="S-1-5-21-1-2-3-1001";c.assigned=c.clientOptIn=c.clientTransitions=c.signinRuntimeQualified=true;
  out.binding={c.stream,c.service,{200,2000},c.lease,c.baseline,c.nonce,c.consoleSession,c.workerGeneration,c.mediaEpoch,Role::signin};
  out.target={1,-7,2,3,16,16,0,0,1};out.serviceQuery=123;return out;
}
Start start(){Start out;out.transition[0]=11;out.firstFrame[0]=12;out.scopeGeneration=2;out.fps=60;out.bitrate=8000000;return out;}
std::vector<uint8_t> idr(){return {0,0,0,1,0x67,66,0,31,0x80,0,0,0,1,0x68,0x80,0,0,0,1,0x65,0x88};}
Json keyPhase(){
  // Deliberately independent public wire fixture, not generated from Init by
  // a restamping helper. The child must compare every original field.
  return JsonParser::parse(R"({"type":"input.phase","protocol":"signin-stream-v1","sessionId":"01000000-0000-0000-0000-000000000000","transitionId":"0b000000000000000000000000000000","workerGeneration":7,"mediaScope":{"kind":"composite","epoch":5},"input":{"type":"input","event":"key","vk":65,"scan":30,"down":true,"extended":false}})");
}
struct FakePipeline final:Pipeline {
  native::Status status{native::Outcome::ready,native::Step::none,0};
  unsigned queries=0,opens=0,captures=0,encodes=0,inputs=0,delivered=0,releases=0,closes=0;
  uint64_t sequence=0;bool noFrame=false,wrongPts=false,headersMissing=false,waitOutput=false,inputFails=false;
  size_t held=0;std::function<void()> duringQuery,duringOpen,duringEncode;std::vector<uint64_t> delayed;
  native::Status current()override{++queries;if(duringQuery)duringQuery();return status;}
  native::Status open(const Start&)override{++opens;if(duringOpen)duringOpen();return status;}
  native::Status capture(native::BgraFrame& frame,uint32_t timeout)override{
    ++captures;expect(timeout<=50,"bounded synthetic capture wait");if(noFrame)return {native::Outcome::noFrame,native::Step::acquire,0};
    frame={16,16,++sequence,std::vector<uint8_t>(16*16*4,73)};return status;
  }
  bool encode(const native::BgraFrame& frame,uint64_t pts,std::vector<Frame>& out)override{
    ++encodes;expect(frame.sequence==sequence&&frame.pixels[0]==73,"actual fake capture reaches encoder seam");
    if(duringEncode)duringEncode();if(waitOutput){delayed.push_back(pts);return true;}
    for(const auto previous:delayed)out.push_back({idr(),previous,true});delayed.clear();
    out.push_back({headersMissing?std::vector<uint8_t>{0,0,0,1,0x65,0x88}:idr(),pts+(wrongPts?1:0),true});return true;
  }
  bool input(const Json&,const Gate& gate)override{
    ++inputs;if(!gate()||inputFails)return false;++delivered;held=1;return true;
  }
  size_t release(const Gate& gate)override{++releases;if(held&&gate())held=0;return held;}
  void close()noexcept override{++closes;}
};
struct Sink final:Output {
  std::vector<Json> controls;std::vector<Frame> mediaFrames;std::vector<uint16_t> epochs;bool failMedia=false;
  std::function<void()> onMedia;
  bool text(const std::string& packet)override{controls.push_back(JsonParser::parse(packet));return true;}
  bool media(Frame frame,uint16_t epoch)override{if(failMedia)return false;mediaFrames.push_back(std::move(frame));epochs.push_back(epoch);if(onMedia)onMedia();return true;}
};
struct Harness {
  channel::Init binding=init();Start options=start();Deferred deferred;FakePipeline pipeline;Sink sink;uint64_t now=1000000;
  Loop loop{binding,deferred,pipeline,sink,[this]{return now;},now};
  void push(const std::string& packet){expect(deferred.push(JsonParser::parse(packet)),"bounded control admitted into queue");}
  FirstFrame frame(){
    push(startPacket(binding,options));expect(loop.tick().cause==Cause::running,"valid injected start runs");
    expect(sink.mediaFrames.empty()&&!loop.active(),"native initialization/first output does not publish or enable input");
    expect(sink.controls.size()==1,"exactly one first-frame proof before media");
    return parseFirstFrame(sink.controls[0],binding);
  }
  FirstFrame publish(){auto proof=frame();push(grantPacket(binding,proof,Grant::publish));expect(loop.tick().cause==Cause::running,"exact parent publish grant");return proof;}
  FirstFrame activate(){auto proof=publish();expect(sink.mediaFrames.size()==1&&!loop.active(),"first actual media still has no decoder authority");push(grantPacket(binding,proof,Grant::decoded));expect(loop.tick().cause==Cause::running&&loop.active(),"exact injected decoder grant activates input gate");return proof;}
};
void realCorePositive(){
  Harness h;auto proof=h.activate();
  expect(proof.sequence==1&&proof.ptsUs==1&&proof.width==16&&proof.height==16,"proof binds actual capture sequence/PTS/geometry");
  expect(h.sink.controls[0].get("payloadSHA256").string()=="ba638fca98106cc1a1c6a87ea9cc3bf92dfd931762fb2540619bc1e213bb78e6","proof SHA matches independent fixed-vector digest");
  expect(h.sink.mediaFrames[0].ptsUs==proof.ptsUs&&h.sink.epochs[0]==5,"published output uses unchanged bound epoch and exact PTS");
  h.push(inputPacket(h.binding,keyPhase()));expect(h.loop.tick().cause==Cause::running&&h.pipeline.delivered==1,"current original client phase reaches injected delivery");
  h.push(inputPacket(h.binding,keyPhase()));expect(h.loop.tick().cause==Cause::running&&h.pipeline.delivered==2,"intentional same-phase repeated key is not content/time deduplicated");
  h.push(stopPacket(h.binding));const auto stopped=h.loop.tick();expect(stopped.cause==Cause::stopped&&stopped.releasePending==0&&h.pipeline.closes==1,"stop fences then releases only own synthetic held input");
}
void admissionNegatives(){
  for(unsigned variant=0;variant<10;++variant){
    Harness h;h.activate();auto phase=keyPhase();auto& value=std::get<Json::Object>(phase.value);
    switch(variant){
      case 0:value["workerGeneration"].value=6.0;break;
      case 1:std::get<Json::Object>(value["mediaScope"].value)["epoch"].value=4.0;break;
      case 2:value["transitionId"].value=std::string(32,'1');break;
      case 3:value["sessionId"].value=std::string("02000000-0000-0000-0000-000000000000");break;
      case 4:value["protocol"].value=std::string("legacy");break;
      case 5:std::get<Json::Object>(value["mediaScope"].value)["kind"].value=std::string("track");break;
      case 6:value["input"]=JsonParser::parse(R"({"type":"input","event":"moveRelative","dx":1,"dy":2})");break;
      case 7:value["input"]=JsonParser::parse(R"({"type":"usb.detach","port":1})");break;
      case 8:value["input"]=JsonParser::parse(R"({"type":"input","event":"sas"})");break;
      default:std::get<Json::Object>(value["input"].value)["sample"].value=1.0;break;
    }
    h.push(inputPacket(h.binding,phase));expect(h.loop.tick().cause==Cause::protocol&&h.pipeline.inputs==0,"stale/unsupported public scope cannot reach ordinary input adapter");
  }
  Harness raw;raw.activate();raw.push(R"({"type":"input","event":"key","vk":65,"scan":30,"down":true,"extended":false})");
  expect(raw.loop.tick().cause==Cause::protocol&&raw.pipeline.inputs==0,"raw input is never restamped into current phase");
}
void firstFrameFences(){
  for(unsigned variant=0;variant<4;++variant){
    Harness h;auto proof=h.frame();auto wrong=proof;
    if(variant==0)++wrong.sequence;else if(variant==1)++wrong.ptsUs;else if(variant==2)wrong.payloadSha256[0]^=1;else wrong.firstFrame[0]^=1;
    h.push(grantPacket(h.binding,wrong,Grant::publish));expect(h.loop.tick().cause==Cause::protocol&&h.sink.mediaFrames.empty(),"publish requires exact captured output correlation");
  }
  Harness early;auto proof=early.frame();early.push(grantPacket(early.binding,proof,Grant::decoded));expect(early.loop.tick().cause==Cause::protocol&&early.sink.mediaFrames.empty(),"decoder claim before first media cannot activate");
  Harness inQuery;proof=inQuery.frame();inQuery.push(grantPacket(inQuery.binding,proof,Grant::publish));
  const auto at=inQuery.pipeline.queries;
  inQuery.pipeline.duringQuery=[&]{if(inQuery.pipeline.queries==at+2)inQuery.push(grantPacket(inQuery.binding,proof,Grant::decoded));};
  expect(inQuery.loop.tick().cause==Cause::running&&inQuery.sink.mediaFrames.empty(),"nested query only queues early ACK without publishing");
  expect(inQuery.loop.tick().cause==Cause::protocol&&!inQuery.loop.active(),"queued early ACK remains invalid on next main-loop dispatch");
  for(unsigned variant=0;variant<2;++variant){
    Harness h;h.pipeline.wrongPts=variant==0;h.pipeline.headersMissing=variant==1;
    h.push(startPacket(h.binding,h.options));expect(h.loop.tick().cause==Cause::protocol&&h.sink.controls.empty()&&h.sink.mediaFrames.empty(),"unsubmitted PTS or headerless output cannot create readiness proof");
  }
}
void lifetimeAndDefer(){
  Harness silent;silent.pipeline.noFrame=true;silent.push(startPacket(silent.binding,silent.options));expect(silent.loop.tick().cause==Cause::running,"no synthetic frame is not ready");
  silent.now+=15000000;expect(silent.loop.tick().cause==Cause::deadline&&silent.sink.controls.empty(),"absolute initial deadline is never extended by no-frame polls");
  Harness slow;slow.pipeline.duringOpen=[&]{slow.now+=15000000;};slow.push(startPacket(slow.binding,slow.options));expect(slow.loop.tick().cause==Cause::deadline&&slow.pipeline.captures==0,"blocking open cannot manufacture a fresh operation budget");
  Harness revoked;revoked.activate();revoked.pipeline.held=1;revoked.pipeline.status={native::Outcome::handoffRequired,native::Step::desktop,5};
  const auto end=revoked.loop.tick();expect(end.cause==Cause::handoff&&end.releasePending==1&&revoked.pipeline.closes==1,"changed desktop cannot receive releases or claim cleanup success");
  Harness stop;stop.activate();stop.push(inputPacket(stop.binding,keyPhase()));const auto before=stop.pipeline.queries;
  stop.pipeline.duringQuery=[&]{if(stop.pipeline.queries==before+2)stop.push(stopPacket(stop.binding));};
  expect(stop.loop.tick().cause==Cause::refused&&stop.pipeline.inputs==0,"stop deferred during per-input lease query fences press without recursive dispatch");
  Harness failed;failed.activate();failed.pipeline.inputFails=true;failed.push(inputPacket(failed.binding,keyPhase()));expect(failed.loop.tick().cause==Cause::input&&failed.pipeline.delivered==0,"failed delivery is fixed failure without press retry");
  Harness transport;auto proof=transport.frame();transport.sink.failMedia=true;transport.push(grantPacket(transport.binding,proof,Grant::publish));expect(transport.loop.tick().cause==Cause::transport&&!transport.loop.active(),"failed first media cannot activate decoder/input state");
  Deferred queue;const auto control=JsonParser::parse(stopPacket(init()));for(unsigned i=0;i<32;++i)expect(queue.push(control),"bounded queue fills");expect(!queue.push(control)&&queue.poisoned(),"queue overflow refuses rather than dropping a fence");
}
FirstFrame batch(Harness& h,bool initial){
  if(initial){h.push(startPacket(h.binding,h.options));}
  h.pipeline.waitOutput=true;h.now+=20000;expect(h.loop.tick().cause==Cause::running,"first fake batch input retained by encoder");
  h.pipeline.waitOutput=false;h.now+=20000;expect(h.loop.tick().cause==Cause::running,"actual two-output fake batch completes");
  if(initial){expect(h.sink.controls.size()==1&&h.sink.mediaFrames.empty(),"two real core outputs remain buffered before publish grant");return parseFirstFrame(h.sink.controls[0],h.binding);}
  return *h.loop.firstFrame();
}
void perFrameRevocation(){
  for(const bool initial:{true,false})for(const bool revoke:{true,false}){
    Harness h;FirstFrame proof;
    if(initial){proof=batch(h,true);}else{proof=h.activate();}
    const auto before=h.sink.mediaFrames.size();
    h.sink.onMedia=[&]{
      if(h.sink.mediaFrames.size()!=before+1)return;
      if(revoke)h.pipeline.status={native::Outcome::refused,native::Step::lease,0};
      else h.push(stopPacket(h.binding));
    };
    Exit outcome;
    if(initial){h.push(grantPacket(h.binding,proof,Grant::publish));outcome=h.loop.tick();}
    else{
      h.pipeline.waitOutput=true;h.now+=20000;expect(h.loop.tick().cause==Cause::running,"active encoder holds first batch input");
      h.pipeline.waitOutput=false;h.now+=20000;outcome=h.loop.tick();
    }
    expect(h.sink.mediaFrames.size()==before+1,"revoke/queued stop after first output prevents every later flush/batch output");
    if(revoke)expect(outcome.cause==Cause::refused&&!h.loop.active(),"per-frame native authority failure retires immediately");
    else{
      expect(!h.loop.active(),"pending stop fences input before outer dispatch");
      expect(h.loop.tick().cause==Cause::stopped&&h.sink.mediaFrames.size()==before+1,"queued stop is processed without stale media or recursive RPC");
    }
  }
  Harness budget;const auto proof=batch(budget,true);
  budget.sink.onMedia=[&]{if(budget.sink.mediaFrames.size()==1)budget.now+=15000000;};
  budget.push(grantPacket(budget.binding,proof,Grant::publish));
  expect(budget.loop.tick().cause==Cause::deadline&&budget.sink.mediaFrames.size()==1,"absolute decoder budget is rechecked before every buffered output");
}
void scopedFence(){
  for(unsigned variant=0;variant<4;++variant){
    Harness h;h.activate();auto phase=keyPhase();auto& fields=std::get<Json::Object>(phase.value);
    fields["input"]=JsonParser::parse(R"({"type":"input.fence"})");
    if(variant==1)std::get<Json::Object>(fields["input"].value)["sessionId"].value=std::string("01000000-0000-0000-0000-000000000000");
    if(variant==2)std::get<Json::Object>(fields["input"].value)["sequence"].value=1.0;
    if(variant==3)std::get<Json::Object>(fields["mediaScope"].value)["epoch"].value=4.0;
    h.push(inputPacket(h.binding,phase));const auto result=h.loop.tick();
    expect(h.pipeline.inputs==0&&h.pipeline.delivered==0,"scoped ordering fence never calls input adapter");
    expect(variant<2?result.cause==Cause::running&&h.loop.active():result.cause==Cause::protocol,"only exact authenticated fence shape/scope is a no-op");
  }
  Harness early;early.publish();auto phase=keyPhase();std::get<Json::Object>(phase.value)["input"]=JsonParser::parse(R"({"type":"input.fence"})");
  early.push(inputPacket(early.binding,phase));expect(early.loop.tick().cause==Cause::running&&!early.loop.active()&&early.pipeline.inputs==0,"fence cannot manufacture decoder readiness");
  early.now+=15000000;expect(early.loop.tick().cause==Cause::deadline,"ordering fence cannot extend initial deadline");
}
}
int main(){
  try{realCorePositive();admissionNegatives();firstFrameFences();lifetimeAndDefer();perFrameRevocation();scopedFence();
    std::cout<<"{\"kind\":\"signin-entry-injected-core-tests\",\"checks\":"<<checks<<",\"nativeAdaptersInvoked\":false,\"actualSystemCaptureInputEncoderRuns\":0,\"runtimeQualified\":false}\n";return 0;
  }catch(const std::exception& failure){std::cerr<<failure.what()<<'\n';return 1;}
}
