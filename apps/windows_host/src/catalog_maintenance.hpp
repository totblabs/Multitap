#pragma once
#include "display_operation.hpp"
#include <condition_variable>

namespace host {
inline std::string catalogMaintenanceReason(const std::string& reason) {
  static const std::set<std::string> allowed{"", "host_maintenance", "mode_catalog_update_required", "mode_catalog_failed", "mode_catalog_timeout", "mode_catalog_cancelled", "mode_catalog_registration_timeout", "mode_catalog_not_verified", "viewport_rollback_incomplete", "reboot_required", "interactive_console_required", "administrator_required", "viewport_recovery_required", "existing_virtual_display_requires_review", "shared_vendor_path_conflict", "owned_virtual_output_inactive", "owned_virtual_display_not_ready", "viewport_busy", "driver_hash_mismatch", "driver_signature_untrusted", "driver_catalog_membership_invalid", "viewport_settings_modified_externally", "viewport_original_settings_modified", "viewport_installed_driver_mismatch", "viewport_topology_changed", "viewport_ccd_validate_failed", "owned_device_restart_failed", "owned_device_restart_status_unavailable"};
  return allowed.count(reason) ? reason : "mode_catalog_failed";
}
enum class CatalogPhase { Idle, Checking, Applying, Ready, Deferred, Failed, Unknown };
struct CatalogOutcome {
  CatalogPhase phase = CatalogPhase::Unknown; std::string reason = "mode_catalog_failed";
  bool recoveryRequired = false, rebootRequired = false, rollbackAttempted = false, rollbackSucceeded = false;
  bool deferredWithoutMutation = false;
};
struct CatalogOperation {
  DisplayOperationKey key; DWORD session = 0xffffffff; uint64_t generation = 0;
  std::atomic<bool> cancelled{false}, committed{false};
  void setAbort(std::function<void()> abort) {
    bool call = false; { std::lock_guard lock(abortMutex_); abort_ = abort; call = cancelled; }
    if (call && abort) abort();
  }
  void cancel() {
    cancelled = true; std::function<void()> abort;
    { std::lock_guard lock(abortMutex_); abort = abort_; }
    if (abort) try { abort(); } catch (...) {}
  }
 private: std::mutex abortMutex_; std::function<void()> abort_;
};
struct CatalogSharedState {
  std::mutex mutex; std::condition_variable completion;
  size_t sessions = 0, topologies = 0; uint64_t generation = 1, checkedGeneration = 0;
  DWORD checkedSession = 0xffffffff;
  bool attempted = false, stopping = false; std::atomic<bool> changed{false};
  CatalogOutcome result{CatalogPhase::Idle, ""}; std::shared_ptr<CatalogOperation> operation;
};
class CatalogActivity {
 public:
  CatalogActivity(std::shared_ptr<CatalogSharedState> state, bool topology) : state_(std::move(state)), topology_(topology) {}
  ~CatalogActivity() { std::lock_guard lock(state_->mutex); auto& count = topology_ ? state_->topologies : state_->sessions; if (count) --count; }
  CatalogActivity(const CatalogActivity&) = delete;
 private: std::shared_ptr<CatalogSharedState> state_; bool topology_;
};
class CatalogMaintenance {
 public:
  using Commit = std::function<bool()>;
  using Runner = std::function<CatalogOutcome(const std::shared_ptr<CatalogOperation>&, const Commit&)>;
  explicit CatalogMaintenance(Runner runner) : runner_(std::move(runner)) {}
  ~CatalogMaintenance() { shutdown(); }
  std::shared_ptr<CatalogActivity> reserveSession() { return reserve(false); }
  std::shared_ptr<CatalogActivity> reserveTopology() { return reserve(true); }
  bool busy() const { std::lock_guard lock(state_->mutex); return bool(state_->operation); }
  std::string reason() const {
    std::lock_guard lock(state_->mutex); return state_->operation ? "host_maintenance" : state_->result.reason;
  }
  CatalogOutcome result() const { std::lock_guard lock(state_->mutex); return state_->result; }
  bool changed() { return state_->changed.exchange(false); }
  void cancel() {
    std::shared_ptr<CatalogOperation> operation;
    { std::lock_guard lock(state_->mutex); ++state_->generation; operation = state_->operation; }
    if (operation) operation->cancel();
  }
  // Only fresh, read-only worker evidence calls consider. No helper or wait is
  // executed on the heartbeat/service-control thread. A ready catalog is a
  // process-lifetime no-op; an attempted failure never auto-repeats revision 1.
  bool consider(DWORD session, bool eligible) {
    std::lock_guard launch(launchMutex_);
    if (!eligible || !session || session == 0xffffffff) return false;
    std::shared_ptr<CatalogOperation> operation;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->stopping || state_->operation || state_->sessions || state_->topologies || state_->attempted || state_->result.phase == CatalogPhase::Ready || state_->result.phase == CatalogPhase::Failed || state_->result.phase == CatalogPhase::Unknown || (state_->checkedGeneration == state_->generation && state_->checkedSession == session)) return false;
      operation = std::make_shared<CatalogOperation>(); operation->session = session; operation->generation = state_->generation;
      GUID id{}; check(CoCreateGuid(&id), "Catalog operation identity"); wchar_t guid[40]{}; StringFromGUID2(id, guid, 40); operation->key.id = utf8(std::wstring(guid + 1, guid + 37));
      std::array<BYTE, 32> random{}; check(BCryptGenRandom(nullptr, random.data(), DWORD(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG), "Catalog operation proof");
      DWORD length = 0; winCheck(CryptBinaryToStringA(random.data(), DWORD(random.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length), "Catalog proof encoding");
      std::string proof(length, 0); winCheck(CryptBinaryToStringA(random.data(), DWORD(random.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, proof.data(), &length), "Catalog proof encoding"); proof.resize(43); for (auto& c : proof) { if (c == '+') c = '-'; else if (c == '/') c = '_'; } operation->key.nonce = std::move(proof);
      state_->checkedGeneration = state_->generation; state_->checkedSession = session; state_->operation = operation; state_->result = {CatalogPhase::Checking, "host_maintenance"}; state_->changed = true;
    }
    if (worker_.joinable()) worker_.join(); // Prior worker has published completion; never an active transaction.
    const auto state = state_; const auto runner = runner_;
    try {
      worker_ = std::thread([state, runner, operation] {
        const auto commit = [state, operation] {
          std::lock_guard lock(state->mutex);
          if (state->stopping || operation->cancelled || state->generation != operation->generation || state->operation != operation || state->sessions || state->topologies) return false;
          state->attempted = true; operation->committed = true; state->result = {CatalogPhase::Applying, "host_maintenance"}; state->changed = true; return true;
        };
        CatalogOutcome result;
        try { result = runner(operation, commit); } catch (...) { result = {operation->committed ? CatalogPhase::Unknown : CatalogPhase::Failed, "mode_catalog_failed"}; }
        result.reason = catalogMaintenanceReason(result.reason);
        if (operation->cancelled) { result.phase = operation->committed ? CatalogPhase::Unknown : CatalogPhase::Deferred; result.reason = "mode_catalog_cancelled"; result.rollbackSucceeded = false; }
        { std::lock_guard lock(state->mutex); if (state->operation == operation) { if(state->stopping||state->generation!=operation->generation||operation->cancelled){result.phase=operation->committed?CatalogPhase::Unknown:CatalogPhase::Deferred;result.reason="mode_catalog_cancelled";result.rollbackSucceeded=false;result.deferredWithoutMutation=false;}if(result.phase==CatalogPhase::Deferred&&result.deferredWithoutMutation&&!operation->cancelled)state->attempted=false;state->operation.reset(); state->result = std::move(result); state->changed = true; } }
        state->completion.notify_all();
      });
    } catch (...) {
      std::lock_guard lock(state_->mutex); if (state_->operation == operation) { state_->operation.reset(); state_->result = {CatalogPhase::Failed, "mode_catalog_failed"}; state_->changed = true; } throw;
    }
    return true;
  }
  void shutdown() {
    std::lock_guard launch(launchMutex_);
    { std::lock_guard lock(state_->mutex); state_->stopping = true; }
    cancel();
    { std::unique_lock lock(state_->mutex); state_->completion.wait_for(lock, std::chrono::milliseconds(1500), [&] { return !state_->operation; }); }
    // Runner owns its operation, shared gate, isolated job and buffers. An OS
    // call which refuses cancellation cannot hold SCM shutdown indefinitely or
    // access a destroyed manager/bridge. Process exit closes the owned job.
    if (worker_.joinable()) { if (busy()) worker_.detach(); else worker_.join(); }
  }
 private:
  std::shared_ptr<CatalogSharedState> state_ = std::make_shared<CatalogSharedState>(); Runner runner_; std::thread worker_;std::mutex launchMutex_;
  std::shared_ptr<CatalogActivity> reserve(bool topology) {
    std::lock_guard lock(state_->mutex); if (state_->stopping || state_->operation || state_->topologies || (topology && state_->sessions)) return {};
    auto lease = std::make_shared<CatalogActivity>(state_, topology); if (topology) ++state_->topologies; else ++state_->sessions; return lease;
  }
};
}
