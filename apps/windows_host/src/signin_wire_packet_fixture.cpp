#include "signin_bridge_wire.hpp"
#include "signin_worker_entry.hpp"
#include <iostream>

// Serialization only: no ServiceBridge, native Permit, WTS, capture, input,
// credentials, socket, driver, or machine-settings adapter is constructed.
int main(){
  using namespace host;using namespace host::signin;
  using host::signin::broker::Admission;
  namespace wire=host::signin::broker::wire;
  try{
    const std::string session="01000000-0000-0000-0000-000000000000",instance="0123456789abcdef0123456789abcdef";
    Admission initial;initial.stream[0]=1;initial.transition[0]=0x0b;initial.workerGeneration=7;initial.mediaEpoch=5;
    auto next=initial;++next.workerGeneration;++next.mediaEpoch;next.transition[0]=0x0d;
    broker::PhaseId frame{};frame[0]=0x0e;
    const auto info=JsonParser::parse("{\"type\":\"stream.info\",\"sessionId\":"+jsonQuote(session)+",\"epoch\":6,\"codec\":\"h264\",\"width\":1920,\"height\":1080,\"sourceWidth\":1920,\"sourceHeight\":1080,\"fpsTarget\":60,\"audio\":{\"available\":false}}");
    channel::Init child;auto& c=child.context;c.stream=initial.stream;c.service={100,1000};c.serviceIsLocalSystem=true;
    c.lease={"ROOT\\DISPLAY\\0000",4};c.leaseOwned=true;c.baseline[0]=2;c.nonce[0]=3;c.consoleSession=1;c.workerGeneration=7;c.mediaEpoch=5;c.ownedOutputMask=1;
    c.configuredSid="S-1-5-21-1-2-3-1001";c.assigned=c.clientOptIn=c.clientTransitions=c.signinRuntimeQualified=true;
    child.binding={c.stream,c.service,{200,2000},c.lease,c.baseline,c.nonce,c.consoleSession,c.workerGeneration,c.mediaEpoch,Role::signin};child.target={1,-7,2,3,16,16,0,0,1};child.serviceQuery=123;
    std::cout<<"{\"kind\":\"actual-cpp-signin-producer-wire-fixture-v1\",\"runtimeQualified\":false,\"nativeAdaptersInvoked\":false,\"packets\":{"
      <<"\"eligibility\":"<<wire::eligibility(instance,9,WindowsState::locked)
      <<",\"eligibilityNoUser\":"<<wire::eligibility(instance,10,WindowsState::noUser)
      <<",\"withdraw\":"<<wire::eligibilityWithdraw(instance,11)
      <<",\"initialAuthority\":"<<wire::authority(session,instance,1,initial,Role::signin,false)
      <<",\"prepare\":"<<wire::prepare(session,next,Role::user)
      <<",\"previousWorkerExited\":"<<wire::previousExited(session,instance,next,Role::user)
      <<",\"nextAuthority\":"<<wire::authority(session,instance,2,next,Role::user,false)
      <<",\"info\":"<<wire::info(session,next,Role::user,frame,info)
      <<",\"cleanupComplete\":"<<entry::cleanupPacket(child,{entry::Cause::input,entry::Stage::cleanup,0,0})
      <<",\"cleanupPending\":"<<entry::cleanupPacket(child,{entry::Cause::handoff,entry::Stage::cleanup,0,1})<<"}}\n";
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
