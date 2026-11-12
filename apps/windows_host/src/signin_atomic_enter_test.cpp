#include "agent.hpp"
#include "signin_worker_entry.hpp"
#include "signin_bridge_policy.hpp"
#include <iostream>

using namespace host;
using namespace host::signin;
using namespace host::signin::entry;
using host::signin::broker::Admission;
using host::signin::broker::Observation;
using host::signin::broker::PhaseId;
using host::signin::broker::PhaseFence;
namespace {
unsigned checks=0;
void expect(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
Json key(bool down,unsigned vk=VK_RETURN,unsigned scan=0x1c){return JsonParser::parse("{\"event\":\"key\",\"vk\":"+std::to_string(vk)+",\"scan\":"+std::to_string(scan)+",\"extended\":false,\"down\":"+(down?"true":"false")+'}');}
RECT bounds(){return {0,0,16,16};}
channel::Init init(){
  channel::Init out;auto& c=out.context;c.stream[0]=1;c.service={100,1000};c.serviceIsLocalSystem=true;c.lease={"ROOT\\DISPLAY\\0000",4};c.leaseOwned=true;
  c.baseline[0]=2;c.nonce[0]=3;c.consoleSession=1;c.workerGeneration=7;c.mediaEpoch=5;c.ownedOutputMask=1;c.configuredSid="S-1-5-21-1-2-3-1001";
  c.assigned=c.clientOptIn=c.clientTransitions=c.signinRuntimeQualified=true;out.binding={c.stream,c.service,{200,2000},c.lease,c.baseline,c.nonce,c.consoleSession,c.workerGeneration,c.mediaEpoch,Role::signin};out.target={1,-7,2,3,16,16,0,0,1};out.serviceQuery=123;return out;
}
PhaseId id(uint8_t n){PhaseId value{};value[0]=n;return value;}
PhaseFence retiring(){
  Admission a;a.stream[0]=1;a.service={100,1000};a.transition=id(1);a.workerGeneration=7;a.mediaEpoch=5;
  Observation before{{100,1000},1,1,1000,WindowsState::locked,"S-1-5-21-1-2-3-1001",true,true,true};
  PhaseFence f;expect(f.begin(a,before)&&f.worker({200,2000})&&f.firstFrame(1,id(2))&&f.decoded(7,5,id(1),id(2)),"actual active secure phase");
  auto after=before;after.windows=WindowsState::unlocked;++after.generation;
  expect(f.transition(Role::user,after,id(3),1000),"native observed unlock advances phase");
  expect(f.exited({200,2000},true)&&f.restored(true),"held old exit and full original restoration are separate facts");return f;
}
void acceptedWholeTap(){
  unsigned batches=0,singles=0;RemoteInput input([&](INPUT&){++singles;return 1u;},false,bounds,{},[&](std::array<INPUT,2>& events){
    ++batches;expect(events[0].type==INPUT_KEYBOARD&&events[1].type==INPUT_KEYBOARD,"keyboard batch only");
    expect(events[0].ki.wScan==0x1c&&events[1].ki.wScan==0x1c,"same Enter scan");expect(!(events[0].ki.dwFlags&KEYEVENTF_KEYUP)&&(events[1].ki.dwFlags&KEYEVENTF_KEYUP),"DOWN then UP in one batch");return RemoteInput::EnterBatchResult{2,true};});
  expect(input.apply(key(true))&&input.pending()==0,"exact accepted two clears owned held state");
  expect(input.apply(key(true))&&batches==1,"repeat down before physical up cannot submit password twice");
  expect(input.apply(key(false))&&singles==0,"later physical UP is consumed without native injection");
  expect(input.apply(key(true))&&batches==2,"next deliberate Enter is a new tap");
}
void unlockDuringWholeTap(){
  bool current=true;unsigned acceptedSingles=0,batches=0;RemoteInput input([&](INPUT&){if(current){++acceptedSingles;return 1u;}return 0u;},false,bounds,{},[&](std::array<INPUT,2>&){++batches;current=false;return RemoteInput::EnterBatchResult{2,false};});
  expect(!input.apply(key(true))&&input.pending()==0,"post-batch scope change closes input but both accepted events leave no held key");input.reset();expect(acceptedSingles==0&&batches==1,"no later UP is injected into new desktop");
  const auto binding=init();const auto receipt=JsonParser::parse(cleanupPacket(binding,{Cause::handoff,Stage::cleanup,0,input.pending()}));expect(cleanupComplete(receipt,binding),"exact post-reset receipt independently proves no held records");
  auto f=retiring();expect(!f.successor(),"exit code and original alone do not open successor");expect(f.released({200,2000},cleanupComplete(receipt,binding))&&f.successor(),"actual receipt plus exit plus restore permits fresh worker");
  expect(!f.decoded(7,5,id(1),id(2)),"late old decoded ACK stays retired");expect(f.worker({300,3000})&&f.firstFrame(2,id(4)),"new desktop worker/frame still required");
  Observation after{{100,1000},1,2,1000,WindowsState::unlocked,"S-1-5-21-1-2-3-1001",true,true,true};
  expect(!f.input(7,5,id(1),after,1000),"old Enter UP cannot enter new binding");expect(!f.input(8,6,id(3),after,1000),"new input awaits actual decode ACK");expect(f.decoded(8,6,id(3),id(4))&&f.input(8,6,id(3),after,1000),"only fresh acknowledged desktop binding opens input");
}
void partialTapStaysPending(){
  bool current=true;unsigned acceptedSingles=0;RemoteInput input([&](INPUT&){if(current){++acceptedSingles;return 1u;}return 0u;},false,bounds,{},[&](std::array<INPUT,2>&){current=false;return RemoteInput::EnterBatchResult{1,false};});
  expect(!input.apply(key(true))&&input.pending()==1,"accepted one records exactly the owned Enter DOWN");input.reset();expect(input.pending()==1&&acceptedSingles==0,"changed desktop cannot receive cleanup UP");
  const auto binding=init();const auto receipt=JsonParser::parse(cleanupPacket(binding,{Cause::handoff,Stage::cleanup,0,input.pending()}));expect(!cleanupComplete(receipt,binding),"pending count cannot claim reset complete");auto f=retiring();expect(!f.released({200,2000},cleanupComplete(receipt,binding))&&!f.successor(),"pending Enter blocks successor readiness after exit/restore");expect(!f.decoded(7,5,id(1),id(2)),"late ACK cannot cancel pending cleanup");
}
void zeroAndLegacy(){
  unsigned batches=0,singles=0;RemoteInput zero([&](INPUT&){++singles;return 1u;},false,bounds,{},[&](std::array<INPUT,2>&){++batches;return RemoteInput::EnterBatchResult{0,true};});expect(!zero.apply(key(true))&&zero.pending()==0&&singles==0&&batches==1,"zero accepted cannot invent a held key or replay DOWN");
  RemoteInput ordinary([&](INPUT&){++singles;return 1u;},false,bounds,{});expect(ordinary.apply(key(true))&&ordinary.pending()==1,"ordinary desktop still holds Enter");expect(ordinary.apply(key(true))&&ordinary.pending()==1,"ordinary desktop repeat stays unchanged");expect(ordinary.apply(key(false))&&ordinary.pending()==0&&singles==3,"ordinary desktop retains original three single injections");
  batches=0;RemoteInput modified([](INPUT&){return 1u;},false,bounds,{},[&](std::array<INPUT,2>&){++batches;return RemoteInput::EnterBatchResult{2,true};});expect(modified.apply(key(true,VK_CONTROL,0x1d))&&modified.apply(key(true)),"modifier Enter retains normal hold semantics");expect(batches==0&&modified.pending()==2,"modified Enter cannot enter signin atomic path");modified.reset();
}
void strictReceipt(){
  const auto binding=init();auto value=JsonParser::parse(cleanupPacket(binding,{Cause::stopped,Stage::cleanup,0,0}));expect(cleanupComplete(value,binding),"clean exact receipt");
  auto reject=[&](Json bad){try{(void)cleanupComplete(bad,binding);return false;}catch(...){return true;}};
  auto wrong=binding;++wrong.binding.worker.created;expect(reject(JsonParser::parse(cleanupPacket(wrong,{Cause::stopped,Stage::cleanup,0,0}))),"wrong worker birth cannot attest cleanup");
  std::get<Json::Object>(value.value)["extra"].value=true;expect(reject(value),"extra peer fields refused");value=JsonParser::parse(cleanupPacket(binding,{Cause::stopped,Stage::cleanup,0,1}));std::get<Json::Object>(std::get<Json::Object>(value.value)["release"].value)["state"].value=std::string("complete");expect(reject(value),"pending count cannot be relabeled complete");
  expect(!cleanupComplete(JsonParser::parse(cleanupPacket(binding,{Cause::protocol,Stage::cleanup,0,SIZE_MAX})),binding),"unknown count remains pending");
}
}
int main(){try{acceptedWholeTap();unlockDuringWholeTap();partialTapStaysPending();zeroAndLegacy();strictReceipt();std::cout<<"{\"kind\":\"signin-atomic-enter-and-cleanup-pure-tests\",\"checks\":"<<checks<<",\"nativeAdaptersInvoked\":false,\"SendInputCalls\":0,\"runtimeQualified\":false}\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
