#pragma once
#include "support.hpp"
#include <algorithm>
#include <array>
#include <mutex>
#include <set>
namespace host {
// Startup/profile recovery explanation for automatic diagnostics. Every value
// is one entry of a compiled vocabulary: no path, digest, identity, process id,
// SID, device name, lease or exception text can be stored or exported. This is
// observation only; it never authorizes recovery, capture or readiness.
enum class NativeRecoveryField : size_t {path,blocker,quiescent,ledger,retirement,worker,hold,count};
inline const char* nativeRecoveryFieldName(NativeRecoveryField field){
  static constexpr std::array<const char*,size_t(NativeRecoveryField::count)> names{"path","blocker","quiescent","ledger","retirement","worker","hold"};
  return names[size_t(field)];
}
inline const std::set<std::string>& nativeRecoveryCodes(NativeRecoveryField field){
  static const std::array<std::set<std::string>,size_t(NativeRecoveryField::count)> codes{{
    // path: which startup recovery transaction was last attempted.
    {"none","expired_restore","retirement","expired_retirement","in_service"},
    // blocker: the service gate (recoverPendingStreamProfile) that stopped the
    // most recent evaluation, or "started" when a worker was launched.
    {"none","started","recovery_running","reservation_held","session_active","not_selected","retirement_archive_mismatch","profile_missing","launch_missing","records_invalid","profile_restored","launch_mismatch","launch_current_service","console_session_required","logon_unverified","sdk_not_quiescent","original_service_alive","original_worker_alive","retry_budget_exhausted"},
    // quiescent: same-logon expired-console admission (SDK/ledger absence proof).
    {"none","ok","not_selected","controller_active","reservation_held","change_pending","instance_mismatch","records_missing","profile_changed","ledger_unresolved","transport_unavailable","sdk_present","failed"},
    // ledger: first private-ledger resolver rule that rejected the records.
    {"none","ok","launch_mismatch","profile_restored","snapshot_shape","runtime_missing","identity_missing","runtime_invalid","identity_invalid","runtime_instance","reclaim_present","settled_shape","settled_predecessor","pending_baseline","pending_scope","pending_identity","pending_outputs","pending_evidence","runtime_not_stable","settled_resize","launch_identity","launch_runtime","launch_viewport_conflict","viewport_missing","viewport_invalid","viewport_baseline","viewport_source","viewport_lease","viewport_outputs","viewport_pending_profile","viewport_committed_profile","viewport_connector","ledger_bytes","private_displays","failed"},
    // retirement: new-logon (or completed expired-console) retirement admission.
    {"none","ok","not_selected","controller_active","reservation_conflict","change_pending","instance_mismatch","transport_unavailable","handoff_rejected","records_missing","records_invalid","profile_restored","launch_mismatch","runtime_mismatch","observation_missing","expired_console_proof","owned_output_active","logon_same","observation_stale","console_hardware","scope_incomplete","target_unavailable","console_not_unique","console_not_primary","primary_ambiguous","observation_invalid","marker_conflict","archive_failed","identity_rejected","original_process_alive","executor_not_running","observation_changed","previous_marker","owner_unlinked","leases_present","commit_failed","runtime_unverified","failed"},
    // worker: outcome of the authenticated interactive recovery worker exchange.
    {"none","succeeded","launch_missing","launch_mismatch","original_alive","worker_unavailable","identity_unverified","reserve_rejected","restore_rejected","restore_authority_lost","restore_topology_changed","restore_history","restore_prepare","restore_validate","restore_apply","restore_not_restored","restore_complete","restore_unverified","observe_failed","retire_rejected","complete_rejected","reclaim_rejected","recover_unacknowledged","failed"},
    // hold: why the native display service refuses to start a controller.
    {"none","reservation","retirement_prepared","retirement_invalid","profile_not_restored","profile_without_launch","launch_profile_mismatch","launch_service_alive","launch_worker_alive","records_changed","records_unreadable","service_identity"},
  }};
  return codes[size_t(field)];
}
inline std::string nativeRecoveryCode(NativeRecoveryField field,const std::string& code){return nativeRecoveryCodes(field).count(code)?code:std::string(field==NativeRecoveryField::path?"none":"failed");}
// Maps the expired-console transaction's fixed failure text to a worker code.
inline std::string nativeRecoveryRestoreCode(const std::string& reason){
  static const std::array<std::pair<const char*,const char*>,11> known{{{"native_stream_expired_console_authority_lost","restore_authority_lost"},{"native_stream_expired_console_completed_topology_changed","restore_topology_changed"},{"native_stream_expired_console_history_unacknowledged","restore_history"},{"native_stream_expired_console_prepare_unacknowledged","restore_prepare"},{"native_stream_expired_console_prepare_unverified","restore_prepare"},{"native_stream_expired_console_validate_failed","restore_validate"},{"native_stream_expired_console_apply_failed","restore_apply"},{"native_stream_expired_console_not_restored","restore_not_restored"},{"native_stream_expired_console_complete_unacknowledged","restore_complete"},{"native_stream_expired_console_complete_unverified","restore_complete"},{"native_stream_expired_console_unverified","restore_unverified"}}};
  for(const auto& [text,code]:known)if(reason==text)return code;return "restore_rejected";
}
class NativeRecoveryTrace {
 public:
  void note(NativeRecoveryField field,const std::string& code)noexcept{try{std::lock_guard lock(mutex_);values_[size_t(field)]=nativeRecoveryCode(field,code);}catch(...){ }}
  // A new attempt clears per-attempt outcomes; gate/hold codes stay current.
  void attempt(const std::string& path,uint32_t attempts,uint64_t tick)noexcept{
    try{std::lock_guard lock(mutex_);values_[size_t(NativeRecoveryField::path)]=nativeRecoveryCode(NativeRecoveryField::path,path);
      for(const auto field:{NativeRecoveryField::quiescent,NativeRecoveryField::retirement,NativeRecoveryField::worker})values_[size_t(field)]="none";
      attempts_=attempts;lastAttempt_=tick;}catch(...){ }
  }
  void budget(uint32_t attempts,bool exhausted)noexcept{std::lock_guard lock(mutex_);attempts_=attempts;exhausted_=exhausted;}
  void reset()noexcept{std::lock_guard lock(mutex_);values_={};attempts_=0;exhausted_=false;lastAttempt_=0;}
  std::string value(NativeRecoveryField field)const{std::lock_guard lock(mutex_);const auto& code=values_[size_t(field)];return code.empty()?"none":code;}
  // "attempts":N,"budgetExhausted":b,"lastAttemptAgeMs":N|null (no braces).
  std::string budgetJson(uint64_t now)const{std::lock_guard lock(mutex_);return "\"attempts\":"+std::to_string(std::min<uint32_t>(attempts_,255))+",\"budgetExhausted\":"+(exhausted_?"true":"false")+",\"lastAttemptAgeMs\":"+(lastAttempt_&&now>=lastAttempt_?std::to_string(std::min<uint64_t>(now-lastAttempt_,4294967295ull)):std::string("null"));}
  // Fixed-field object body (without braces); ages use the caller's tick clock.
  std::string json(uint64_t now)const{
    std::lock_guard lock(mutex_);std::string out;
    for(size_t i=0;i<values_.size();++i){if(!out.empty())out+=',';out+=jsonQuote(nativeRecoveryFieldName(NativeRecoveryField(i)))+':'+jsonQuote(values_[i].empty()?"none":values_[i]);}
    out+=",\"attempts\":"+std::to_string(std::min<uint32_t>(attempts_,255))+",\"budgetExhausted\":"+(exhausted_?"true":"false")+",\"lastAttemptAgeMs\":"+(lastAttempt_&&now>=lastAttempt_?std::to_string(std::min<uint64_t>(now-lastAttempt_,4294967295ull)):std::string("null"));
    return out;
  }
 private:
  mutable std::mutex mutex_;std::array<std::string,size_t(NativeRecoveryField::count)> values_{};uint32_t attempts_=0;bool exhausted_=false;uint64_t lastAttempt_=0;
};
inline NativeRecoveryTrace& nativeRecoveryTrace(){static NativeRecoveryTrace trace;return trace;}
inline void nativeRecoveryNote(NativeRecoveryField field,const std::string& code)noexcept{nativeRecoveryTrace().note(field,code);}
// Record-inventory vocabulary (computed read-only from protected files).
inline const std::set<std::string>& nativeRecoveryRecordStates(){static const std::set<std::string> states{"absent","present","invalid"};return states;}
inline const std::set<std::string>& nativeRecoveryProfilePhases(){static const std::set<std::string> phases{"none","prepared","active","restoring","restored","recovery","invalid"};return phases;}
inline const std::set<std::string>& nativeRecoveryRuntimePhases(){static const std::set<std::string> phases{"none","stable","starting","changing","stopped","recovery","invalid"};return phases;}
inline const std::set<std::string>& nativeRecoveryProcessStates(){static const std::set<std::string> states{"none","current","running","gone","unknown"};return states;}
inline const std::set<std::string>& nativeRecoveryLogonStates(){static const std::set<std::string> states{"none","same","new","unknown"};return states;}
inline const std::set<std::string>& nativeRecoveryMarkerPhases(){static const std::set<std::string> phases{"none","prepared","retired","completed","invalid"};return phases;}
inline constexpr std::array<const char*,9> nativeRecoveryRecordNames{"profile","launch","runtime","identity","baseline","viewport","reclaim","retirement","expiredConsole"};
}
