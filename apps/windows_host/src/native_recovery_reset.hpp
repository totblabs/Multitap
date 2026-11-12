#pragma once
#include "native_recovery_trace.hpp"
#include "broker_status.hpp"
#include "native_stream_expired_integration.hpp"
#include "native_stream_expired_console.hpp"
#include <set>
namespace host {
// Administrator escape hatch for stale native stream-profile recovery records.
// It is never automatic and never a substitute for the bounded recovery
// transactions: the service refuses while any session or display change is
// active, proves every known Multitap lease/output absent in the SDK, lets the
// elevated console-session settings process verify (and, only for the
// positively identified QEMU console, re-enable) an active display, and then
// MOVES the records into a timestamped backup folder. No delete, no reboot, no
// driver, unrelated display, registry or security-setting change.
struct NativeResetStep {std::string step,code;bool ok=false;};
inline const std::set<std::string>& nativeResetStepNames(){static const std::set<std::string> names{"confirm","session","service","sessions","pending","sdk","console","archive","reinitialize"};return names;}
inline const std::set<std::string>& nativeResetCodes(){
  static const std::set<std::string> codes{"confirmed","confirmation_required","console","remote_session","not_console","locked","connected","service_unavailable","service_busy","protocol",
    "idle","stream_active","rdp_active","rdp_session_active","console_operation","display_operation","recovery_running","worker_running",
    "none","change_pending","controller_active","reservation_held","absent","present","transport_unavailable","identity_unavailable","not_selected",
    "console_active","console_reenabled","display_active","no_active_display","owned_output_active","reenable_rejected","reenable_unverified","query_failed","ineligible",
    "moved","nothing_to_move","backup_failed","move_partial","reinitialized","failed"};
  return codes;
}
inline std::string nativeResetStepsJson(const std::vector<NativeResetStep>& steps){
  std::string out="[";for(const auto& step:steps){if(!nativeResetStepNames().count(step.step))continue;if(out.size()>1)out+=',';out+="{\"step\":"+jsonQuote(step.step)+",\"ok\":"+(step.ok?"true":"false")+",\"code\":"+jsonQuote(nativeResetCodes().count(step.code)?step.code:"failed")+'}';}
  return out+']';
}
inline std::string nativeResetResultJson(bool ok,const std::vector<NativeResetStep>& steps,size_t moved){
  std::string code="none";for(const auto& step:steps)if(!step.ok){code=nativeResetCodes().count(step.code)?step.code:"failed";break;}
  return "{\"type\":\"native.reset.result\",\"ok\":"+std::string(ok?"true":"false")+",\"code\":"+jsonQuote(ok?"reinitialized":code)+",\"moved\":"+std::to_string(std::min<size_t>(moved,64))+",\"steps\":"+nativeResetStepsJson(steps)+'}';
}
// Re-encodes a received result through the fixed vocabulary (client side).
inline std::string nativeResetResultProjection(const Json& value,std::vector<NativeResetStep> prefix){
  const auto* steps=std::get_if<Json::Array>(&value.get("steps").value);const auto* ok=std::get_if<bool>(&value.get("ok").value);const auto moved=value.get("moved").number(-1);
  if(value.get("type").string()!="native.reset.result"||!steps||steps->size()>16||!ok||!nativeDisplayInteger(moved,0,64))throw std::runtime_error("protocol");
  for(const auto& item:*steps){const auto* bit=std::get_if<bool>(&item.get("ok").value);const auto name=item.get("step").string(),code=item.get("code").string();if(!bit||!nativeResetStepNames().count(name)||!nativeResetCodes().count(code))throw std::runtime_error("protocol");prefix.push_back({name,code,*bit});}
  const bool all=*ok&&std::all_of(prefix.begin(),prefix.end(),[](const auto& step){return step.ok;});
  return nativeResetResultJson(all,prefix,size_t(moved));
}
inline std::wstring nativeResetPipeName(DWORD pid){return L"\\\\.\\pipe\\MultitapHost.NativeReset."+std::to_wstring(pid);}
// Bounded framed transfer (4-byte length + UTF-8 JSON, at most 8 KiB).
inline bool nativeResetTransfer(HANDLE pipe,bool write,void* bytes,DWORD size,DWORD timeoutMs){
  auto* at=static_cast<uint8_t*>(bytes);const uint64_t begun=GetTickCount64();
  while(size){
    WinHandle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.value)return false;
    OVERLAPPED operation{};operation.hEvent=event.value;DWORD count=0;
    BOOL ok=write?WriteFile(pipe,at,size,&count,&operation):ReadFile(pipe,at,size,&count,&operation);
    if(!ok&&GetLastError()==ERROR_IO_PENDING){
      for(;;){const DWORD wait=WaitForSingleObject(event.value,25);if(wait==WAIT_OBJECT_0)break;
        if(wait!=WAIT_TIMEOUT||GetTickCount64()-begun>timeoutMs){CancelIoEx(pipe,&operation);GetOverlappedResult(pipe,&operation,&count,TRUE);return false;}}
      ok=GetOverlappedResult(pipe,&operation,&count,FALSE);
    }
    if(!ok||!count||count>size)return false;at+=count;size-=count;
  }
  return true;
}
inline bool nativeResetSend(HANDLE pipe,const std::string& bytes,DWORD timeoutMs=2000){if(bytes.empty()||bytes.size()>8192)return false;DWORD length=DWORD(bytes.size());auto copy=bytes;return nativeResetTransfer(pipe,true,&length,sizeof(length),timeoutMs)&&nativeResetTransfer(pipe,true,copy.data(),length,timeoutMs);}
inline std::optional<Json> nativeResetReceive(HANDLE pipe,DWORD timeoutMs){
  DWORD length=0;if(!nativeResetTransfer(pipe,false,&length,sizeof(length),timeoutMs)||!length||length>8192)return {};
  std::string bytes(length,'\0');if(!nativeResetTransfer(pipe,false,bytes.data(),length,timeoutMs))return {};
  try{const auto value=JsonParser::parse(bytes);if(!std::holds_alternative<Json::Object>(value.value))return {};return value;}catch(...){return {};}
}
#ifdef MULTITAP_NATIVE_DISPLAY
// Moved, in this order. Identity (DPAPI owner) and history archives are kept.
inline constexpr std::array<const wchar_t*,8> nativeResetRecordFiles{L"native-stream-profile.json",L"native-stream-launch.json",L"native-stream-viewport-commit.json",L"native-stream-reclaim.json",L"native-stream-expired-console.json",L"native-stream-retirement.json",L"native-display-baseline.json",L"native-display-runtime.json"};
struct NativeResetLeaseSet {std::vector<uint64_t> leases;std::array<uint64_t,2> displays{};};
// Production binds real private files/SDK; fixtures inject memory equivalents.
struct NativeResetIo {
  std::function<std::optional<NativeResetLeaseSet>()> leases;
  std::function<std::string(const NativeResetLeaseSet&)> sdk;
  std::function<std::string(size_t&)> move;
};
// Every lease the protected records can name. Unreadable optional records only
// remove their own candidates; the DPAPI identity (owner displays) is required.
inline std::optional<NativeResetLeaseSet> nativeResetKnownLeases(const std::filesystem::path& directory){
  NativeResetLeaseSet result;
  try{const auto bytes=nativeDisplayIdentityFileAccess(directory/L"native-display.dpapi").load();if(!bytes)return {};const auto identity=unsealNativeDisplayIdentity(*bytes);result.leases.push_back(identity.value.lease);result.displays={identity.value.displays[0],identity.value.displays[1]};}catch(...){return {};}
  const auto add=[&](uint64_t lease){if(lease>=0x6000000000000000ull&&std::find(result.leases.begin(),result.leases.end(),lease)==result.leases.end())result.leases.push_back(lease);};
  try{if(const auto bytes=nativeStreamLaunchFileAccess(directory/L"native-stream-launch.json").load())add(parseNativeStreamLaunch(*bytes).lease);}catch(...){ }
  try{if(const auto bytes=nativeStreamReclaimFileAccess(directory/L"native-stream-reclaim.json").load()){const auto intent=parseNativeStreamReclaim(*bytes);add(intent.previous);add(intent.replacement);}}catch(...){ }
  try{if(const auto bytes=nativeStreamRetirementMarkerAccess(directory/L"native-stream-retirement.json").load())add(parseNativeStreamRetirement(*bytes).replacement);}catch(...){ }
  return result;
}
// "absent" only when the SDK answers that no known lease and no owned display
// exists; any query failure is "present" (unknown is never absence).
inline std::string nativeResetSdkCode(multitap::vdisplay::api::ControlClient& client,const NativeResetLeaseSet& known){
  try{
    if(known.leases.empty())return "present";
    for(size_t at=0;at<known.leases.size();at+=4){const auto count=std::min<size_t>(4,known.leases.size()-at);if(!nativeStreamRetirementLeasesAbsent(client,std::span<const uint64_t>(known.leases.data()+at,count),known.displays))return "present";}
    return "absent";
  }catch(...){return "present";}
}
inline std::string nativeResetStamp(){SYSTEMTIME now{};GetSystemTime(&now);wchar_t text[32]{};swprintf_s(text,L"%04u%02u%02uT%02u%02u%02uZ",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);return utf8(text)+"-"+utf8(randomName()).substr(0,8);}
// Same-volume renames keep each file's security descriptor. The backup folder
// inherits the protected private-directory ACL and must itself be trusted.
inline std::string moveNativeRecoveryRecords(const std::filesystem::path& directory,const std::string& stamp,size_t& moved,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  moved=0;
  try{
    if(directory.filename()!=L"private"||!trusted(directory)||stamp.empty()||stamp.size()>40||stamp.find_first_not_of("0123456789TZabcdef-")!=std::string::npos)return "backup_failed";
    std::vector<std::filesystem::path> present;
    for(const auto* name:nativeResetRecordFiles){const auto path=directory/name;const auto attributes=GetFileAttributesW(path.c_str());if(attributes==INVALID_FILE_ATTRIBUTES){if(GetLastError()==ERROR_FILE_NOT_FOUND)continue;return "backup_failed";}if((attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||!trusted(path))return "backup_failed";present.push_back(path);}
    if(present.empty())return "nothing_to_move";
    const auto root=directory/L"native-recovery-reset",target=root/wide(stamp);
    for(const auto& folder:{root,target}){if(!CreateDirectoryW(folder.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return "backup_failed";const auto attributes=GetFileAttributesW(folder.c_str());if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)||!trusted(folder))return "backup_failed";}
    for(const auto& path:present){const auto destination=target/path.filename();
      if(!MoveFileExW(path.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)||GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES||!trusted(destination))return moved?"move_partial":"backup_failed";
      ++moved;}
    return "moved";
  }catch(...){return moved?"move_partial":"backup_failed";}
}
// Console verification runs in the elevated settings process inside the
// active console session (never in an RDP session).
struct NativeResetConsoleAccess {
  std::function<bool()> eligible;
  std::function<NativeCcdData()> query;
  std::function<LONG(const NativeCcdData&,UINT32)> configure;
  NativeConsoleIdentityResolver adapter;
  std::function<std::optional<std::string>()> profile; // protected profile record bytes, if any
  std::string ownedInstance,machine;
};
struct NativeResetConsoleResult {bool ok=false;std::string code="query_failed";bool consoleKnown=false;size_t activeDisplays=0;DWORD systemError=0;};
inline NativeResetConsoleResult nativeResetConsoleStep(const NativeResetConsoleAccess& access){
  NativeResetConsoleResult result;
  try{
    if(!access.eligible||!access.query||!access.configure||!access.adapter||!access.eligible()){result.code="ineligible";return result;}
    const auto selected=nativeConsoleAscii(wide(access.ownedInstance),200);if(!selected){result.code="ineligible";return result;}
    NativeCcdData ccd;try{ccd=access.query();}catch(const NativeTopologyWindowsError& error){result.systemError=error.code;return result;}
    const auto active=nativeActiveCcd(ccd);result.activeDisplays=active.paths.size();
    for(const auto& path:active.paths)for(const auto luid:{path.sourceInfo.adapterId,path.targetInfo.adapterId}){
      const auto device=access.adapter(luid);const auto canonical=device?canonicalNativeConsoleAdapter(*device):std::nullopt;
      if(canonical&&canonical->instanceId==*selected){result.code="owned_output_active";return result;}
    }
    // Positive identity comes only from the protected profile binding plus the
    // same live hardware; a display number, name or count is never evidence.
    std::optional<NativeStreamProfileRecord> record;
    try{if(access.profile)if(const auto bytes=access.profile())record=parseNativeStreamProfileRecord(*bytes);}catch(...){record.reset();}
    if(record&&record->profile.suppressedConsole&&nativeConsoleUuid(access.machine)==nativeConsoleUuid(record->console.proof.machineUuid)){
      const auto& key=*record->profile.suppressedConsole;const auto hardware=access.adapter(key.targetAdapter);
      auto current=record->console;if(hardware)current.device=*hardware;
      if(hardware&&sameNativeConsoleBinding(current,record->console)){
        result.consoleKnown=true;const DISPLAYCONFIG_PATH_INFO* available=nullptr;size_t count=0;
        for(const auto& path:ccd.paths)if(nativeStreamConsolePathMatches(path,key)&&path.targetInfo.targetAvailable){available=&path;++count;}
        if(count==1&&(available->flags&DISPLAYCONFIG_PATH_ACTIVE)){result.ok=true;result.code="console_active";return result;}
        if(count!=1){result.code=result.activeDisplays?"display_active":"no_active_display";result.ok=result.activeDisplays>0;result.consoleKnown=false;return result;}
        // Re-enable ONLY this console path with its saved modes; every other
        // active path is supplied unchanged. Windows validates before apply.
        constexpr UINT32 options=SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_VIRTUAL_MODE_AWARE;
        NativeCcdData restored;try{restored=nativeStreamProfileWithConsole(record->profile,ccd,active);}catch(...){result.code="reenable_rejected";return result;}
        if(!access.eligible()){result.code="ineligible";return result;}
        if(const auto error=access.configure(restored,SDC_VALIDATE|options)){result.code="reenable_rejected";result.systemError=DWORD(error);return result;}
        const auto prepared=access.query();if(!access.eligible()||nativeActiveSignatures(prepared)!=nativeActiveSignatures(ccd)){result.code="reenable_rejected";return result;}
        if(const auto error=access.configure(restored,SDC_APPLY|options)){result.code="reenable_rejected";result.systemError=DWORD(error);return result;}
        const auto after=access.query();const auto console=std::find_if(after.paths.begin(),after.paths.end(),[&](const auto& path){return (path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativeStreamConsolePathMatches(path,key);});
        // Preserve every other previously active path's identity (source/target).
        const auto kept=std::all_of(active.paths.begin(),active.paths.end(),[&](const auto& before){return std::any_of(after.paths.begin(),after.paths.end(),[&](const auto& path){return (path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativeStreamProfileTargetKey(path)==nativeStreamProfileTargetKey(before)&&nativeSourceKey(path)==nativeSourceKey(before);});});
        result.activeDisplays=nativeActiveCcd(after).paths.size();
        if(console==after.paths.end()||!kept){result.code="reenable_unverified";return result;}
        result.ok=true;result.code="console_reenabled";return result;
      }
    }
    // No protected binding: an active QEMU std adapter is still positive identity.
    for(const auto& path:active.paths){const auto device=access.adapter(path.targetInfo.adapterId);const auto canonical=device?canonicalNativeConsoleAdapter(*device):std::nullopt;if(canonical&&nativeConsoleIsStdAdapter(*canonical)){result.consoleKnown=true;result.ok=true;result.code="console_active";return result;}}
    result.ok=result.activeDisplays>0;result.code=result.ok?"display_active":"no_active_display";return result;
  }catch(...){result.ok=false;result.code="query_failed";return result;}
}
inline std::string nativeResetConsoleReply(const NativeResetConsoleResult& value){
  return "{\"type\":\"native.reset.console.result\",\"ok\":"+std::string(value.ok?"true":"false")+",\"code\":"+jsonQuote(nativeResetCodes().count(value.code)?value.code:"failed")+",\"consoleKnown\":"+(value.consoleKnown?"true":"false")+",\"activeDisplays\":"+std::to_string(std::min<size_t>(value.activeDisplays,64))+",\"systemError\":"+std::to_string(value.systemError)+'}';
}
inline std::optional<NativeResetConsoleResult> parseNativeResetConsoleReply(const Json& value){
  const auto* ok=std::get_if<bool>(&value.get("ok").value);const auto* known=std::get_if<bool>(&value.get("consoleKnown").value);const auto code=value.get("code").string();const auto displays=value.get("activeDisplays").number(-1),error=value.get("systemError").number(-1);
  if(value.get("type").string()!="native.reset.console.result"||!ok||!known||!nativeResetCodes().count(code)||!nativeDisplayInteger(displays,0,64)||!nativeDisplayInteger(error,0,UINT32_MAX))return {};
  NativeResetConsoleResult result;result.ok=*ok;result.code=code;result.consoleKnown=*known;result.activeDisplays=size_t(displays);result.systemError=DWORD(error);
  // A positive console outcome must be one of the defined success codes.
  if(result.ok!=(code=="console_active"||code=="console_reenabled"||code=="display_active"))return {};return result;
}
// Read-only explanation of the protected recovery records plus the trace of
// the last decisions. Output is the fixed-field object consumed by
// host_diagnostics.hpp; nothing here can name a path, digest or identity.
struct NativeRecoveryInventoryAccess {
  std::filesystem::path directory;
  NativeStreamProcessIdentity service;
  std::function<NativeStreamProcessState(const NativeStreamProcessIdentity&)> observe;
  std::optional<NativeRecoveryScope> logon;  // current console login, if readable
  std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile;
};
inline std::string nativeRecoveryInventoryJson(const NativeRecoveryInventoryAccess& access,const NativeRecoveryTrace& trace,uint64_t now){
  const auto& d=access.directory;const auto& trusted=access.trusted;
  std::array<std::string,9> states;states.fill("absent");
  const auto read=[&](size_t index,const NativeRuntimeJournalAccess& journal,const std::function<void(const std::string&)>& parse)->std::optional<std::string>{
    try{const auto bytes=journal.load();if(!bytes)return {};parse(*bytes);states[index]="present";return bytes;}catch(...){states[index]="invalid";return {};}
  };
  std::optional<NativeStreamProfileRecord> profile;std::optional<NativeStreamLaunchReceipt> launch;std::optional<NativeRuntimeRecord> runtime;std::optional<NativeDisplayIdentity> identity;std::string retirementPhase="none",expiredPhase="none";
  (void)read(0,nativeStreamProfileFileAccess(d/L"native-stream-profile.json",trusted),[&](const std::string& b){profile=parseNativeStreamProfileRecord(b);});
  (void)read(1,nativeStreamLaunchFileAccess(d/L"native-stream-launch.json",trusted),[&](const std::string& b){launch=parseNativeStreamLaunch(b);});
  (void)read(2,nativeRuntimeFileAccess(d/L"native-display-runtime.json",trusted),[&](const std::string& b){runtime=parseNativeRuntimeRecord(b);});
  (void)read(3,nativeDisplayIdentityFileAccess(d/L"native-display.dpapi",trusted),[&](const std::string& b){identity=unsealNativeDisplayIdentity(b).value;});
  (void)read(4,nativeBaselineFileAccess(d/L"native-display-baseline.json",trusted),[&](const std::string& b){(void)parseNativeRecoveryBaseline(b);});
  (void)read(5,nativeStreamViewportReceiptFileAccess(d/L"native-stream-viewport-commit.json",trusted),[&](const std::string& b){(void)parseNativeStreamViewportReceipt(b);});
  (void)read(6,nativeStreamReclaimFileAccess(d/L"native-stream-reclaim.json",trusted),[&](const std::string& b){(void)parseNativeStreamReclaim(b);});
  (void)read(7,nativeStreamRetirementMarkerAccess(d/L"native-stream-retirement.json",trusted),[&](const std::string& b){const auto phase=parseNativeStreamRetirement(b).phase;retirementPhase=phase=="prepared"||phase=="retired"?phase:"invalid";});
  (void)read(8,nativeStreamExpiredConsoleFileAccess(d/L"native-stream-expired-console.json",trusted),[&](const std::string& b){const auto phase=parseNativeStreamExpiredConsoleRecord(b).phase;expiredPhase=phase=="prepared"||phase=="completed"?phase:"invalid";});
  if(states[7]=="invalid")retirementPhase="invalid";if(states[8]=="invalid")expiredPhase="invalid";
  const auto bit=[](std::optional<bool> value){return value?std::string(*value?"true":"false"):std::string("null");};
  const auto process=[&](const NativeStreamProcessIdentity& p,bool service){if(service&&p==access.service)return std::string("current");if(!access.observe)return std::string("unknown");switch(access.observe(p)){case NativeStreamProcessState::running:return std::string("running");case NativeStreamProcessState::gone:return std::string("gone");default:return std::string("unknown");}};
  std::optional<bool> runtimeMatches,launchMatches,identityMatches;
  if(runtime&&profile){bool same=runtime->generation==profile->current.generation&&runtime->outputs.size()==profile->current.targets.size();for(size_t i=0;same&&i<runtime->outputs.size();++i){const auto& a=runtime->outputs[i];const auto& b=profile->current.targets[i];same=a.id==b.id&&a.width==b.width&&a.height==b.height;}runtimeMatches=same;}
  if(launch&&profile)launchMatches=nativeStreamLaunchMatches(*launch,*profile);
  if(identity&&launch){bool same=identity->lease==launch->lease&&!launch->outputs.empty()&&launch->outputs.size()<=2;for(size_t i=0;same&&i<launch->outputs.size();++i)same=identity->displays[i]==launch->outputs[i].display;identityMatches=same;}
  std::string ledger="none";
  if(profile&&launch&&profile->phase!=NativeStreamProfilePhase::restored){std::string code;try{(void)nativeExpiredResolvedSource(*profile,*launch,nativeExpiredPrivateSources(d,trusted),&code);}catch(...){code="failed";}ledger=nativeRecoveryCode(NativeRecoveryField::ledger,code.empty()?"failed":code);}
  std::string logon="none";if(profile)logon=!access.logon?"unknown":access.logon->logon==profile->scope.logon?"same":"new";
  std::string out="{";
  for(const auto field:{NativeRecoveryField::path,NativeRecoveryField::blocker,NativeRecoveryField::quiescent,NativeRecoveryField::retirement,NativeRecoveryField::worker,NativeRecoveryField::hold})out+=jsonQuote(nativeRecoveryFieldName(field))+':'+jsonQuote(trace.value(field))+',';
  out+="\"ledger\":"+jsonQuote(ledger)+",\"records\":{";
  for(size_t i=0;i<states.size();++i){if(i)out+=',';out+=jsonQuote(nativeRecoveryRecordNames[i])+':'+jsonQuote(states[i]);}
  out+="},\"profilePhase\":"+jsonQuote(states[0]=="invalid"?"invalid":profile?nativeStreamProfilePhaseName(profile->phase):"none")+",\"profileResize\":"+bit(profile?std::optional<bool>(profile->resize.has_value()):std::nullopt)+",\"profileStopRequested\":"+bit(profile?std::optional<bool>(profile->stopRequested):std::nullopt)+",\"profileLaunchBound\":"+bit(profile?std::optional<bool>(!profile->launchSha256.empty()):std::nullopt);
  out+=",\"runtimePhase\":"+jsonQuote(states[2]=="invalid"?"invalid":runtime?runtime->phase:"none")+",\"runtimeCheckpoint\":"+bit(runtime?std::optional<bool>(!runtime->baselineHash.empty()):std::nullopt)+",\"runtimeMatchesProfile\":"+bit(runtimeMatches)+",\"launchMatchesProfile\":"+bit(launchMatches);
  out+=",\"launchService\":"+jsonQuote(launch?process(launch->service,true):"none")+",\"launchWorker\":"+jsonQuote(launch?process(launch->worker,false):"none")+",\"logon\":"+jsonQuote(logon)+",\"identityMatchesLaunch\":"+bit(identityMatches);
  out+=",\"retirementPhase\":"+jsonQuote(retirementPhase)+",\"expiredConsolePhase\":"+jsonQuote(expiredPhase)+','+trace.budgetJson(now)+'}';
  return out;
}
// Any active Windows RDP login (not only a Multitap relay) blocks the reset.
inline bool nativeResetRdpSessionActive(){
  WTS_SESSION_INFOW* sessions=nullptr;DWORD count=0;if(!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE,0,1,&sessions,&count))return true;
  bool active=false;
  for(DWORD i=0;i<count&&!active;++i){if(sessions[i].State!=WTSActive||!sessions[i].SessionId)continue;LPWSTR buffer=nullptr;DWORD bytes=0;
    if(!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,sessions[i].SessionId,WTSClientProtocolType,&buffer,&bytes)){active=true;continue;}
    if(bytes>=sizeof(USHORT)&&*reinterpret_cast<USHORT*>(buffer)==2)active=true;WTSFreeMemory(buffer);}
  WTSFreeMemory(sessions);return active;
}
struct NativeResetPreflight {std::string pending="none",sdk="present";};
struct NativeResetCommit {bool ok=false;std::vector<NativeResetStep> steps;size_t moved=0;};
// Service-side orchestration. The caller holds the bridge lifecycle and
// recovery-launch locks for the whole call, so no stream, RDP, display change
// or recovery worker can start between the checks and the record move.
struct NativeResetOrchestration {
  std::function<std::string()> busy;                          // "" when idle
  std::function<NativeResetPreflight()> preflight;            // native service, under its mutex
  std::function<std::optional<NativeResetConsoleResult>()> console; // settings-process CCD step
  std::function<NativeResetCommit()> commit;                  // re-check + move + reinitialize
};
inline bool runNativeRecoveryReset(const Json& request,const NativeResetOrchestration& access,std::vector<NativeResetStep>& steps,size_t& moved){
  moved=0;
  const auto* confirmed=std::get_if<bool>(&request.get("confirmed").value);
  if(request.get("type").string()!="native.reset.request"||request.get("protocol").number()!=1||!confirmed||!*confirmed){steps.push_back({"confirm","confirmation_required",false});return false;}
  if(!access.busy||!access.preflight||!access.console||!access.commit){steps.push_back({"service","protocol",false});return false;}
  const auto idle=[&]{auto code=access.busy();if(code.empty())code="idle";steps.push_back({"sessions",code,code=="idle"});return code=="idle";};
  if(!idle())return false;
  const auto first=access.preflight();
  steps.push_back({"pending",first.pending,first.pending=="none"});if(first.pending!="none")return false;
  steps.push_back({"sdk",first.sdk,first.sdk=="absent"});if(first.sdk!="absent")return false;
  const auto console=access.console();
  if(!console){steps.push_back({"console","protocol",false});return false;}
  steps.push_back({"console",console->code,console->ok});if(!console->ok)return false;
  if(!idle())return false;
  auto committed=access.commit();moved=committed.moved;
  for(auto& step:committed.steps)steps.push_back(std::move(step));
  return committed.ok&&!steps.empty()&&std::all_of(steps.begin(),steps.end(),[](const auto& step){return step.ok;});
}
// Admin-only (SYSTEM + elevated Administrators) local pipe served by the boot
// service. One conversation at a time; the handler owns all decisions.
class NativeRecoveryResetServer {
 public:
  using Handler=std::function<std::string(HANDLE,const Json&)>;
  using Verify=std::function<bool(HANDLE)>;
  NativeRecoveryResetServer(Handler handler,Verify verify,std::wstring endpoint={},std::wstring security={}):handler_(std::move(handler)),verify_(std::move(verify)),endpoint_(endpoint.empty()?nativeResetPipeName(GetCurrentProcessId()):std::move(endpoint)),security_(security.empty()?L"D:P(A;;GA;;;SY)(A;;GA;;;BA)":std::move(security)){worker_=std::thread([this]{run();});}
  ~NativeRecoveryResetServer(){stopped_=true;if(stop_.value)SetEvent(stop_.value);if(worker_.joinable())worker_.join();}
  NativeRecoveryResetServer(const NativeRecoveryResetServer&)=delete;
 private:
  Handler handler_;Verify verify_;std::wstring endpoint_,security_;std::atomic<bool> stopped_{false};
  // Ends the idle wait for a client (waitPipeClient, broker_status.hpp).
  WinHandle stop_{CreateEventW(nullptr,TRUE,FALSE,nullptr)};std::thread worker_;
  void run()noexcept{
    PSECURITY_DESCRIPTOR descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(security_.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return;
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    WinHandle pipe(CreateNamedPipeW(endpoint_.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,8192,8192,0,&security));LocalFree(descriptor);
    if(pipe.value==INVALID_HANDLE_VALUE)return;
    while(!stopped_){
      WinHandle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.value)return;
      OVERLAPPED operation{};operation.hEvent=event.value;BOOL connected=ConnectNamedPipe(pipe.value,&operation);const DWORD error=GetLastError();
      if(!connected&&error==ERROR_IO_PENDING){DWORD count=0;const DWORD waited=waitPipeClient(event.value,stop_.value,stopped_);
        if(stopped_||waited!=WAIT_OBJECT_0){CancelIoEx(pipe.value,&operation);GetOverlappedResult(pipe.value,&operation,&count,TRUE);return;}connected=GetOverlappedResult(pipe.value,&operation,&count,FALSE);}
      else if(!connected&&error==ERROR_PIPE_CONNECTED)connected=TRUE;
      if(connected&&!stopped_)try{
        std::string reply;
        if(!verify_||!verify_(pipe.value)){std::vector<NativeResetStep> steps{{"session","not_console",false}};reply=nativeResetResultJson(false,steps,0);}
        else if(const auto request=nativeResetReceive(pipe.value,2000))reply=handler_(pipe.value,*request);
        if(!reply.empty())(void)nativeResetSend(pipe.value,reply,3000);
        FlushFileBuffers(pipe.value);
      }catch(...){ }
      DisconnectNamedPipe(pipe.value);
    }
  }
};
// Service side: the client must be the installed host tool in the ACTIVE
// CONSOLE session (the pipe DACL already requires an elevated administrator).
inline bool nativeResetClientAllowed(HANDLE pipe){
  ULONG pid=0;if(!GetNamedPipeClientProcessId(pipe,&pid)||!pid)return false;
  WinHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));if(!process.value)return false;
  DWORD session=UINT32_MAX;if(!ProcessIdToSessionId(pid,&session)||!session||session!=WTSGetActiveConsoleSessionId())return false;
  const auto expected=std::filesystem::path(executablePath()).parent_path()/L"MultitapHostTool.exe";
  return brokerProcessExecutable(process.value,expected.wstring());
}
// Settings-process side: authenticate the running boot service (SCM PID, the
// protected installed image and the pipe server PID), then converse.
inline std::string requestNativeRecoveryReset(const std::function<NativeResetConsoleResult()>& consoleStep,std::vector<NativeResetStep> prefix){
  const auto refuse=[&](const char* step,const char* code){prefix.push_back({step,code,false});return nativeResetResultJson(false,prefix,0);};
  DWORD pid=0;
  try{
    ServiceHandle manager(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT));ServiceHandle service(OpenServiceW(manager.value,L"MultitapHost",SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG));
    SERVICE_STATUS_PROCESS status{};DWORD bytes=0;if(!QueryServiceStatusEx(service.value,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&bytes)||status.dwCurrentState!=SERVICE_RUNNING||!status.dwProcessId)return refuse("service","service_unavailable");
    const auto executable=brokerServiceExecutable(service.value);WinHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,status.dwProcessId));
    if(!process.value||!brokerProcessExecutable(process.value,executable))return refuse("service","service_unavailable");pid=status.dwProcessId;
  }catch(...){return refuse("service","service_unavailable");}
  const auto name=nativeResetPipeName(pid);if(!WaitNamedPipeW(name.c_str(),3000))return refuse("service","service_busy");
  WinHandle pipe(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
  if(pipe.value==INVALID_HANDLE_VALUE)return refuse("service",GetLastError()==ERROR_PIPE_BUSY?"service_busy":"service_unavailable");
  ULONG server=0;if(!GetNamedPipeServerProcessId(pipe.value,&server)||server!=pid)return refuse("service","service_unavailable");
  prefix.push_back({"service","connected",true});
  if(!nativeResetSend(pipe.value,"{\"type\":\"native.reset.request\",\"protocol\":1,\"confirmed\":true}"))return refuse("service","protocol");
  for(int exchange=0;exchange<4;++exchange){
    const auto message=nativeResetReceive(pipe.value,45000);if(!message)return refuse("service","protocol");
    const auto type=message->get("type").string();
    if(type=="native.reset.console"){const auto result=consoleStep();if(!nativeResetSend(pipe.value,nativeResetConsoleReply(result)))return refuse("service","protocol");continue;}
    if(type=="native.reset.result")try{return nativeResetResultProjection(*message,prefix);}catch(...){return refuse("service","protocol");}
    return refuse("service","protocol");
  }
  return refuse("service","protocol");
}
#endif
}
