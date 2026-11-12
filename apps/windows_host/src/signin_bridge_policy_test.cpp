#include "signin_bridge_policy.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <future>
#include <chrono>
#include <atomic>
using namespace host::signin;
using namespace host::signin::broker;
namespace {
std::atomic<unsigned> checks=0;
void expect(bool value,const char* label){++checks;if(!value)throw std::runtime_error(label);}
PhaseId phaseId(uint8_t n){PhaseId value{};value[0]=n;return value;}
Admission admission(){Admission a;a.stream[0]=1;a.transition=phaseId(2);a.service={100,101};a.workerGeneration=7;a.mediaEpoch=3;a.grantRevision=4;a.eligibilityRevision=9;a.receivedAt=1000;a.validForMs=1000;a.serviceInstance="0123456789abcdef0123456789abcdef";return a;}
Observation observation(){return {{100,101},3,10,1000,WindowsState::noUser,"",true,true,true};}
PhaseFence active(){PhaseFence f;expect(f.begin(admission(),observation()),"begin");expect(f.worker({200,201}),"actual child");expect(f.firstFrame(1,phaseId(3)),"frame");expect(f.decoded(7,3,phaseId(2),phaseId(3)),"decoded");return f;}
void defaultOffAndAdmission(){
  const auto a=admission();auto o=observation();expect(!admissionCurrent(a,{},true,a.serviceInstance,9,o,1000),"default OFF");
  expect(!admissionCurrent(a,{true},false,a.serviceInstance,9,o,1000),"selection cannot forge qualification");
  expect(admissionCurrent(a,{true},true,a.serviceInstance,9,o,1000),"current authenticated service scope");
  expect(!admissionCurrent(a,{true},true,a.serviceInstance,10,o,1000),"old eligibility");
  expect(!admissionCurrent(a,{true},true,"different",9,o,1000),"different service owner");
  expect(!admissionCurrent(a,{true},true,a.serviceInstance,9,o,2000),"expired one-shot grant");
  for(unsigned field=0;field<7;field++){auto bad=o;switch(field){case 0:bad.system=false;break;case 1:bad.console=0;break;case 2:bad.consoleProtocol=false;break;case 3:bad.ownedSingleOutput=false;break;case 4:bad.windows=WindowsState::unlocked;break;case 5:bad.sid="S-1-5-18";break;default:bad.at=1;break;}expect(!admissionCurrent(a,{true},true,a.serviceInstance,9,bad,1500),"bad native fact refused before mutation");}
}
void frameAndAck(){PhaseFence f;expect(f.begin(admission(),observation()),"begin");expect(f.worker({200,201}),"own worker");expect(!f.input(7,3,phaseId(2),observation(),1000),"no input before first frame");expect(f.firstFrame(1,phaseId(3)),"frame");expect(!f.decoded(6,3,phaseId(2),phaseId(3)),"stale generation ACK");expect(!f.decoded(7,2,phaseId(2),phaseId(3)),"stale epoch ACK");expect(!f.decoded(7,3,phaseId(2),phaseId(4)),"other frame ACK");expect(!f.input(7,3,phaseId(2),observation(),1000),"input still closed");expect(f.decoded(7,3,phaseId(2),phaseId(3)),"exact decoded first frame");expect(f.input(7,3,phaseId(2),observation(),1000),"owned input after ACK");auto stale=observation();stale.generation++;expect(!f.input(7,3,phaseId(2),stale,1000),"changed desktop observation blocks input");f.revoke();expect(!f.input(7,3,phaseId(2),observation(),1000),"capture/auth failure immediately retires input");}
void transitionAndExactExit(){auto f=active();auto desktop=observation();desktop.generation++;desktop.windows=WindowsState::unlocked;desktop.sid="S-1-5-21-1001";expect(f.transition(Role::user,desktop,phaseId(5),1000),"native actual user transition without saved password");expect(!f.input(7,3,phaseId(2),desktop,1000),"old input retired at prepare");expect(!f.successor(),"no successor on pipe/ACK only");expect(!f.exited({200,202},true),"PID reuse is not old exit");expect(!f.exited({200,201},false),"unknown is not exit");expect(f.exited({200,201},true),"held old child exited");expect(!f.successor(),"exit alone cannot drop original layout");expect(!f.restored(false),"failed restoration stays pending");expect(f.restored(true),"full original verified");expect(!f.successor(),"exit and layout cannot substitute for owned input cleanup");expect(f.released({200,201},true),"exact private owned input cleanup");expect(f.successor(),"fresh worker only after all three facts");expect(f.admission().workerGeneration==8&&f.admission().mediaEpoch==4,"generation and epoch advanced");expect(f.worker({300,301}),"new ordinary worker");expect(f.firstFrame(2,phaseId(6)),"new actual user frame");expect(!f.input(8,4,phaseId(5),desktop,1000),"successor input awaits decode");expect(f.decoded(8,4,phaseId(5),phaseId(6)),"new decode ACK");expect(f.input(8,4,phaseId(5),desktop,1000),"fresh user scope input");auto locked=desktop;locked.generation++;locked.windows=WindowsState::locked;expect(f.transition(Role::signin,locked,phaseId(7),1000),"same-session relock requires another fenced phase");expect(!f.input(8,4,phaseId(5),locked,1000),"desktop input cannot leak to relock");}
void accountAndConsoleFences(){auto f=active();auto next=observation();next.windows=WindowsState::unlocked;next.generation++;next.sid="S-1-5-21-1001";auto other=next;other.console++;expect(!f.transition(Role::user,other,phaseId(5),1000),"other console rejected");expect(!f.transition(Role::user,next,phaseId(2),1000),"transition nonce reuse rejected");expect(f.transition(Role::user,next,phaseId(5),1000),"first observed authenticated local user");expect(f.exited({200,201},true)&&f.released({200,201},true)&&f.restored(true)&&f.successor(),"cleanup");expect(f.worker({300,301})&&f.firstFrame(2,phaseId(6))&&f.decoded(8,4,phaseId(5),phaseId(6)),"user frame");auto bad=next;bad.sid="S-1-5-21-9999";expect(!f.input(8,4,phaseId(5),bad,1000),"different signed-in user rejected");bad.windows=WindowsState::locked;bad.generation++;expect(!f.transition(Role::signin,bad,phaseId(7),1000),"different account lock cannot gain secure scope");}
void heldEnterStaysPending(){
  auto f=active();auto desktop=observation();desktop.generation++;desktop.windows=WindowsState::unlocked;desktop.sid="S-1-5-21-1001";
  expect(f.transition(Role::user,desktop,phaseId(5),1000),"Enter DOWN may cause native unlock");
  expect(f.exited({200,201},true)&&f.restored(true),"old child exit and original are independently verified");
  expect(!f.released({200,201},false),"pending held Enter is not a release receipt");
  expect(!f.released({200,202},true),"reused PID cannot certify reset");
  expect(!f.successor(),"unknown or pending Enter release forbids new desktop readiness");
  expect(!f.decoded(7,3,phaseId(2),phaseId(3)),"late secure ACK cannot reactivate retired phase");
  expect(!f.input(7,3,phaseId(2),desktop,1000),"late old physical Enter UP is not replayed to desktop");
  expect(!f.input(8,4,phaseId(5),desktop,1000),"fresh binding still has no worker/frame/ACK");
}
void discoveryDoesNotRevokeActive(){
  DiscoveryPublication d;expect(!d.observe(false),"no withdrawal before an offer");expect(d.observe(true)==DiscoveryPublication::Action::offer,"eligible discovery publishes");expect(d.observe(true)==DiscoveryPublication::Action::offer,"same eligible state gets a new revision publication");
  auto f=active();expect(d.observe(false)==DiscoveryPublication::Action::withdraw,"native unlock/busy withdraws discovery once");expect(!d.observe(false),"absence does not repeatedly withdraw");expect(f.input(7,3,phaseId(2),observation(),1000),"discovery state cannot revoke active session authority");expect(d.observe(true)==DiscoveryPublication::Action::offer,"later eligible state can be offered again");
}
void outputRetirement(){
  std::promise<void> entered,release;auto releaseFuture=release.get_future().share();std::atomic<unsigned> calls=0;
  OutputLease lease([&](bool,std::vector<uint8_t>){++calls;entered.set_value();releaseFuture.wait();});
  std::thread producer([&]{expect(lease.send(true,{1}),"in-flight owned callback");});entered.get_future().wait();
  auto retired=std::async(std::launch::async,[&]{lease.retire();});expect(retired.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout,"retirement drains actual in-flight callback");release.set_value();producer.join();retired.get();expect(!lease.send(false,{2}),"late worker callback cannot touch retired Agent");expect(calls==1,"no callback replay after retirement");
}
}
int main(){try{defaultOffAndAdmission();frameAndAck();transitionAndExactExit();accountAndConsoleFences();heldEnterStaysPending();discoveryDoesNotRevokeActive();outputRetirement();std::cout<<"{\"kind\":\"signin-production-bridge-policy-pure-tests\",\"passed\":true,\"checks\":"<<checks<<",\"hostWindowsAdaptersInvoked\":0,\"nativeCapture\":false,\"hostBinaryRun\":false,\"runtimeQualified\":false}\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
