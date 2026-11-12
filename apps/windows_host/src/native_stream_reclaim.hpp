#pragma once
#include "native_stream_launch.hpp"
#include "native_display_identity.hpp"
#ifdef MULTITAP_NATIVE_DISPLAY
#include "runtime.hpp"

namespace host {
// The first restart transaction admits unchanged live outputs only. A resized
// profile needs a durable checkpoint of its new private SDK mappings; it must
// not borrow the launch mappings. Expired leases need a separate restore plan.
struct NativeStreamReclaimSource {
  std::string launchBytes,profileBytes,runtimeBytes,identityBytes;
  std::string observedRuntimeBytes,observedIdentityBytes;
  NativeStreamLaunchReceipt launch;
  NativeStreamProfileRecord profile;
  NativeRuntimeRecord runtime;
};
struct NativeStreamReclaimRecord {
  NativeStreamProfileKey key;
  std::string operation,launchSha256,profileSha256,runtimeSha256;
  uint64_t previous=0,replacement=0;
  uint32_t generation=0;
  NativeStreamProcessIdentity service,worker;
  // Ciphertext only. Persist both original snapshots before either file is
  // replaced, so a lost commit ACK can be reconciled without guessing.
  std::string identityBefore,runtimeBefore;
};
inline std::string nativeStreamReclaimBlob(const std::string& bytes){
  nativeStreamJournalRequire(!bytes.empty()&&bytes.size()<=8192);std::string result;result.reserve(bytes.size()*2);for(const unsigned char b:bytes){result+="0123456789abcdef"[b>>4];result+="0123456789abcdef"[b&15];}return result;
}
inline std::string parseNativeStreamReclaimBlob(const Json& value){
  const auto text=nativeStreamJournalString(value);nativeStreamJournalRequire(!text.empty()&&text.size()<=16384&&text.size()%2==0&&text.find_first_not_of("0123456789abcdef")==std::string::npos);
  const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};std::string result;result.reserve(text.size()/2);for(size_t i=0;i<text.size();i+=2)result+=char((digit(text[i])<<4)|digit(text[i+1]));return result;
}
inline std::string nativeStreamReclaimRuntime(const NativeStreamReclaimRecord& r){
  auto next=parseNativeRuntimeRecord(r.runtimeBefore);nativeStreamJournalRequire(next.instance==r.key.instance&&next.phase=="stable"&&next.generation<UINT32_MAX&&next.generation+1==r.generation&&next.baselineHash.empty());next.generation=r.generation;next.phase="recovery";return nativeRuntimeBytes(next);
}
inline std::string nativeStreamReclaimBytes(const NativeStreamReclaimRecord& r){
  nativeStreamJournalRequire(nativeStreamJournalId(r.key.stream)&&nativeRecoveryHex(r.key.operation)&&nativeDisplayInstanceId(r.key.instance)&&r.key.session&&r.key.session!=UINT32_MAX);
  for(const auto& digest:{r.operation,r.launchSha256,r.profileSha256,r.runtimeSha256})nativeStreamJournalRequire(nativeRecoveryHex(digest));
  nativeStreamJournalRequire(r.operation!=r.key.operation&&r.previous>=0x6000000000000000ull&&r.replacement>=0x6000000000000000ull&&r.previous!=r.replacement&&r.generation>1);
  for(const auto& p:{r.service,r.worker})nativeStreamJournalRequire(p.pid&&p.pid!=UINT32_MAX&&p.created);
  nativeStreamJournalRequire(r.service.pid!=r.worker.pid);
  const bool snapshot=!r.identityBefore.empty()||!r.runtimeBefore.empty();
  if(snapshot){const auto identity=unsealNativeDisplayIdentity(r.identityBefore);nativeStreamJournalRequire(identity.value.lease==r.previous&&r.runtimeSha256==nativeBaselineDigest(r.runtimeBefore));(void)nativeStreamReclaimRuntime(r);}
  const auto process=[](const auto& p){return "{\"pid\":"+std::to_string(p.pid)+",\"created\":"+jsonQuote(nativeStreamU64(p.created))+'}';};
  const auto bytes=std::string("{\"schema\":")+(snapshot?"2":"1")+",\"kind\":\"native-stream-reclaim\",\"stream\":"+jsonQuote(r.key.stream)+",\"profileOperation\":"+jsonQuote(r.key.operation)+",\"deviceInstanceId\":"+jsonQuote(r.key.instance)+",\"session\":"+std::to_string(r.key.session)+",\"operation\":"+jsonQuote(r.operation)+",\"launchSha256\":"+jsonQuote(r.launchSha256)+",\"profileSha256\":"+jsonQuote(r.profileSha256)+",\"runtimeSha256\":"+jsonQuote(r.runtimeSha256)+",\"previous\":"+jsonQuote(nativeStreamU64(r.previous))+",\"replacement\":"+jsonQuote(nativeStreamU64(r.replacement))+",\"generation\":"+std::to_string(r.generation)+",\"service\":"+process(r.service)+",\"worker\":"+process(r.worker)+(snapshot?",\"identityBefore\":"+jsonQuote(nativeStreamReclaimBlob(r.identityBefore))+",\"runtimeBefore\":"+jsonQuote(r.runtimeBefore):std::string())+'}';
  nativeStreamJournalRequire(bytes.size()<=32768);return bytes;
}
inline NativeStreamReclaimRecord parseNativeStreamReclaim(const std::string& bytes){
  nativeStreamJournalRequire(!bytes.empty()&&bytes.size()<=32768);const auto root=JsonParser::parse(bytes);const auto schema=root.get("schema").number();nativeStreamJournalRequire(schema==1||schema==2);nativeStreamJournalFields(root,schema==1?15:17);
  nativeStreamJournalRequire(root.get("kind").string()=="native-stream-reclaim");NativeStreamReclaimRecord r;
  r.key={nativeStreamJournalString(root.get("stream")),nativeStreamJournalString(root.get("profileOperation")),nativeStreamJournalString(root.get("deviceInstanceId")),nativeStreamJournalU32(root.get("session"),1,UINT32_MAX-1)};
  r.operation=nativeStreamJournalString(root.get("operation"));r.launchSha256=nativeStreamJournalString(root.get("launchSha256"));r.profileSha256=nativeStreamJournalString(root.get("profileSha256"));r.runtimeSha256=nativeStreamJournalString(root.get("runtimeSha256"));
  r.previous=parseNativeStreamU64(root.get("previous"));r.replacement=parseNativeStreamU64(root.get("replacement"));r.generation=nativeStreamJournalU32(root.get("generation"),2);
  const auto process=[](const Json& p){nativeStreamJournalFields(p,2);return NativeStreamProcessIdentity{nativeStreamJournalU32(p.get("pid"),1,UINT32_MAX-1),parseNativeStreamU64(p.get("created"))};};r.service=process(root.get("service"));r.worker=process(root.get("worker"));
  if(schema==2){r.identityBefore=parseNativeStreamReclaimBlob(root.get("identityBefore"));r.runtimeBefore=nativeStreamJournalString(root.get("runtimeBefore"));}
  nativeStreamJournalRequire(nativeStreamReclaimBytes(r)==bytes);return r;
}
inline NativeRuntimeJournalAccess nativeStreamReclaimFileAccess(const std::filesystem::path& path,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  nativeStreamJournalRequire(path.filename()==L"native-stream-reclaim.json");return nativeJournalFileAccess(path,32768,[](const auto& bytes){(void)parseNativeStreamReclaim(bytes);},std::move(trusted));
}
struct NativeStreamReclaimAuthority {
  // Production supplies held lifetime proofs, an authenticated same-login
  // worker's fresh CCD observation and current trusted provider identity.
  // This journal separately checks the exact before/after DPAPI ownership.
  // None of these callbacks may perform display mutations or create an intent.
  std::function<NativeStreamProcessState(const NativeStreamProcessIdentity&)> process;
  std::function<std::optional<NativeStreamProfileEvidence>()> display;
  std::function<bool(const NativeStreamReclaimSource&)> identity;
  std::function<uint64_t()> now;
};
inline multitap::vdisplay::LiveLeaseRecoveryPlan nativeStreamReclaimPlan(const NativeStreamReclaimSource& source,const NativeStreamReclaimRecord& r){
  using namespace multitap::vdisplay;LiveLeaseRecoveryPlan plan;plan.previous=r.previous;plan.replacement=r.replacement;
  for(size_t i=0;i<source.launch.outputs.size();++i){const auto& output=source.launch.outputs[i];const auto& target=source.launch.entry.targets[i];
    plan.outputs.push_back({output.display,target.width,target.height});api::CreateTemporaryDisplayResult mapping{};mapping.lease_id=r.previous;mapping.display_id=output.display;mapping.connector_index=output.connector;mapping.os_adapter_luid={target.adapter.LowPart,target.adapter.HighPart};mapping.target_id=target.target;plan.targets.push_back(mapping);
  }nativeStreamJournalRequire(validLiveLeaseRecoveryPlan(plan));return plan;
}
struct NativeStreamReclaimCommit {std::string intentSha256,identitySha256,runtimeSha256;uint32_t generation=0;};
inline std::string nativeStreamReclaimCommitBytes(const NativeStreamReclaimCommit& value){
  for(const auto& hash:{value.intentSha256,value.identitySha256,value.runtimeSha256})nativeStreamJournalRequire(nativeRecoveryHex(hash));
  nativeStreamJournalRequire(value.generation>1);
  return "{\"intentSha256\":"+jsonQuote(value.intentSha256)+",\"identitySha256\":"+jsonQuote(value.identitySha256)+",\"runtimeSha256\":"+jsonQuote(value.runtimeSha256)+",\"generation\":"+std::to_string(value.generation)+'}';
}
inline NativeStreamReclaimCommit parseNativeStreamReclaimCommit(const Json& value){
  nativeStreamJournalFields(value,4);NativeStreamReclaimCommit result{nativeStreamJournalString(value.get("intentSha256")),nativeStreamJournalString(value.get("identitySha256")),nativeStreamJournalString(value.get("runtimeSha256")),nativeStreamJournalU32(value.get("generation"),2)};
  (void)nativeStreamReclaimCommitBytes(result);return result;
}
// The commit packet is only a selector for protected state. Before every CCD
// mutation, the recovery worker independently re-reads all five root-protected
// records and proves both executor lifetimes and the original logon/device.
// No lease ID or owner capability is trusted from the packet or exposed by it.
inline bool nativeStreamReclaimRestorationProof(const NativeStreamProfileRecord& profile,const NativeStreamProfileEvidence& evidence,const NativeStreamReclaimCommit& commit,
    const NativeRuntimeJournalAccess& intents,const NativeRuntimeJournalAccess& launches,const NativeRuntimeJournalAccess& profiles,const NativeRuntimeJournalAccess& runtimes,const NativeRuntimeJournalAccess& identities,
    const NativeStreamProcessIdentity& service,const NativeStreamProcessIdentity& worker,const std::function<NativeStreamProcessState(const NativeStreamProcessIdentity&)>& process,uint64_t now)noexcept{
  try{
    if(!process||!evidence.scope.tick||evidence.scope.tick>now||now-evidence.scope.tick>3000)return false;
    (void)nativeStreamReclaimCommitBytes(commit);
    const auto intentBytes=intents.load(),launchBytes=launches.load(),profileBytes=profiles.load(),runtimeBytes=runtimes.load(),identityBytes=identities.load();
    if(!intentBytes||!launchBytes||!profileBytes||!runtimeBytes||!identityBytes||nativeBaselineDigest(*intentBytes)!=commit.intentSha256||nativeBaselineDigest(*runtimeBytes)!=commit.runtimeSha256||nativeBaselineDigest(*identityBytes)!=commit.identitySha256||*profileBytes!=nativeStreamProfileRecordBytes(profile))return false;
    const auto intent=parseNativeStreamReclaim(*intentBytes);const auto launch=parseNativeStreamLaunch(*launchBytes);
    if(intent.identityBefore.empty()||intent.runtimeBefore.empty()||intent.service!=service||intent.worker!=worker||intent.generation!=commit.generation||intent.key!=profile.key||intent.launchSha256!=nativeBaselineDigest(*launchBytes)||!nativeStreamLaunchMatches(launch,profile)||*runtimeBytes!=nativeStreamReclaimRuntime(intent))return false;
    if(service==launch.service||worker==launch.worker||service==launch.worker||worker==launch.service||process(launch.service)!=NativeStreamProcessState::gone||process(launch.worker)!=NativeStreamProcessState::gone||process(service)!=NativeStreamProcessState::running||process(worker)!=NativeStreamProcessState::running)return false;
    const auto before=unsealNativeDisplayIdentity(intent.identityBefore),current=unsealNativeDisplayIdentity(*identityBytes);
    if(before.value.lease!=intent.previous||current.value.lease!=intent.replacement||intent.previous!=launch.lease||!sameNativeDisplayOwner(before.value,current.value)||profile.resize||profile.resizeSequence)return false;
    for(size_t i=0;i<launch.outputs.size();++i)if(before.value.displays[i]!=launch.outputs[i].display)return false;
    auto replacement=launch.entry;replacement.servicePid=service.pid;replacement.generation=commit.generation;
    if(!sameNativeStreamLease(evidence.lease,replacement))return false;
    if(sameNativeStreamLease(profile.current,launch.entry)){
      if(nativeBaselineDigest(*profileBytes)!=intent.profileSha256)return false;
    }else if(!sameNativeStreamLease(profile.current,replacement)||!profile.stopRequested||
             (profile.phase!=NativeStreamProfilePhase::restoring&&profile.phase!=NativeStreamProfilePhase::recovery&&profile.phase!=NativeStreamProfilePhase::restored))return false;
    if(!nativeStreamProfileEvidenceMatches(profile,evidence,NativeStreamProfileView::restorable,true))return false;
    // Re-read after the potentially slow process/DPAPI/CCD validation. A stale
    // receipt never authorizes mutation over another service's new state.
    return intents.load()==intentBytes&&launches.load()==launchBytes&&profiles.load()==profileBytes&&runtimes.load()==runtimeBytes&&identities.load()==identityBytes&&
      process(launch.service)==NativeStreamProcessState::gone&&process(launch.worker)==NativeStreamProcessState::gone&&process(service)==NativeStreamProcessState::running&&process(worker)==NativeStreamProcessState::running;
  }catch(...){return false;}
}
// Serialized by the service's exclusive recovery reservation. The intent is
// immutable and cannot replace another operation. Driver success is NOT a
// restored topology, committed DPAPI/runtime state, or admission for capture.
class NativeStreamReclaimJournal {
 public:
  NativeStreamReclaimJournal(NativeRuntimeJournalAccess intent,NativeRuntimeJournalAccess launches,NativeRuntimeJournalAccess profiles,NativeRuntimeJournalAccess runtime,NativeRuntimeJournalAccess identity)
      :intent_(std::move(intent)),launches_(std::move(launches)),profiles_(std::move(profiles)),runtime_(std::move(runtime)),identity_(std::move(identity)){}
  std::optional<NativeStreamReclaimRecord> prepare(const std::string& operation,uint64_t replacement,NativeStreamProcessIdentity service,NativeStreamProcessIdentity worker,const NativeStreamReclaimAuthority& authority){
    try{
      const auto s=source();if(s.profile.current.generation==UINT32_MAX)return {};
      NativeStreamReclaimRecord r{s.profile.key,operation,nativeBaselineDigest(s.launchBytes),nativeBaselineDigest(s.profileBytes),nativeBaselineDigest(s.runtimeBytes),s.launch.lease,replacement,s.profile.current.generation+1,service,worker,s.identityBytes,s.runtimeBytes};
      (void)nativeStreamReclaimPlan(s,r);
      const auto desired=nativeStreamReclaimBytes(r);const auto previous=intent_.load();
      if((previous&&*previous!=desired)||!authorized(s,r,authority)||!unchanged(s)||intent_.load()!=previous)return {};
      if(!previous&&!intent_.save({},desired))return {};
      if(intent_.load()!=desired||!authorized(s,r,authority)||!unchanged(s)||intent_.load()!=desired)return {};
      return r;
    }catch(...){return {};}
  }
  bool current(const NativeStreamReclaimRecord& r,const NativeStreamReclaimAuthority& authority)const noexcept{
    try{const auto bytes=nativeStreamReclaimBytes(r);if(intent_.load()!=bytes)return false;const auto s=source(&r);return matches(s,r)&&authorized(s,r,authority)&&unchanged(s)&&intent_.load()==bytes;}catch(...){return false;}
  }
  multitap::vdisplay::LiveLeaseRecoveryResult reconcile(multitap::vdisplay::api::ControlClient& client,const multitap::vdisplay::api::OwnerCapability& owner,const NativeStreamReclaimRecord& r,const NativeStreamReclaimAuthority& authority)const noexcept{
    try{
      if(!current(r,authority))return {};const auto origin=unsealNativeDisplayIdentity(r.identityBefore);uint8_t difference=0;for(size_t i=0;i<owner.bytes.size();++i)difference|=uint8_t(owner.bytes[i]^origin.value.owner.bytes[i]);if(difference)return {};const auto plan=nativeStreamReclaimPlan(source(&r),r);
      return multitap::vdisplay::reconcileLiveLease(client,plan,owner,[&]{return current(r,authority);},[&]{return intent_.load()==nativeStreamReclaimBytes(r);});
    }catch(...){return {};}
  }
  bool adopt(multitap::vdisplay::Controller& controller,const NativeStreamReclaimRecord& r,const NativeStreamReclaimAuthority& authority)const noexcept{
    try{if(!current(r,authority))return false;return controller.adoptReclaimed(nativeStreamReclaimPlan(source(&r),r),[&]{return current(r,authority);},[&]{return intent_.load()==nativeStreamReclaimBytes(r);});}catch(...){return false;}
  }
  std::optional<NativeStreamReclaimCommit> commit(multitap::vdisplay::api::ControlClient& client,const NativeStreamReclaimRecord& r,const NativeStreamReclaimAuthority& authority)const noexcept{
    try{
      if(!current(r,authority))return {};const auto plan=nativeStreamReclaimPlan(source(&r),r);
      const auto authorized=[&]{return current(r,authority)&&multitap::vdisplay::inspectLiveLeaseRecovery(client,plan)==multitap::vdisplay::LiveLeaseLocation::replacement&&current(r,authority);};
      if(!authorized())return {};multitap::vdisplay::api::LeaseRequest feed{};feed.lease_id=r.replacement;feed.requested_timeout_ms=15000;if(!client.feed_lease(feed).ok()||!authorized())return {};
      const auto before=unsealNativeDisplayIdentity(r.identityBefore);NativeDisplayIdentityStore identities(identity_);
      if(!identities.rotate(before,r.replacement,authorized)||!authorized())return {};
      const auto next=nativeStreamReclaimRuntime(r);const auto previous=runtime_.load();if(!previous||(*previous!=r.runtimeBefore&&*previous!=next))return {};
      if(*previous!=next){if(!authorized()||runtime_.load()!=previous)return {};(void)runtime_.save(previous,next);}
      if(runtime_.load()!=next||!authorized())return {};const auto finalIdentity=identities.existing();
      if(finalIdentity.value.lease!=r.replacement||!sameNativeDisplayOwner(before.value,finalIdentity.value))return {};
      const NativeStreamReclaimCommit result{nativeBaselineDigest(nativeStreamReclaimBytes(r)),nativeBaselineDigest(finalIdentity.sealed),nativeBaselineDigest(next),r.generation};
      return authorized()&&identity_.load()==finalIdentity.sealed&&runtime_.load()==next?std::optional(result):std::nullopt;
    }catch(...){return {};}
  }
 private:
  NativeStreamReclaimSource source(const NativeStreamReclaimRecord* r=nullptr)const{
    NativeStreamReclaimSource s;const auto launch=launches_.load(),profile=profiles_.load(),runtime=runtime_.load(),identity=identity_.load();nativeStreamJournalRequire(launch&&profile&&runtime&&identity);s.launchBytes=*launch;s.profileBytes=*profile;s.observedRuntimeBytes=*runtime;s.observedIdentityBytes=*identity;s.runtimeBytes=r?r->runtimeBefore:*runtime;s.identityBytes=r?r->identityBefore:*identity;
    s.launch=parseNativeStreamLaunch(*launch);s.profile=parseNativeStreamProfileRecord(*profile);s.runtime=parseNativeRuntimeRecord(s.runtimeBytes);
    const auto owner=unsealNativeDisplayIdentity(s.identityBytes),observedOwner=unsealNativeDisplayIdentity(*identity);nativeStreamJournalRequire(owner.value.lease==s.launch.lease&&sameNativeDisplayOwner(owner.value,observedOwner.value));
    for(size_t i=0;i<s.launch.outputs.size();++i)nativeStreamJournalRequire(owner.value.displays[i]==s.launch.outputs[i].display);
    if(r){const bool rotated=observedOwner.value.lease==r->replacement;nativeStreamJournalRequire(rotated||*identity==r->identityBefore);nativeStreamJournalRequire(*runtime==r->runtimeBefore||(rotated&&*runtime==nativeStreamReclaimRuntime(*r)));}
    nativeStreamJournalRequire(nativeStreamLaunchMatches(s.launch,s.profile)&&s.profile.phase!=NativeStreamProfilePhase::restored&&!s.profile.resize&&s.profile.resizeSequence==0&&sameNativeStreamLease(s.profile.entry,s.profile.current));
    nativeStreamJournalRequire(s.runtime.instance==s.profile.key.instance&&s.runtime.phase=="stable"&&s.runtime.baselineHash.empty()&&s.runtime.generation==s.profile.current.generation&&s.runtime.outputs.size()==s.profile.current.targets.size());
    for(size_t i=0;i<s.runtime.outputs.size();++i){const auto& a=s.runtime.outputs[i];const auto& b=s.profile.current.targets[i];nativeStreamJournalRequire(a.id==b.id&&a.width==b.width&&a.height==b.height);}
    return s;
  }
  bool unchanged(const NativeStreamReclaimSource& s)const{return launches_.load()==s.launchBytes&&profiles_.load()==s.profileBytes&&runtime_.load()==s.observedRuntimeBytes&&identity_.load()==s.observedIdentityBytes;}
  static bool matches(const NativeStreamReclaimSource& s,const NativeStreamReclaimRecord& r){
    return r.key==s.profile.key&&r.launchSha256==nativeBaselineDigest(s.launchBytes)&&r.profileSha256==nativeBaselineDigest(s.profileBytes)&&r.runtimeSha256==nativeBaselineDigest(s.runtimeBytes)&&r.previous==s.launch.lease&&r.identityBefore==s.identityBytes&&r.runtimeBefore==s.runtimeBytes&&s.profile.current.generation<UINT32_MAX&&r.generation==s.profile.current.generation+1;
  }
  static bool authorized(const NativeStreamReclaimSource& s,const NativeStreamReclaimRecord& r,const NativeStreamReclaimAuthority& a){
    if(!matches(s,r)||!a.process||!a.display||!a.identity||!a.now)return false;
    // Exact lifetime identity permits safe PID reuse, but never a live prior
    // service/worker or a newly invented replacement for a pending executor.
    if(r.service==s.launch.service||r.worker==s.launch.worker||r.service==s.launch.worker||r.worker==s.launch.service)return false;
    if(a.process(s.launch.service)!=NativeStreamProcessState::gone||a.process(s.launch.worker)!=NativeStreamProcessState::gone||a.process(r.service)!=NativeStreamProcessState::running||a.process(r.worker)!=NativeStreamProcessState::running||!a.identity(s))return false;
    const auto proof=a.display();const auto now=a.now();
    if(!proof||!proof->scope.tick||proof->scope.tick>now||now-proof->scope.tick>3000||!nativeStreamProfileEvidenceMatches(s.profile,*proof,NativeStreamProfileView::restorable))return false;
    return a.identity(s)&&a.process(s.launch.service)==NativeStreamProcessState::gone&&a.process(s.launch.worker)==NativeStreamProcessState::gone&&a.process(r.service)==NativeStreamProcessState::running&&a.process(r.worker)==NativeStreamProcessState::running;
  }
  NativeRuntimeJournalAccess intent_,launches_,profiles_,runtime_,identity_;
};
}
#endif
