#pragma once
#include "native_display_baseline_codec.hpp"
#include "owned_output_size.hpp"
#include "native_display_journal.hpp"
#include "native_stream_console_identity.hpp"
#include "native_stream_profile_payload.hpp"
#include <mutex>

namespace host {
// Private service journal, separate from the short-lived viewport rollback
// journal. No method queries or changes Windows display configuration. The
// service must authenticate its worker and serialize display operations before
// supplying fresh evidence. A returned ACK means durable bytes were read back;
// it is not evidence that a Windows operation or a network delivery succeeded.
inline constexpr size_t nativeStreamProfileJournalLimit=60000;
inline constexpr size_t nativeStreamProfilePacketLimit=65536;
inline constexpr size_t nativeStreamProfilePacketOverhead=4096;
enum class NativeStreamProfilePhase { prepared,active,restoring,restored,recovery };
inline const char* nativeStreamProfilePhaseName(NativeStreamProfilePhase phase){
  switch(phase){case NativeStreamProfilePhase::prepared:return "prepared";case NativeStreamProfilePhase::active:return "active";case NativeStreamProfilePhase::restoring:return "restoring";case NativeStreamProfilePhase::restored:return "restored";case NativeStreamProfilePhase::recovery:return "recovery";}throw std::runtime_error("native_stream_journal_invalid");
}
struct NativeStreamProfileKey {
  std::string stream,operation,instance;DWORD session=0;
  friend bool operator==(const NativeStreamProfileKey&,const NativeStreamProfileKey&)=default;
};
struct NativeStreamProfileLease {
  DWORD servicePid=0;uint32_t generation=0;std::vector<NativeDisplayTarget> targets;
};
struct NativeStreamProfileResize {
  std::string operation,requestSha256;uint32_t sequence=0;
};
struct NativeStreamProfileRecord {
  NativeStreamProfileKey key;
  NativeStreamProfilePhase phase=NativeStreamProfilePhase::prepared;
  uint32_t revision=1,resizeSequence=0;bool stopRequested=false;
  NativeStreamProfileLease entry,current;
  NativeRecoveryScope scope;
  NativeConsoleBinding console;
  NativeStreamPrimaryProfile profile;
  std::string profileSha256,launchSha256;
  std::optional<NativeStreamProfileResize> resize;
};
struct NativeStreamProfileAck {uint32_t revision=0;std::string sha256;};
struct NativeStreamProfileEvidence {
  NativeStreamProfileKey key;NativeStreamProfileLease lease;
  NativeRecoveryScope scope;NativeConsoleBinding console;NativeCcdData topology;
};
inline void nativeStreamJournalRequire(bool condition){if(!condition)throw std::runtime_error("native_stream_journal_invalid");}
inline bool nativeStreamJournalId(const std::string& value){return !value.empty()&&value.size()<=128&&value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:-")==std::string::npos;}
inline std::string nativeStreamJournalString(const Json& value){nativeStreamJournalRequire(std::holds_alternative<std::string>(value.value));return value.string();}
inline uint32_t nativeStreamJournalU32(const Json& value,uint32_t low=0,uint32_t high=UINT32_MAX){const auto n=value.number(-1);nativeStreamJournalRequire(nativeDisplayInteger(n,low,high));return uint32_t(n);}
inline LONG nativeStreamJournalI32(const Json& value){const auto n=value.number(-2147483649.0);nativeStreamJournalRequire(nativeDisplayInteger(n,INT32_MIN,INT32_MAX));return LONG(n);}
inline void nativeStreamJournalFields(const Json& value,size_t count){const auto* fields=std::get_if<Json::Object>(&value.value);nativeStreamJournalRequire(fields&&fields->size()==count);}
inline std::string nativeStreamLeaseBytes(const NativeStreamProfileLease& lease){
  nativeStreamJournalRequire(lease.servicePid&&lease.generation&&!lease.targets.empty()&&lease.targets.size()<=2);
  std::string targets="[";
  for(size_t i=0;i<lease.targets.size();++i){const auto& item=lease.targets[i];nativeStreamJournalRequire(item.id==std::to_string(i)&&ownedOutputSize(item.width,item.height));
    for(size_t j=0;j<i;++j)nativeStreamJournalRequire(!nativeSameLuid(item.adapter,lease.targets[j].adapter)||item.target!=lease.targets[j].target);
    if(i)targets+=',';targets+="{\"id\":"+jsonQuote(item.id)+",\"adapterHigh\":"+std::to_string(item.adapter.HighPart)+",\"adapterLow\":"+std::to_string(item.adapter.LowPart)+",\"target\":"+std::to_string(item.target)+",\"width\":"+std::to_string(item.width)+",\"height\":"+std::to_string(item.height)+'}';
  }
  return "{\"servicePid\":"+std::to_string(lease.servicePid)+",\"generation\":"+std::to_string(lease.generation)+",\"targets\":"+targets+"]}";
}
inline NativeStreamProfileLease parseNativeStreamLease(const Json& value){
  nativeStreamJournalFields(value,3);NativeStreamProfileLease result;result.servicePid=nativeStreamJournalU32(value.get("servicePid"),1);result.generation=nativeStreamJournalU32(value.get("generation"),1);
  const auto* items=std::get_if<Json::Array>(&value.get("targets").value);nativeStreamJournalRequire(items&&!items->empty()&&items->size()<=2);
  for(const auto& item:*items){nativeStreamJournalFields(item,6);result.targets.push_back({nativeStreamJournalString(item.get("id")),{nativeStreamJournalU32(item.get("adapterLow")),nativeStreamJournalI32(item.get("adapterHigh"))},nativeStreamJournalU32(item.get("target")),nativeStreamJournalU32(item.get("width")),nativeStreamJournalU32(item.get("height"))});}
  (void)nativeStreamLeaseBytes(result);return result;
}
inline bool sameNativeStreamLease(const NativeStreamProfileLease& a,const NativeStreamProfileLease& b){return nativeStreamLeaseBytes(a)==nativeStreamLeaseBytes(b);}
inline std::string nativeStreamConsoleBindingBytes(const NativeConsoleBinding& raw){
  const auto canonical=canonicalNativeConsoleBinding(raw);nativeStreamJournalRequire(canonical.has_value());const auto& value=*canonical;const auto& device=value.device;const auto& path=value.path;
  std::string ids="[";for(const auto& id:device.hardwareIds){if(ids.size()>1)ids+=',';ids+=jsonQuote(utf8(id));}
  return "{\"machineUuid\":"+jsonQuote(value.proof.machineUuid)+",\"model\":"+jsonQuote(value.proof.model)+",\"adapterHigh\":"+std::to_string(device.adapter.HighPart)+",\"adapterLow\":"+std::to_string(device.adapter.LowPart)+",\"interfacePath\":"+jsonQuote(utf8(device.interfacePath))+",\"instanceId\":"+jsonQuote(utf8(device.instanceId))+",\"enumerator\":"+jsonQuote(utf8(device.enumerator))+",\"hardwareIds\":"+ids+"],\"classGuid\":\"4d36e968-e325-11ce-bfc1-08002be10318\",\"present\":true,\"started\":true,\"sourceAdapterHigh\":"+std::to_string(path.sourceAdapter.HighPart)+",\"sourceAdapterLow\":"+std::to_string(path.sourceAdapter.LowPart)+",\"source\":"+std::to_string(path.source)+",\"targetAdapterHigh\":"+std::to_string(path.targetAdapter.HighPart)+",\"targetAdapterLow\":"+std::to_string(path.targetAdapter.LowPart)+",\"target\":"+std::to_string(path.target)+'}';
}
inline NativeConsoleBinding parseNativeStreamConsoleBinding(const Json& value){
  nativeStreamJournalFields(value,17);nativeStreamJournalRequire(value.get("classGuid").string()=="4d36e968-e325-11ce-bfc1-08002be10318"&&value.get("present").boolean()&&value.get("started").boolean());
  NativeConsoleBinding result;result.proof={nativeStreamJournalString(value.get("machineUuid")),nativeStreamJournalString(value.get("model"))};auto& device=result.device;
  device.adapter={nativeStreamJournalU32(value.get("adapterLow")),nativeStreamJournalI32(value.get("adapterHigh"))};device.interfacePath=wide(nativeStreamJournalString(value.get("interfacePath")));device.instanceId=wide(nativeStreamJournalString(value.get("instanceId")));device.enumerator=wide(nativeStreamJournalString(value.get("enumerator")));device.classGuid=nativeConsoleDisplayClass;device.present=device.started=true;
  const auto* ids=std::get_if<Json::Array>(&value.get("hardwareIds").value);nativeStreamJournalRequire(ids&&!ids->empty()&&ids->size()<=16);for(const auto& id:*ids)device.hardwareIds.push_back(wide(nativeStreamJournalString(id)));
  result.path={{nativeStreamJournalU32(value.get("sourceAdapterLow")),nativeStreamJournalI32(value.get("sourceAdapterHigh"))},{nativeStreamJournalU32(value.get("targetAdapterLow")),nativeStreamJournalI32(value.get("targetAdapterHigh"))},nativeStreamJournalU32(value.get("source")),nativeStreamJournalU32(value.get("target"))};
  nativeStreamJournalRequire(canonicalNativeConsoleBinding(result).has_value());return result;
}
inline void validateNativeStreamProfileRecord(const NativeStreamProfileRecord& record){
  nativeStreamJournalRequire(record.launchSha256.empty()||nativeRecoveryHex(record.launchSha256));
  nativeStreamJournalRequire(nativeStreamJournalId(record.key.stream)&&nativeRecoveryHex(record.key.operation)&&nativeDisplayInstanceId(record.key.instance)&&record.key.session&&record.key.session!=UINT32_MAX&&record.revision);
  (void)nativeStreamProfilePhaseName(record.phase);(void)nativeStreamLeaseBytes(record.entry);(void)nativeStreamLeaseBytes(record.current);
  nativeStreamJournalRequire(record.current.generation>=record.entry.generation&&record.current.targets.size()==record.entry.targets.size());
  auto profileLease=record.entry;profileLease.targets=record.profile.targets;nativeStreamJournalRequire(sameNativeStreamLease(record.entry,profileLease));
  const auto payload=nativeStreamProfilePayloadBytes(record.profile);nativeStreamJournalRequire(nativeRecoveryHex(record.profileSha256)&&record.profileSha256==nativeBaselineDigest(payload));
  const auto baseline=captureNativeTopologyBaseline(record.profile.before,record.profile.targets,"0");validateNativeRecoveryScope(record.scope,baseline);
  nativeStreamJournalRequire(record.scope.session==record.key.session&&nativeRecoveryScopeComplete(record.scope)&&record.profile.suppressedConsole.has_value());
  (void)nativeStreamConsoleBindingBytes(record.console);const auto& console=record.console.path;const auto& suppressed=*record.profile.suppressedConsole;
  nativeStreamJournalRequire(nativeSameLuid(console.sourceAdapter,suppressed.sourceAdapter)&&nativeSameLuid(console.targetAdapter,suppressed.targetAdapter)&&console.source==suppressed.source&&console.target==suppressed.target);
  const auto consoleScope=std::find_if(record.scope.foreign.begin(),record.scope.foreign.end(),[&](const auto& item){return nativeSameLuid(item.sourceAdapter,console.sourceAdapter)&&nativeSameLuid(item.targetAdapter,console.targetAdapter)&&item.source==console.source&&item.target==console.target;});
  nativeStreamJournalRequire(consoleScope!=record.scope.foreign.end());const auto canonicalConsole=*canonicalNativeConsoleBinding(record.console);
  const auto sourcePath=nativeConsoleAscii(wide(consoleScope->sourcePath),127),targetPath=nativeConsoleAscii(wide(consoleScope->targetPath),127);
  nativeStreamJournalRequire(sourcePath&&targetPath&&*sourcePath==canonicalConsole.device.interfacePath&&*targetPath==canonicalConsole.device.interfacePath);
  if(record.phase==NativeStreamProfilePhase::prepared)nativeStreamJournalRequire(!record.resize&&record.resizeSequence==0&&sameNativeStreamLease(record.entry,record.current));
  if(record.phase==NativeStreamProfilePhase::restoring||record.phase==NativeStreamProfilePhase::restored||record.phase==NativeStreamProfilePhase::recovery)nativeStreamJournalRequire(record.stopRequested);
  if(record.resize)nativeStreamJournalRequire((record.phase==NativeStreamProfilePhase::active||record.phase==NativeStreamProfilePhase::recovery)&&nativeRecoveryHex(record.resize->operation)&&nativeRecoveryHex(record.resize->requestSha256)&&record.resize->sequence&&record.resize->sequence==record.resizeSequence);
}
inline std::string nativeStreamProfileRecordBytes(const NativeStreamProfileRecord& record){
  validateNativeStreamProfileRecord(record);
  const auto resize=record.resize?"{\"operation\":"+jsonQuote(record.resize->operation)+",\"requestSha256\":"+jsonQuote(record.resize->requestSha256)+",\"sequence\":"+std::to_string(record.resize->sequence)+'}':std::string("null");
  // Preserve schema-1 bytes for historical records. Missing launch authority
  // remains missing; parsing never manufactures a restart-recovery proof.
  const auto bytes=std::string("{\"schema\":")+(record.launchSha256.empty()?"1":"2")+",\"kind\":\"native-stream-profile\",\"stream\":"+jsonQuote(record.key.stream)+",\"operation\":"+jsonQuote(record.key.operation)+",\"deviceInstanceId\":"+jsonQuote(record.key.instance)+",\"session\":"+std::to_string(record.key.session)+",\"phase\":"+jsonQuote(nativeStreamProfilePhaseName(record.phase))+",\"revision\":"+std::to_string(record.revision)+",\"resizeSequence\":"+std::to_string(record.resizeSequence)+",\"stopRequested\":"+(record.stopRequested?"true":"false")+",\"entry\":"+nativeStreamLeaseBytes(record.entry)+",\"current\":"+nativeStreamLeaseBytes(record.current)+",\"scope\":"+nativeRecoveryScopeBytes(record.scope)+",\"console\":"+nativeStreamConsoleBindingBytes(record.console)+",\"profileSha256\":"+jsonQuote(record.profileSha256)+",\"profile\":"+nativeStreamProfilePayloadBytes(record.profile)+",\"resize\":"+resize+(record.launchSha256.empty()?std::string():",\"launchSha256\":"+jsonQuote(record.launchSha256))+'}';
  nativeStreamJournalRequire(bytes.size()<=nativeStreamProfileJournalLimit&&bytes.size()+nativeStreamProfilePacketOverhead<=nativeStreamProfilePacketLimit);return bytes;
}
inline NativeStreamProfileRecord parseNativeStreamProfileRecord(const std::string& bytes){
  nativeStreamJournalRequire(!bytes.empty()&&bytes.size()<=nativeStreamProfileJournalLimit);const auto root=JsonParser::parse(bytes);
  const auto schema=root.get("schema").number();nativeStreamJournalRequire((schema==1||schema==2)&&root.get("kind").string()=="native-stream-profile");nativeStreamJournalFields(root,schema==1?17:18);NativeStreamProfileRecord record;
  if(schema==2){record.launchSha256=nativeStreamJournalString(root.get("launchSha256"));nativeStreamJournalRequire(nativeRecoveryHex(record.launchSha256));}
  record.key={nativeStreamJournalString(root.get("stream")),nativeStreamJournalString(root.get("operation")),nativeStreamJournalString(root.get("deviceInstanceId")),nativeStreamJournalU32(root.get("session"),1,UINT32_MAX-1)};
  const auto phase=nativeStreamJournalString(root.get("phase"));bool found=false;for(const auto candidate:{NativeStreamProfilePhase::prepared,NativeStreamProfilePhase::active,NativeStreamProfilePhase::restoring,NativeStreamProfilePhase::restored,NativeStreamProfilePhase::recovery})if(phase==nativeStreamProfilePhaseName(candidate)){record.phase=candidate;found=true;}nativeStreamJournalRequire(found);
  record.revision=nativeStreamJournalU32(root.get("revision"),1);record.resizeSequence=nativeStreamJournalU32(root.get("resizeSequence"));nativeStreamJournalRequire(std::holds_alternative<bool>(root.get("stopRequested").value));record.stopRequested=root.get("stopRequested").boolean();
  record.entry=parseNativeStreamLease(root.get("entry"));record.current=parseNativeStreamLease(root.get("current"));record.scope=parseNativeRecoveryScope(root.get("scope"));record.console=parseNativeStreamConsoleBinding(root.get("console"));record.profileSha256=nativeStreamJournalString(root.get("profileSha256"));
  // Nested fixed codecs validate shape first; canonical reencoding of the
  // complete record below rejects aliases, field order and alternate spelling.
  record.profile=parseNativeStreamProfilePayload(root.get("profile"));
  const auto& resize=root.get("resize");if(!std::holds_alternative<std::nullptr_t>(resize.value)){nativeStreamJournalFields(resize,3);record.resize=NativeStreamProfileResize{nativeStreamJournalString(resize.get("operation")),nativeStreamJournalString(resize.get("requestSha256")),nativeStreamJournalU32(resize.get("sequence"),1)};}
  nativeStreamJournalRequire(nativeStreamProfileRecordBytes(record)==bytes);return record;
}
inline NativeRuntimeJournalAccess nativeStreamProfileFileAccess(const std::filesystem::path& path,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  // Do not accidentally replace the independent short-lived viewport journal.
  nativeStreamJournalRequire(path.filename()==L"native-stream-profile.json");
  return nativeJournalFileAccess(path,nativeStreamProfileJournalLimit,[](const std::string& bytes){(void)parseNativeStreamProfileRecord(bytes);},std::move(trusted));
}
enum class NativeStreamProfileView {before,active,restorable,restored};
inline bool nativeStreamProfileEvidenceMatches(const NativeStreamProfileRecord& record,const NativeStreamProfileEvidence& proof,NativeStreamProfileView view,bool allowNewLease=false){
  try{
    if(!(record.key==proof.key)||!sameNativeConsoleBinding(record.console,proof.console)||!nativeRecoveryLogonMatches(record.scope,proof.scope)||(!allowNewLease&&!sameNativeStreamLease(record.current,proof.lease))||proof.lease.targets.size()!=record.entry.targets.size())return false;
    (void)nativeStreamLeaseBytes(proof.lease);const auto active=validateNativeStreamProfileCcd(proof.topology,proof.lease.targets);
    validateNativeRecoveryScope(proof.scope,captureNativeTopologyBaseline(active,proof.lease.targets,"0"));if(!nativeRecoveryScopeComplete(proof.scope))return false;
    auto expectedScope=record.scope;const bool consoleActive=std::any_of(active.paths.begin(),active.paths.end(),[&](const auto& path){return nativeStreamConsolePathMatches(path,*record.profile.suppressedConsole);});
    if(!consoleActive){const auto& console=*record.profile.suppressedConsole;expectedScope.foreign.erase(std::remove_if(expectedScope.foreign.begin(),expectedScope.foreign.end(),[&](const auto& item){return nativeSameLuid(item.sourceAdapter,console.sourceAdapter)&&nativeSameLuid(item.targetAdapter,console.targetAdapter)&&item.source==console.source&&item.target==console.target;}),expectedScope.foreign.end());}
    if(!nativeRecoveryScopeMatches(expectedScope,proof.scope))return false;
    const auto foreign=nativeStreamProfileSignatures(active,proof.lease.targets);
    if(view==NativeStreamProfileView::before)return sameNativeStreamLease(record.entry,proof.lease)&&nativeStreamProfileSignatures(record.profile.before)==nativeStreamProfileSignatures(active);
    if(view==NativeStreamProfileView::restored)return consoleActive&&nativeStreamProfileSignatures(record.profile.before,record.entry.targets)==foreign&&nativeStreamOriginalPrimaryMatches(record.profile,active,proof.lease.targets);
    if(view==NativeStreamProfileView::restorable){(void)planNativeStreamPrimaryRestore(record.profile,proof.topology,proof.lease.targets);return true;}
    if(consoleActive||nativeStreamProfileSignatures(record.profile.applied,record.entry.targets)!=foreign)return false;
    const auto zero=std::find_if(active.paths.begin(),active.paths.end(),[&](const auto& path){return nativeSameTarget(path,proof.lease.targets.front());});if(zero==active.paths.end())return false;const auto& source=nativeCcdSource(active,*zero);return !source.position.x&&!source.position.y;
  }catch(...){return false;}
}
class NativeStreamProfileJournal {
 public:
  // Callbacks run under this object's mutex and must not reenter the journal.
  // Validate an already obtained native transaction receipt; never wait for
  // worker/IPC work from a callback while holding display/service locks.
  using TransitionProof=std::function<bool(const NativeStreamProfileRecord&,const NativeStreamProfileEvidence&)>;
  explicit NativeStreamProfileJournal(NativeRuntimeJournalAccess access):access_(std::move(access)){}
  std::optional<NativeStreamProfileRecord> load(const std::string& instance){
    std::lock_guard lock(mutex_);nativeStreamJournalRequire(nativeDisplayInstanceId(instance));if(loaded_){nativeStreamJournalRequire(instance_==instance);return record_;}
    const auto bytes=access_.load();std::optional<NativeStreamProfileRecord> previous;if(bytes){previous=parseNativeStreamProfileRecord(*bytes);nativeStreamJournalRequire(previous->key.instance==instance);}
    instance_=instance;expected_=bytes;record_=std::move(previous);loaded_=true;return record_;
  }
  std::optional<NativeStreamProfileRecord> record()const{std::lock_guard lock(mutex_);return record_;}
  std::optional<NativeStreamProfileAck> ack()const{std::lock_guard lock(mutex_);return ackLocked();}
  bool verified(const NativeStreamProfileAck& expected)const{std::lock_guard lock(mutex_);try{return matchesLocked(expected)&&live_&&access_.load()==expected_;}catch(...){return false;}}
  bool streamingAllowed()const{std::lock_guard lock(mutex_);return live_&&!uncertain_&&record_&&record_->phase==NativeStreamProfilePhase::active&&!record_->stopRequested&&!record_->resize;}
  std::optional<NativeStreamProfileAck> prepare(NativeStreamProfileRecord value,const NativeStreamProfileEvidence& proof){
    std::lock_guard lock(mutex_);try{
      if(!loaded_||uncertain_||value.key.instance!=instance_||(record_&&(record_->phase!=NativeStreamProfilePhase::restored||record_->key.operation==value.key.operation))||value.phase!=NativeStreamProfilePhase::prepared||value.revision!=1||value.stopRequested||value.resize||value.resizeSequence)return {};
      validateNativeStreamProfileRecord(value);if(!nativeStreamProfileEvidenceMatches(value,proof,NativeStreamProfileView::before))return {};const auto result=saveLocked(std::move(value));if(result)live_=owner_=true;return result;
    }catch(...){return {};}
  }
  std::optional<NativeStreamProfileAck> activate(const NativeStreamProfileAck& expected,const NativeStreamProfileEvidence& proof){
    std::lock_guard lock(mutex_);if(!matchesLocked(expected)||!live_||record_->phase!=NativeStreamProfilePhase::prepared||record_->stopRequested||!nativeStreamProfileEvidenceMatches(*record_,proof,NativeStreamProfileView::active))return {};
    auto next=*record_;next.phase=NativeStreamProfilePhase::active;return advanceLocked(std::move(next));
  }
  std::optional<NativeStreamProfileAck> reserveResize(const NativeStreamProfileAck& expected,const std::string& operation,const std::string& requestSha256){
    std::lock_guard lock(mutex_);if(!matchesLocked(expected)||!live_||record_->phase!=NativeStreamProfilePhase::active||record_->stopRequested||record_->resize||record_->resizeSequence==UINT32_MAX||!nativeRecoveryHex(operation)||!nativeRecoveryHex(requestSha256))return {};
    auto next=*record_;next.resize=NativeStreamProfileResize{operation,requestSha256,++next.resizeSequence};return advanceLocked(std::move(next));
  }
  // The proof callback must confirm the completed matching native viewport
  // transaction (old lease + operation/request + new lease), not merely shape.
  std::optional<NativeStreamProfileAck> commitResize(const NativeStreamProfileAck& expected,const NativeStreamProfileEvidence& proof,const TransitionProof& completed){
    std::lock_guard lock(mutex_);try{
      if(!matchesLocked(expected)||!live_||record_->phase!=NativeStreamProfilePhase::active||!record_->resize||!completed||proof.lease.servicePid!=record_->current.servicePid||proof.lease.generation<=record_->current.generation||!nativeStreamProfileEvidenceMatches(*record_,proof,NativeStreamProfileView::active,true)||!completed(*record_,proof))return {};
      auto next=*record_;next.current=proof.lease;next.resize.reset();return advanceLocked(std::move(next));
    }catch(...){return {};}
  }
  std::optional<NativeStreamProfileAck> cancelResize(const NativeStreamProfileAck& expected,const NativeStreamProfileEvidence& unchanged,const TransitionProof& completed={}){
    std::lock_guard lock(mutex_);try{if(!matchesLocked(expected)||!live_||record_->phase!=NativeStreamProfilePhase::active||!record_->resize||!completed||!nativeStreamProfileEvidenceMatches(*record_,unchanged,NativeStreamProfileView::active)||!completed(*record_,unchanged))return {};
    auto next=*record_;next.resize.reset();return advanceLocked(std::move(next));}catch(...){return {};}
  }
  std::optional<NativeStreamProfileAck> requestStop(const NativeStreamProfileAck& expected){
    std::lock_guard lock(mutex_);if(!matchesLocked(expected)||record_->phase==NativeStreamProfilePhase::restored)return {};if(record_->stopRequested)return ackLocked();auto next=*record_;next.stopRequested=true;return advanceLocked(std::move(next));
  }
  std::optional<NativeStreamProfileAck> beginRestore(const NativeStreamProfileAck& expected,const NativeStreamProfileEvidence& proof){
    std::lock_guard lock(mutex_);if(!matchesLocked(expected)||!live_||record_->resize||record_->phase==NativeStreamProfilePhase::restored||!nativeStreamProfileEvidenceMatches(*record_,proof,NativeStreamProfileView::restorable))return {};
    auto next=*record_;next.phase=NativeStreamProfilePhase::restoring;next.stopRequested=true;return advanceLocked(std::move(next));
  }
  // Stop-time restoration by the SAME journal instance that prepared this
  // profile, for a viewport reservation its worker never forwarded to a native
  // driver mutation (the caller proves that). Fresh evidence must still show
  // the exact reserved lease, so a pending, recovered or committed service
  // transaction cannot be dropped here; those keep the receipt/exit-fenced
  // recovery path. This only begins console restoration, never resumes media.
  std::optional<NativeStreamProfileAck> beginUnchangedRestore(const NativeStreamProfileAck& expected,const NativeStreamProfileEvidence& proof){
    std::lock_guard lock(mutex_);try{
      if(!matchesLocked(expected)||!owner_||!record_->resize||!record_->stopRequested||(record_->phase!=NativeStreamProfilePhase::active&&record_->phase!=NativeStreamProfilePhase::recovery)||!sameNativeStreamLease(record_->current,proof.lease)||!nativeStreamProfileEvidenceMatches(*record_,proof,NativeStreamProfileView::restorable))return {};
      auto next=*record_;next.resize.reset();next.phase=NativeStreamProfilePhase::restoring;const auto result=advanceLocked(std::move(next));if(result)live_=true;return result;
    }catch(...){return {};}
  }
  // Reopening an active/prepared journal never resumes streaming. An exited
  // worker fence plus fresh same-logon/device/console evidence is mandatory.
  // A changed service lease or interrupted resize needs explicit reconciliation
  // against the native runtime transaction journal before its binding advances.
  std::optional<NativeStreamProfileAck> beginRecoveryRestore(const NativeStreamProfileAck& expected,const NativeStreamProfileEvidence& proof,const std::function<bool(const NativeStreamProfileRecord&)>& workerExited,const TransitionProof& reconciled={}){
    std::lock_guard lock(mutex_);try{
      if(!matchesLocked(expected)||live_||record_->phase==NativeStreamProfilePhase::restored||!workerExited||!workerExited(*record_))return {};
      const bool changed=!sameNativeStreamLease(record_->current,proof.lease);if(changed&&(proof.lease.generation<record_->current.generation||!reconciled))return {};
      if((changed||record_->resize)&&(!reconciled||!reconciled(*record_,proof)))return {};
      if(!nativeStreamProfileEvidenceMatches(*record_,proof,NativeStreamProfileView::restorable,changed))return {};
      auto next=*record_;next.current=proof.lease;next.resize.reset();next.phase=NativeStreamProfilePhase::restoring;next.stopRequested=true;const auto result=advanceLocked(std::move(next));if(result)live_=true;return result;
    }catch(...){return {};}
  }
  std::optional<NativeStreamProfileAck> restored(const NativeStreamProfileAck& expected,const NativeStreamProfileEvidence& proof){
    std::lock_guard lock(mutex_);if(!matchesLocked(expected)||!live_||record_->phase!=NativeStreamProfilePhase::restoring||record_->resize||!nativeStreamProfileEvidenceMatches(*record_,proof,NativeStreamProfileView::restored))return {};
    auto next=*record_;next.phase=NativeStreamProfilePhase::restored;const auto result=advanceLocked(std::move(next));if(result)live_=false;return result;
  }
  std::optional<NativeStreamProfileAck> recovery(const NativeStreamProfileAck& expected){
    std::lock_guard lock(mutex_);if(!matchesLocked(expected)||record_->phase==NativeStreamProfilePhase::restored)return {};auto next=*record_;next.phase=NativeStreamProfilePhase::recovery;next.stopRequested=true;const auto result=advanceLocked(std::move(next));live_=false;return result;
  }
 private:
  std::optional<NativeStreamProfileAck> ackLocked()const{if(!loaded_||uncertain_||!record_||!expected_)return {};return NativeStreamProfileAck{record_->revision,nativeBaselineDigest(*expected_)};}
  bool matchesLocked(const NativeStreamProfileAck& expected)const{const auto actual=ackLocked();return actual&&actual->revision==expected.revision&&nativeRecoveryHex(expected.sha256)&&actual->sha256==expected.sha256;}
  std::optional<NativeStreamProfileAck> saveLocked(NativeStreamProfileRecord value){
    try{const auto bytes=nativeStreamProfileRecordBytes(value);if(!access_.save(expected_,bytes)||access_.load()!=std::optional<std::string>(bytes)){uncertain_=true;return {};}expected_=bytes;record_=std::move(value);return ackLocked();}catch(...){uncertain_=true;return {};}
  }
  std::optional<NativeStreamProfileAck> advanceLocked(NativeStreamProfileRecord value){if(!record_||record_->revision==UINT32_MAX)return {};value.revision=record_->revision+1;return saveLocked(std::move(value));}
  // owner_: this instance durably prepared the record (never set by load()).
  NativeRuntimeJournalAccess access_;mutable std::mutex mutex_;bool loaded_=false,live_=false,owner_=false,uncertain_=false;std::string instance_;std::optional<std::string> expected_;std::optional<NativeStreamProfileRecord> record_;
};
} // namespace host
