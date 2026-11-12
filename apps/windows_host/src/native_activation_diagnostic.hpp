#pragma once
#include "machine.hpp"
namespace host {
inline uint32_t nativeTopologyReasonCode(const std::string& reason){
  if(reason.empty())return 0;
  static const std::array<const char*,22> known{"interactive_console_required","native_topology_query_failed","native_topology_plan_invalid","native_topology_mode_invalid","native_topology_baseline_required","native_topology_validate_failed","native_topology_changed","native_topology_apply_failed","native_topology_not_applied","native_topology_failed","native_topology_foreign_changed","native_topology_target_unavailable","native_topology_source_changed","native_topology_transform_mismatch","native_topology_refresh_mismatch","native_topology_scanline_mismatch","native_topology_size_mismatch","native_topology_format_mismatch","native_topology_placement_mismatch","native_topology_departure_pending","native_topology_exact_layout_failed","native_stream_console_profile_changed"};
  for(size_t i=0;i<known.size();++i)if(reason==known[i])return uint32_t(i+1);return 255;
}
// Fixed worker pre-change rejection sites (phase1/phase2 of the native viewport
// coordinator). They extend nativeTopologyReasonCode's 1..22 space; 255 stays
// "unknown". Each value names one check, never exception text or identities.
enum class NativeViewportReason : uint32_t {
  workerIneligible=23,         // interactive/elevated/topology eligibility lost
  requestMismatch=24,          // helper size differs from the request
  snapshotUnavailable=25,      // protected service lease snapshot unreadable/stale
  snapshotPending=26,          // service snapshot already has a pending change
  probeFailed=27,              // fresh owned display probe threw
  displayNotReady=28,          // fresh probe is not ready (pending/recovery/no output)
  remoteSession=29,            // fresh probe reports a remote/RDP session
  ownedOutputMissing=30,       // requested owned monitor id absent from fresh probe
  captureSelectionInvalid=31,  // fresh capture selection is not exactly one owned output
  snapshotTargetsInvalid=32,   // snapshot owned targets could not be parsed
  baselineInvalid=33,          // CCD baseline for the owned targets failed
  consoleUnavailable=34,       // profile console binding absent for this baseline
  leaseChanged=35,             // lease key changed during the read-only checks
  recoveryScopeUnavailable=36, // logon/foreign-display recovery scope failed
  checkpointInvalid=37,        // durable recovery checkpoint could not be encoded
  budgetExpired=38,            // transaction deadline or cancellation
};
inline constexpr uint32_t nativeViewportReasonMax=38;
inline uint32_t nativeViewportReasonCode(NativeViewportReason reason){return uint32_t(reason);}
// Fixed status codes only. Never forward exception text, device paths or lease
// capabilities from an interactive worker to administrative diagnostics.
inline bool nativeActivationReasonKnown(const std::string& reason){
  static const std::set<std::string> allowed{"","interactive_console_required","native_display_snapshot_invalid","native_display_configuration_untrusted","native_display_topology_mismatch","native_topology_query_failed","native_topology_plan_invalid","native_topology_mode_invalid","native_topology_baseline_required","native_topology_validate_failed","native_topology_changed","native_topology_apply_failed","native_topology_not_applied","native_topology_failed","native_activation_observation_pending","native_activation_lease_changed","native_activation_worker_unavailable"};
  static const std::set<std::string> mismatch{"native_topology_foreign_changed","native_topology_target_unavailable","native_topology_source_changed","native_topology_transform_mismatch","native_topology_refresh_mismatch","native_topology_scanline_mismatch","native_topology_size_mismatch","native_topology_format_mismatch","native_topology_placement_mismatch"};
  return allowed.count(reason)!=0||mismatch.count(reason)!=0;
}
// Local worker ACK detail. stage=sample(1 before/2 all/3 after)*10+mode type.
// issue: 1 absent, 2 range, 3 type, 4 identity, 5 clone conflict, 6 changed, 7 overflow.
struct NativeCcdDiagnostic {uint32_t stage=0,index=0,expected=0,observed=0,issue=0;};
inline std::string nativeCcdDiagnosticJson(const NativeCcdDiagnostic& value){
  return "{\"stage\":"+std::to_string(value.stage)+",\"index\":"+std::to_string(value.index)+",\"expected\":"+std::to_string(value.expected)+",\"observed\":"+std::to_string(value.observed)+",\"issue\":"+std::to_string(value.issue)+'}';
}
inline NativeCcdDiagnostic parseNativeCcdDiagnostic(const Json& value){
  const auto* object=std::get_if<Json::Object>(&value.value);
  if(!object||object->size()!=5)throw std::runtime_error("native_activation_diagnostic_invalid");
  for(const auto* key:{"stage","index","expected","observed","issue"})if(!nativeDisplayInteger(value.get(key).number(-1),0,UINT32_MAX))throw std::runtime_error("native_activation_diagnostic_invalid");
  NativeCcdDiagnostic result{uint32_t(value.get("stage").number()),uint32_t(value.get("index").number()),uint32_t(value.get("expected").number()),uint32_t(value.get("observed").number()),uint32_t(value.get("issue").number())};
  if(result.stage/10<1||result.stage/10>3||result.stage%10<1||result.stage%10>3||result.expected<1||result.expected>3||result.issue<1||result.issue>7)throw std::runtime_error("native_activation_diagnostic_invalid");
  return result;
}
struct NativeActivationDiagnostic {
  std::string reason;
  DWORD systemError=0;
  bool attempted=false,windowsVerified=false;
  std::optional<NativeCcdDiagnostic> ccd;
};
inline std::string nativeActivationDiagnosticJson(const NativeActivationDiagnostic& value){
  return "{\"reason\":"+jsonQuote(nativeActivationReasonKnown(value.reason)?value.reason:"native_topology_failed")+",\"systemError\":"+std::to_string(value.systemError)+",\"attempted\":"+(value.attempted?"true":"false")+",\"windowsVerified\":"+(value.windowsVerified?"true":"false")+(value.ccd?",\"ccd\":"+nativeCcdDiagnosticJson(*value.ccd):"")+'}';
}
inline NativeActivationDiagnostic parseNativeActivationDiagnostic(const Json& value){
  const auto* object=std::get_if<Json::Object>(&value.value);
  const auto reason=value.get("reason").string();const auto code=value.get("systemError").number(-1);
  if(!object||(object->size()!=4&&object->size()!=5)||!std::holds_alternative<std::string>(value.get("reason").value)||!nativeActivationReasonKnown(reason)||!nativeDisplayInteger(code,0,UINT32_MAX)||!std::holds_alternative<bool>(value.get("attempted").value)||!std::holds_alternative<bool>(value.get("windowsVerified").value))throw std::runtime_error("native_activation_diagnostic_invalid");
  NativeActivationDiagnostic result{reason,DWORD(code),value.get("attempted").boolean(),value.get("windowsVerified").boolean()};
  if(object->size()==5)result.ccd=parseNativeCcdDiagnostic(value.get("ccd"));
  if(result.windowsVerified&&(!result.reason.empty()||result.systemError||result.ccd))throw std::runtime_error("native_activation_diagnostic_invalid");
  return result;
}
}
