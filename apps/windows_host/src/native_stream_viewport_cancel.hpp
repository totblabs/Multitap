#pragma once
#include "native_stream_viewport_commit.hpp"
#include "native_track_change.hpp"
#include "native_display_baseline_codec.hpp"
#include <type_traits>
#include <variant>

namespace host {
// Candidate codec only. A protected receipt is produced by the authenticated
// service AFTER a fresh private-controller check, never by this parser. Embed
// the unchanged checkpoint so an old group receipt's global baseline cannot be
// overwritten before its replacement receipt becomes durable.
using NativeStreamCancelRequest=std::variant<StreamResize,NativeTrackResize>;
inline std::string nativeStreamCancelRequestFields(const NativeStreamCancelRequest& request){
  return std::visit([](const auto& value){
    if constexpr(std::is_same_v<std::decay_t<decltype(value)>,StreamResize>){nativeStreamJournalRequire(nativeResizeValid(value));return nativeResizeFields(value);}
    else{nativeStreamJournalRequire(nativeTrackResizeValid(value)&&value.topologyEpoch>=2);return nativeTrackResizeFields(value);}
  },request);
}
struct NativeStreamViewportCancel {
  NativeStreamCancelRequest request;
  std::string instance,profileOperation,resizeOperation,profileBeforeSha256,profileBeforeBytes,baselineSha256,baselineBytes,runtimeSha256;
  NativeStreamProfileLease current;NativeRecoveryScope scope;
  uint64_t lease=0;std::vector<NativeStreamLaunchOutput> outputs;
};
struct NativeStreamViewportCancelEvidence {NativeStreamViewportCancel receipt;std::string runtimeBytes;};
inline void validateNativeStreamViewportCancel(const NativeStreamViewportCancel& r){
  const auto fields=nativeStreamCancelRequestFields(r.request);
  nativeStreamJournalRequire(nativeDisplayInstanceId(r.instance)&&nativeRecoveryHex(r.profileOperation)&&nativeRecoveryHex(r.resizeOperation)&&nativeRecoveryHex(r.profileBeforeSha256)&&nativeRecoveryHex(r.baselineSha256)&&nativeRecoveryHex(r.runtimeSha256));
  const auto original=parseNativeStreamProfileRecord(r.profileBeforeBytes);
  nativeStreamJournalRequire(nativeBaselineDigest(r.profileBeforeBytes)==r.profileBeforeSha256&&original.phase==NativeStreamProfilePhase::active&&!original.stopRequested&&original.resize&&original.key.instance==r.instance&&original.key.operation==r.profileOperation&&original.resize->operation==r.resizeOperation&&original.resize->requestSha256==nativeBaselineDigest(fields)&&sameNativeStreamLease(original.current,r.current));
  std::visit([&](const auto& request){
    nativeStreamJournalRequire(request.sessionId==original.key.stream);
    if constexpr(std::is_same_v<std::decay_t<decltype(request)>,StreamResize>)nativeStreamJournalRequire((request.monitor=="0"?size_t(0):size_t(1))<r.current.targets.size());
    else nativeStreamJournalRequire(request.outputs.size()==r.current.targets.size());
  },r.request);
  nativeStreamJournalRequire(nativeBaselineDigest(r.baselineBytes)==r.baselineSha256);
  const auto saved=parseNativeRecoveryBaseline(r.baselineBytes);
  nativeStreamJournalRequire(saved.instance==r.instance&&saved.generation==r.current.generation&&saved.scope&&nativeRecoveryScopeMatches(*saved.scope,r.scope)&&r.scope.tick>=saved.scope->tick&&r.scope.tick-saved.scope->tick<=streamResizeTimeoutMs&&sameNativeStreamLease({r.current.servicePid,saved.generation,saved.topology.targets},r.current));
  const auto monitor=std::visit([](const auto& request)->std::string{if constexpr(std::is_same_v<std::decay_t<decltype(request)>,StreamResize>)return request.monitor;else return "0";},r.request);
  nativeStreamJournalRequire(saved.topology.replacedMonitor==monitor&&nativeStreamProfileEvidenceMatches(original,{original.key,r.current,*saved.scope,original.console,saved.topology.ccd},NativeStreamProfileView::active));
  nativeStreamJournalRequire(r.lease>=0x6000000000000000ull&&r.outputs.size()==r.current.targets.size());
  for(size_t i=0;i<r.outputs.size();++i){nativeStreamJournalRequire(r.outputs[i].display&&r.outputs[i].connector!=UINT32_MAX);for(size_t j=0;j<i;++j)nativeStreamJournalRequire(r.outputs[i].display!=r.outputs[j].display&&r.outputs[i].connector!=r.outputs[j].connector);}
}
inline std::string nativeStreamViewportCancelBytes(const NativeStreamViewportCancel& r){
  validateNativeStreamViewportCancel(r);std::string outputs="[";for(const auto& output:r.outputs){if(outputs.size()>1)outputs+=',';outputs+="{\"display\":"+jsonQuote(nativeStreamU64(output.display))+",\"connector\":"+std::to_string(output.connector)+'}';}
  const auto bytes="{\"schema\":1,\"kind\":\"native-stream-viewport-cancel\",\"requestKind\":"+jsonQuote(r.request.index()==0?"single":"tracks")+",\"request\":{"+nativeStreamCancelRequestFields(r.request)+"},\"deviceInstanceId\":"+jsonQuote(r.instance)+",\"profileOperation\":"+jsonQuote(r.profileOperation)+",\"resizeOperation\":"+jsonQuote(r.resizeOperation)+",\"profileBeforeSha256\":"+jsonQuote(r.profileBeforeSha256)+",\"profileBefore\":"+jsonQuote(r.profileBeforeBytes)+",\"baselineSha256\":"+jsonQuote(r.baselineSha256)+",\"baseline\":"+jsonQuote(r.baselineBytes)+",\"runtimeSha256\":"+jsonQuote(r.runtimeSha256)+",\"current\":"+nativeStreamLeaseBytes(r.current)+",\"scope\":"+nativeRecoveryScopeBytes(r.scope)+",\"lease\":"+jsonQuote(nativeStreamU64(r.lease))+",\"outputs\":"+outputs+"]}";
  nativeStreamJournalRequire(bytes.size()<=131072);return bytes;
}
inline NativeStreamViewportCancel parseNativeStreamViewportCancel(const std::string& bytes){
  nativeStreamJournalRequire(!bytes.empty()&&bytes.size()<=131072);const auto root=JsonParser::parse(bytes);nativeStreamJournalFields(root,16);nativeStreamJournalRequire(root.get("schema").number()==1&&root.get("kind").string()=="native-stream-viewport-cancel");
  NativeStreamViewportCancel r;const auto kind=root.get("requestKind").string();
  if(kind=="single"){nativeStreamJournalFields(root.get("request"),8);r.request=streamResizeRequest(root.get("request"));}
  else if(kind=="tracks"){nativeStreamJournalFields(root.get("request"),6);r.request=nativeTrackResizeRequest(root.get("request"));}
  else throw std::runtime_error("native_stream_cancel_request_invalid");
  r.instance=nativeStreamJournalString(root.get("deviceInstanceId"));r.profileOperation=nativeStreamJournalString(root.get("profileOperation"));r.resizeOperation=nativeStreamJournalString(root.get("resizeOperation"));r.profileBeforeSha256=nativeStreamJournalString(root.get("profileBeforeSha256"));r.profileBeforeBytes=nativeStreamJournalString(root.get("profileBefore"));r.baselineSha256=nativeStreamJournalString(root.get("baselineSha256"));r.baselineBytes=nativeStreamJournalString(root.get("baseline"));r.runtimeSha256=nativeStreamJournalString(root.get("runtimeSha256"));r.current=parseNativeStreamLease(root.get("current"));r.scope=parseNativeRecoveryScope(root.get("scope"));r.lease=parseNativeStreamU64(root.get("lease"));
  const auto* rows=std::get_if<Json::Array>(&root.get("outputs").value);nativeStreamJournalRequire(rows&&!rows->empty()&&rows->size()<=2);for(const auto& output:*rows){nativeStreamJournalFields(output,2);r.outputs.push_back({parseNativeStreamU64(output.get("display")),nativeStreamJournalU32(output.get("connector"))});}
  nativeStreamJournalRequire(nativeStreamViewportCancelBytes(r)==bytes);return r;
}
inline bool nativeStreamViewportCancelSourceMatches(const NativeStreamProfileRecord& profile,const NativeStreamViewportCancel& receipt,bool recovery=false){
  try{validateNativeStreamViewportCancel(receipt);if(nativeStreamProfileRecordBytes(profile)==receipt.profileBeforeBytes)return true;const auto before=parseNativeStreamProfileRecord(receipt.profileBeforeBytes);if(!recovery||(profile.phase!=NativeStreamProfilePhase::active&&profile.phase!=NativeStreamProfilePhase::recovery)||!profile.stopRequested||profile.revision<=before.revision)return false;auto normalized=profile;normalized.phase=before.phase;normalized.stopRequested=before.stopRequested;normalized.revision=before.revision;return nativeStreamProfileRecordBytes(normalized)==receipt.profileBeforeBytes;}catch(...){return false;}
}
inline bool nativeStreamViewportCancelledProfileMatches(const NativeStreamProfileRecord& profile,const NativeStreamViewportCancel& receipt){
  try{validateNativeStreamViewportCancel(receipt);const auto before=parseNativeStreamProfileRecord(receipt.profileBeforeBytes);if(profile.resize||profile.phase==NativeStreamProfilePhase::prepared||profile.revision<=before.revision||profile.resizeSequence!=before.resizeSequence||!sameNativeStreamLease(profile.current,receipt.current))return false;auto normalized=profile;normalized.resize=before.resize;normalized.phase=before.phase;normalized.stopRequested=before.stopRequested;normalized.revision=before.revision;return nativeStreamProfileRecordBytes(normalized)==receipt.profileBeforeBytes;}catch(...){return false;}
}
inline bool nativeStreamViewportCancelProof(const NativeStreamProfileRecord& profile,const NativeStreamProfileEvidence& proof,const NativeStreamCancelRequest& request,const NativeStreamViewportCancel& receipt,const std::string& runtimeBytes,bool recovery=false){
  try{
    if(request.index()!=receipt.request.index()||nativeStreamCancelRequestFields(request)!=nativeStreamCancelRequestFields(receipt.request)||!nativeStreamViewportCancelSourceMatches(profile,receipt,recovery)||!sameNativeStreamLease(proof.lease,receipt.current)||nativeBaselineDigest(runtimeBytes)!=receipt.runtimeSha256||!nativeRecoveryScopeMatches(receipt.scope,proof.scope))return false;
    const auto runtime=parseNativeRuntimeRecord(runtimeBytes);if(runtime.instance!=receipt.instance||runtime.phase!="stable"||runtime.generation!=receipt.current.generation||!runtime.baselineHash.empty()||runtime.outputs.size()!=receipt.current.targets.size())return false;
    for(size_t i=0;i<runtime.outputs.size();++i){const auto& a=runtime.outputs[i];const auto& b=receipt.current.targets[i];if(a.id!=b.id||a.width!=b.width||a.height!=b.height)return false;}
    const auto saved=parseNativeRecoveryBaseline(receipt.baselineBytes);return nativeStreamProfileSignatures(nativeActiveCcd(proof.topology))==nativeStreamProfileSignatures(nativeActiveCcd(saved.topology.ccd))&&nativeStreamProfileEvidenceMatches(profile,proof,NativeStreamProfileView::active);
  }catch(...){return false;}
}
inline NativeRuntimeJournalAccess nativeStreamViewportCancelFileAccess(const std::filesystem::path& path,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  nativeStreamJournalRequire(path.filename()==L"native-stream-viewport-commit.json");return nativeJournalFileAccess(path,131072,[](const auto& bytes){(void)parseNativeStreamViewportCancel(bytes);},std::move(trusted));
}
}
