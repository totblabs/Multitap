#pragma once
#include "native_stream_expired_console.hpp"
#include "native_stream_launch.hpp"

namespace host {
struct NativeStreamExpiredConsoleExecutor {
  NativeStreamProcessIdentity service,worker;
  bool operator==(const NativeStreamExpiredConsoleExecutor&)const=default;
};
struct NativeStreamExpiredConsoleRecord {
  std::string operation,profileSha256,launchSha256,contextSha256,planSha256,beforeSha256,restoredSha256,ledgerSha256;
  std::string phase="prepared",previousSha256;uint32_t revision=1;
  NativeStreamExpiredConsoleExecutor executor;
};
inline std::string nativeExpiredExecutorBytes(const NativeStreamExpiredConsoleExecutor& value){
  for(const auto& p:{value.service,value.worker})nativeStreamJournalRequire(p.pid&&p.pid!=UINT32_MAX&&p.created);
  nativeStreamJournalRequire(value.service.pid!=value.worker.pid);
  const auto process=[](const auto& p){return "{\"pid\":"+std::to_string(p.pid)+",\"created\":"+jsonQuote(nativeStreamU64(p.created))+'}';};
  return "{\"service\":"+process(value.service)+",\"worker\":"+process(value.worker)+'}';
}
inline std::string nativeStreamExpiredConsoleRecordBytes(const NativeStreamExpiredConsoleRecord& value){
  for(const auto& hash:{value.operation,value.profileSha256,value.launchSha256,value.contextSha256,value.planSha256,value.beforeSha256,value.restoredSha256,value.ledgerSha256})nativeStreamJournalRequire(nativeRecoveryHex(hash));
  nativeStreamJournalRequire((value.phase=="prepared"||value.phase=="completed")&&value.revision&&value.revision<=32&&((value.revision==1&&value.previousSha256.empty())||(value.revision>1&&nativeRecoveryHex(value.previousSha256))));
  return "{\"schema\":1,\"kind\":\"native-stream-expired-console\",\"operation\":"+jsonQuote(value.operation)+",\"profileSha256\":"+jsonQuote(value.profileSha256)+",\"launchSha256\":"+jsonQuote(value.launchSha256)+",\"contextSha256\":"+jsonQuote(value.contextSha256)+",\"planSha256\":"+jsonQuote(value.planSha256)+",\"beforeSha256\":"+jsonQuote(value.beforeSha256)+",\"restoredSha256\":"+jsonQuote(value.restoredSha256)+",\"ledgerSha256\":"+jsonQuote(value.ledgerSha256)+",\"phase\":"+jsonQuote(value.phase)+",\"revision\":"+std::to_string(value.revision)+",\"previousSha256\":"+jsonQuote(value.previousSha256)+",\"executor\":"+nativeExpiredExecutorBytes(value.executor)+'}';
}
inline NativeStreamExpiredConsoleRecord parseNativeStreamExpiredConsoleRecord(const std::string& bytes){
  nativeStreamJournalRequire(!bytes.empty()&&bytes.size()<=4096);const auto value=JsonParser::parse(bytes);nativeStreamJournalFields(value,14);nativeStreamJournalRequire(value.get("schema").number()==1&&value.get("kind").string()=="native-stream-expired-console");
  NativeStreamExpiredConsoleRecord result;result.operation=nativeStreamJournalString(value.get("operation"));result.profileSha256=nativeStreamJournalString(value.get("profileSha256"));result.launchSha256=nativeStreamJournalString(value.get("launchSha256"));result.contextSha256=nativeStreamJournalString(value.get("contextSha256"));result.planSha256=nativeStreamJournalString(value.get("planSha256"));result.beforeSha256=nativeStreamJournalString(value.get("beforeSha256"));result.restoredSha256=nativeStreamJournalString(value.get("restoredSha256"));result.phase=nativeStreamJournalString(value.get("phase"));result.previousSha256=nativeStreamJournalString(value.get("previousSha256"));result.revision=nativeStreamJournalU32(value.get("revision"),1,32);
  result.ledgerSha256=nativeStreamJournalString(value.get("ledgerSha256"));
  const auto& executor=value.get("executor");nativeStreamJournalFields(executor,2);const auto process=[](const Json& p){nativeStreamJournalFields(p,2);return NativeStreamProcessIdentity{nativeStreamJournalU32(p.get("pid"),1,UINT32_MAX-1),parseNativeStreamU64(p.get("created"))};};result.executor={process(executor.get("service")),process(executor.get("worker"))};nativeStreamJournalRequire(nativeStreamExpiredConsoleRecordBytes(result)==bytes);return result;
}
inline NativeRuntimeJournalAccess nativeStreamExpiredConsoleFileAccess(const std::filesystem::path& path,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  nativeStreamJournalRequire(path.filename()==L"native-stream-expired-console.json");return nativeJournalFileAccess(path,4096,[](const std::string& bytes){(void)parseNativeStreamExpiredConsoleRecord(bytes);},std::move(trusted));
}
inline NativeRuntimeJournalAccess nativeStreamExpiredConsoleHistoryAccess(const std::filesystem::path& directory,const std::string& sha,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  nativeStreamJournalRequire(nativeRecoveryHex(sha));
  // One fixed protected directory, one 64-hex filename. No nested operation
  // directory or concatenated operation+record names that exceed MAX_PATH.
  return nativeJournalFileAccess(directory/(wide(sha)+L".json"),4096,[sha](const std::string& bytes){(void)parseNativeStreamExpiredConsoleRecord(bytes);nativeStreamJournalRequire(nativeBaselineDigest(bytes)==sha);},std::move(trusted));
}
inline bool nativeExpiredRecordPinsEqual(const NativeStreamExpiredConsoleRecord& a,const NativeStreamExpiredConsoleRecord& b){
  return a.operation==b.operation&&a.profileSha256==b.profileSha256&&a.launchSha256==b.launchSha256&&a.contextSha256==b.contextSha256&&a.planSha256==b.planSha256&&a.beforeSha256==b.beforeSha256&&a.restoredSha256==b.restoredSha256&&a.ledgerSha256==b.ledgerSha256;
}
inline std::string nativeExpiredCcdDigest(const NativeCcdData& data){
  auto signatures=nativeStreamProfileSignatures(data);std::sort(signatures.begin(),signatures.end());std::string bytes="[";
  for(const auto& row:signatures){if(bytes.size()>1)bytes+=',';bytes+='[';for(size_t i=0;i<row.size();++i){if(i)bytes+=',';bytes+=jsonQuote(std::to_string(row[i]));}bytes+=']';}return nativeBaselineDigest(bytes+']');
}
enum class NativeStreamExpiredConsolePrivateKind {runtime,identity,viewport,reclaim,baseline};
struct NativeStreamExpiredConsolePrivateSource {NativeStreamExpiredConsolePrivateKind kind;NativeRuntimeJournalAccess journal;};
struct NativeStreamExpiredConsolePrivateSnapshot {NativeStreamExpiredConsolePrivateKind kind;std::string bytes;};
enum class NativeStreamExpiredConsoleLedgerKind {launch,committedViewport,pendingViewport,reclaimed,pendingNative,pendingReservation};
struct NativeStreamExpiredConsoleLeaseMapping {uint64_t lease=0;std::vector<NativeStreamLaunchOutput> outputs;};
struct NativeStreamExpiredConsoleLedger {
  NativeStreamExpiredConsoleLedgerKind kind=NativeStreamExpiredConsoleLedgerKind::launch;
  std::string profileSha256,launchSha256,pendingOperation,pendingRequestSha256;
  NativeStreamProfileLease current;
  std::vector<NativeStreamExpiredConsoleLeaseMapping> leases;
};
inline std::string nativeExpiredLedgerBytes(const NativeStreamExpiredConsoleLedger& ledger,const std::vector<NativeStreamExpiredConsolePrivateSnapshot>& snapshots){
  nativeStreamJournalRequire(nativeRecoveryHex(ledger.profileSha256)&&nativeRecoveryHex(ledger.launchSha256)&&!ledger.leases.empty()&&ledger.leases.size()<=8);
  auto leases=ledger.leases;std::sort(leases.begin(),leases.end(),[](const auto& a,const auto& b){return a.lease<b.lease;});std::string mappings="[",pins="[";uint64_t previous=0;
  for(const auto& mapping:leases){nativeStreamJournalRequire(mapping.lease>=0x6000000000000000ull&&mapping.lease!=previous&&!mapping.outputs.empty()&&mapping.outputs.size()<=8);previous=mapping.lease;if(mappings.size()>1)mappings+=',';mappings+="{\"lease\":"+jsonQuote(nativeStreamU64(mapping.lease))+",\"outputs\":[";for(size_t i=0;i<mapping.outputs.size();++i){const auto& output=mapping.outputs[i];nativeStreamJournalRequire(output.display&&output.connector!=UINT32_MAX);for(size_t j=0;j<i;++j)nativeStreamJournalRequire(output.display!=mapping.outputs[j].display);if(i)mappings+=',';mappings+="{\"display\":"+jsonQuote(nativeStreamU64(output.display))+",\"connector\":"+std::to_string(output.connector)+'}';}mappings+="]}";}
  auto ordered=snapshots;std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return int(a.kind)<int(b.kind);});for(const auto& pin:ordered){if(pins.size()>1)pins+=',';pins+="{\"kind\":"+std::to_string(int(pin.kind))+",\"sha256\":"+jsonQuote(nativeBaselineDigest(pin.bytes))+'}';}
  return "{\"kind\":"+std::to_string(int(ledger.kind))+",\"profileSha256\":"+jsonQuote(ledger.profileSha256)+",\"launchSha256\":"+jsonQuote(ledger.launchSha256)+",\"pendingOperation\":"+jsonQuote(ledger.pendingOperation)+",\"pendingRequestSha256\":"+jsonQuote(ledger.pendingRequestSha256)+",\"current\":"+nativeStreamLeaseBytes(ledger.current)+",\"leases\":"+mappings+"],\"pins\":"+pins+"]}";
}
struct NativeStreamExpiredConsoleSource {
  std::string profileBytes,launchBytes;NativeStreamProfileRecord profile;NativeStreamLaunchReceipt launch;NativeStreamExpiredConsoleContext context;
  std::vector<NativeStreamExpiredConsolePrivateSnapshot> privateSnapshots;NativeStreamExpiredConsoleLedger ledger;
  std::vector<NativeStreamExpiredConsolePrivateKind> privateMissingKinds;
};
struct NativeStreamExpiredConsoleObservation {NativeCcdData topology;NativeRecoveryScope scope;std::string machine;};
enum class NativeStreamExpiredConsoleStage {prepare,handoff,validate,apply,complete};
struct NativeStreamExpiredConsoleAuthorityRequest {
  NativeStreamExpiredConsoleRecord record;NativeStreamLaunchReceipt launch;NativeStreamProfileRecord profile;
  NativeStreamExpiredConsoleLedger ledger;
  NativeStreamExpiredConsoleExecutor executor;std::optional<NativeStreamExpiredConsoleExecutor> previousExecutor;
  NativeStreamExpiredConsoleStage stage=NativeStreamExpiredConsoleStage::prepare;
};
enum class NativeStreamExpiredSdkObject {unknown,absent,present};
struct NativeStreamExpiredLeaseObservation {uint64_t lease=0;NativeStreamExpiredSdkObject state=NativeStreamExpiredSdkObject::unknown;uint32_t temporaryDisplays=0,pendingDepartures=0;};
struct NativeStreamExpiredDisplayObservation {uint64_t lease=0,display=0;NativeStreamExpiredSdkObject state=NativeStreamExpiredSdkObject::unknown;};
struct NativeStreamExpiredProcessObservation {NativeStreamProcessIdentity identity;NativeStreamProcessState state=NativeStreamProcessState::unknown;};
struct NativeStreamExpiredConsoleAuthorityProof {
  // Production creates this only while holding the exclusive native recovery
  // reservation and held authenticated child/service process lifetime handles.
  // Geometry and a caller-supplied boolean cannot construct that authority.
  std::string operation,profileSha256,launchSha256,reservation;
  NativeStreamExpiredConsoleExecutor executor;DWORD session=0;std::string logon;uint64_t tick=0;
  std::vector<NativeStreamExpiredProcessObservation> processes;
  std::vector<NativeStreamExpiredLeaseObservation> leases;
  std::vector<NativeStreamExpiredDisplayObservation> displays;
};
struct NativeStreamExpiredConsoleTransactionAccess {
  NativeRuntimeJournalAccess marker,profiles,launches;
  std::function<NativeRuntimeJournalAccess(const std::string&)> history;
  std::function<NativeStreamExpiredConsoleObservation()> observe;
  NativeStreamExpiredConsoleAccess identity;
  std::function<std::optional<NativeStreamExpiredConsoleAuthorityProof>(const NativeStreamExpiredConsoleAuthorityRequest&)> authority;
  std::function<LONG(const NativeCcdData&,UINT32)> configure;
  std::function<uint64_t()> now=[]{return GetTickCount64();};
  // Optional extension, populated only by trusted service-owned protected-file
  // readers. Its resolver proves DPAPI owner IDs + compiled receipt/runtime
  // relationships. The coordinator pins every byte and every resolved SDK ID.
  std::vector<NativeStreamExpiredConsolePrivateSource> privateSources;
  std::function<std::optional<NativeStreamExpiredConsoleLedger>(const NativeStreamProfileRecord&,const NativeStreamLaunchReceipt&,const std::vector<NativeStreamExpiredConsolePrivateSnapshot>&)> resolveLedger;
};
struct NativeStreamExpiredConsoleTransactionResult {
  bool ok=false,preparedAcknowledged=false,applyAttempted=false,consoleVerified=false,completedAcknowledged=false,recoveryRequired=true;
  DWORD systemError=0;std::string reason="native_stream_expired_console_unverified";std::optional<NativeStreamExpiredConsoleRecord> record;
};
// Console restoration only. Never creates/reclaims an SDK display, starts
// capture, rewrites the old profile/launch, or blindly rolls back OS changes.
class NativeStreamExpiredConsoleTransaction {
 public:
  explicit NativeStreamExpiredConsoleTransaction(NativeStreamExpiredConsoleTransactionAccess access):access_(std::move(access)){}
  NativeStreamExpiredConsoleTransactionResult restore(const std::string& operation,const NativeStreamExpiredConsoleExecutor& executor)noexcept{
    NativeStreamExpiredConsoleTransactionResult result;
    try{
      const auto source=readSource();auto desired=makeRecord(source,operation,executor);const auto observed=access_.marker.load();std::optional<NativeStreamExpiredConsoleRecord> previous;
      if(observed){previous=parseNativeStreamExpiredConsoleRecord(*observed);nativeStreamJournalRequire(nativeExpiredRecordPinsEqual(*previous,desired)&&verifyHistory(*previous));desired=*previous;}
      auto plan=freshPlan(source);std::string reservation;
      auto stage=previous&&previous->executor!=executor?NativeStreamExpiredConsoleStage::handoff:NativeStreamExpiredConsoleStage::prepare;
      if(!authorized(source,desired,executor,previous&&previous->executor!=executor?std::optional(previous->executor):std::nullopt,stage,reservation)||!unchanged(source)||access_.marker.load()!=observed)return fail(result,"native_stream_expired_console_authority_lost");
      if(previous&&previous->phase=="completed"&&!plan.alreadyRestored)return fail(result,"native_stream_expired_console_completed_topology_changed");
      if(previous&&previous->executor!=executor){
        desired.executor=executor;if(!advanceHistory(*previous,desired))return fail(result,"native_stream_expired_console_history_unacknowledged");
      }
      if(!previous||previous->executor!=executor){
        if(!unchanged(source)||access_.marker.load()!=observed||!authorized(source,desired,executor,previous&&previous->executor!=executor?std::optional(previous->executor):std::nullopt,stage,reservation)||!unchanged(source)||access_.marker.load()!=observed)return fail(result,"native_stream_expired_console_authority_lost");
        // False, including a lost acknowledgement, is never upgraded by an
        // immediate readback in this same attempt. No Windows apply follows.
        if(!access_.marker.save(observed,nativeStreamExpiredConsoleRecordBytes(desired)))return fail(result,"native_stream_expired_console_prepare_unacknowledged");
      }
      const auto preparedBytes=nativeStreamExpiredConsoleRecordBytes(desired);if(access_.marker.load()!=std::optional(preparedBytes))return fail(result,"native_stream_expired_console_prepare_unverified");
      result.preparedAcknowledged=true;result.record=desired;
      const auto current=[&](NativeStreamExpiredConsoleStage phase){return unchanged(source)&&access_.marker.load()==std::optional(preparedBytes)&&authorized(source,desired,executor,{},phase,reservation)&&unchanged(source)&&access_.marker.load()==std::optional(preparedBytes);};
      if(!current(NativeStreamExpiredConsoleStage::validate))return fail(result,"native_stream_expired_console_authority_lost");plan=freshPlan(source);
      if(!plan.alreadyRestored){
        constexpr UINT32 options=SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_VIRTUAL_MODE_AWARE;
        if(!current(NativeStreamExpiredConsoleStage::validate))return fail(result,"native_stream_expired_console_authority_lost");
        result.systemError=DWORD(access_.configure(plan.restored,SDC_VALIDATE|options));if(result.systemError)return fail(result,"native_stream_expired_console_validate_failed");
        // Reprove scope, foreign CCD, exact authenticated executors and SDK
        // absence after VALIDATE and immediately before the mutating boundary.
        if(!current(NativeStreamExpiredConsoleStage::apply))return fail(result,"native_stream_expired_console_authority_lost");plan=freshPlan(source);
        if(!plan.alreadyRestored){if(!current(NativeStreamExpiredConsoleStage::apply))return fail(result,"native_stream_expired_console_authority_lost");result.applyAttempted=true;result.systemError=DWORD(access_.configure(plan.restored,SDC_APPLY|options));if(result.systemError)return fail(result,"native_stream_expired_console_apply_failed");}
      }
      if(!current(NativeStreamExpiredConsoleStage::complete))return fail(result,"native_stream_expired_console_authority_lost");const auto complete=freshPlan(source);if(!complete.alreadyRestored)return fail(result,"native_stream_expired_console_not_restored");result.consoleVerified=true;
      if(desired.phase!="completed"){
        auto completed=desired;completed.phase="completed";if(!advanceHistory(desired,completed))return fail(result,"native_stream_expired_console_history_unacknowledged");
        if(!current(NativeStreamExpiredConsoleStage::complete)||!freshPlan(source).alreadyRestored)return fail(result,"native_stream_expired_console_authority_lost");
        if(!access_.marker.save(std::optional(preparedBytes),nativeStreamExpiredConsoleRecordBytes(completed)))return fail(result,"native_stream_expired_console_complete_unacknowledged");desired=completed;
      }
      const auto completedBytes=nativeStreamExpiredConsoleRecordBytes(desired);
      if(access_.marker.load()!=std::optional(completedBytes)||!unchanged(source)||!authorized(source,desired,executor,{},NativeStreamExpiredConsoleStage::complete,reservation)||!freshPlan(source).alreadyRestored||!unchanged(source)||access_.marker.load()!=std::optional(completedBytes))return fail(result,"native_stream_expired_console_complete_unverified");
      result.ok=result.consoleVerified=result.completedAcknowledged=true;result.recoveryRequired=false;result.reason.clear();result.record=desired;return result;
    }catch(...){return fail(result,"native_stream_expired_console_unverified");}
  }
 private:
  NativeStreamExpiredConsoleTransactionAccess access_;
  static NativeStreamExpiredConsoleTransactionResult fail(NativeStreamExpiredConsoleTransactionResult value,const char* reason){value.ok=false;value.reason=reason;value.recoveryRequired=true;return value;}
  NativeStreamExpiredConsoleSource readSource()const{
    const auto profile=access_.profiles.load(),launch=access_.launches.load();nativeStreamJournalRequire(profile&&launch);NativeStreamExpiredConsoleSource s;s.profileBytes=*profile;s.launchBytes=*launch;s.profile=parseNativeStreamProfileRecord(*profile);s.launch=parseNativeStreamLaunch(*launch);
    nativeStreamJournalRequire(nativeStreamLaunchMatches(s.launch,s.profile)&&s.profile.phase!=NativeStreamProfilePhase::restored&&access_.privateSources.size()<=5);
    std::set<int> kinds,presentKinds;for(const auto& input:access_.privateSources){const auto kind=int(input.kind);nativeStreamJournalRequire(kind>=0&&kind<=4&&kinds.insert(kind).second&&bool(input.journal.load));const auto bytes=input.journal.load();if(!bytes){nativeStreamJournalRequire(input.kind!=NativeStreamExpiredConsolePrivateKind::runtime&&input.kind!=NativeStreamExpiredConsolePrivateKind::identity);s.privateMissingKinds.push_back(input.kind);continue;}const size_t maximum=input.kind==NativeStreamExpiredConsolePrivateKind::runtime?4096:input.kind==NativeStreamExpiredConsolePrivateKind::identity?8192:input.kind==NativeStreamExpiredConsolePrivateKind::viewport?131072:input.kind==NativeStreamExpiredConsolePrivateKind::baseline?65536:32768;nativeStreamJournalRequire(!bytes->empty()&&bytes->size()<=maximum);presentKinds.insert(kind);s.privateSnapshots.push_back({input.kind,*bytes});}
    if(!access_.privateSources.empty())nativeStreamJournalRequire(presentKinds.contains(int(NativeStreamExpiredConsolePrivateKind::runtime))&&presentKinds.contains(int(NativeStreamExpiredConsolePrivateKind::identity)));
    if(!s.privateSnapshots.empty()){
      nativeStreamJournalRequire(bool(access_.resolveLedger));const auto resolved=access_.resolveLedger(s.profile,s.launch,s.privateSnapshots);nativeStreamJournalRequire(resolved.has_value());s.ledger=*resolved;
    }else{
      nativeStreamJournalRequire(!s.profile.resize&&!s.profile.resizeSequence&&sameNativeStreamLease(s.profile.current,s.launch.entry));s.ledger={NativeStreamExpiredConsoleLedgerKind::launch,nativeBaselineDigest(s.profileBytes),nativeBaselineDigest(s.launchBytes),{},{},s.launch.entry,{{s.launch.lease,s.launch.outputs}}};
    }
    nativeStreamJournalRequire(s.ledger.profileSha256==nativeBaselineDigest(s.profileBytes)&&s.ledger.launchSha256==nativeBaselineDigest(s.launchBytes));
    const auto has=[&](NativeStreamExpiredConsolePrivateKind kind){return presentKinds.contains(int(kind));};
    if(s.ledger.kind==NativeStreamExpiredConsoleLedgerKind::launch){nativeStreamJournalRequire(!s.profile.resize&&!s.profile.resizeSequence&&sameNativeStreamLease(s.profile.current,s.launch.entry)&&sameNativeStreamLease(s.ledger.current,s.launch.entry)&&s.ledger.pendingOperation.empty()&&s.ledger.pendingRequestSha256.empty());}
    else{
      nativeStreamJournalRequire(has(NativeStreamExpiredConsolePrivateKind::runtime)&&has(NativeStreamExpiredConsolePrivateKind::identity));
      if(s.ledger.kind==NativeStreamExpiredConsoleLedgerKind::pendingNative){nativeStreamJournalRequire(has(NativeStreamExpiredConsolePrivateKind::baseline)&&s.profile.resize&&s.ledger.pendingOperation==s.profile.resize->operation&&s.ledger.pendingRequestSha256==s.profile.resize->requestSha256&&sameNativeStreamLease(s.ledger.current,s.profile.current));}
      else if(s.ledger.kind==NativeStreamExpiredConsoleLedgerKind::pendingReservation){nativeStreamJournalRequire(s.profile.resize&&s.profile.resizeSequence&&s.ledger.pendingOperation==s.profile.resize->operation&&s.ledger.pendingRequestSha256==s.profile.resize->requestSha256&&sameNativeStreamLease(s.ledger.current,s.profile.current));}
      else if(s.ledger.kind==NativeStreamExpiredConsoleLedgerKind::pendingViewport){nativeStreamJournalRequire(has(NativeStreamExpiredConsolePrivateKind::viewport)&&s.profile.resize&&s.ledger.pendingOperation==s.profile.resize->operation&&s.ledger.pendingRequestSha256==s.profile.resize->requestSha256&&s.ledger.current.servicePid==s.profile.current.servicePid&&s.ledger.current.generation>=s.profile.current.generation&&s.ledger.current.targets.size()==s.profile.current.targets.size());}
      else{nativeStreamJournalRequire(!s.profile.resize&&s.ledger.pendingOperation.empty()&&s.ledger.pendingRequestSha256.empty()&&sameNativeStreamLease(s.ledger.current,s.profile.current));if(s.ledger.kind==NativeStreamExpiredConsoleLedgerKind::committedViewport)nativeStreamJournalRequire(has(NativeStreamExpiredConsolePrivateKind::viewport)&&s.profile.resizeSequence);else if(s.ledger.kind==NativeStreamExpiredConsoleLedgerKind::reclaimed)nativeStreamJournalRequire(has(NativeStreamExpiredConsolePrivateKind::reclaim)&&(!s.profile.resizeSequence||has(NativeStreamExpiredConsolePrivateKind::viewport)));else nativeStreamJournalRequire(false);}
    }
    size_t launchMappings=0;for(const auto& mapping:s.ledger.leases)if(mapping.lease==s.launch.lease){for(const auto& output:s.launch.outputs)nativeStreamJournalRequire(std::count_if(mapping.outputs.begin(),mapping.outputs.end(),[&](const auto& item){return item.display==output.display&&item.connector==output.connector;})==1);++launchMappings;}nativeStreamJournalRequire(launchMappings==1);(void)nativeExpiredLedgerBytes(s.ledger,s.privateSnapshots);
    // Resized/reclaimed current IDs are admitted only by an explicit trusted
    // protected-source resolver. Merely matching output counts never suffices.
    s.context={s.profile.profile,s.profile.scope,s.profile.console,s.profile.key.instance};return s;
  }
  bool unchanged(const NativeStreamExpiredConsoleSource& s)const{if(access_.profiles.load()!=std::optional(s.profileBytes)||access_.launches.load()!=std::optional(s.launchBytes)||access_.privateSources.size()!=s.privateSnapshots.size()+s.privateMissingKinds.size())return false;for(const auto& snapshot:s.privateSnapshots){const auto input=std::find_if(access_.privateSources.begin(),access_.privateSources.end(),[&](const auto& value){return value.kind==snapshot.kind;});if(input==access_.privateSources.end()||input->journal.load()!=std::optional(snapshot.bytes))return false;}for(const auto kind:s.privateMissingKinds){const auto input=std::find_if(access_.privateSources.begin(),access_.privateSources.end(),[&](const auto& value){return value.kind==kind;});if(input==access_.privateSources.end()||input->journal.load())return false;}return true;}
  static NativeStreamExpiredConsoleRecord makeRecord(const NativeStreamExpiredConsoleSource& s,const std::string& operation,const NativeStreamExpiredConsoleExecutor& executor){
    // Keep the service's present-only ledger hash stable. Absence is separate
    // immutable context evidence; removing an absent reader changes this pin.
    auto missing=s.privateMissingKinds;std::sort(missing.begin(),missing.end(),[](auto a,auto b){return int(a)<int(b);});std::string absent="[";for(const auto kind:missing){if(absent.size()>1)absent+=',';absent+=std::to_string(int(kind));}absent+=']';
    NativeStreamExpiredConsoleRecord r;r.operation=operation;r.profileSha256=nativeBaselineDigest(s.profileBytes);r.launchSha256=nativeBaselineDigest(s.launchBytes);r.contextSha256=nativeBaselineDigest(nativeStreamProfilePayloadBytes(s.context.profile)+nativeRecoveryScopeBytes(s.context.scope)+nativeStreamConsoleBindingBytes(s.context.console)+jsonQuote(s.context.ownedInstance)+"|privateAbsent:"+absent);r.beforeSha256=nativeExpiredCcdDigest(nativeForeignCcd(s.context.profile.applied,s.context.profile.targets));r.restoredSha256=nativeExpiredCcdDigest(nativeForeignCcd(s.context.profile.before,s.context.profile.targets));r.planSha256=nativeBaselineDigest(r.contextSha256+r.beforeSha256+r.restoredSha256);r.ledgerSha256=nativeBaselineDigest(nativeExpiredLedgerBytes(s.ledger,s.privateSnapshots));r.executor=executor;(void)nativeStreamExpiredConsoleRecordBytes(r);nativeStreamJournalRequire(operation!=s.profile.key.operation);return r;
  }
  NativeStreamExpiredConsolePlan freshPlan(const NativeStreamExpiredConsoleSource& s)const{
    nativeStreamJournalRequire(unchanged(s));const auto observed=access_.observe();const auto now=access_.now();auto plan=planNativeStreamExpiredConsole(s.context,observed.topology,observed.scope,observed.machine,access_.identity,now);
    const auto before=nativeExpiredCcdDigest(nativeForeignCcd(s.context.profile.applied,s.context.profile.targets)),restored=nativeExpiredCcdDigest(nativeForeignCcd(s.context.profile.before,s.context.profile.targets));
    nativeStreamJournalRequire(nativeExpiredCcdDigest(plan.restored)==restored&&nativeExpiredCcdDigest(plan.before)==(plan.alreadyRestored?restored:before)&&unchanged(s));return plan;
  }
  bool authorized(const NativeStreamExpiredConsoleSource& s,const NativeStreamExpiredConsoleRecord& record,const NativeStreamExpiredConsoleExecutor& executor,const std::optional<NativeStreamExpiredConsoleExecutor>& previous,NativeStreamExpiredConsoleStage stage,std::string& reservation)const{
    if(!access_.authority)return false;const auto proof=access_.authority({record,s.launch,s.profile,s.ledger,executor,previous,stage});if(!proof)return false;const auto now=access_.now();
    if(proof->operation!=record.operation||proof->profileSha256!=record.profileSha256||proof->launchSha256!=record.launchSha256||proof->executor!=executor||proof->session!=s.profile.key.session||proof->logon!=s.profile.scope.logon||!proof->tick||proof->tick>now||now-proof->tick>3000||!nativeRecoveryHex(proof->reservation)||proof->processes.size()>6||proof->leases.size()>8||proof->displays.size()>64)return false;
    if(reservation.empty())reservation=proof->reservation;else if(reservation!=proof->reservation)return false;
    if(executor.service==s.launch.service||executor.service==s.launch.worker||executor.worker==s.launch.service||executor.worker==s.launch.worker)return false;
    const auto process=[&](const NativeStreamProcessIdentity& id,NativeStreamProcessState state){size_t count=0;for(const auto& p:proof->processes)if(p.identity==id){if(p.state!=state)return false;++count;}return count==1;};
    if(!process(s.launch.service,NativeStreamProcessState::gone)||!process(s.launch.worker,NativeStreamProcessState::gone)||!process(executor.service,NativeStreamProcessState::running)||!process(executor.worker,NativeStreamProcessState::running))return false;
    if(previous){if(previous->worker==executor.worker||!process(previous->worker,NativeStreamProcessState::gone))return false;if(previous->service!=executor.service&&!process(previous->service,NativeStreamProcessState::gone))return false;}
    if(proof->leases.size()!=s.ledger.leases.size())return false;size_t displayCount=0;
    for(const auto& mapping:s.ledger.leases){size_t count=0;for(const auto& query:proof->leases)if(query.lease==mapping.lease&&query.state==NativeStreamExpiredSdkObject::absent&&!query.temporaryDisplays&&!query.pendingDepartures)++count;if(count!=1)return false;displayCount+=mapping.outputs.size();for(const auto& output:mapping.outputs){count=0;for(const auto& d:proof->displays)if(d.lease==mapping.lease&&d.display==output.display&&d.state==NativeStreamExpiredSdkObject::absent)++count;if(count!=1)return false;}}
    return proof->displays.size()==displayCount&&unchanged(s);
  }
  bool verifyHistory(const NativeStreamExpiredConsoleRecord& latest)const{
    auto child=latest;for(uint32_t depth=0;depth<32;++depth){if(child.revision==1)return child.previousSha256.empty();if(!access_.history)return false;const auto bytes=access_.history(child.previousSha256).load();if(!bytes||nativeBaselineDigest(*bytes)!=child.previousSha256)return false;const auto parent=parseNativeStreamExpiredConsoleRecord(*bytes);if(!nativeExpiredRecordPinsEqual(parent,child)||parent.revision+1!=child.revision||(parent.phase=="completed"&&child.phase!="completed")||(parent.executor==child.executor&&parent.phase==child.phase))return false;child=parent;}return false;
  }
  bool advanceHistory(const NativeStreamExpiredConsoleRecord& previous,NativeStreamExpiredConsoleRecord& next)const{
    if(!access_.history||previous.revision>=32||!verifyHistory(previous))return false;const auto bytes=nativeStreamExpiredConsoleRecordBytes(previous),hash=nativeBaselineDigest(bytes);auto history=access_.history(hash);const auto held=history.load();if(held&&*held!=bytes)return false;if(!held&&!history.save({},bytes))return false;if(history.load()!=std::optional(bytes))return false;
    next.revision=previous.revision+1;next.previousSha256=hash;return nativeExpiredRecordPinsEqual(previous,next)&&verifyHistory(next);
  }
};
} // namespace host
