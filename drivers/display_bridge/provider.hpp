#pragma once
#include "lease_recovery.hpp"
#include <array>
#include <algorithm>
#include <cstring>
#include <span>
#include <stdexcept>
#include <chrono>
#include <thread>

// Multitap adapter for the unchanged upstream driver ABI. This does not install
// a driver, import certificates, select a production provider, or change any
// foreign/permanent output. The caller must be an authorized host session.
namespace multitap::vdisplay {
class Session {
 public:
  explicit Session(api::ControlClient& client):client_(client){}
  bool adoptReclaimed(const LiveLeaseRecoveryPlan& plan,const std::function<bool()>& identityCurrent,const std::function<bool()>& durableIntentCurrent){
    try{
      if(lease_||!identityCurrent||!durableIntentCurrent||!identityCurrent()||!durableIntentCurrent()||inspectLiveLeaseRecovery(client_,plan)!=LiveLeaseLocation::replacement||!identityCurrent()||!durableIntentCurrent())return false;
      api::LeaseRequest request{};request.lease_id=plan.replacement;const auto current=client_.query_lease(request);
      if(!current.ok()||!recoveryLeasePresent(current.value,plan.replacement,plan.outputs.size())||!identityCurrent()||!durableIntentCurrent())return false;
      for(size_t i=0;i<plan.targets.size();++i){outputs_[i]=plan.targets[i];outputs_[i].lease_id=plan.replacement;outputs_[i].effective_timeout_ms=current.value.effective_timeout_ms;}
      lease_=plan.replacement;count_=plan.outputs.size();active_=true;return true;
    }catch(...){return false;}
  }
  bool start(uint64_t lease,const api::OwnerCapability& owner,std::span<const Output> outputs) {
    if(lease_||lease<api::kMinOpaqueLeaseId||outputs.empty()||outputs.size()>2||
      std::all_of(owner.bytes.begin(),owner.bytes.end(),[](uint8_t b){return b==0;}))return false;
    for(size_t i=0;i<outputs.size();++i){const auto& o=outputs[i];
      if(!o.identity||!ownedOutputSize(o.width,o.height)||!ownedOutputModeSupported(o.width,o.height))return false;
      for(size_t j=0;j<i;++j)if(outputs[j].identity==o.identity)return false;
    }
    if(!client_.check_protocol_compatible().ok())return false;
    api::LeaseRequest query{};query.lease_id=lease;
    const auto previous=client_.query_lease(query);
    if(!previous.ok()||previous.value.lease_id!=lease||previous.value.lease_exists||previous.value.temporary_display_count||previous.value.pending_departure_count)return false;
    lease_=lease;
    for(size_t i=0;i<outputs.size();++i) {
      api::CreateTemporaryDisplayOwnedRequest request{};
      request.owner_capability=owner;auto& d=request.display;
      d.lease_id=lease;d.display_id=outputs[i].identity;
      d.width=outputs[i].width;d.height=outputs[i].height;
      // A portrait output is a portrait panel (its EDID size follows it).
      d.physical_width_mm=d.height>d.width?340:600;d.physical_height_mm=d.height>d.width?600:340;
      d.refresh_rate_millihz=60000;d.requested_timeout_ms=15000;
      const char* label=i==0?"Multitap 1":"Multitap 2";
      std::memcpy(d.display_name,label,std::strlen(label)+1);
      const auto result=client_.create_temporary_display_owned(request);
      if(!result.ok()||result.value.lease_id!=lease||result.value.display_id!=d.display_id||
        result.value.effective_timeout_ms<3000||result.value.effective_timeout_ms>15000){stop();return false;}
      outputs_[count_++]=result.value;
    }
    active_=true;return true;
  }
  bool heartbeat(){
    if(!lease_||!active_)return false;api::LeaseRequest request{};request.lease_id=lease_;request.requested_timeout_ms=15000;
    return client_.feed_lease(request).ok();
  }
  // Read-only driver evidence for re-verifying Windows activation. This checks
  // the owned lease/count, not CCD modes or capture readiness.
  bool leaseCurrent(){
    if(!lease_||!active_||!count_)return false;
    api::LeaseRequest request{};request.lease_id=lease_;
    const auto result=client_.query_lease(request);
    return result.ok()&&result.value.lease_id==lease_&&result.value.lease_exists&&
      result.value.temporary_display_count==count_&&!result.value.pending_departure_count;
  }
  bool append(const api::OwnerCapability& owner,const Output& output,size_t ordinal){
    if(!lease_||!active_||count_>=2||ordinal>1||!output.identity||
       !ownedOutputSize(output.width,output.height)||!ownedOutputModeSupported(output.width,output.height)||
       std::any_of(outputs_.begin(),outputs_.begin()+count_,[&](const auto& value){return value.display_id==output.identity;}))return false;
    api::CreateTemporaryDisplayOwnedRequest request{};request.owner_capability=owner;
    auto& display=request.display;display.lease_id=lease_;display.display_id=output.identity;
    display.width=output.width;display.height=output.height;display.physical_width_mm=output.height>output.width?340:600;display.physical_height_mm=output.height>output.width?600:340;
    display.refresh_rate_millihz=60000;display.requested_timeout_ms=15000;
    const char* label=ordinal==0?"Multitap 1":"Multitap 2";
    std::memcpy(display.display_name,label,std::strlen(label)+1);
    const auto result=client_.create_temporary_display_owned(request);
    if(!result.ok())return false;
    if(result.value.lease_id!=lease_||result.value.display_id!=output.identity||result.value.effective_timeout_ms<3000||result.value.effective_timeout_ms>15000){active_=false;return false;}
    outputs_[count_++]=result.value;return true;
  }
  bool remove(uint64_t identity){
    if(!lease_||!active_)return false;
    const auto found=std::find_if(outputs_.begin(),outputs_.begin()+count_,[&](const auto& value){return value.display_id==identity;});
    if(found==outputs_.begin()+count_)return false;
    api::LeaseDisplayRequest request{};request.lease_id=lease_;request.display_id=identity;
    if(!client_.remove_temporary_display(request).ok())return false;
    std::move(found+1,outputs_.begin()+count_,found);outputs_[--count_]={};return true;
  }
  bool departuresSettled(std::chrono::milliseconds budget=std::chrono::milliseconds(1500)){
    if(!lease_||!active_||budget.count()<0||budget>std::chrono::milliseconds(3000))return false;
    const auto deadline=std::chrono::steady_clock::now()+budget;
    for(;;){
      api::LeaseRequest request{};request.lease_id=lease_;const auto state=client_.query_lease(request);
      if(!state.ok()||state.value.lease_id!=lease_||
         state.value.pending_departure_count>state.value.temporary_display_count||
         state.value.temporary_display_count-state.value.pending_departure_count!=count_)return false;
      if(!state.value.pending_departure_count)return state.value.lease_exists||count_==0;
      if(std::chrono::steady_clock::now()>=deadline)return false;
      if(state.value.lease_exists&&!heartbeat())return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  bool stop(){
    active_=false;if(!lease_)return true;api::LeaseRequest request{};request.lease_id=lease_;
    if(!client_.release_lease(request).ok())return false;
    lease_=0;count_=0;outputs_={};return true;
  }
  bool cleanupPending()const{return lease_!=0&&!active_;}
  std::span<const api::CreateTemporaryDisplayResult> outputs()const{return {outputs_.data(),active_?count_:0};}
 private:
  api::ControlClient& client_;uint64_t lease_=0;size_t count_=0;bool active_=false;
  std::array<api::CreateTemporaryDisplayResult,2> outputs_{};
};
}
