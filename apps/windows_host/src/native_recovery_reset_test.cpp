// Fixed-code recovery diagnostics and the administrator record reset, using
// the reclaim transaction fixtures (actual DPAPI, SDK dispatcher/store and
// private-file codecs; CCD, processes and Windows display changes injected).
#define main nativeReclaimTransactionMain
#include "native_stream_reclaim_test.cpp"
#undef main
#include "native_recovery_reset.hpp"
#include "host_diagnostics.hpp"
#include <sddl.h>
#ifdef MULTITAP_NATIVE_DISPLAY
namespace {
using Kind=NativeStreamExpiredConsolePrivateKind;
using Snapshots=std::vector<NativeStreamExpiredConsolePrivateSnapshot>;
int resetChecks=0;
void expect(bool value,const std::string& what){if(!value)throw std::runtime_error("recovery reset check failed: "+what);++resetChecks;}
Snapshots privateSnapshots(Fixture& f){return {{Kind::runtime,*f.runtimes.bytes},{Kind::identity,*f.identities.bytes}};}
// The field record of a failed viewport (see native_stream_reclaim_test.cpp).
void vm149Shape(Fixture& f){f.profile.phase=NativeStreamProfilePhase::recovery;f.profile.stopRequested=true;f.profile.revision=4;f.profile.resizeSequence=1;f.profile.resize=NativeStreamProfileResize{std::string(64,'c'),std::string(64,'d'),1};f.runtime.phase="recovery";f.persist();}
std::string ledgerCode(Fixture& f,const Snapshots& snapshots,std::optional<NativeStreamLaunchReceipt> launch={}){std::string code;(void)resolveNativeStreamExpiredLedgerTraced(f.profile,launch?*launch:f.launch,snapshots,code);return code;}
std::string sealed(uint64_t lease,const api::OwnerCapability& owner){NativeDisplayIdentitySnapshot value;std::memcpy(value.value.magic,"TOTBVDD1",8);value.value.lease=lease;value.value.displays[0]=71;value.value.displays[1]=72;value.value.owner=owner;return sealNativeDisplayIdentity(value.value);}

void traceVocabulary(){
  NativeRecoveryTrace trace;trace.note(NativeRecoveryField::blocker,"sdk_not_quiescent");trace.note(NativeRecoveryField::ledger,"C:\\ProgramData\\secret");trace.note(NativeRecoveryField::path,"bogus");
  expect(trace.value(NativeRecoveryField::blocker)=="sdk_not_quiescent"&&trace.value(NativeRecoveryField::ledger)=="failed"&&trace.value(NativeRecoveryField::path)=="none","unknown codes collapse to fixed values");
  trace.note(NativeRecoveryField::worker,"retire_rejected");trace.attempt("retirement",2,1000);expect(trace.value(NativeRecoveryField::worker)=="none"&&trace.value(NativeRecoveryField::path)=="retirement","attempt clears per-attempt outcome");
  const auto budget=JsonParser::parse("{"+trace.budgetJson(1500)+"}");expect(budget.get("attempts").number()==2&&budget.get("lastAttemptAgeMs").number()==500&&!budget.get("budgetExhausted").boolean(),"attempt budget/age");
  trace.reset();expect(std::holds_alternative<std::nullptr_t>(JsonParser::parse("{"+trace.budgetJson(1500)+"}").get("lastAttemptAgeMs").value),"reset age");
  expect(nativeRecoveryRestoreCode("native_stream_expired_console_apply_failed")=="restore_apply"&&nativeRecoveryRestoreCode("DO_NOT_EXPORT")=="restore_rejected","restore reason mapping");
  for(size_t i=0;i<size_t(NativeRecoveryField::count);++i)for(const auto& code:nativeRecoveryCodes(NativeRecoveryField(i)))expect(code.size()<=48&&code.find_first_not_of("abcdefghijklmnopqrstuvwxyz_")==std::string::npos,"vocabulary is lowercase fixed codes");
}
void quarantineCodes(){
  const NativeStreamProcessIdentity current{9999,999};
  const auto reason=[&](Fixture& f,std::function<NativeStreamProcessState(const NativeStreamProcessIdentity&)> observe){return std::string(nativeStreamLaunchQuarantineReason(f.profiles.access(),f.launches.access(),current,observe));};
  const auto gone=[](const auto&){return NativeStreamProcessState::gone;};
  {Fixture f;expect(reason(f,gone)=="profile_not_restored"&&nativeStreamLaunchQuarantined(f.profiles.access(),f.launches.access(),current,gone),"active profile holds");}
  {Fixture f;f.profile.phase=NativeStreamProfilePhase::restored;f.profile.stopRequested=true;f.persist();expect(reason(f,gone)=="none"&&!nativeStreamLaunchQuarantined(f.profiles.access(),f.launches.access(),current,gone),"restored+gone releases");
    expect(reason(f,[&](const auto& p){return p==f.launch.worker?NativeStreamProcessState::running:NativeStreamProcessState::gone;})=="launch_worker_alive","worker alive");
    expect(reason(f,[&](const auto& p){return p==f.launch.service?NativeStreamProcessState::unknown:NativeStreamProcessState::gone;})=="launch_service_alive","service unproven");
    f.launches.bytes.reset();expect(reason(f,gone)=="profile_without_launch","restored launch-bound profile without receipt");
    auto other=f.launch;other.worker.created++;f.launches.bytes=nativeStreamLaunchBytes(other);expect(reason(f,gone)=="launch_profile_mismatch","same-operation receipt mismatch");
    f.profiles.bytes="{}";expect(reason(f,gone)=="records_unreadable","corrupt record");}
  {Fixture f;expect(std::string(nativeStreamLaunchQuarantineReason(f.profiles.access(),f.launches.access(),{},gone))=="service_identity","no service identity");}
}
void ledgerCodes(){
  {Fixture f;vm149Shape(f);expect(ledgerCode(f,privateSnapshots(f))=="ok","settled reservation resolves");}
  {Fixture f;expect(ledgerCode(f,privateSnapshots(f))=="ok","unchanged launch resolves");}
  {Fixture f;vm149Shape(f);auto s=privateSnapshots(f);s.push_back({Kind::reclaim,"{}"});expect(ledgerCode(f,s)=="reclaim_present","unfinished reclaim");}
  {Fixture f;vm149Shape(f);f.runtime.generation++;f.persist();expect(ledgerCode(f,privateSnapshots(f))=="settled_shape","settled runtime generation moved");}
  {Fixture f;vm149Shape(f);f.runtime.phase="changing";f.runtime.baselineHash=std::string(64,'a');f.persist();expect(ledgerCode(f,privateSnapshots(f))=="pending_baseline","checkpoint without baseline");}
  {Fixture f;expect(ledgerCode(f,{{Kind::runtime,*f.runtimes.bytes},{Kind::viewport,"x"}})=="identity_missing","identity missing");}
  {Fixture f;expect(ledgerCode(f,{{Kind::runtime,*f.runtimes.bytes}})=="snapshot_shape","single snapshot");}
  {Fixture f;expect(ledgerCode(f,{{Kind::runtime,"{}"},{Kind::identity,*f.identities.bytes}})=="runtime_invalid","runtime invalid");}
  {Fixture f;expect(ledgerCode(f,{{Kind::runtime,*f.runtimes.bytes},{Kind::identity,"x"}})=="identity_invalid","identity invalid");}
  {Fixture f;f.runtime.instance="ROOT\\DISPLAY\\00b2";f.persist();expect(ledgerCode(f,privateSnapshots(f))=="runtime_instance","runtime instance");}
  {Fixture f;f.runtime.generation++;f.persist();expect(ledgerCode(f,privateSnapshots(f))=="launch_runtime","unchanged runtime mismatch");}
  {Fixture f;f.identities.bytes=sealed(oldLease+9,f.owner);expect(ledgerCode(f,privateSnapshots(f))=="launch_identity","owner lease not the launch lease");}
  {Fixture f;f.profile.resizeSequence=1;f.persist();expect(ledgerCode(f,privateSnapshots(f))=="viewport_missing","committed viewport without receipt");}
  {Fixture f;f.profile.phase=NativeStreamProfilePhase::restored;f.profile.stopRequested=true;f.persist();expect(ledgerCode(f,privateSnapshots(f))=="profile_restored","restored profile");}
  {Fixture f;auto other=f.launch;other.worker.created++;expect(ledgerCode(f,privateSnapshots(f),other)=="launch_mismatch","launch mismatch");}
  {Fixture f;f.runtime.phase="stopped";f.persist();expect(ledgerCode(f,privateSnapshots(f))=="settled_shape","stopped runtime without a reservation");}
  {Fixture f;f.runtime.phase="starting";f.persist();expect(ledgerCode(f,privateSnapshots(f))=="runtime_not_stable","starting runtime");}
  // The traced resolver never changes the decision of the untraced one.
  for(int scenario=0;scenario<4;++scenario){Fixture f;if(scenario&1)vm149Shape(f);if(scenario&2){f.runtime.generation++;f.persist();}std::string code;expect(resolveNativeStreamExpiredLedger(f.profile,f.launch,privateSnapshots(f)).has_value()==resolveNativeStreamExpiredLedgerTraced(f.profile,f.launch,privateSnapshots(f),code).has_value(),"traced decision equality");}
}
void sdkAbsence(){
  RetirementFixture f;const NativeResetLeaseSet known{{oldLease},{71,72}};
  expect(nativeResetSdkCode(f.client,known)=="present","live owned lease is present");f.expire();
  expect(nativeResetSdkCode(f.client,known)=="absent","released lease and displays absent");
  expect(nativeResetSdkCode(f.client,NativeResetLeaseSet{{},{71,72}})=="present","no known lease is never absence");
  f.wire.queryFailure=true;expect(nativeResetSdkCode(f.client,known)=="present","query failure is never absence");f.wire.queryFailure=false;
  NativeResetLeaseSet many{{oldLease,newLease,oldLease+5,oldLease+6,oldLease+7},{71,72}};expect(nativeResetSdkCode(f.client,many)=="absent","more than four known leases are checked in chunks");
}
void retirementCodes(){
  {RetirementFixture f;f.expire();std::string reason;expect(nativeStreamRetirementObservationValid(f.profile,f.observed,f.now,false,&reason),"baseline observation");
    const auto check=[&](std::function<void(NativeStreamRetirementObservation&)> mutate,const char* code){auto o=f.observed;mutate(o);std::string why;expect(!nativeStreamRetirementObservationValid(f.profile,o,f.now,false,&why)&&why==code,std::string("observation ")+code+" got "+why);};
    check([](auto& o){o.ownedOutputActive=true;},"owned_output_active");check([&](auto& o){o.scope.logon=f.profile.scope.logon;},"logon_same");check([&](auto& o){o.scope.tick=f.now+1;},"observation_stale");
    check([](auto& o){o.console.device.instanceId.back()=L'X';},"console_hardware");check([](auto& o){o.topology.modes[0].sourceMode.position.x=50;},"console_not_primary");check([](auto& o){o.topology.paths[0].flags=0;},"observation_invalid");}
  const auto rejection=[](RetirementFixture& f){NativeStreamRetirementJournal journal(f.io());const bool prepared=f.prepareRetirement(journal).has_value();return prepared?std::string("prepared"):journal.rejection();};
  {RetirementFixture f;expect(rejection(f)=="leases_present","live old lease");}
  {RetirementFixture f;f.expire();f.observed.ownedOutputActive=true;expect(rejection(f)=="owned_output_active","owned output active");}
  {RetirementFixture f;f.expire();f.observed.scope.logon=f.profile.scope.logon;expect(rejection(f)=="expired_console_proof","same login without completed console proof");}
  {RetirementFixture f;f.expire();f.retirementAuthority.process=[&](const auto& p){return p==f.launch.worker?NativeStreamProcessState::unknown:f.authority.process(p);};expect(rejection(f)=="original_process_alive","original worker unproven");}
  {RetirementFixture f;f.expire();f.eligible=false;expect(rejection(f)=="identity_rejected","service/worker identity");}
  {RetirementFixture f;f.expire();f.profiles.bytes.reset();expect(rejection(f)=="records_missing","profile missing");}
  {RetirementFixture f;f.expire();f.failArchive=2;expect(rejection(f)=="archive_failed","archive write");}
  {RetirementFixture f;f.expire();expect(rejection(f)=="prepared","admitted retirement");}
}
struct TempPrivate {
  std::filesystem::path root,directory;
  TempPrivate(){root=std::filesystem::current_path()/(L"rrq-"+randomName().substr(0,8));directory=root/L"private";expect(std::filesystem::create_directory(root)&&std::filesystem::create_directory(directory),"temp directory");}
  ~TempPrivate(){std::error_code error;std::filesystem::remove_all(root,error);}
  std::function<bool(const std::filesystem::path&)> trusted()const{const auto base=root;return [base](const std::filesystem::path& path){const auto relative=path.lexically_relative(base);return path==base||(!relative.empty()&&*relative.begin()!=L"..");};}
  void write(const wchar_t* name,const std::string& bytes)const{expect(nativeJournalFileAccess(directory/name,65536,[](const std::string&){},trusted()).save({},bytes),"fixture write");}
};
void inventory(){
  Fixture f;vm149Shape(f);TempPrivate files;
  files.write(L"native-stream-profile.json",*f.profiles.bytes);files.write(L"native-stream-launch.json",*f.launches.bytes);files.write(L"native-display-runtime.json",*f.runtimes.bytes);files.write(L"native-display.dpapi",*f.identities.bytes);
  NativeRecoveryTrace trace;trace.note(NativeRecoveryField::hold,"profile_not_restored");trace.attempt("retirement",3,1000);trace.note(NativeRecoveryField::blocker,"retry_budget_exhausted");trace.note(NativeRecoveryField::retirement,"console_not_primary");trace.note(NativeRecoveryField::worker,"retire_rejected");trace.budget(3,true);
  NativeRecoveryInventoryAccess access;access.directory=files.directory;access.service={9999,999};access.observe=[](const auto&){return NativeStreamProcessState::gone;};access.logon=NativeRecoveryScope{2,std::string(64,'9'),1,{}};access.trusted=files.trusted();
  const auto bytes=nativeRecoveryInventoryJson(access,trace,5000);const auto value=JsonParser::parse(bytes);
  expect(value.get("records").get("profile").string()=="present"&&value.get("records").get("baseline").string()=="absent"&&value.get("records").get("identity").string()=="present","record presence");
  expect(value.get("profilePhase").string()=="recovery"&&value.get("profileResize").boolean()&&value.get("profileStopRequested").boolean()&&value.get("profileLaunchBound").boolean(),"profile facts");
  expect(value.get("runtimePhase").string()=="recovery"&&!value.get("runtimeCheckpoint").boolean()&&value.get("runtimeMatchesProfile").boolean()&&value.get("launchMatchesProfile").boolean()&&value.get("identityMatchesLaunch").boolean(),"runtime/launch/identity facts");
  expect(value.get("launchService").string()=="gone"&&value.get("launchWorker").string()=="gone"&&value.get("logon").string()=="new"&&value.get("ledger").string()=="ok","process/logon/ledger facts");
  expect(value.get("hold").string()=="profile_not_restored"&&value.get("retirement").string()=="console_not_primary"&&value.get("worker").string()=="retire_rejected"&&value.get("path").string()=="retirement"&&value.get("attempts").number()==3&&value.get("budgetExhausted").boolean()&&value.get("lastAttemptAgeMs").number()==4000,"trace facts");
  for(const auto* secret:{"stream-149","ROOT","PCI","VEN_","aaaaaaaaaaaaaaaa","cccccccccccccccc","dddddddddddddddd","4321","4322"})expect(bytes.find(secret)==std::string::npos,std::string("inventory leaked ")+secret);
  const auto projected=hostDiagnosticRecovery(value);expect(!projected.empty()&&JsonParser::parse("{"+projected.substr(1)+"}").get("recovery").get("ledger").string()=="ok","projection keeps valid explanation");
  // Same login → "same"; a same-generation corrupted profile → invalid.
  access.logon=f.profile.scope;expect(JsonParser::parse(nativeRecoveryInventoryJson(access,trace,5000)).get("logon").string()=="same","same login");
  files.write(L"native-stream-reclaim.json",std::string("{}"));expect(JsonParser::parse(nativeRecoveryInventoryJson(access,trace,5000)).get("records").get("reclaim").string()=="invalid","invalid reclaim record");
  std::filesystem::remove(files.directory/L"native-stream-profile.json");const auto none=JsonParser::parse(nativeRecoveryInventoryJson(access,trace,5000));
  expect(none.get("profilePhase").string()=="none"&&std::holds_alternative<std::nullptr_t>(none.get("profileResize").value)&&none.get("logon").string()=="none"&&none.get("ledger").string()=="none","absent profile");
  // Hostile/oversized explanations never reach the report; the base remains.
  for(int scenario=0;scenario<6;++scenario){auto hostile=value;auto& fields=std::get<Json::Object>(hostile.value);switch(scenario){case 0:fields["blocker"].value=std::string("DO_NOT_EXPORT");break;case 1:fields["records"].value=Json::Object{};break;case 2:fields["attempts"].value=256.0;break;case 3:fields["lastAttemptAgeMs"].value=-1.0;break;case 4:fields.erase("hold");break;case 5:fields["profileResize"].value=std::string("true");break;}expect(hostDiagnosticRecovery(hostile).empty(),"hostile explanation dropped "+std::to_string(scenario));}
  DisplayState display;display.installed=display.deviceReady=display.owned=true;display.recoveryRequired=true;
  const auto native=JsonParser::parse(R"({"schema":1,"installed":false,"started":false,"leaseActive":false,"activationVerified":false,"configurationPending":false,"recoveryRequired":true,"countControlAvailable":false,"phase":"empty","failureReason":"native_stream_profile_recovery_required"})");
  std::string maximum;for(const auto* key:{"action","stage","monitors","width","height","error"}){if(!maximum.empty())maximum+=',';maximum+=jsonQuote(key)+":4294967295";}for(const auto* key:{"ok","installed","started","active","owned","pending","recovery","reboot","remote","running","registered","online","changed","restartAttempted"})maximum+=","+jsonQuote(key)+":false";
  std::string events="[";for(int i=0;i<12;++i){if(i)events+=',';events+="{\"time\":"+jsonQuote(hostLogTime())+",\"event\":\"hostAction\",\"values\":{"+maximum+"}}";}events+=']';
  const auto consoleRecovery=JsonParser::parse(R"({"reason":"windows_session_locked","ready":false,"eligible":false,"complete":true,"configuredIdentity":true,"consoleIdentityKnown":true,"consoleOccupied":true,"activeRdpSessions":64,"disconnectedRdpSessions":64,"disconnectedConsoleSessions":64,"unknownSessions":64,"lockedSessions":64})");
  const auto report=buildHostDiagnosticReport(display,true,true,false,"native_topology_recovery_required",native,JsonParser::parse(events),true,4294967295LL,consoleRecovery,value);
  const auto parsed=JsonParser::parse(report);const auto& kept=std::get<Json::Array>(parsed.get("events").value);
  expect(report.size()<=6144&&parsed.get("native").get("recovery").get("hold").string()=="profile_not_restored"&&!kept.empty()&&kept.size()<=8,"report fits with recovery and newest events ("+std::to_string(report.size())+" bytes, "+std::to_string(kept.size())+" events)");
}
// Fake Windows CCD for the settings-process console step.
struct ConsoleCcd {
  Fixture f;NativeCcdData ccd;int validates=0,applies=0;LONG validateResult=ERROR_SUCCESS;bool eligible=true,record=true;std::string machine;
  ConsoleCcd(){machine=f.profile.console.proof.machineUuid;}
  static DISPLAYCONFIG_PATH_INFO inactive(DISPLAYCONFIG_PATH_INFO path){path.flags&=~DISPLAYCONFIG_PATH_ACTIVE;path.sourceInfo.modeInfoIdx=path.targetInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;return path;}
  void suppressed(){ccd=nativeForeignCcd(f.profile.profile.applied,f.profile.entry.targets);ccd.paths.push_back(inactive(f.profile.profile.before.paths.front()));}
  NativeResetConsoleAccess access(){
    NativeResetConsoleAccess a;a.eligible=[this]{return eligible;};a.query=[this]{return ccd;};
    a.configure=[this](const NativeCcdData& value,UINT32 flags){if(flags&SDC_VALIDATE){++validates;return validateResult;}++applies;auto next=nativeActiveCcd(value);for(const auto& path:ccd.paths)if(!(path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&std::none_of(next.paths.begin(),next.paths.end(),[&](const auto& p){return nativeStreamProfileTargetKey(p)==nativeStreamProfileTargetKey(path);}))next.paths.push_back(path);ccd=next;return LONG(ERROR_SUCCESS);};
    a.adapter=[this](LUID luid)->std::optional<NativeConsoleAdapterIdentity>{if(nativeSameLuid(luid,{77,-1}))return f.profile.console.device;if(nativeSameLuid(luid,{88,-1}))return NativeConsoleAdapterIdentity{{88,-1},L"\\\\?\\PCI#VEN_8086&DEV_1234#0",L"PCI\\VEN_8086&DEV_1234\\0",L"PCI",{L"PCI\\VEN_8086&DEV_1234"},nativeConsoleDisplayClass,true,true};if(nativeSameLuid(luid,{33,-1}))return NativeConsoleAdapterIdentity{{33,-1},L"\\\\?\\ROOT#DISPLAY#00A1#0",L"ROOT\\DISPLAY\\00A1",L"ROOT",{L"MULTITAP_FIXTURE"},nativeConsoleDisplayClass,true,true};return {};};
    a.profile=[this]()->std::optional<std::string>{if(!record)return {};return f.profiles.bytes;};a.ownedInstance=f.profile.key.instance;a.machine=machine;return a;
  }
  bool consoleActive()const{return std::any_of(ccd.paths.begin(),ccd.paths.end(),[](const auto& p){return (p.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativeSameLuid(p.targetInfo.adapterId,{77,-1});});}
};
void consoleStep(){
  {ConsoleCcd c;c.suppressed();const auto foreign=nativeActiveSignatures(c.ccd);const auto r=nativeResetConsoleStep(c.access());
    expect(r.ok&&r.code=="console_reenabled"&&r.consoleKnown&&c.validates==1&&c.applies==1&&c.consoleActive(),"suppressed QEMU console re-enabled ("+r.code+")");
    const auto after=nativeActiveCcd(c.ccd);expect(std::any_of(after.paths.begin(),after.paths.end(),[](const auto& p){return nativeSameLuid(p.targetInfo.adapterId,{88,-1});})&&!foreign.empty(),"unrelated display kept");}
  {ConsoleCcd c;c.ccd=nativeForeignCcd(c.f.profile.profile.before,c.f.profile.entry.targets);const auto r=nativeResetConsoleStep(c.access());expect(r.ok&&r.code=="console_active"&&c.validates==0&&c.applies==0,"active console untouched");}
  {ConsoleCcd c;c.suppressed();c.validateResult=ERROR_INVALID_PARAMETER;const auto r=nativeResetConsoleStep(c.access());expect(!r.ok&&r.code=="reenable_rejected"&&r.systemError==ERROR_INVALID_PARAMETER&&c.applies==0&&!c.consoleActive(),"Windows validation refusal stops before apply");}
  {ConsoleCcd c;c.ccd=c.f.profile.profile.applied;const auto r=nativeResetConsoleStep(c.access());expect(!r.ok&&r.code=="owned_output_active"&&!c.validates&&!c.applies,"owned output still active");}
  {ConsoleCcd c;c.record=false;c.ccd=nativeForeignCcd(c.f.profile.profile.before,c.f.profile.entry.targets);const auto r=nativeResetConsoleStep(c.access());expect(r.ok&&r.code=="console_active"&&r.consoleKnown,"no record: active std adapter identified");}
  {ConsoleCcd c;c.record=false;c.suppressed();const auto r=nativeResetConsoleStep(c.access());expect(r.ok&&r.code=="display_active"&&!r.consoleKnown&&!c.applies,"no record: never enable an unidentified console");}
  {ConsoleCcd c;c.record=false;c.ccd={};c.ccd.paths.push_back(ConsoleCcd::inactive(c.f.profile.profile.before.paths.front()));const auto r=nativeResetConsoleStep(c.access());expect(!r.ok&&r.code=="no_active_display","no active display");}
  {ConsoleCcd c;c.machine="ffffffff-0000-0000-0000-000000000001";c.suppressed();const auto r=nativeResetConsoleStep(c.access());expect(r.ok&&r.code=="display_active"&&!c.applies,"different VM: binding not trusted");}
  {ConsoleCcd c;c.eligible=false;c.suppressed();const auto r=nativeResetConsoleStep(c.access());expect(!r.ok&&r.code=="ineligible"&&!c.validates,"ineligible session");}
  {ConsoleCcd c;c.suppressed();auto a=c.access();a.query=[]()->NativeCcdData{throw NativeTopologyWindowsError(ERROR_ACCESS_DENIED);};const auto r=nativeResetConsoleStep(a);expect(!r.ok&&r.code=="query_failed"&&r.systemError==ERROR_ACCESS_DENIED,"CCD query failure");}
  // Reply codec: fixed codes only, ok must agree with the code.
  NativeResetConsoleResult good;good.ok=true;good.code="console_reenabled";good.consoleKnown=true;good.activeDisplays=2;expect(parseNativeResetConsoleReply(JsonParser::parse(nativeResetConsoleReply(good))).has_value(),"console reply round trip");
  for(const auto* hostile:{R"({"type":"native.reset.console.result","ok":true,"code":"no_active_display","consoleKnown":false,"activeDisplays":0,"systemError":0})",R"({"type":"native.reset.console.result","ok":true,"code":"DO_NOT_EXPORT","consoleKnown":false,"activeDisplays":0,"systemError":0})",R"({"type":"native.reset.console.result","ok":true,"code":"console_active","consoleKnown":true,"activeDisplays":65,"systemError":0})"})expect(!parseNativeResetConsoleReply(JsonParser::parse(hostile)),"hostile console reply rejected");
}
std::string securityBytes(const std::filesystem::path& path){DWORD size=0;GetFileSecurityW(path.c_str(),OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,nullptr,0,&size);std::string bytes(size,'\0');expect(size&&GetFileSecurityW(path.c_str(),OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,reinterpret_cast<PSECURITY_DESCRIPTOR>(bytes.data()),size,&size),"security descriptor");return bytes;}
void recordMove(){
  Fixture f;vm149Shape(f);
  {TempPrivate files;files.write(L"native-stream-profile.json",*f.profiles.bytes);files.write(L"native-stream-launch.json",*f.launches.bytes);files.write(L"native-display-runtime.json",*f.runtimes.bytes);files.write(L"native-display.dpapi",*f.identities.bytes);files.write(L"native-stream-retirement.json",std::string("{\"stale\":true}"));
    std::filesystem::create_directory(files.directory/L"native-stream-history");const auto acl=securityBytes(files.directory/L"native-stream-profile.json");
    size_t moved=0;const auto code=moveNativeRecoveryRecords(files.directory,"20261025T070000Z-0123abcd",moved,files.trusted());const auto backup=files.directory/L"native-recovery-reset"/L"20261025T070000Z-0123abcd";
    expect(code=="moved"&&moved==4,"four stale records moved ("+code+")");
    for(const auto* name:{L"native-stream-profile.json",L"native-stream-launch.json",L"native-display-runtime.json",L"native-stream-retirement.json"})expect(!std::filesystem::exists(files.directory/name)&&std::filesystem::exists(backup/name),"moved into backup");
    expect(nativeJournalFileAccess(backup/L"native-stream-profile.json",65536,[](const std::string&){},files.trusted()).load()==f.profiles.bytes,"backup bytes identical");
    expect(securityBytes(backup/L"native-stream-profile.json")==acl,"rename preserved the security descriptor");
    expect(std::filesystem::exists(files.directory/L"native-display.dpapi")&&std::filesystem::exists(files.directory/L"native-stream-history"),"identity and history kept");
    expect(moveNativeRecoveryRecords(files.directory,"20261025T070001Z-0123abcd",moved,files.trusted())=="nothing_to_move"&&moved==0,"idempotent");}
  {TempPrivate files;files.write(L"native-stream-profile.json",*f.profiles.bytes);std::filesystem::create_directory(files.directory/L"native-stream-launch.json");size_t moved=0;
    expect(moveNativeRecoveryRecords(files.directory,"20261025T070000Z-0123abcd",moved,files.trusted())=="backup_failed"&&moved==0&&std::filesystem::exists(files.directory/L"native-stream-profile.json"),"unexpected directory refuses before any move");}
  {TempPrivate files;files.write(L"native-stream-profile.json",*f.profiles.bytes);size_t moved=0;
    expect(moveNativeRecoveryRecords(files.directory,"bad/stamp",moved,files.trusted())=="backup_failed"&&moveNativeRecoveryRecords(files.root,"20261025T070000Z-0123abcd",moved,files.trusted())=="backup_failed","stamp/directory boundary");
    expect(moveNativeRecoveryRecords(files.directory,"20261025T070000Z-0123abcd",moved,[](const auto&){return false;})=="backup_failed"&&std::filesystem::exists(files.directory/L"native-stream-profile.json"),"untrusted directory refused");}
}
void orchestration(){
  const auto request=JsonParser::parse(R"({"type":"native.reset.request","protocol":1,"confirmed":true})");
  struct Calls{int busy=0,preflight=0,console=0,commit=0;};
  const auto make=[](Calls& calls,std::vector<std::string> busy,NativeResetPreflight pre,std::optional<NativeResetConsoleResult> console,bool commitOk){
    NativeResetOrchestration access;
    access.busy=[&calls,busy]{const auto code=calls.busy<int(busy.size())?busy[calls.busy]:std::string();++calls.busy;return code;};
    access.preflight=[&calls,pre]{++calls.preflight;return pre;};access.console=[&calls,console]{++calls.console;return console;};
    access.commit=[&calls,commitOk]{++calls.commit;NativeResetCommit c;c.moved=commitOk?4:0;c.ok=commitOk;c.steps={{"archive",commitOk?"moved":"backup_failed",commitOk},{"reinitialize",commitOk?"reinitialized":"failed",commitOk}};if(!commitOk)c.steps.pop_back();return c;};return access;};
  NativeResetConsoleResult active;active.ok=true;active.code="console_active";active.consoleKnown=true;active.activeDisplays=1;
  NativeResetConsoleResult blocked;blocked.ok=false;blocked.code="owned_output_active";
  {Calls calls;std::vector<NativeResetStep> steps;size_t moved=0;expect(runNativeRecoveryReset(request,make(calls,{},{"none","absent"},active,true),steps,moved)&&moved==4&&steps.size()==7&&calls.commit==1&&calls.busy==2,"success path re-checks sessions after the console step");
    const auto json=JsonParser::parse(nativeResetResultJson(true,steps,moved));expect(json.get("ok").boolean()&&json.get("code").string()=="reinitialized","success result");}
  const auto refused=[&](const Json& value,NativeResetOrchestration access,const char* code,Calls& calls){std::vector<NativeResetStep> steps;size_t moved=0;const bool ok=runNativeRecoveryReset(value,access,steps,moved);const auto failed=std::find_if(steps.begin(),steps.end(),[](const auto& s){return !s.ok;});return !ok&&failed!=steps.end()&&failed->code==code&&moved==0&&calls.commit==0;};
  {Calls c;expect(refused(JsonParser::parse(R"({"type":"native.reset.request","protocol":1,"confirmed":false})"),make(c,{},{},active,true),"confirmation_required",c)&&!c.busy,"unconfirmed");}
  {Calls c;expect(refused(request,make(c,{"stream_active"},{},active,true),"stream_active",c)&&!c.preflight,"active stream");}
  {Calls c;expect(refused(request,make(c,{"rdp_session_active"},{},active,true),"rdp_session_active",c)&&!c.preflight,"active Windows RDP login");}
  {Calls c;expect(refused(request,make(c,{},{"change_pending","present"},active,true),"change_pending",c)&&!c.console,"pending change");}
  {Calls c;expect(refused(request,make(c,{},{"controller_active","present"},active,true),"controller_active",c)&&!c.console,"live controller");}
  {Calls c;expect(refused(request,make(c,{},{"none","present"},active,true),"present",c)&&!c.console,"SDK lease/output still present");}
  {Calls c;expect(refused(request,make(c,{},{"none","identity_unavailable"},active,true),"identity_unavailable",c)&&!c.console,"owner identity unavailable");}
  {Calls c;expect(refused(request,make(c,{},{"none","absent"},std::nullopt,true),"protocol",c),"console step lost");}
  {Calls c;expect(refused(request,make(c,{},{"none","absent"},blocked,true),"owned_output_active",c),"console step refused");}
  {Calls c;expect(refused(request,make(c,{"","display_operation"},{"none","absent"},active,true),"display_operation",c),"operation started during console step");}
  {Calls c;std::vector<NativeResetStep> steps;size_t moved=0;expect(!runNativeRecoveryReset(request,make(c,{},{"none","absent"},active,false),steps,moved)&&c.commit==1&&steps.back().code=="backup_failed","archive failure reported");}
  // Client projection keeps only the fixed vocabulary.
  const auto server=nativeResetResultJson(true,{{"sessions","idle",true},{"archive","moved",true}},2);
  const auto projected=JsonParser::parse(nativeResetResultProjection(JsonParser::parse(server),{{"confirm","confirmed",true},{"session","console",true},{"service","connected",true}}));
  expect(projected.get("ok").boolean()&&std::get<Json::Array>(projected.get("steps").value).size()==5&&projected.get("moved").number()==2,"client projection");
  for(const auto* hostile:{R"({"type":"native.reset.result","ok":true,"code":"x","moved":0,"steps":[{"step":"sessions","ok":true,"code":"DO_NOT_EXPORT"}]})",R"({"type":"native.reset.result","ok":true,"code":"x","moved":65,"steps":[]})",R"({"type":"other","ok":true,"moved":0,"steps":[]})"})
    expect([&]{try{(void)nativeResetResultProjection(JsonParser::parse(hostile),{});return false;}catch(...){return true;}}(),"hostile result rejected");
  const auto partial=JsonParser::parse(nativeResetResultProjection(JsonParser::parse(server),{{"session","locked",false}}));expect(!partial.get("ok").boolean(),"any failed client step fails the reset");
}
std::wstring ownSid(){HANDLE raw=nullptr;OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw);WinHandle token(raw);DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);std::vector<BYTE> bytes(size);GetTokenInformation(token.value,TokenUser,bytes.data(),size,&size);LPWSTR text=nullptr;ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&text);std::wstring sid(text);LocalFree(text);return sid;}
void pipeConversation(){
  for(int scenario=0;scenario<3;++scenario){
    const auto endpoint=L"\\\\.\\pipe\\MultitapHost.NativeReset.QA."+randomName();int handled=0;
    NativeRecoveryResetServer server([&](HANDLE pipe,const Json& request){++handled;NativeResetOrchestration access;access.busy=[]{return std::string();};access.preflight=[]{return NativeResetPreflight{"none","absent"};};
        access.console=[pipe]()->std::optional<NativeResetConsoleResult>{if(!nativeResetSend(pipe,"{\"type\":\"native.reset.console\"}"))return {};const auto reply=nativeResetReceive(pipe,5000);if(!reply)return {};return parseNativeResetConsoleReply(*reply);};
        access.commit=[]{NativeResetCommit c;c.ok=true;c.moved=3;c.steps={{"archive","moved",true},{"reinitialize","reinitialized",true}};return c;};
        std::vector<NativeResetStep> steps;size_t moved=0;const bool ok=runNativeRecoveryReset(request,access,steps,moved);return nativeResetResultJson(ok,steps,moved);},
      [scenario](HANDLE){return scenario!=2;},endpoint,L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;"+ownSid()+L")");
    bool available=false;for(int wait=0;wait<100&&!available;++wait){available=WaitNamedPipeW(endpoint.c_str(),50)!=FALSE;if(!available)Sleep(20);}expect(available,"QA reset pipe available");
    WinHandle pipe(CreateFileW(endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));expect(pipe.value!=INVALID_HANDLE_VALUE,"QA reset client connect");
    expect(nativeResetSend(pipe.value,"{\"type\":\"native.reset.request\",\"protocol\":1,\"confirmed\":true}"),"request sent");
    std::optional<Json> final;
    for(int exchange=0;exchange<3&&!final;++exchange){const auto message=nativeResetReceive(pipe.value,5000);expect(message.has_value(),"server message");
      if(message->get("type").string()=="native.reset.console"){NativeResetConsoleResult r;r.ok=scenario==0;r.code=scenario==0?"console_reenabled":"no_active_display";r.consoleKnown=scenario==0;expect(nativeResetSend(pipe.value,nativeResetConsoleReply(r)),"console reply sent");}
      else final=message;}
    expect(final&&final->get("type").string()=="native.reset.result","final result");
    if(scenario==0)expect(final->get("ok").boolean()&&final->get("moved").number()==3&&handled==1,"pipe conversation success");
    if(scenario==1)expect(!final->get("ok").boolean()&&final->get("code").string()=="no_active_display"&&handled==1,"pipe conversation console refusal");
    if(scenario==2)expect(!final->get("ok").boolean()&&final->get("code").string()=="not_console"&&handled==0,"unverified client refused before the handler");
  }
}
}
int main(){
  try{traceVocabulary();quarantineCodes();ledgerCodes();sdkAbsence();retirementCodes();inventory();consoleStep();recordMove();orchestration();pipeConversation();
    std::cout<<"{\"kind\":\"native-recovery-diagnostics-and-reset\",\"checks\":"<<resetChecks<<",\"fixtureChecks\":"<<checks<<",\"actualDpapiAndCodecs\":true,\"actualFileRenameAndAcl\":true,\"actualPrivatePipe\":true,\"ccd\":\"substituted\",\"processes\":\"substituted\",\"installedHostTouched\":false,\"windowsDisplayChanges\":0}\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<" after "<<resetChecks<<" checks\n";return 1;}
}
#else
int main(){std::cout<<"Native recovery reset unavailable in the SDK-less build.\n";return 0;}
#endif
