#pragma once
#include <virtual_display/driver/control_client.h>
#include <algorithm>
#include <functional>
#include <span>
#include <vector>
#include <limits>
#include "../../native/windows_video/frame_layout.hpp"

namespace multitap::vdisplay {
namespace api=virtual_display::driver;
struct Output {uint64_t identity;uint32_t width,height;friend bool operator==(const Output&,const Output&)=default;};
// Geometry representation/alignment only; the selected SDK mode ABI is
// checked independently before any working target is removed.
inline bool ownedOutputSize(uint32_t width,uint32_t height,uint32_t minimumHeight=180){
  // Wire fields are uint32; only enforce representability/alignment here.
  // The unmodified signed driver's actual create response determines support.
  multitap::video::FrameLayout layout;
  return width>=320&&height>=minimumHeight&&multitap::video::frameLayout(width,height,layout);
}
// Limits of the pinned, unchanged signed provider's protocol, not a Multitap
// policy ceiling. Reject a mode before releasing any working target.
inline constexpr bool ownedOutputModeSupported(uint32_t width,uint32_t height){
  return width>=api::kMinWidth&&width<=api::kMaxWidth&&height>=api::kMinHeight&&height<=api::kMaxHeight&&
    uint64_t(width)*height<=uint64_t(api::kMaxWidth)*api::kMaxHeight;
}
struct LiveLeaseRecoveryPlan {
  uint64_t previous=0,replacement=0;
  std::vector<Output> outputs;
  // Original SDK creation results, authenticated by the private host receipt
  // and freshly matched to Windows CCD before this driver-only transaction.
  std::vector<api::CreateTemporaryDisplayResult> targets;
};
enum class LiveLeaseLocation {unknown,previous,replacement,absent};
struct LiveLeaseRecoveryResult {LiveLeaseLocation location=LiveLeaseLocation::unknown;bool attempted=false,acknowledged=false;};
inline bool validLiveLeaseRecoveryPlan(const LiveLeaseRecoveryPlan& plan){
  if(plan.previous<api::kMinOpaqueLeaseId||plan.replacement<api::kMinOpaqueLeaseId||plan.previous==plan.replacement||plan.outputs.empty()||plan.outputs.size()>2||plan.targets.size()!=plan.outputs.size())return false;
  for(size_t i=0;i<plan.outputs.size();++i){const auto& output=plan.outputs[i];const auto& target=plan.targets[i];
    if(!output.identity||!ownedOutputSize(output.width,output.height,200)||target.api_namespace!=api::kApiNamespaceGuid||target.lease_id!=plan.previous||target.display_id!=output.identity||target.target_id==UINT32_MAX||target.connector_index==UINT32_MAX||(!target.os_adapter_luid.low_part&&!target.os_adapter_luid.high_part)||target.reserved)return false;
    for(size_t j=0;j<i;++j)if(output.identity==plan.outputs[j].identity||target.connector_index==plan.targets[j].connector_index||(target.os_adapter_luid==plan.targets[j].os_adapter_luid&&target.target_id==plan.targets[j].target_id))return false;
  }return true;
}
inline bool recoveryLeaseAbsent(const api::QueryLeaseResult& value,uint64_t expected){return value.lease_id==expected&&!value.lease_exists&&!value.temporary_display_count&&!value.pending_departure_count;}
inline bool recoveryLeasePresent(const api::QueryLeaseResult& value,uint64_t expected,size_t count){return value.lease_id==expected&&value.lease_exists==1&&value.temporary_display_count==count&&!value.pending_departure_count&&value.effective_timeout_ms>=3000&&value.effective_timeout_ms<=15000&&value.remaining_ms&&value.remaining_ms<=value.effective_timeout_ms;}
inline LiveLeaseLocation inspectLiveLeaseRecovery(api::ControlClient& client,const LiveLeaseRecoveryPlan& plan){
  if(!validLiveLeaseRecoveryPlan(plan)||!client.check_protocol_compatible().ok())return LiveLeaseLocation::unknown;
  api::LeaseRequest a{},b{};a.lease_id=plan.previous;b.lease_id=plan.replacement;
  const auto old=client.query_lease(a),next=client.query_lease(b);const auto state=client.query_display_state();
  if(!old.ok()||!next.ok()||!state.ok()||state.value.reserved||state.value.entry_count>api::kMaxDisplayStateEntries||uint64_t(state.value.entry_count)!=uint64_t(state.value.permanent_display_count)+state.value.temporary_display_count)return LiveLeaseLocation::unknown;
  LiveLeaseLocation location=LiveLeaseLocation::unknown;uint64_t active=0;
  if(recoveryLeasePresent(old.value,plan.previous,plan.outputs.size())&&recoveryLeaseAbsent(next.value,plan.replacement)){location=LiveLeaseLocation::previous;active=plan.previous;}
  else if(recoveryLeaseAbsent(old.value,plan.previous)&&recoveryLeasePresent(next.value,plan.replacement,plan.outputs.size())){location=LiveLeaseLocation::replacement;active=plan.replacement;}
  else if(recoveryLeaseAbsent(old.value,plan.previous)&&recoveryLeaseAbsent(next.value,plan.replacement))location=LiveLeaseLocation::absent;
  if(location==LiveLeaseLocation::unknown)return location;
  std::vector<bool> seen(plan.outputs.size(),false);size_t temporary=0,permanent=0;
  for(size_t i=0;i<state.value.entry_count;++i){const auto& entry=state.value.entries[i];
    if(entry.kind==api::kDisplayStateKindTemporary)++temporary;else if(entry.kind==api::kDisplayStateKindPermanent)++permanent;else return LiveLeaseLocation::unknown;
    const auto expected=std::find_if(plan.outputs.begin(),plan.outputs.end(),[&](const auto& output){return output.identity==entry.display_id;});
    if(expected==plan.outputs.end()){if(entry.lease_id==plan.previous||entry.lease_id==plan.replacement)return LiveLeaseLocation::unknown;continue;}
    // The pinned SDK leaves temporary entries' lease_id zero.
    // Ownership comes from the protected creation receipt, fenced processes
    // and authenticated reclaim; this public enumeration is geometry evidence.
    // If a provider does return a lease ID it must agree, never be foreign.
    if(!active||entry.kind!=api::kDisplayStateKindTemporary||(entry.lease_id&&entry.lease_id!=active))return LiveLeaseLocation::unknown;
    const auto index=size_t(expected-plan.outputs.begin());
    // Match the physical panel orientation used by Session::start/append.
    const bool portrait=expected->height>expected->width;
    if(seen[index]||entry.connector_index!=plan.targets[index].connector_index||entry.width!=expected->width||entry.height!=expected->height||entry.refresh_rate_millihz!=60000||entry.physical_width_mm!=(portrait?340u:600u)||entry.physical_height_mm!=(portrait?600u:340u))return LiveLeaseLocation::unknown;
    seen[index]=true;
  }
  if(temporary!=state.value.temporary_display_count||permanent!=state.value.permanent_display_count||(active&&std::find(seen.begin(),seen.end(),false)!=seen.end()))return LiveLeaseLocation::unknown;
  // Lease expiry/reclaim during the multi-query observation must not authorize
  // a write. Foreign state is never selected for removal or reconfiguration.
  const auto finalOld=client.query_lease(a),finalNext=client.query_lease(b);if(!finalOld.ok()||!finalNext.ok())return LiveLeaseLocation::unknown;
  if(location==LiveLeaseLocation::previous&&(!recoveryLeasePresent(finalOld.value,plan.previous,plan.outputs.size())||!recoveryLeaseAbsent(finalNext.value,plan.replacement)))return LiveLeaseLocation::unknown;
  if(location==LiveLeaseLocation::replacement&&(!recoveryLeaseAbsent(finalOld.value,plan.previous)||!recoveryLeasePresent(finalNext.value,plan.replacement,plan.outputs.size())))return LiveLeaseLocation::unknown;
  if(location==LiveLeaseLocation::absent&&(!recoveryLeaseAbsent(finalOld.value,plan.previous)||!recoveryLeaseAbsent(finalNext.value,plan.replacement)))return LiveLeaseLocation::unknown;
  return location;
}
// Both callbacks must verify existing evidence, not manufacture a new intent.
// Host persists old/new IDs and validates login/process/CCD first. This function
// never creates/removes displays, changes permanent settings or feeds an old
// lease. After lost ACK it reconciles only the SAME protected replacement ID.
inline LiveLeaseRecoveryResult reconcileLiveLease(api::ControlClient& client,const LiveLeaseRecoveryPlan& plan,const api::OwnerCapability& owner,const std::function<bool()>& identityCurrent,const std::function<bool()>& durableIntentCurrent){
  LiveLeaseRecoveryResult result;
  try{
    const auto authorized=[&]{return identityCurrent&&durableIntentCurrent&&identityCurrent()&&durableIntentCurrent();};
    if(!validLiveLeaseRecoveryPlan(plan)||std::all_of(owner.bytes.begin(),owner.bytes.end(),[](uint8_t b){return !b;})||!authorized())return result;
    result.location=inspectLiveLeaseRecovery(client,plan);if(!authorized()){result.location=LiveLeaseLocation::unknown;return result;}
    if(result.location!=LiveLeaseLocation::previous)return result;
    api::ReclaimTemporaryDisplayRequest request{};request.display_id=plan.outputs.front().identity;request.new_lease_id=plan.replacement;request.requested_timeout_ms=15000;request.owner_capability=owner;
    struct Wipe{api::OwnerCapability& value;~Wipe(){volatile uint8_t* bytes=value.bytes.data();for(size_t i=0;i<value.bytes.size();++i)bytes[i]=0;}}wipe{request.owner_capability};
    result.attempted=true;const auto ack=client.reclaim_temporary_display(request);
    result.acknowledged=ack.ok()&&ack.value.lease_id==plan.replacement&&ack.value.display_id==plan.outputs.front().identity&&ack.value.connector_index==plan.targets.front().connector_index&&ack.value.temporary_display_count==plan.outputs.size()&&ack.value.effective_timeout_ms>=3000&&ack.value.effective_timeout_ms<=15000;
    result.location=inspectLiveLeaseRecovery(client,plan);if(!authorized())result.location=LiveLeaseLocation::unknown;return result;
  }catch(...){result.location=LiveLeaseLocation::unknown;return result;}
}
}
