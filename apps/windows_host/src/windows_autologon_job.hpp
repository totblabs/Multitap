#pragma once
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace host::autologon {
enum class RecoveryStopState { running, requested, unknown };
inline bool queuedRecoveryMayStart(uint64_t queued, uint64_t now, bool closing,
                                   RecoveryStopState stop = RecoveryStopState::running) {
  return !closing && stop == RecoveryStopState::running && now >= queued && now - queued < 30000;
}
// Scheduling/observation only: no Windows calls or detached callbacks. An
// observer deadline never grants permission to terminate/relaunch an owner.
class RecoveryObservation {
 public:
  bool begin(uint64_t now, uint64_t timeout = 30000) {
    if (begun_) return false;
    begun_ = true; active_ = true;
    deadline_ = timeout > std::numeric_limits<uint64_t>::max() - now ? std::numeric_limits<uint64_t>::max() : now + timeout;
    return true;
  }
  void launchFailed() { if (begun_) { active_ = false; code_ = "launch_failed"; } }
  void observe(uint64_t now, bool actualExited, std::optional<uint32_t> exitCode, bool identityValid) {
    if (!active_) return;
    if (actualExited && exitCode) {
      active_ = false;
      code_ = identityValid ? (*exitCode == 0 ? "completed" : "repair_required") : "identity_unverified";
      exit_ = exitCode; return;
    }
    if (now >= deadline_) { expired_ = true; code_ = "outcome_unknown"; }
  }
  bool active() const { return active_; }
  bool expired() const { return expired_; }
  bool begun() const { return begun_; }
  const std::string& code() const { return code_; }
  std::optional<uint32_t> exitCode() const { return exit_; }
 private:
  bool begun_ = false, active_ = false, expired_ = false;
  uint64_t deadline_ = 0;
  std::string code_ = "not_started";
  std::optional<uint32_t> exit_;
};
// Callers may delete their *own recovery metadata* only after the fixed local
// cleanup command actually ended and explicitly reported a successful result.
// This does not authorize removing program/service/driver files.
inline bool cleanupAllowsStateDeletion(bool confirmed, bool actualExited, bool identityValid,
                                       std::optional<uint32_t> exitCode, const std::string& result) {
  return confirmed && actualExited && identityValid && exitCode && *exitCode == 0 &&
    (result == "cleaned" || result == "unchanged");
}
} // namespace host::autologon
