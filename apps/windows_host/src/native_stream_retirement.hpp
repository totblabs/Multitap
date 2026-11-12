#pragma once
#include "native_stream_reclaim.hpp"
#include "native_display_baseline_journal.hpp"
#ifdef MULTITAP_NATIVE_DISPLAY
#include <array>

namespace host {
// A new Windows login cannot inherit the previous login's CCD restoration
// authority. Retirement archives the original records and starts a fresh owned
// lease only after proving all old owned displays gone and a live recovery
// console on this same VM. It never replays old modes or claims "restored".
struct NativeStreamRetirementObservation {NativeRecoveryScope scope;NativeConsoleBinding console;NativeCcdData topology;bool ownedOutputActive=true;};
inline bool nativeSameConsoleHardware(const NativeConsoleBinding& before,const NativeConsoleBinding& fresh){
  const auto a=canonicalNativeConsoleBinding(before),b=canonicalNativeConsoleBinding(fresh);
  return a&&b&&a->proof.machineUuid==b->proof.machineUuid&&a->proof.model==b->proof.model&&a->device.instanceId==b->device.instanceId&&a->device.interfacePath==b->device.interfacePath&&a->device.enumerator==b->device.enumerator&&a->device.hardwareIds==b->device.hardwareIds;
}
inline std::string nativeRetirementGeometryDigest(const NativeStreamRetirementObservation& observed){
  const auto active=nativeActiveCcd(observed.topology);nativeStreamJournalRequire(!active.paths.empty()&&active.paths.size()<=64&&active.modes.size()<=256);
  std::string bytes;for(const auto& row:nativeStreamProfileSignatures(active)){for(const auto number:row)bytes+=std::to_string(number)+',';bytes+=';';}return nativeBaselineDigest(bytes);
}
// reason (optional, diagnostics only) receives one fixed retirement code; the
// decision is identical with or without it.
inline bool nativeStreamRetirementObservationValid(const NativeStreamProfileRecord& previous,const NativeStreamRetirementObservation& fresh,uint64_t now,bool expiredSameLogin=false,std::string* reason=nullptr){
  const auto fail=[reason](const char* code){if(reason)*reason=code;return false;};
  try{
    if(fresh.ownedOutputActive)return fail("owned_output_active");
    if(!fresh.scope.session||fresh.scope.session==UINT32_MAX||!nativeRecoveryHex(fresh.scope.logon))return fail("observation_invalid");
    if(fresh.scope.logon==previous.scope.logon&&!expiredSameLogin)return fail("logon_same");
    if(!fresh.scope.tick||fresh.scope.tick>now||now-fresh.scope.tick>3000)return fail("observation_stale");
    if(!nativeSameConsoleHardware(previous.console,fresh.console))return fail("console_hardware");
    const auto active=nativeActiveCcd(fresh.topology);(void)nativeRetirementGeometryDigest(fresh);NativeTopologyBaseline baseline;baseline.ccd=active;validateNativeRecoveryScope(fresh.scope,baseline);if(!nativeRecoveryScopeComplete(fresh.scope))return fail("scope_incomplete");
    std::set<decltype(nativeSourceKey(DISPLAYCONFIG_PATH_INFO{}))> primaries;std::set<decltype(nativeStreamProfileTargetKey(DISPLAYCONFIG_PATH_INFO{}))> targets;size_t console=0;
    for(const auto& path:active.paths){
      if(!path.targetInfo.targetAvailable||!targets.insert(nativeStreamProfileTargetKey(path)).second)return fail("target_unavailable");
      const auto& source=nativeCcdSource(active,path);if(!displayRectangleFits(source.position.x,source.position.y,source.width,source.height))return fail("observation_invalid");
      if(!source.position.x&&!source.position.y)primaries.insert(nativeSourceKey(path));
      const auto& key=fresh.console.path;
      if(nativeSameLuid(path.sourceInfo.adapterId,key.sourceAdapter)&&nativeSameLuid(path.targetInfo.adapterId,key.targetAdapter)&&path.sourceInfo.id==key.source&&path.targetInfo.id==key.target){if(source.position.x||source.position.y)return fail("console_not_primary");++console;}
    }
    if(console!=1)return fail("console_not_unique");
    if(primaries.size()!=1)return fail("primary_ambiguous");
    return true;
  }catch(...){return fail("observation_invalid");}
}
inline NativeStreamRetirementObservation observeNativeStreamRetirement(const NativeStreamProfileRecord& previous,const std::string& machine,const std::string& ownedInstance,const NativeCcdData& topology,const NativeRecoveryScope& scope,const NativeConsoleIdentityResolver& resolver,bool expiredSameLogin=false){
  nativeStreamJournalRequire(nativeDisplayInstanceId(ownedInstance)&&ownedInstance==previous.key.instance&&nativeConsoleUuid(machine)==nativeConsoleUuid(previous.console.proof.machineUuid)&&bool(resolver));
  const auto active=nativeActiveCcd(topology);NativeStreamRetirementObservation observed;observed.scope=scope;observed.topology=active;observed.ownedOutputActive=false;size_t consoles=0;
  const auto selected=nativeConsoleAscii(wide(ownedInstance),200);nativeStreamJournalRequire(selected.has_value());
  for(const auto& path:active.paths){
    const auto target=resolver(path.targetInfo.adapterId),source=resolver(path.sourceInfo.adapterId);nativeStreamJournalRequire(target&&source);
    const auto targetDevice=canonicalNativeConsoleAdapter(*target),sourceDevice=canonicalNativeConsoleAdapter(*source);nativeStreamJournalRequire(targetDevice&&sourceDevice);
    if(targetDevice->instanceId==*selected||sourceDevice->instanceId==*selected)observed.ownedOutputActive=true;
    if(!nativeConsoleIsStdAdapter(*targetDevice))continue;
    nativeStreamJournalRequire(nativeSameLuid(path.sourceInfo.adapterId,path.targetInfo.adapterId)&&++consoles==1);
    const auto binding=canonicalNativeConsoleBinding({previous.console.proof,*targetDevice,{path.sourceInfo.adapterId,path.targetInfo.adapterId,path.sourceInfo.id,path.targetInfo.id}});nativeStreamJournalRequire(binding.has_value());observed.console=*binding;
  }
  nativeStreamJournalRequire(consoles==1&&nativeStreamRetirementObservationValid(previous,observed,scope.tick,expiredSameLogin));return observed;
}
inline std::string nativeStreamRetirementObservationBytes(const NativeStreamProfileRecord& previous,const NativeStreamRetirementObservation& observed,const std::string& nonce,bool expiredSameLogin=false){
  nativeStreamJournalRequire(nativeRecoveryHex(nonce)&&nativeStreamRetirementObservationValid(previous,observed,observed.scope.tick,expiredSameLogin));
  const auto active=nativeActiveCcd(observed.topology);std::string paths="[";for(const auto& path:active.paths){if(paths.size()>1)paths+=',';paths+='[';const auto row=nativeBaselinePathRow(active,path);for(size_t i=0;i<row.size();++i){if(i)paths+=',';paths+=jsonQuote(std::to_string(row[i]));}paths+=']';}
  const auto packet="{\"type\":\"worker.profile.retirement.observed\",\"nonce\":"+jsonQuote(nonce)+",\"journalSha256\":"+jsonQuote(nativeBaselineDigest(nativeStreamProfileRecordBytes(previous)))+",\"scope\":"+nativeRecoveryScopeBytes(observed.scope)+",\"console\":"+nativeStreamConsoleBindingBytes(observed.console)+",\"paths\":"+paths+"],\"ownedOutputActive\":false}";
  nativeStreamJournalRequire(packet.size()<=65536);return packet;
}
inline NativeStreamRetirementObservation parseNativeStreamRetirementObservation(const Json& packet,const NativeStreamProfileRecord& previous,const std::string& nonce,uint64_t now,bool expiredSameLogin=false){
  nativeStreamJournalFields(packet,7);nativeStreamJournalRequire(nativeRecoveryHex(nonce)&&packet.get("type").string()=="worker.profile.retirement.observed"&&nativeStreamJournalString(packet.get("nonce"))==nonce&&nativeStreamJournalString(packet.get("journalSha256"))==nativeBaselineDigest(nativeStreamProfileRecordBytes(previous))&&std::holds_alternative<bool>(packet.get("ownedOutputActive").value)&&!packet.get("ownedOutputActive").boolean());
  NativeStreamRetirementObservation observed{parseNativeRecoveryScope(packet.get("scope")),parseNativeStreamConsoleBinding(packet.get("console")),parseNativeStreamProfilePaths(packet.get("paths")),false};nativeStreamJournalRequire(nativeStreamRetirementObservationValid(previous,observed,now,expiredSameLogin));return observed;
}
inline bool nativeStreamRetirementLeasesAbsent(multitap::vdisplay::api::ControlClient& client,std::span<const uint64_t> leases,std::span<const uint64_t> displays){
  using namespace multitap::vdisplay;
  if(leases.empty()||leases.size()>4||displays.size()!=2||!displays[0]||!displays[1]||displays[0]==displays[1]||!client.check_protocol_compatible().ok())return false;
  const auto absent=[&]{for(const auto lease:leases){if(lease<api::kMinOpaqueLeaseId)return false;api::LeaseRequest request{};request.lease_id=lease;const auto result=client.query_lease(request);if(!result.ok()||!recoveryLeaseAbsent(result.value,lease))return false;}return true;};
  if(!absent())return false;const auto state=client.query_display_state();if(!state.ok()||state.value.reserved||state.value.entry_count>api::kMaxDisplayStateEntries||uint64_t(state.value.entry_count)!=uint64_t(state.value.permanent_display_count)+state.value.temporary_display_count)return false;
  size_t temporary=0,permanent=0;
  for(size_t i=0;i<state.value.entry_count;++i){const auto& entry=state.value.entries[i];if(entry.kind==api::kDisplayStateKindTemporary)++temporary;else if(entry.kind==api::kDisplayStateKindPermanent)++permanent;else return false;if(std::find(displays.begin(),displays.end(),entry.display_id)!=displays.end()||(entry.lease_id&&std::find(leases.begin(),leases.end(),entry.lease_id)!=leases.end()))return false;}
  return temporary==state.value.temporary_display_count&&permanent==state.value.permanent_display_count&&absent();
}

// Index order is fixed and never supplied by a client. Archives preserve exact
// bytes, including the original DPAPI ciphertext and interrupted resize data.
inline constexpr std::array<const wchar_t*,7> nativeRetirementFiles={L"native-stream-profile.json",L"native-stream-launch.json",L"native-display-runtime.json",L"native-display.dpapi",L"native-stream-reclaim.json",L"native-display-baseline.json",L"native-stream-retirement.json"};
struct NativeStreamRetirementIo {
  NativeRuntimeJournalAccess marker;
  std::array<NativeRuntimeJournalAccess,6> active;
  std::function<std::optional<std::string>(const std::string&,size_t)> archive;
  std::function<bool(const std::string&,size_t,const std::string&)> saveArchive;
  std::function<bool(size_t,const std::string&)> erase;
  std::function<std::optional<std::string>(const std::string&,const std::string&)> history;
  std::function<bool(const std::string&,const std::string&,const std::string&)> saveHistory;
};
struct NativeStreamRetirementRecord {
  NativeStreamProfileKey key;std::string operation,phase="prepared",geometry,previousSha256,expiredConsoleSha256;
  NativeStreamProcessIdentity service,worker;NativeRecoveryScope fresh;NativeConsoleBinding console;
  uint64_t replacement=0;uint32_t generation=0;std::array<std::string,6> pins;
};
inline std::string nativeStreamRetirementBytes(const NativeStreamRetirementRecord& r){
  nativeStreamJournalRequire(nativeStreamJournalId(r.key.stream)&&nativeRecoveryHex(r.key.operation)&&nativeDisplayInstanceId(r.key.instance)&&r.key.session&&r.key.session!=UINT32_MAX&&nativeRecoveryHex(r.operation)&&r.operation!=r.key.operation&&nativeRecoveryHex(r.geometry)&&(r.phase=="prepared"||r.phase=="retired")&&r.replacement>=0x6000000000000000ull&&r.generation>1);
  nativeStreamJournalRequire(r.service.pid&&r.service.created&&r.worker.pid&&r.worker.created&&r.service.pid!=r.worker.pid&&r.fresh.session&&r.fresh.session!=UINT32_MAX&&nativeRecoveryHex(r.fresh.logon)&&r.fresh.tick&&!r.fresh.foreign.size());
  nativeStreamJournalRequire(r.previousSha256.empty()||nativeRecoveryHex(r.previousSha256));
  nativeStreamJournalRequire(r.expiredConsoleSha256.empty()||nativeRecoveryHex(r.expiredConsoleSha256));
  for(size_t i=0;i<r.pins.size();++i)nativeStreamJournalRequire((i>=4&&r.pins[i].empty())||nativeRecoveryHex(r.pins[i]));
  const auto process=[](const auto& p){return "{\"pid\":"+std::to_string(p.pid)+",\"created\":"+jsonQuote(nativeStreamU64(p.created))+'}';};
  std::string pins="[";for(const auto& pin:r.pins){if(pins.size()>1)pins+=',';pins+=jsonQuote(pin);}return std::string("{\"schema\":")+(r.expiredConsoleSha256.empty()?"1":"2")+",\"kind\":\"native-stream-retirement\",\"stream\":"+jsonQuote(r.key.stream)+",\"profileOperation\":"+jsonQuote(r.key.operation)+",\"deviceInstanceId\":"+jsonQuote(r.key.instance)+",\"session\":"+std::to_string(r.key.session)+",\"operation\":"+jsonQuote(r.operation)+",\"phase\":"+jsonQuote(r.phase)+",\"geometry\":"+jsonQuote(r.geometry)+",\"service\":"+process(r.service)+",\"worker\":"+process(r.worker)+",\"fresh\":"+nativeRecoveryScopeBytes(r.fresh)+",\"console\":"+nativeStreamConsoleBindingBytes(r.console)+",\"replacement\":"+jsonQuote(nativeStreamU64(r.replacement))+",\"generation\":"+std::to_string(r.generation)+",\"previousSha256\":"+jsonQuote(r.previousSha256)+",\"pins\":"+pins+"]"+(r.expiredConsoleSha256.empty()?std::string():",\"expiredConsoleSha256\":"+jsonQuote(r.expiredConsoleSha256))+'}';
}
inline NativeStreamRetirementRecord parseNativeStreamRetirement(const std::string& bytes){
  nativeStreamJournalRequire(!bytes.empty()&&bytes.size()<=16384);const auto root=JsonParser::parse(bytes);const auto schema=root.get("schema").number();nativeStreamJournalRequire(schema==1||schema==2);nativeStreamJournalFields(root,schema==1?17:18);nativeStreamJournalRequire(root.get("kind").string()=="native-stream-retirement");
  NativeStreamRetirementRecord r;r.key={nativeStreamJournalString(root.get("stream")),nativeStreamJournalString(root.get("profileOperation")),nativeStreamJournalString(root.get("deviceInstanceId")),nativeStreamJournalU32(root.get("session"),1,UINT32_MAX-1)};
  r.operation=nativeStreamJournalString(root.get("operation"));r.phase=nativeStreamJournalString(root.get("phase"));r.geometry=nativeStreamJournalString(root.get("geometry"));
  r.previousSha256=nativeStreamJournalString(root.get("previousSha256"));
  if(schema==2){r.expiredConsoleSha256=nativeStreamJournalString(root.get("expiredConsoleSha256"));nativeStreamJournalRequire(nativeRecoveryHex(r.expiredConsoleSha256));}
  const auto process=[](const Json& p){nativeStreamJournalFields(p,2);return NativeStreamProcessIdentity{nativeStreamJournalU32(p.get("pid"),1,UINT32_MAX-1),parseNativeStreamU64(p.get("created"))};};r.service=process(root.get("service"));r.worker=process(root.get("worker"));r.fresh=parseNativeRecoveryScope(root.get("fresh"));r.console=parseNativeStreamConsoleBinding(root.get("console"));r.replacement=parseNativeStreamU64(root.get("replacement"));r.generation=nativeStreamJournalU32(root.get("generation"),2);
  const auto* pins=std::get_if<Json::Array>(&root.get("pins").value);nativeStreamJournalRequire(pins&&pins->size()==r.pins.size());for(size_t i=0;i<r.pins.size();++i)r.pins[i]=nativeStreamJournalString((*pins)[i]);nativeStreamJournalRequire(nativeStreamRetirementBytes(r)==bytes);return r;
}
inline NativeRuntimeJournalAccess nativeStreamRetirementMarkerAccess(const std::filesystem::path& path,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  nativeStreamJournalRequire(path.filename()==L"native-stream-retirement.json");return nativeJournalFileAccess(path,16384,[](const auto& bytes){(void)parseNativeStreamRetirement(bytes);},std::move(trusted));
}
inline NativeStreamRetirementIo nativeStreamRetirementFileAccess(const std::filesystem::path& directory,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  nativeStreamJournalRequire(directory.filename()==L"private");
  const std::array<size_t,7> limits={60000,8192,4096,8192,32768,65536,16384};
  const auto archivePath=[directory](const std::string& operation,size_t index){nativeStreamJournalRequire(nativeRecoveryHex(operation)&&index<nativeRetirementFiles.size());return directory/L"native-stream-history"/wide(operation)/nativeRetirementFiles[index];};
  NativeStreamRetirementIo io;io.marker=nativeStreamRetirementMarkerAccess(directory/L"native-stream-retirement.json",trusted);
  io.active={nativeStreamProfileFileAccess(directory/nativeRetirementFiles[0],trusted),nativeStreamLaunchFileAccess(directory/nativeRetirementFiles[1],trusted),nativeRuntimeFileAccess(directory/nativeRetirementFiles[2],trusted),nativeDisplayIdentityFileAccess(directory/nativeRetirementFiles[3],trusted),nativeStreamReclaimFileAccess(directory/nativeRetirementFiles[4],trusted),nativeBaselineFileAccess(directory/nativeRetirementFiles[5],trusted)};
  io.archive=[archivePath,limits,trusted](const std::string& operation,size_t index)->std::optional<std::string>{
    const auto path=archivePath(operation,index);const auto attributes=GetFileAttributesW(path.parent_path().c_str());if(attributes==INVALID_FILE_ATTRIBUTES){const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return {};throw std::runtime_error("native_retirement_archive_unreadable");}
    if(!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("native_retirement_archive_untrusted");return readNativeJournalFile(path,trusted,limits[index]);
  };
  io.saveArchive=[archivePath,limits,trusted,directory](const std::string& operation,size_t index,const std::string& bytes){
    try{
      const auto path=archivePath(operation,index);if(!trusted(directory))return false;
      for(const auto& parent:{directory/L"native-stream-history",path.parent_path()}){
        if(!CreateDirectoryW(parent.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return false;const auto attributes=GetFileAttributesW(parent.c_str());if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)||!trusted(parent))return false;
      }
      const auto archived=nativeJournalFileAccess(path,limits[index],[](const std::string&){},trusted);return archived.save({},bytes)&&archived.load()==std::optional(bytes);
    }catch(...){return false;}
  };
  io.erase=[directory,limits,trusted](size_t index,const std::string& expected){
    try{
      if(index!=0&&index!=1&&index!=4&&index!=5)return false;const auto path=directory/nativeRetirementFiles[index];if(!trusted(directory)||!trusted(path)||expected.size()>limits[index])return false;
      WinHandle file(CreateFileW(path.c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};
      if(file.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(file.value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||info.nFileSizeHigh||info.nFileSizeLow!=expected.size())return false;
      std::string bytes(expected.size(),'\0');DWORD read=0;if(!ReadFile(file.value,bytes.data(),DWORD(bytes.size()),&read,nullptr)||read!=bytes.size()||bytes!=expected)return false;
      // Delete the exact locked handle whose bytes were checked. Never follow
      // a path a second time after proving the active source snapshot.
      FILE_DISPOSITION_INFO disposition{TRUE};if(!SetFileInformationByHandle(file.value,FileDispositionInfo,&disposition,sizeof(disposition)))return false;file=WinHandle();return true;
    }catch(...){return false;}
  };
  io.history=[directory,trusted](const std::string& operation,const std::string& digest)->std::optional<std::string>{
    nativeStreamJournalRequire(nativeRecoveryHex(operation)&&nativeRecoveryHex(digest));const auto path=directory/L"native-stream-history"/L"records"/(wide(digest)+L".record.json");
    const auto attributes=GetFileAttributesW(path.parent_path().c_str());if(attributes==INVALID_FILE_ATTRIBUTES){const auto error=GetLastError();if(error==ERROR_PATH_NOT_FOUND||error==ERROR_FILE_NOT_FOUND)return {};throw std::runtime_error("native_retirement_archive_unreadable");}
    if(!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("native_retirement_archive_untrusted");const auto bytes=readNativeJournalFile(path,trusted,16384);if(bytes)nativeStreamJournalRequire(nativeBaselineDigest(*bytes)==digest&&parseNativeStreamRetirement(*bytes).operation==operation);return bytes;
  };
  io.saveHistory=[directory,trusted](const std::string& operation,const std::string& digest,const std::string& bytes){
    try{nativeStreamJournalRequire(nativeRecoveryHex(operation)&&nativeRecoveryHex(digest)&&nativeBaselineDigest(bytes)==digest&&parseNativeStreamRetirement(bytes).operation==operation);const auto parent=directory/L"native-stream-history"/L"records";if(!CreateDirectoryW(parent.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return false;const auto attributes=GetFileAttributesW(parent.c_str());if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)||!trusted(parent))return false;const auto path=parent/(wide(digest)+L".record.json");return nativeJournalFileAccess(path,16384,[](const auto& value){(void)parseNativeStreamRetirement(value);},trusted).save({},bytes);}catch(...){return false;}
  };
  return io;
}
struct NativeStreamRetirementExpiredConsoleProof {std::string recordSha256,profileSha256,launchSha256,geometry;};
struct NativeStreamRetirementAuthority {
  std::function<NativeStreamProcessState(const NativeStreamProcessIdentity&)> process;
  std::function<std::optional<NativeStreamRetirementObservation>()> display;
  std::function<bool(const NativeStreamRetirementRecord&)> identity;
  std::function<bool(std::span<const uint64_t>,std::span<const uint64_t>)> leasesAbsent;
  std::function<uint64_t()> now;
  // The production callback independently validates a protected COMPLETED
  // expiry transaction and its unchanged source pins/current restored CCD.
  std::function<std::optional<NativeStreamRetirementExpiredConsoleProof>(const NativeStreamProfileRecord&,const NativeStreamRetirementObservation&)> expiredConsole;
};
class NativeStreamRetirementJournal {
 public:
  explicit NativeStreamRetirementJournal(NativeStreamRetirementIo access):io_(std::move(access)){}
  // Fixed code of the most recent rejection (diagnostics only, never a decision).
  const std::string& rejection()const{return rejection_;}
  std::optional<NativeStreamRetirementRecord> prepare(const std::string& operation,uint64_t replacement,NativeStreamProcessIdentity service,NativeStreamProcessIdentity worker,const NativeStreamRetirementAuthority& authority){
    rejection_.clear();const auto refuse=[&](const char* code){if(rejection_.empty())rejection_=code;return std::optional<NativeStreamRetirementRecord>{};};
    try{
      std::array<std::optional<std::string>,6> original;NativeStreamRetirementRecord r;r.operation=operation;r.service=service;r.worker=worker;r.replacement=replacement;
      for(size_t i=0;i<original.size();++i){original[i]=io_.active[i].load();if(original[i])r.pins[i]=nativeBaselineDigest(*original[i]);}
      if(!original[0]||!original[1]||!original[2]||!original[3]||!authority.display)return refuse("records_missing");
      const auto profile=parseNativeStreamProfileRecord(*original[0]);const auto launch=parseNativeStreamLaunch(*original[1]);const auto runtime=parseNativeRuntimeRecord(*original[2]);
      if(profile.phase==NativeStreamProfilePhase::restored)return refuse("profile_restored");
      if(!nativeStreamLaunchMatches(launch,profile))return refuse("launch_mismatch");
      if(runtime.instance!=profile.key.instance||runtime.generation<profile.current.generation||runtime.generation==UINT32_MAX)return refuse("runtime_mismatch");
      r.key=profile.key;r.generation=runtime.generation+1;const auto observed=authority.display();if(!observed)return refuse("observation_missing");if(observed->scope.logon==profile.scope.logon){const auto proof=authority.expiredConsole?authority.expiredConsole(profile,*observed):std::nullopt;if(!proof||!nativeRecoveryHex(proof->recordSha256)||proof->profileSha256!=r.pins[0]||proof->launchSha256!=r.pins[1]||proof->geometry!=nativeRetirementGeometryDigest(*observed))return refuse("expired_console_proof");r.expiredConsoleSha256=proof->recordSha256;}
      {std::string reason;if(!nativeStreamRetirementObservationValid(profile,*observed,authority.now(),!r.expiredConsoleSha256.empty(),&reason))return refuse(reason.empty()?"observation_invalid":reason.c_str());}
      r.fresh=observed->scope;r.fresh.foreign.clear();r.console=observed->console;r.geometry=nativeRetirementGeometryDigest(*observed);
      const auto desired=nativeStreamRetirementBytes(r);const auto before=io_.marker.load();
      if(before&&*before!=desired){const auto previous=parseNativeStreamRetirement(*before);if(previous.phase!="retired"||previous.key.operation==r.key.operation||!io_.history||io_.history(previous.operation,nativeBaselineDigest(*before))!=before)return refuse("marker_conflict");}
      // Archive before touching any active file. Partial archive writes cannot
      // authorize retirement or destroy the existing recovery checkpoint.
      for(size_t i=0;i<original.size();++i)if(original[i]){const auto saved=io_.archive(operation,i);if(saved&&saved!=original[i])return refuse("archive_failed");if(!saved&&!io_.saveArchive(operation,i,*original[i]))return refuse("archive_failed");if(io_.archive(operation,i)!=original[i]||io_.active[i].load()!=original[i])return refuse("archive_failed");}
      if(!authorized(r,authority))return refuse("failed");
      if(!activeAllowed(r))return refuse("archive_failed");
      if(io_.marker.load()!=before)return refuse("marker_conflict");
      if(before!=std::optional(desired)&&!io_.marker.save(before,desired))return refuse("marker_conflict");if(io_.marker.load()!=desired)return refuse("marker_conflict");if(!authorized(r,authority))return refuse("failed");if(!activeAllowed(r))return refuse("archive_failed");return r;
    }catch(...){return refuse("records_invalid");}
  }
  bool commit(const NativeStreamRetirementRecord& r,const NativeStreamRetirementAuthority& authority){
    rejection_.clear();const bool ok=commitImpl(r,authority);if(!ok&&rejection_.empty())rejection_="commit_failed";return ok;
  }
 private:
  bool commitImpl(const NativeStreamRetirementRecord& r,const NativeStreamRetirementAuthority& authority){
    try{
      if(r.phase!="prepared"||io_.marker.load()!=nativeStreamRetirementBytes(r)||!authorized(r,authority)||!activeAllowed(r))return false;
      const auto identity=unsealNativeDisplayIdentity(*io_.archive(r.operation,3));const auto current=[&]{return io_.marker.load()==nativeStreamRetirementBytes(r)&&authorized(r,authority)&&activeAllowed(r);};
      if(!NativeDisplayIdentityStore(io_.active[3]).rotate(identity,r.replacement,current)||!current())return false;
      const auto runtime=desiredRuntime(r);const auto previous=io_.active[2].load();if(!previous)return false;if(*previous!=runtime){(void)io_.active[2].save(previous,runtime);if(io_.active[2].load()!=runtime)return false;}
      if(!current())return false;
      for(const auto index:{size_t(0),size_t(1),size_t(4),size_t(5)}){
        const auto value=io_.active[index].load();if(!value)continue;if(nativeBaselineDigest(*value)!=r.pins[index]||!current())return false;(void)io_.erase(index,*value);if(io_.active[index].load())return false;
      }
      if(!current()||!completed(r))return false;auto retired=r;retired.phase="retired";const auto bytes=nativeStreamRetirementBytes(retired);
      if(!io_.history||!io_.saveHistory)return false;const auto digest=nativeBaselineDigest(bytes);const auto archived=io_.history(r.operation,digest);if(archived&&*archived!=bytes)return false;if(!archived&&!io_.saveHistory(r.operation,digest,bytes))return false;if(io_.history(r.operation,digest)!=std::optional(bytes)||!current())return false;
      (void)io_.marker.save(nativeStreamRetirementBytes(r),bytes);return io_.marker.load()==bytes&&completed(r)&&authorized(r,authority);
    }catch(...){return false;}
  }
public:
  std::optional<NativeStreamRetirementRecord> handoff(NativeStreamProcessIdentity service,NativeStreamProcessIdentity worker,const NativeStreamRetirementAuthority& authority){
    try{
      const auto previous=io_.marker.load();if(!previous||!authority.process||!authority.display||!io_.history||!io_.saveHistory)return {};const auto held=parseNativeStreamRetirement(*previous);
      const auto exited=[&]{return held.worker!=worker&&authority.process(held.worker)==NativeStreamProcessState::gone&&(held.service==service||authority.process(held.service)==NativeStreamProcessState::gone);};
      if(held.phase!="prepared"||!exited())return {};auto next=held;next.service=service;next.worker=worker;next.previousSha256=nativeBaselineDigest(*previous);
      const auto observed=authority.display();if(!observed)return {};next.fresh=observed->scope;next.fresh.foreign.clear();next.console=observed->console;next.geometry=nativeRetirementGeometryDigest(*observed);const auto source=io_.archive(held.operation,0);if(!source)return {};const auto profile=parseNativeStreamProfileRecord(*source);if(observed->scope.logon==profile.scope.logon){const auto proof=authority.expiredConsole?authority.expiredConsole(profile,*observed):std::nullopt;if(!proof||proof->profileSha256!=held.pins[0]||proof->launchSha256!=held.pins[1]||proof->geometry!=next.geometry||!nativeRecoveryHex(proof->recordSha256))return {};next.expiredConsoleSha256=proof->recordSha256;}
      const auto desired=nativeStreamRetirementBytes(next);if(!activeAllowed(next)||io_.marker.load()!=previous||!exited())return {};
      const auto archived=io_.history(held.operation,next.previousSha256);if(archived&&archived!=previous)return {};if(!archived&&!io_.saveHistory(held.operation,next.previousSha256,*previous))return {};
      if(io_.history(held.operation,next.previousSha256)!=previous||!authorized(next,authority)||!activeAllowed(next)||io_.marker.load()!=previous||!exited())return {};
      (void)io_.marker.save(previous,desired);if(io_.marker.load()!=std::optional(desired)||!authorized(next,authority)||!activeAllowed(next)||!exited())return {};return next;
    }catch(...){return {};}
  }
 private:
  std::string desiredRuntime(const NativeStreamRetirementRecord& r)const{auto value=parseNativeRuntimeRecord(*io_.archive(r.operation,2));value.phase="stopped";value.generation=r.generation;value.baselineHash.clear();return nativeRuntimeBytes(value);}
  bool completed(const NativeStreamRetirementRecord& r)const{
    try{const auto runtime=io_.active[2].load(),identity=io_.active[3].load();if(!runtime||*runtime!=desiredRuntime(r)||!identity||unsealNativeDisplayIdentity(*identity).value.lease!=r.replacement)return false;for(const auto index:{size_t(0),size_t(1),size_t(4),size_t(5)})if(io_.active[index].load())return false;return activeAllowed(r);}catch(...){return false;}
  }
  bool activeAllowed(const NativeStreamRetirementRecord& r)const{
    try{
      std::array<std::optional<std::string>,6> original,current;
      for(size_t i=0;i<r.pins.size();++i){original[i]=io_.archive(r.operation,i);current[i]=io_.active[i].load();if(r.pins[i].empty()){if(original[i]||current[i])return false;}else if(!original[i]||nativeBaselineDigest(*original[i])!=r.pins[i])return false;}
      const auto before=unsealNativeDisplayIdentity(*original[3]);if(!current[3]||!current[2])return false;const auto observed=unsealNativeDisplayIdentity(*current[3]);const bool rotated=observed.value.lease==r.replacement;
      if(!sameNativeDisplayOwner(before.value,observed.value)||(!rotated&&current[3]!=original[3]))return false;
      if(current[2]!=original[2]&&(!rotated||*current[2]!=desiredRuntime(r)))return false;
      for(const auto index:{size_t(0),size_t(1),size_t(4),size_t(5)})if(current[index]&&current[index]!=original[index])return false;
      return true;
    }catch(...){return false;}
  }
  bool authorized(const NativeStreamRetirementRecord& r,const NativeStreamRetirementAuthority& a)const{
    const auto deny=[this](const char* code){if(rejection_.empty())rejection_=code;return false;};
    try{
      if(!a.process||!a.display||!a.identity||!a.leasesAbsent||!a.now)return deny("failed");if(!a.identity(r))return deny("identity_rejected");
      const auto profileBytes=io_.archive(r.operation,0),launchBytes=io_.archive(r.operation,1),identityBytes=io_.archive(r.operation,3);if(!profileBytes||!launchBytes||!identityBytes)return deny("records_missing");
      const auto profile=parseNativeStreamProfileRecord(*profileBytes);const auto launch=parseNativeStreamLaunch(*launchBytes);const auto identity=unsealNativeDisplayIdentity(*identityBytes);
      if(r.key!=profile.key||!nativeStreamLaunchMatches(launch,profile)||r.service==launch.service||r.worker==launch.worker||r.service==launch.worker||r.worker==launch.service)return deny("launch_mismatch");
      if(a.process(launch.service)!=NativeStreamProcessState::gone||a.process(launch.worker)!=NativeStreamProcessState::gone)return deny("original_process_alive");
      if(a.process(r.service)!=NativeStreamProcessState::running||a.process(r.worker)!=NativeStreamProcessState::running)return deny("executor_not_running");
      if(!r.previousSha256.empty()){
        if(!io_.history)return deny("previous_marker");const auto bytes=io_.history(r.operation,r.previousSha256);if(!bytes||nativeBaselineDigest(*bytes)!=r.previousSha256)return deny("previous_marker");const auto previous=parseNativeStreamRetirement(*bytes);
        if(previous.phase!="prepared"||previous.operation!=r.operation||previous.key!=r.key||previous.pins!=r.pins||previous.replacement!=r.replacement||previous.generation!=r.generation||previous.worker==r.worker||a.process(previous.worker)!=NativeStreamProcessState::gone||(previous.service!=r.service&&a.process(previous.service)!=NativeStreamProcessState::gone))return deny("previous_marker");
      }
      const auto observed=a.display();if(!observed)return deny("observation_missing");
      {std::string reason;if(!nativeStreamRetirementObservationValid(profile,*observed,a.now(),!r.expiredConsoleSha256.empty(),&reason))return deny(reason.empty()?"observation_invalid":reason.c_str());}
      if(observed->scope.session!=r.fresh.session||observed->scope.logon!=r.fresh.logon||!sameNativeConsoleBinding(observed->console,r.console)||nativeRetirementGeometryDigest(*observed)!=r.geometry)return deny("observation_changed");
      if(observed->scope.logon==profile.scope.logon){const auto proof=a.expiredConsole?a.expiredConsole(profile,*observed):std::nullopt;if(!proof||proof->recordSha256!=r.expiredConsoleSha256||proof->profileSha256!=r.pins[0]||proof->launchSha256!=r.pins[1]||proof->geometry!=r.geometry)return deny("expired_console_proof");}
      for(size_t i=0;i<launch.outputs.size();++i)if(identity.value.displays[i]!=launch.outputs[i].display)return deny("owner_unlinked");
      bool ownerLinked=identity.value.lease==launch.lease;
      std::vector<uint64_t> leases={identity.value.lease,launch.lease,r.replacement};const auto reclaim=io_.archive(r.operation,4);if(reclaim){const auto pending=parseNativeStreamReclaim(*reclaim);if(pending.key!=profile.key||pending.previous!=launch.lease||pending.launchSha256!=nativeBaselineDigest(*launchBytes)||pending.identityBefore.empty()||!sameNativeDisplayOwner(identity.value,unsealNativeDisplayIdentity(pending.identityBefore).value))return deny("owner_unlinked");ownerLinked=ownerLinked||identity.value.lease==pending.replacement;leases.push_back(pending.previous);leases.push_back(pending.replacement);}
      std::sort(leases.begin(),leases.end());leases.erase(std::unique(leases.begin(),leases.end()),leases.end());if(leases.size()>4||r.replacement==identity.value.lease||r.replacement==launch.lease)return deny("owner_unlinked");
      if(!ownerLinked)return deny("owner_unlinked");if(!a.leasesAbsent(leases,std::span<const uint64_t>(identity.value.displays,2)))return deny("leases_present");if(!a.identity(r))return deny("identity_rejected");
      if(a.process(r.service)!=NativeStreamProcessState::running||a.process(r.worker)!=NativeStreamProcessState::running)return deny("executor_not_running");return true;
    }catch(...){return deny("records_invalid");}
  }
  NativeStreamRetirementIo io_;mutable std::string rejection_;
};
}
#endif
