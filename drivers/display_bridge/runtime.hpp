#pragma once
#include "provider.hpp"
#include <mutex>
#include <vector>

namespace multitap::vdisplay {
// An interrupted process is not evidence that its driver lease has gone away.
// This read-only check never releases an extant lease, even if it is ours.
// Session::start repeats the query before creating anything, closing the race.
inline bool quiescentOwnedLease(api::ControlClient& client,uint64_t lease){
  if(lease<api::kMinOpaqueLeaseId)return false;
  api::LeaseRequest request{};request.lease_id=lease;
  const auto result=client.query_lease(request);
  return result.ok()&&result.value.lease_id==lease&&!result.value.lease_exists&&
      !result.value.temporary_display_count&&!result.value.pending_departure_count;
}
struct ControllerState {
  bool leaseActive=false,recoveryRequired=false;
  // These are driver identities, not proof of an active capturable Windows
  // output. The host worker must match fresh CCD paths before capture.
  std::vector<api::CreateTemporaryDisplayResult> targets;
};
class Controller {
 public:
  Controller(api::ControlClient& client,uint64_t lease,api::OwnerCapability owner,
      std::chrono::milliseconds departureBudget=std::chrono::milliseconds(1500))
      :session_(client),lease_(lease),owner_(owner),departureBudget_(departureBudget){
    if(departureBudget.count()<0||departureBudget>std::chrono::milliseconds(3000))throw std::invalid_argument("Invalid native departure budget");
  }
  Controller(const Controller&)=delete;
  Controller& operator=(const Controller&)=delete;
  ~Controller(){std::lock_guard lock(mutex_);session_.stop();volatile uint8_t* bytes=owner_.bytes.data();for(size_t i=0;i<owner_.bytes.size();++i)bytes[i]=0;}
  bool start(std::span<const Output> outputs){
    std::lock_guard lock(mutex_);
    if(!session_.start(lease_,owner_,outputs))return false;
    requested_.assign(outputs.begin(),outputs.end());active_=true;recovery_=false;return true;
  }
  bool adoptReclaimed(const LiveLeaseRecoveryPlan& plan,const std::function<bool()>& identityCurrent,const std::function<bool()>& durableIntentCurrent){
    std::lock_guard lock(mutex_);
    if(active_||plan.replacement!=lease_)return false;
    auto desired=plan.outputs;
    if(!session_.adoptReclaimed(plan,identityCurrent,durableIntentCurrent)){recovery_=true;return false;}
    requested_=std::move(desired);active_=true;recovery_=false;return true;
  }
  bool heartbeat(){
    std::lock_guard lock(mutex_);
    if(!active_||!session_.heartbeat()){active_=false;recovery_=true;return false;}return true;
  }
  bool leaseCurrent(){std::lock_guard lock(mutex_);return active_&&!recovery_&&session_.leaseCurrent();}
  bool reconfigure(std::span<const Output> outputs){
    std::lock_guard lock(mutex_);
    if(!active_)return false;
    // Prevalidate before removing the working lease. Display identities remain
    // stable across viewport/count changes and no global manifest is changed.
    if(outputs.empty()||outputs.size()>2)return false;
    for(size_t i=0;i<outputs.size();++i){const auto& output=outputs[i];
      if(!output.identity||!ownedOutputSize(output.width,output.height)||!ownedOutputModeSupported(output.width,output.height))return false;
      for(size_t j=0;j<i;++j)if(outputs[j].identity==output.identity)return false;
    }
    const auto previous=requested_;const std::vector<Output> desired(outputs.begin(),outputs.end());
    if(previous==desired)return true;
    active_=false;
    // Keep every unchanged output alive. Adding/removing monitor 2 must not
    // recreate monitor 1; resizing one output must not tear down its neighbor.
    if(reconcile(previous,desired)){
      requested_=desired;active_=true;recovery_=false;return true;
    }
    // A completed removal can remain pending in IddCx. Wait for the actual
    // lease query before recreating that identity, including rollback.
    if(!session_.cleanupPending()&&reconcile(desired,previous)){
      requested_=previous;active_=true;recovery_=false;
    }else recovery_=true;
    return false;
  }
  bool stop(){std::lock_guard lock(mutex_);active_=false;const bool ok=session_.stop();recovery_=!ok;return ok;}
  ControllerState state()const {
    std::lock_guard lock(mutex_);ControllerState state;state.leaseActive=active_;state.recoveryRequired=recovery_;
    if(active_){const auto targets=session_.outputs();for(const auto& expected:requested_){
      const auto found=std::find_if(targets.begin(),targets.end(),[&](const auto& target){return target.display_id==expected.identity;});
      if(found==targets.end()){state.leaseActive=false;state.recoveryRequired=true;state.targets.clear();break;}state.targets.push_back(*found);
    }}
    return state;
  }
 private:
  bool reconcile(const std::vector<Output>& from,const std::vector<Output>& to){
    auto actual=session_.outputs();std::vector<uint64_t> remove;
    for(const auto& target:actual){
      const auto old=std::find_if(from.begin(),from.end(),[&](const auto& value){return value.identity==target.display_id;});
      const auto wanted=std::find_if(to.begin(),to.end(),[&](const auto& value){return value.identity==target.display_id;});
      if(wanted==to.end()||old==from.end()||*old!=*wanted)remove.push_back(target.display_id);
    }
    for(const auto id:remove)if(!session_.remove(id))return false;
    if(!session_.departuresSettled(departureBudget_))return false;
    for(size_t i=0;i<to.size();++i){actual=session_.outputs();
      if(std::none_of(actual.begin(),actual.end(),[&](const auto& target){return target.display_id==to[i].identity;})&&!session_.append(owner_,to[i],i))return false;
    }
    return true;
  }
  mutable std::mutex mutex_;Session session_;uint64_t lease_;api::OwnerCapability owner_;
  std::chrono::milliseconds departureBudget_;
  std::vector<Output> requested_;bool active_=false,recovery_=false;
};
}
