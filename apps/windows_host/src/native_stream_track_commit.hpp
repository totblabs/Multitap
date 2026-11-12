#pragma once
#include "native_stream_viewport_commit.hpp"
#include "native_track_change.hpp"
#include "native_stream_viewport_cancel.hpp"
#include <type_traits>

namespace host {
// A group receipt is a distinct typed request. It must not be decoded as a
// StreamResize or used to authorize only the first member of a two-output lease.
struct NativeStreamTrackCommit {
  NativeTrackResize request;
  std::string instance,profileOperation,resizeOperation,profileBeforeSha256,profileBeforeBytes,baselineSha256,runtimeSha256;
  bool rollback=false;NativeStreamProfileLease before,after;NativeRecoveryScope scope;
  uint64_t lease=0;std::vector<NativeStreamLaunchOutput> outputs;
  std::vector<NativeOutputPlacement> placements;
};
inline void validateNativeStreamTrackCommit(const NativeStreamTrackCommit& r){
  nativeStreamJournalRequire(nativeTrackResizeValid(r.request)&&r.request.topologyEpoch>=2&&nativeDisplayInstanceId(r.instance)&&nativeRecoveryHex(r.profileOperation)&&nativeRecoveryHex(r.resizeOperation)&&nativeRecoveryHex(r.profileBeforeSha256)&&nativeRecoveryHex(r.baselineSha256)&&nativeRecoveryHex(r.runtimeSha256));
  const auto original=parseNativeStreamProfileRecord(r.profileBeforeBytes);
  nativeStreamJournalRequire(nativeBaselineDigest(r.profileBeforeBytes)==r.profileBeforeSha256&&original.phase==NativeStreamProfilePhase::active&&!original.stopRequested&&original.resize&&original.key.stream==r.request.sessionId&&original.key.instance==r.instance&&original.key.operation==r.profileOperation&&original.resize->operation==r.resizeOperation&&original.resize->requestSha256==nativeBaselineDigest(nativeTrackResizeFields(r.request))&&sameNativeStreamLease(original.current,r.before));
  (void)nativeStreamLeaseBytes(r.before);(void)nativeStreamLeaseBytes(r.after);
  nativeStreamJournalRequire(r.before.servicePid==r.after.servicePid&&r.after.generation>=r.before.generation&&r.before.targets.size()==r.after.targets.size()&&r.after.targets.size()==r.outputs.size()&&r.after.targets.size()==r.placements.size()&&r.after.targets.size()==r.request.outputs.size()&&r.scope.session&&r.scope.session!=UINT32_MAX&&nativeRecoveryHex(r.scope.logon)&&r.scope.tick&&r.scope.foreign.empty()&&nativeRecoveryLogonMatches(original.scope,r.scope)&&r.lease>=0x6000000000000000ull);
  bool changed=false;unsigned primary=0;
  for(size_t i=0;i<r.after.targets.size();++i){
    const auto& a=r.before.targets[i];const auto& b=r.after.targets[i];const auto& output=r.request.outputs[i];const auto& place=r.placements[i];
    nativeStreamJournalRequire(output.id==std::to_string(i)&&place.id==output.id&&a.id==output.id&&b.id==output.id&&r.outputs[i].display&&r.outputs[i].connector!=UINT32_MAX&&displayRectangleFits(place.left,place.top,b.width,b.height)&&place.primary==(!place.left&&!place.top));
    primary+=place.primary?1:0;
    for(size_t j=0;j<i;++j){const auto& other=r.placements[j];const auto& target=r.after.targets[j];nativeStreamJournalRequire(r.outputs[i].display!=r.outputs[j].display&&r.outputs[i].connector!=r.outputs[j].connector&&!(int64_t(place.left)<int64_t(other.left)+target.width&&int64_t(other.left)<int64_t(place.left)+b.width&&int64_t(place.top)<int64_t(other.top)+target.height&&int64_t(other.top)<int64_t(place.top)+b.height));}
    nativeStreamJournalRequire(b.width==(r.rollback?a.width:output.width)&&b.height==(r.rollback?a.height:output.height));
    changed|=a.width!=b.width||a.height!=b.height||!nativeSameLuid(a.adapter,b.adapter)||a.target!=b.target;
  }
  nativeStreamJournalRequire(primary==1&&r.placements.front().primary&&(!changed||r.after.generation>r.before.generation));
  if(!r.rollback){const auto& anchor=r.request.outputs.front();for(size_t i=0;i<r.placements.size();++i)nativeStreamJournalRequire(int64_t(r.placements[i].left)==int64_t(r.request.outputs[i].left)-anchor.left&&int64_t(r.placements[i].top)==int64_t(r.request.outputs[i].top)-anchor.top);}
}
inline std::string nativeStreamTrackCommitBytes(const NativeStreamTrackCommit& r){
  validateNativeStreamTrackCommit(r);std::string outputs="[",placements="[";
  for(size_t i=0;i<r.outputs.size();++i){const auto& output=r.outputs[i];const auto& p=r.placements[i];if(i){outputs+=',';placements+=',';}outputs+="{\"display\":"+jsonQuote(nativeStreamU64(output.display))+",\"connector\":"+std::to_string(output.connector)+'}';placements+="{\"id\":"+jsonQuote(p.id)+",\"left\":"+std::to_string(p.left)+",\"top\":"+std::to_string(p.top)+",\"primary\":"+(p.primary?"true":"false")+'}';}
  const auto bytes="{\"schema\":1,\"kind\":\"native-stream-track-commit\",\"request\":{"+nativeTrackResizeFields(r.request)+"},\"deviceInstanceId\":"+jsonQuote(r.instance)+",\"profileOperation\":"+jsonQuote(r.profileOperation)+",\"resizeOperation\":"+jsonQuote(r.resizeOperation)+",\"profileBeforeSha256\":"+jsonQuote(r.profileBeforeSha256)+",\"profileBefore\":"+jsonQuote(r.profileBeforeBytes)+",\"baselineSha256\":"+jsonQuote(r.baselineSha256)+",\"runtimeSha256\":"+jsonQuote(r.runtimeSha256)+",\"rollback\":"+(r.rollback?"true":"false")+",\"before\":"+nativeStreamLeaseBytes(r.before)+",\"after\":"+nativeStreamLeaseBytes(r.after)+",\"scope\":"+nativeRecoveryScopeBytes(r.scope)+",\"lease\":"+jsonQuote(nativeStreamU64(r.lease))+",\"outputs\":"+outputs+"],\"placements\":"+placements+"]}";
  nativeStreamJournalRequire(bytes.size()<=131072);return bytes;
}
inline NativeStreamTrackCommit parseNativeStreamTrackCommit(const std::string& bytes){
  nativeStreamJournalRequire(!bytes.empty()&&bytes.size()<=131072);const auto root=JsonParser::parse(bytes);nativeStreamJournalFields(root,17);nativeStreamJournalRequire(root.get("schema").number()==1&&root.get("kind").string()=="native-stream-track-commit");
  NativeStreamTrackCommit r;r.request=nativeTrackResizeRequest(root.get("request"));r.instance=nativeStreamJournalString(root.get("deviceInstanceId"));r.profileOperation=nativeStreamJournalString(root.get("profileOperation"));r.resizeOperation=nativeStreamJournalString(root.get("resizeOperation"));r.profileBeforeSha256=nativeStreamJournalString(root.get("profileBeforeSha256"));r.baselineSha256=nativeStreamJournalString(root.get("baselineSha256"));r.runtimeSha256=nativeStreamJournalString(root.get("runtimeSha256"));r.profileBeforeBytes=nativeStreamJournalString(root.get("profileBefore"));
  nativeStreamJournalRequire(std::holds_alternative<bool>(root.get("rollback").value));r.rollback=root.get("rollback").boolean();r.before=parseNativeStreamLease(root.get("before"));r.after=parseNativeStreamLease(root.get("after"));r.scope=parseNativeRecoveryScope(root.get("scope"));r.lease=parseNativeStreamU64(root.get("lease"));
  const auto* outputs=std::get_if<Json::Array>(&root.get("outputs").value);const auto* placements=std::get_if<Json::Array>(&root.get("placements").value);nativeStreamJournalRequire(outputs&&!outputs->empty()&&outputs->size()<=2&&placements&&placements->size()==outputs->size());
  for(const auto& output:*outputs){nativeStreamJournalFields(output,2);r.outputs.push_back({parseNativeStreamU64(output.get("display")),nativeStreamJournalU32(output.get("connector"))});}
  for(const auto& p:*placements){nativeStreamJournalFields(p,4);nativeStreamJournalRequire(std::holds_alternative<bool>(p.get("primary").value));r.placements.push_back({nativeStreamJournalString(p.get("id")),nativeStreamJournalI32(p.get("left")),nativeStreamJournalI32(p.get("top")),p.get("primary").boolean()});}
  nativeStreamJournalRequire(nativeStreamTrackCommitBytes(r)==bytes);return r;
}
inline bool nativeStreamViewportCommitSource(const NativeStreamProfileRecord& profile,const NativeStreamTrackCommit& committed,bool recovery){
  try{const auto original=parseNativeStreamProfileRecord(committed.profileBeforeBytes);if(nativeStreamProfileRecordBytes(profile)==committed.profileBeforeBytes)return true;if(!recovery||(profile.phase!=NativeStreamProfilePhase::recovery&&profile.phase!=NativeStreamProfilePhase::active)||!profile.stopRequested||profile.revision<=original.revision)return false;auto normalized=profile;normalized.phase=original.phase;normalized.stopRequested=original.stopRequested;normalized.revision=original.revision;return nativeStreamProfileRecordBytes(normalized)==committed.profileBeforeBytes;}catch(...){return false;}
}
inline bool nativeStreamViewportCommittedProfileMatches(const NativeStreamProfileRecord& profile,const NativeStreamTrackCommit& committed){
  try{validateNativeStreamTrackCommit(committed);const auto original=parseNativeStreamProfileRecord(committed.profileBeforeBytes);if(profile.resize||profile.phase==NativeStreamProfilePhase::prepared||profile.revision<=original.revision||profile.resizeSequence!=original.resizeSequence||!sameNativeStreamLease(profile.current,committed.after))return false;auto normalized=profile;normalized.current=original.current;normalized.resize=original.resize;normalized.phase=original.phase;normalized.stopRequested=original.stopRequested;normalized.revision=original.revision;return nativeStreamProfileRecordBytes(normalized)==committed.profileBeforeBytes;}catch(...){return false;}
}
inline bool nativeStreamTrackCommitBaselineMatches(const NativeStreamTrackCommit& committed,const std::string& baselineBytes){
  try{
    validateNativeStreamTrackCommit(committed);if(nativeBaselineDigest(baselineBytes)!=committed.baselineSha256)return false;
    const auto saved=parseNativeRecoveryBaseline(baselineBytes);if(saved.instance!=committed.instance||saved.generation!=committed.before.generation||!saved.scope||!nativeRecoveryLogonMatches(*saved.scope,committed.scope)||!sameNativeStreamLease({committed.before.servicePid,saved.generation,saved.topology.targets},committed.before))return false;
    const auto original=parseNativeStreamProfileRecord(committed.profileBeforeBytes);const NativeStreamProfileEvidence before{original.key,committed.before,*saved.scope,original.console,saved.topology.ccd};if(!nativeStreamProfileEvidenceMatches(original,before,NativeStreamProfileView::active))return false;
    const auto placements=committed.rollback?saved.topology.placements:planNativeTrackViewport(saved.topology,committed.request.outputs).placements;
    if(placements.size()!=committed.placements.size())return false;for(size_t i=0;i<placements.size();++i){const auto& a=placements[i];const auto& b=committed.placements[i];if(a.id!=b.id||a.left!=b.left||a.top!=b.top||a.primary!=b.primary)return false;}return true;
  }catch(...){return false;}
}
inline bool nativeStreamTrackCommitProof(const NativeStreamProfileRecord& profile,const NativeStreamProfileEvidence& proof,const NativeTrackResize& request,const NativeStreamTrackCommit& committed,const std::string& runtimeBytes,const std::string& baselineBytes,bool recovery=false){
  try{
    if(!nativeStreamTrackCommitBaselineMatches(committed,baselineBytes)||!profile.resize||profile.key.stream!=request.sessionId||profile.key.instance!=committed.instance||profile.key.operation!=committed.profileOperation||profile.resize->operation!=committed.resizeOperation||profile.resize->requestSha256!=nativeBaselineDigest(nativeTrackResizeFields(request))||!nativeStreamViewportCommitSource(profile,committed,recovery)||!nativeSameTrackResize(request,committed.request)||!sameNativeStreamLease(profile.current,committed.before)||!sameNativeStreamLease(proof.lease,committed.after)||!nativeRecoveryLogonMatches(committed.scope,proof.scope)||nativeBaselineDigest(runtimeBytes)!=committed.runtimeSha256)return false;
    const auto runtime=parseNativeRuntimeRecord(runtimeBytes);if(runtime.instance!=committed.instance||runtime.phase!="stable"||runtime.generation!=committed.after.generation||!runtime.baselineHash.empty()||runtime.outputs.size()!=proof.lease.targets.size())return false;
    for(size_t i=0;i<runtime.outputs.size();++i){const auto& t=proof.lease.targets[i];const auto& p=committed.placements[i];if(runtime.outputs[i].id!=t.id||runtime.outputs[i].width!=t.width||runtime.outputs[i].height!=t.height)return false;const auto found=std::find_if(proof.topology.paths.begin(),proof.topology.paths.end(),[&](const auto& path){return(path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativeSameTarget(path,t);});if(found==proof.topology.paths.end())return false;const auto& source=nativeCcdSource(proof.topology,*found);if(source.position.x!=p.left||source.position.y!=p.top||source.width!=t.width||source.height!=t.height)return false;}
    return nativeStreamProfileEvidenceMatches(profile,proof,recovery?NativeStreamProfileView::restorable:NativeStreamProfileView::active,true);
  }catch(...){return false;}
}
struct NativeStreamTrackCommitEvidence {NativeStreamTrackCommit receipt;std::string runtimeBytes,baselineBytes;};
template<class Receipt>inline const NativeStreamProfileLease& nativeStreamReceiptBefore(const Receipt& receipt){if constexpr(std::is_same_v<Receipt,NativeStreamViewportCancel>)return receipt.current;else return receipt.before;}
template<class Receipt>inline const NativeStreamProfileLease& nativeStreamReceiptAfter(const Receipt& receipt){if constexpr(std::is_same_v<Receipt,NativeStreamViewportCancel>)return receipt.current;else return receipt.after;}
template<class Receipt>inline std::string nativeStreamReceiptSession(const Receipt& receipt){if constexpr(std::is_same_v<Receipt,NativeStreamViewportCancel>)return std::visit([](const auto& request){return request.sessionId;},receipt.request);else return receipt.request.sessionId;}
inline bool nativeStreamViewportCommitSource(const NativeStreamProfileRecord& profile,const NativeStreamViewportCancel& receipt,bool recovery){return nativeStreamViewportCancelSourceMatches(profile,receipt,recovery);}
inline bool nativeStreamViewportCommittedProfileMatches(const NativeStreamProfileRecord& profile,const NativeStreamViewportCancel& receipt){return nativeStreamViewportCancelledProfileMatches(profile,receipt);}
using NativeStreamViewportReceipt=std::variant<NativeStreamViewportCommit,NativeStreamTrackCommit,NativeStreamViewportCancel>;
inline NativeStreamViewportReceipt parseNativeStreamViewportReceipt(const std::string& bytes){
  nativeStreamJournalRequire(!bytes.empty()&&bytes.size()<=131072);const auto kind=JsonParser::parse(bytes).get("kind").string();if(kind=="native-stream-viewport-commit")return parseNativeStreamViewportCommit(bytes);if(kind=="native-stream-track-commit")return parseNativeStreamTrackCommit(bytes);if(kind=="native-stream-viewport-cancel")return parseNativeStreamViewportCancel(bytes);throw std::runtime_error("native_stream_viewport_receipt_invalid");
}
inline NativeRuntimeJournalAccess nativeStreamViewportReceiptFileAccess(const std::filesystem::path& path,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  nativeStreamJournalRequire(path.filename()==L"native-stream-viewport-commit.json");return nativeJournalFileAccess(path,131072,[](const auto& bytes){(void)parseNativeStreamViewportReceipt(bytes);},std::move(trusted));
}
}
