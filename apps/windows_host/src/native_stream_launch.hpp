#pragma once
#include "native_stream_profile_journal.hpp"

namespace host {
// PID alone is not a lifetime. Keep the creation FILETIME losslessly, including
// when persisted through the JSON codec (whose numbers are doubles).
struct NativeStreamProcessIdentity {
  DWORD pid=0;uint64_t created=0;
  bool operator==(const NativeStreamProcessIdentity&)const=default;
};
inline NativeStreamProcessIdentity nativeStreamProcessIdentity(HANDLE process){
  FILETIME creation{},exit{},kernel{},user{};
  if(!GetProcessTimes(process,&creation,&exit,&kernel,&user))return {};
  ULARGE_INTEGER time{};time.LowPart=creation.dwLowDateTime;time.HighPart=creation.dwHighDateTime;
  return {GetProcessId(process),time.QuadPart};
}
enum class NativeStreamProcessState {unknown,running,gone};
struct NativeStreamProcessObservation {
  // The open process handle stays alive for as long as this proof is used.
  WinHandle process;NativeStreamProcessState state=NativeStreamProcessState::unknown;
};
inline NativeStreamProcessState classifyNativeStreamProcess(const NativeStreamProcessIdentity& expected,const NativeStreamProcessIdentity& observed,DWORD wait){
  if(!expected.pid||expected.pid==UINT32_MAX||!expected.created||observed.pid!=expected.pid||!observed.created)return NativeStreamProcessState::unknown;
  if(observed.created!=expected.created)return NativeStreamProcessState::gone; // Reused PID; never terminate it.
  if(wait==WAIT_OBJECT_0)return NativeStreamProcessState::gone;
  return wait==WAIT_TIMEOUT?NativeStreamProcessState::running:NativeStreamProcessState::unknown;
}
inline NativeStreamProcessObservation observeNativeStreamProcess(const NativeStreamProcessIdentity& expected){
  NativeStreamProcessObservation result;
  if(!expected.pid||expected.pid==UINT32_MAX||!expected.created)return result;
  result.process=WinHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,expected.pid));
  if(!result.process.value){if(GetLastError()==ERROR_INVALID_PARAMETER)result.state=NativeStreamProcessState::gone;return result;}
  result.state=classifyNativeStreamProcess(expected,nativeStreamProcessIdentity(result.process.value),WaitForSingleObject(result.process.value,0));return result;
}
inline std::string nativeStreamU64(uint64_t value){
  std::string result(16,'0');for(size_t i=0;i<16;++i){result[15-i]="0123456789abcdef"[value&15];value>>=4;}return result;
}
inline uint64_t parseNativeStreamU64(const Json& value){
  const auto text=nativeStreamJournalString(value);nativeStreamJournalRequire(text.size()==16&&text.find_first_not_of("0123456789abcdef")==std::string::npos);
  uint64_t result=0;for(const auto c:text)result=(result<<4)|uint64_t(c<='9'?c-'0':c-'a'+10);return result;
}
struct NativeStreamLaunchOutput {uint64_t display=0;uint32_t connector=0;};
struct NativeStreamLaunchReceipt {
  NativeStreamProfileKey key;
  NativeStreamProcessIdentity service,worker;
  NativeRecoveryScope scope; // Login identity only. Geometry belongs to the profile journal.
  NativeStreamProfileLease entry;
  uint64_t lease=0;std::vector<NativeStreamLaunchOutput> outputs;
};
inline void validateNativeStreamLaunch(const NativeStreamLaunchReceipt& value){
  nativeStreamJournalRequire(nativeStreamJournalId(value.key.stream)&&nativeRecoveryHex(value.key.operation)&&nativeDisplayInstanceId(value.key.instance)&&value.key.session&&value.key.session!=UINT32_MAX);
  for(const auto& process:{value.service,value.worker})nativeStreamJournalRequire(process.pid&&process.pid!=UINT32_MAX&&process.created);
  nativeStreamJournalRequire(value.service.pid!=value.worker.pid&&value.entry.servicePid==value.service.pid);
  nativeStreamJournalRequire(value.scope.session==value.key.session&&nativeRecoveryHex(value.scope.logon)&&value.scope.tick&&value.scope.tick<=INT64_MAX&&value.scope.foreign.empty());
  (void)nativeStreamLeaseBytes(value.entry);
  nativeStreamJournalRequire(value.lease>=0x6000000000000000ull&&value.outputs.size()==value.entry.targets.size());
  for(size_t i=0;i<value.outputs.size();++i){nativeStreamJournalRequire(value.outputs[i].display!=0);for(size_t j=0;j<i;++j)nativeStreamJournalRequire(value.outputs[i].display!=value.outputs[j].display&&value.outputs[i].connector!=value.outputs[j].connector);}
}
inline std::string nativeStreamLaunchBytes(const NativeStreamLaunchReceipt& value){
  validateNativeStreamLaunch(value);
  const auto process=[](const NativeStreamProcessIdentity& p){return "{\"pid\":"+std::to_string(p.pid)+",\"created\":"+jsonQuote(nativeStreamU64(p.created))+'}';};
  std::string outputs="[";for(const auto& output:value.outputs){if(outputs.size()>1)outputs+=',';outputs+="{\"display\":"+jsonQuote(nativeStreamU64(output.display))+",\"connector\":"+std::to_string(output.connector)+'}';}
  return "{\"schema\":1,\"kind\":\"native-stream-launch\",\"stream\":"+jsonQuote(value.key.stream)+",\"operation\":"+jsonQuote(value.key.operation)+",\"deviceInstanceId\":"+jsonQuote(value.key.instance)+",\"session\":"+std::to_string(value.key.session)+",\"service\":"+process(value.service)+",\"worker\":"+process(value.worker)+",\"scope\":"+nativeRecoveryScopeBytes(value.scope)+",\"entry\":"+nativeStreamLeaseBytes(value.entry)+",\"lease\":"+jsonQuote(nativeStreamU64(value.lease))+",\"outputs\":"+outputs+"]}";
}
inline NativeStreamLaunchReceipt parseNativeStreamLaunch(const std::string& bytes){
  nativeStreamJournalRequire(!bytes.empty()&&bytes.size()<=8192);const auto root=JsonParser::parse(bytes);nativeStreamJournalFields(root,12);
  nativeStreamJournalRequire(root.get("schema").number()==1&&root.get("kind").string()=="native-stream-launch");
  NativeStreamLaunchReceipt value;value.key={nativeStreamJournalString(root.get("stream")),nativeStreamJournalString(root.get("operation")),nativeStreamJournalString(root.get("deviceInstanceId")),nativeStreamJournalU32(root.get("session"),1,UINT32_MAX-1)};
  const auto process=[](const Json& p){nativeStreamJournalFields(p,2);return NativeStreamProcessIdentity{nativeStreamJournalU32(p.get("pid"),1,UINT32_MAX-1),parseNativeStreamU64(p.get("created"))};};
  value.service=process(root.get("service"));value.worker=process(root.get("worker"));value.scope=parseNativeRecoveryScope(root.get("scope"));value.entry=parseNativeStreamLease(root.get("entry"));value.lease=parseNativeStreamU64(root.get("lease"));
  const auto* outputs=std::get_if<Json::Array>(&root.get("outputs").value);nativeStreamJournalRequire(outputs&&!outputs->empty()&&outputs->size()<=2);
  for(const auto& output:*outputs){nativeStreamJournalFields(output,2);value.outputs.push_back({parseNativeStreamU64(output.get("display")),nativeStreamJournalU32(output.get("connector"))});}
  nativeStreamJournalRequire(nativeStreamLaunchBytes(value)==bytes);return value;
}
inline NativeRuntimeJournalAccess nativeStreamLaunchFileAccess(const std::filesystem::path& path,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  nativeStreamJournalRequire(path.filename()==L"native-stream-launch.json");
  return nativeJournalFileAccess(path,8192,[](const std::string& bytes){(void)parseNativeStreamLaunch(bytes);},std::move(trusted));
}
// Only the serialized service writes this receipt, before sending session.start.
// A stopped worker, a completed prior profile, CAS and durable readback are all
// required; receipt replacement cannot discard unresolved recovery authority.
inline std::optional<std::string> saveNativeStreamLaunch(const NativeRuntimeJournalAccess& access,const NativeRuntimeJournalAccess& profiles,const NativeStreamLaunchReceipt& value,const std::function<bool(const NativeStreamProcessIdentity&)>& gone){
  try{
    const auto desired=nativeStreamLaunchBytes(value);const auto previous=access.load(),profile=profiles.load();
    if(profile){const auto held=parseNativeStreamProfileRecord(*profile);if(held.phase!=NativeStreamProfilePhase::restored||held.key.operation==value.key.operation)return {};}
    if(previous){
      const auto held=parseNativeStreamLaunch(*previous);
      if(profile){const auto completed=parseNativeStreamProfileRecord(*profile);if(completed.key.operation==held.key.operation&&completed.launchSha256!=nativeBaselineDigest(*previous))return {};}
      if(*previous==desired)return nativeBaselineDigest(desired);if(held.key.operation==value.key.operation||!gone||!gone(held.worker))return {};
    }
    // The old worker may have prepared its profile between the first read and
    // the exit proof. Re-read after exit; never replace that new checkpoint.
    if(profiles.load()!=profile)return {};
    if(!access.save(previous,desired)||access.load()!=std::optional{desired})return {};
    return nativeBaselineDigest(desired);
  }catch(...){return {};}
}
inline bool nativeStreamLaunchMatches(const NativeStreamLaunchReceipt& launch,const NativeStreamProfileRecord& profile){
  try{return !profile.launchSha256.empty()&&nativeBaselineDigest(nativeStreamLaunchBytes(launch))==profile.launchSha256&&launch.key==profile.key&&sameNativeStreamLease(launch.entry,profile.entry)&&nativeRecoveryLogonMatches(launch.scope,profile.scope);}catch(...){return false;}
}
// Fixed diagnostic code for the quarantine decision below ("none" = released).
// The boolean form is derived from this single implementation.
inline const char* nativeStreamLaunchQuarantineReason(const NativeRuntimeJournalAccess& profiles,const NativeRuntimeJournalAccess& launches,const NativeStreamProcessIdentity& currentService,const std::function<NativeStreamProcessState(const NativeStreamProcessIdentity&)>& observe){
  try{
    if(!currentService.pid||!currentService.created||!observe)return "service_identity";
    const auto profile=profiles.load(),launch=launches.load();std::optional<NativeStreamProfileRecord> held;
    if(profile){held=parseNativeStreamProfileRecord(*profile);if(held->phase!=NativeStreamProfilePhase::restored)return "profile_not_restored";}
    if(!launch)return held&&!held->launchSha256.empty()?"profile_without_launch":"none";
    const auto receipt=parseNativeStreamLaunch(*launch);
    // A newer launch can fail before preparing a profile. Its own exited
    // worker is sufficient to retire that launch; it cannot alter the older,
    // already-restored geometry. Matching operations must still bind exactly.
    if(held&&held->key.operation==receipt.key.operation&&!nativeStreamLaunchMatches(receipt,*held))return "launch_profile_mismatch";
    if(receipt.service!=currentService&&observe(receipt.service)!=NativeStreamProcessState::gone)return "launch_service_alive";
    if(observe(receipt.worker)!=NativeStreamProcessState::gone)return "launch_worker_alive";
    return profiles.load()!=profile||launches.load()!=launch?"records_changed":"none";
  }catch(...){return "records_unreadable";}
}
inline bool nativeStreamLaunchQuarantined(const NativeRuntimeJournalAccess& profiles,const NativeRuntimeJournalAccess& launches,const NativeStreamProcessIdentity& currentService,const std::function<NativeStreamProcessState(const NativeStreamProcessIdentity&)>& observe){
  return std::string_view(nativeStreamLaunchQuarantineReason(profiles,launches,currentService,observe))!="none";
}
inline bool nativeStreamUnpreparedLaunchRetirable(const NativeRuntimeJournalAccess& profiles,const NativeRuntimeJournalAccess& launches,const NativeStreamProfileKey& key,uint32_t generation,const NativeStreamProcessIdentity& service,bool startDispatched,bool originalExited,const std::function<NativeStreamProcessState(const NativeStreamProcessIdentity&)>& observe){
  try{
    if(!originalExited||!generation||!nativeStreamJournalId(key.stream)||!nativeRecoveryHex(key.operation)||!nativeDisplayInstanceId(key.instance)||!key.session||key.session==UINT32_MAX)return false;
    const auto profile=profiles.load(),launch=launches.load();
    // A current-operation checkpoint (even restored with a lost ACK) needs the
    // normal fresh-topology recovery path, not this no-mutation retirement.
    if(profile){const auto held=parseNativeStreamProfileRecord(*profile);if(held.phase!=NativeStreamProfilePhase::restored||held.key.operation==key.operation)return false;}
    if(startDispatched){if(!launch)return false;const auto receipt=parseNativeStreamLaunch(*launch);if(receipt.key!=key||receipt.service!=service||receipt.entry.generation!=generation)return false;}
    if(nativeStreamLaunchQuarantined(profiles,launches,service,observe))return false;
    return profiles.load()==profile&&launches.load()==launch;
  }catch(...){return false;}
}
// For the original worker and same-service cleanup only. Restart reclaim has a
// distinct future admission path; a new service PID never inherits authority.
inline uint32_t nativeStreamLaunchWorkerProofFailure(const NativeStreamLaunchReceipt& launch,const NativeStreamProfileRecord& profile,DWORD servicePid,const NativeStreamProcessIdentity& current,bool recoveryWorker,HANDLE verifiedService=nullptr){
  try{
    if(profile.launchSha256.empty()||nativeBaselineDigest(nativeStreamLaunchBytes(launch))!=profile.launchSha256)return 1;
    if(launch.key!=profile.key)return 2;
    if(!sameNativeStreamLease(launch.entry,profile.entry))return 3;
    if(!nativeRecoveryLogonMatches(launch.scope,profile.scope))return 4;
    if(launch.service.pid!=servicePid)return 5;
    if(!current.pid||!current.created||current.pid==servicePid)return 6;
    if(verifiedService){if(nativeStreamProcessIdentity(verifiedService)!=launch.service)return 7;if(WaitForSingleObject(verifiedService,0)!=WAIT_TIMEOUT)return 8;}
    else {const auto service=observeNativeStreamProcess(launch.service);if(service.state!=NativeStreamProcessState::running)return 9;}
    if(!recoveryWorker)return current==launch.worker?0:10;
    if(current==launch.worker)return 11;
    const auto worker=observeNativeStreamProcess(launch.worker);return worker.state==NativeStreamProcessState::gone?0:12;
  }catch(...){return 13;}
}
inline bool nativeStreamLaunchWorkerProof(const NativeStreamLaunchReceipt& launch,const NativeStreamProfileRecord& profile,DWORD servicePid,const NativeStreamProcessIdentity& current,bool recoveryWorker,HANDLE verifiedService=nullptr){
  return nativeStreamLaunchWorkerProofFailure(launch,profile,servicePid,current,recoveryWorker,verifiedService)==0;
}
}
