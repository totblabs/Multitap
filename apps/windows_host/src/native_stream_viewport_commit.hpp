#pragma once
#include "native_stream_launch.hpp"
#include "native_display_change.hpp"

namespace host {
// A private native viewport result binds one profile reservation to the actual
// completed service generation. Shape or a successful decoder callback is not
// authority for advancing the profile's owned-output mappings.
struct NativeStreamViewportCommit {
  StreamResize request;std::string instance,profileOperation,resizeOperation,profileBeforeSha256,profileBeforeBytes,baselineSha256,runtimeSha256;
  bool rollback=false;NativeStreamProfileLease before,after;NativeRecoveryScope scope;
  uint64_t lease=0;std::vector<NativeStreamLaunchOutput> outputs;
};
inline void validateNativeStreamViewportCommit(const NativeStreamViewportCommit& r){
  (void)streamResizeRequest(JsonParser::parse('{'+nativeResizeFields(r.request)+'}'));
  nativeStreamJournalRequire(nativeDisplayInstanceId(r.instance)&&nativeRecoveryHex(r.profileOperation)&&nativeRecoveryHex(r.resizeOperation)&&nativeRecoveryHex(r.profileBeforeSha256)&&nativeRecoveryHex(r.baselineSha256)&&nativeRecoveryHex(r.runtimeSha256));
  const auto original=parseNativeStreamProfileRecord(r.profileBeforeBytes);
  nativeStreamJournalRequire(nativeBaselineDigest(r.profileBeforeBytes)==r.profileBeforeSha256&&original.phase==NativeStreamProfilePhase::active&&!original.stopRequested&&original.resize&&original.key.stream==r.request.sessionId&&original.key.instance==r.instance&&original.key.operation==r.profileOperation&&original.resize->operation==r.resizeOperation&&original.resize->requestSha256==nativeBaselineDigest(nativeResizeFields(r.request))&&sameNativeStreamLease(original.current,r.before));
  (void)nativeStreamLeaseBytes(r.before);(void)nativeStreamLeaseBytes(r.after);
  nativeStreamJournalRequire(r.before.servicePid==r.after.servicePid&&r.after.generation>=r.before.generation&&r.before.targets.size()==r.after.targets.size()&&r.after.targets.size()==r.outputs.size()&&r.scope.session&&r.scope.session!=UINT32_MAX&&nativeRecoveryHex(r.scope.logon)&&r.scope.tick&&r.scope.foreign.empty()&&r.lease>=0x6000000000000000ull);
  for(size_t i=0;i<r.outputs.size();++i){nativeStreamJournalRequire(r.outputs[i].display&&r.outputs[i].connector!=UINT32_MAX);for(size_t j=0;j<i;++j)nativeStreamJournalRequire(r.outputs[i].display!=r.outputs[j].display&&r.outputs[i].connector!=r.outputs[j].connector);}
  const auto index=r.request.monitor=="0"?size_t(0):size_t(1);nativeStreamJournalRequire(index<r.after.targets.size());
  if(!r.rollback)nativeStreamJournalRequire(r.after.targets[index].width==uint32_t(r.request.width)&&r.after.targets[index].height==uint32_t(r.request.height));
  else for(size_t i=0;i<r.after.targets.size();++i)nativeStreamJournalRequire(r.after.targets[i].width==r.before.targets[i].width&&r.after.targets[i].height==r.before.targets[i].height);
}
inline std::string nativeStreamViewportCommitBytes(const NativeStreamViewportCommit& r){
  validateNativeStreamViewportCommit(r);std::string outputs="[";for(const auto& output:r.outputs){if(outputs.size()>1)outputs+=',';outputs+="{\"display\":"+jsonQuote(nativeStreamU64(output.display))+",\"connector\":"+std::to_string(output.connector)+'}';}
  return "{\"schema\":1,\"kind\":\"native-stream-viewport-commit\",\"request\":{"+nativeResizeFields(r.request)+"},\"deviceInstanceId\":"+jsonQuote(r.instance)+",\"profileOperation\":"+jsonQuote(r.profileOperation)+",\"resizeOperation\":"+jsonQuote(r.resizeOperation)+",\"profileBeforeSha256\":"+jsonQuote(r.profileBeforeSha256)+",\"profileBefore\":"+jsonQuote(r.profileBeforeBytes)+",\"baselineSha256\":"+jsonQuote(r.baselineSha256)+",\"runtimeSha256\":"+jsonQuote(r.runtimeSha256)+",\"rollback\":"+(r.rollback?"true":"false")+",\"before\":"+nativeStreamLeaseBytes(r.before)+",\"after\":"+nativeStreamLeaseBytes(r.after)+",\"scope\":"+nativeRecoveryScopeBytes(r.scope)+",\"lease\":"+jsonQuote(nativeStreamU64(r.lease))+",\"outputs\":"+outputs+"]}";
}
inline NativeStreamViewportCommit parseNativeStreamViewportCommit(const std::string& bytes){
  nativeStreamJournalRequire(!bytes.empty()&&bytes.size()<=131072);const auto root=JsonParser::parse(bytes);nativeStreamJournalFields(root,16);nativeStreamJournalRequire(root.get("schema").number()==1&&root.get("kind").string()=="native-stream-viewport-commit");
  NativeStreamViewportCommit r;r.request=streamResizeRequest(root.get("request"));r.instance=nativeStreamJournalString(root.get("deviceInstanceId"));r.profileOperation=nativeStreamJournalString(root.get("profileOperation"));r.resizeOperation=nativeStreamJournalString(root.get("resizeOperation"));r.profileBeforeSha256=nativeStreamJournalString(root.get("profileBeforeSha256"));r.baselineSha256=nativeStreamJournalString(root.get("baselineSha256"));r.runtimeSha256=nativeStreamJournalString(root.get("runtimeSha256"));
  r.profileBeforeBytes=nativeStreamJournalString(root.get("profileBefore"));
  nativeStreamJournalRequire(std::holds_alternative<bool>(root.get("rollback").value));r.rollback=root.get("rollback").boolean();r.before=parseNativeStreamLease(root.get("before"));r.after=parseNativeStreamLease(root.get("after"));r.scope=parseNativeRecoveryScope(root.get("scope"));r.lease=parseNativeStreamU64(root.get("lease"));
  const auto* outputs=std::get_if<Json::Array>(&root.get("outputs").value);nativeStreamJournalRequire(outputs&&!outputs->empty()&&outputs->size()<=2);for(const auto& output:*outputs){nativeStreamJournalFields(output,2);r.outputs.push_back({parseNativeStreamU64(output.get("display")),nativeStreamJournalU32(output.get("connector"))});}
  nativeStreamJournalRequire(nativeStreamViewportCommitBytes(r)==bytes);return r;
}
inline NativeRuntimeJournalAccess nativeStreamViewportCommitFileAccess(const std::filesystem::path& path,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  nativeStreamJournalRequire(path.filename()==L"native-stream-viewport-commit.json");return nativeJournalFileAccess(path,131072,[](const auto& bytes){(void)parseNativeStreamViewportCommit(bytes);},std::move(trusted));
}
inline bool nativeStreamViewportCommitSource(const NativeStreamProfileRecord& profile,const NativeStreamViewportCommit& committed,bool recovery){
  try{
  const auto original=parseNativeStreamProfileRecord(committed.profileBeforeBytes);
  if(nativeStreamProfileRecordBytes(profile)==committed.profileBeforeBytes)return true;
  if(!recovery||(profile.phase!=NativeStreamProfilePhase::recovery&&profile.phase!=NativeStreamProfilePhase::active)||!profile.stopRequested||profile.revision<=original.revision)return false;
  auto normalized=profile;normalized.phase=original.phase;normalized.stopRequested=original.stopRequested;normalized.revision=original.revision;
  return nativeStreamProfileRecordBytes(normalized)==committed.profileBeforeBytes;
  }catch(...){return false;}
}
inline bool nativeStreamViewportCommittedProfileMatches(const NativeStreamProfileRecord& profile,const NativeStreamViewportCommit& committed){
  try{
    validateNativeStreamViewportCommit(committed);const auto original=parseNativeStreamProfileRecord(committed.profileBeforeBytes);
    if(profile.resize||profile.phase==NativeStreamProfilePhase::prepared||profile.revision<=original.revision||profile.resizeSequence!=original.resizeSequence||!sameNativeStreamLease(profile.current,committed.after))return false;
    auto normalized=profile;normalized.current=original.current;normalized.resize=original.resize;normalized.phase=original.phase;normalized.stopRequested=original.stopRequested;normalized.revision=original.revision;
    return nativeStreamProfileRecordBytes(normalized)==committed.profileBeforeBytes;
  }catch(...){return false;}
}
inline bool nativeStreamViewportCommitProof(const NativeStreamProfileRecord& profile,const NativeStreamProfileEvidence& proof,const StreamResize& request,const NativeStreamViewportCommit& committed,const std::string& runtimeBytes,bool recovery=false){
  try{
    validateNativeStreamViewportCommit(committed);
    if(!profile.resize||profile.key.stream!=request.sessionId||profile.key.instance!=committed.instance||profile.key.operation!=committed.profileOperation||profile.resize->operation!=committed.resizeOperation||profile.resize->requestSha256!=nativeBaselineDigest(nativeResizeFields(request))||!nativeStreamViewportCommitSource(profile,committed,recovery)||!nativeSameResize(request,committed.request)||!sameNativeStreamLease(profile.current,committed.before)||!sameNativeStreamLease(proof.lease,committed.after)||!nativeRecoveryLogonMatches(committed.scope,proof.scope)||nativeBaselineDigest(runtimeBytes)!=committed.runtimeSha256)return false;
    const auto runtime=parseNativeRuntimeRecord(runtimeBytes);if(runtime.instance!=committed.instance||runtime.phase!="stable"||runtime.generation!=committed.after.generation||!runtime.baselineHash.empty()||runtime.outputs.size()!=proof.lease.targets.size())return false;
    for(size_t i=0;i<runtime.outputs.size();++i)if(runtime.outputs[i].id!=proof.lease.targets[i].id||runtime.outputs[i].width!=proof.lease.targets[i].width||runtime.outputs[i].height!=proof.lease.targets[i].height)return false;
    return nativeStreamProfileEvidenceMatches(profile,proof,recovery?NativeStreamProfileView::restorable:NativeStreamProfileView::active,true);
  }catch(...){return false;}
}
}
