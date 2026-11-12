#pragma once
#include "native_stream_expired_ledger.hpp"
#include "native_stream_retirement.hpp"

#ifdef MULTITAP_NATIVE_DISPLAY
namespace host {
// The private input inventory is compiled into the host, never selected by a
// remote request. Missing optional prior receipts remain missing, not invented.
inline std::vector<NativeStreamExpiredConsolePrivateSource> nativeExpiredPrivateSources(const std::filesystem::path& directory,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  using Kind=NativeStreamExpiredConsolePrivateKind;
  nativeStreamJournalRequire(directory.filename()==L"private");
  return {{Kind::runtime,nativeRuntimeFileAccess(directory/L"native-display-runtime.json",trusted)},
    {Kind::identity,nativeDisplayIdentityFileAccess(directory/L"native-display.dpapi",trusted)},
    {Kind::viewport,nativeStreamViewportReceiptFileAccess(directory/L"native-stream-viewport-commit.json",trusted)},
    {Kind::reclaim,nativeStreamReclaimFileAccess(directory/L"native-stream-reclaim.json",trusted)},
    {Kind::baseline,nativeBaselineFileAccess(directory/L"native-display-baseline.json",trusted)}};
}
struct NativeExpiredResolvedSource {
  NativeStreamExpiredConsoleLedger ledger;std::vector<NativeStreamExpiredConsolePrivateSnapshot> snapshots;std::array<uint64_t,2> privateDisplays{};
};
struct NativeExpiredSdkAbsence {std::vector<NativeStreamExpiredLeaseObservation> leases;std::vector<NativeStreamExpiredDisplayObservation> displays;};
inline std::optional<NativeExpiredSdkAbsence> nativeExpiredSdkAbsence(multitap::vdisplay::api::ControlClient& client,const NativeStreamExpiredConsoleLedger& ledger,std::span<const uint64_t> additionalDisplays={}){
  using namespace multitap::vdisplay;
  try{
    if(!client.check_protocol_compatible().ok())return {};NativeExpiredSdkAbsence result;
    const auto absent=[&]{for(const auto& mapping:ledger.leases){api::LeaseRequest request{};request.lease_id=mapping.lease;const auto query=client.query_lease(request);if(!query.ok()||!recoveryLeaseAbsent(query.value,mapping.lease))return false;}return true;};
    if(!absent())return {};const auto state=client.query_display_state();if(!state.ok()||state.value.reserved||state.value.entry_count>api::kMaxDisplayStateEntries||uint64_t(state.value.entry_count)!=uint64_t(state.value.permanent_display_count)+state.value.temporary_display_count)return {};
    size_t temporary=0,permanent=0;for(size_t i=0;i<state.value.entry_count;++i){const auto& entry=state.value.entries[i];if(entry.kind==api::kDisplayStateKindTemporary)++temporary;else if(entry.kind==api::kDisplayStateKindPermanent)++permanent;else return {};if(std::find(additionalDisplays.begin(),additionalDisplays.end(),entry.display_id)!=additionalDisplays.end())return {};for(const auto& mapping:ledger.leases){if(entry.lease_id==mapping.lease)return {};for(const auto& output:mapping.outputs)if(entry.display_id==output.display)return {};}}
    if(temporary!=state.value.temporary_display_count||permanent!=state.value.permanent_display_count||!absent())return {};
    for(const auto& mapping:ledger.leases){result.leases.push_back({mapping.lease,NativeStreamExpiredSdkObject::absent,0,0});for(const auto& output:mapping.outputs)result.displays.push_back({mapping.lease,output.display,NativeStreamExpiredSdkObject::absent});}return result;
  }catch(...){return {};}
}
// rejection (optional, diagnostics only): fixed ledger-vocabulary code.
inline std::optional<NativeExpiredResolvedSource> nativeExpiredResolvedSource(const NativeStreamProfileRecord& profile,const NativeStreamLaunchReceipt& launch,const std::vector<NativeStreamExpiredConsolePrivateSource>& sources,std::string* rejection=nullptr){
  const auto note=[rejection](const char* code){if(rejection&&(rejection->empty()||*rejection=="ok"))*rejection=code;};
  try{NativeExpiredResolvedSource result;for(const auto& source:sources){std::optional<std::string> bytes;try{bytes=source.journal.load();}catch(...){note(source.kind==NativeStreamExpiredConsolePrivateKind::runtime?"runtime_invalid":source.kind==NativeStreamExpiredConsolePrivateKind::identity?"identity_invalid":source.kind==NativeStreamExpiredConsolePrivateKind::viewport?"viewport_invalid":"failed");throw;}
      if(bytes){result.snapshots.push_back({source.kind,*bytes});if(source.kind==NativeStreamExpiredConsolePrivateKind::identity){try{const auto identity=unsealNativeDisplayIdentity(*bytes);for(size_t i=0;i<2;++i)result.privateDisplays[i]=identity.value.displays[i];}catch(...){note("identity_invalid");throw;}}}}
    std::string code;const auto ledger=rejection?resolveNativeStreamExpiredLedgerTraced(profile,launch,result.snapshots,code):resolveNativeStreamExpiredLedger(profile,launch,result.snapshots);if(rejection)*rejection=code;
    if(!ledger)return {};if(!result.privateDisplays[0]||!result.privateDisplays[1]||result.privateDisplays[0]==result.privateDisplays[1]){note("private_displays");if(rejection)*rejection="private_displays";return {};}
    result.ledger=*ledger;return result;}catch(...){if(rejection&&rejection->empty())*rejection="failed";return {};}
}
// A completion receipt supplements retirement's independent process/SDK absence
// and current-console proofs. It cannot authorize capture or recreate old maps.
inline std::optional<NativeStreamRetirementExpiredConsoleProof> nativeExpiredRetirementCompletion(
    const NativeStreamProfileRecord& profile,const NativeStreamRetirementObservation& observed,
    const std::string& launchBytes,const std::string& markerBytes){
  try{
    const auto marker=parseNativeStreamExpiredConsoleRecord(markerBytes);const auto launch=parseNativeStreamLaunch(launchBytes);
    if(marker.phase!="completed"||marker.profileSha256!=nativeBaselineDigest(nativeStreamProfileRecordBytes(profile))||marker.launchSha256!=nativeBaselineDigest(launchBytes)||!nativeStreamLaunchMatches(launch,profile)||observed.scope.logon!=profile.scope.logon||!nativeStreamRetirementObservationValid(profile,observed,observed.scope.tick,true)||marker.restoredSha256!=nativeExpiredCcdDigest(observed.topology))return {};
    return NativeStreamRetirementExpiredConsoleProof{nativeBaselineDigest(markerBytes),marker.profileSha256,marker.launchSha256,nativeRetirementGeometryDigest(observed)};
  }catch(...){return {};}
}
inline bool nativeArchiveRetiredExpiredConsole(const std::filesystem::path& directory,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  try{
    const auto path=directory/L"native-stream-expired-console.json";const auto marker=nativeStreamExpiredConsoleFileAccess(path,trusted).load();if(!marker)return true;const auto retiredBytes=nativeStreamRetirementMarkerAccess(directory/L"native-stream-retirement.json",trusted).load();if(!retiredBytes)return false;const auto retired=parseNativeStreamRetirement(*retiredBytes);const auto completed=parseNativeStreamExpiredConsoleRecord(*marker);const auto hash=nativeBaselineDigest(*marker);
    if(retired.phase!="retired"||(!retired.expiredConsoleSha256.empty()&&retired.expiredConsoleSha256!=hash)||completed.phase!="completed"||completed.profileSha256!=retired.pins[0]||completed.launchSha256!=retired.pins[1])return false;
    const auto history=directory/L"native-stream-expired-history";if((!CreateDirectoryW(history.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)||!trusted(history))return false;const auto io=nativeStreamExpiredConsoleHistoryAccess(history,hash,trusted);const auto saved=io.load();if(saved&&saved!=marker)return false;if(!saved&&!io.save({},*marker))return false;if(io.load()!=marker)return false;
    WinHandle file(CreateFileW(path.c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};
    if(file.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(file.value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||info.nFileSizeHigh||info.nFileSizeLow!=marker->size())return false;std::string bytes(marker->size(),'\0');DWORD read=0;if(!ReadFile(file.value,bytes.data(),DWORD(bytes.size()),&read,nullptr)||read!=bytes.size()||bytes!=*marker)return false;FILE_DISPOSITION_INFO remove{TRUE};if(!SetFileInformationByHandle(file.value,FileDispositionInfo,&remove,sizeof(remove)))return false;file=WinHandle();return !nativeStreamExpiredConsoleFileAccess(path,trusted).load();
  }catch(...){return false;}
}
}
#endif
