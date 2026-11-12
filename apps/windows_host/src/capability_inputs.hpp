#pragma once
// The capability report's costly inputs (backlog #70, idle cost). Every regular
// heartbeat (every 10 s) and every announcement rebuilt the whole report: 71-93
// wakeups of the broker channel's sender each (measured, read-only
// thread timeline). Per call, measured in a user process:
//   WTS session list (interactiveSession, sameInteractiveConsole) ~4, six a report
//   WTS session lock state (sessionUnlocked)                      ~2, two a report
//   SCM status of the Host service (lease snapshot)               ~5, three a report
//   console recovery identities (stream_console_recovery.hpp): ~14 there; in the
//     service also the stored account (DPAPI ~1, two LSA lookups ~6) and each
//     session's token, ~20-25 (estimated)
// Each is now read once and kept until a change signal or its maximum age, so a
// missed signal heals by itself:
//   session view: a WTS session notification (counted by the service's control
//     handler, hostSessionChangeEpoch), invalidate(); at most 30 s.
//   console identities: the same, the console session ID and the stored
//     account file's stamp (Host settings saves or clears it); at most 60 s.
//   SCM status: native_display_status.hpp (this process's own status reports).
// invalidate() is called for display/readiness changes (invalidateDisplay),
// Host settings' capabilities request and every session change the loop handles.
// Still read each report: the lease snapshot file (rewritten every ~2 s, ~2.4
// wakeups for the first read after a rewrite), the device node (CM, 0), file
// stamps (0); the display probe keeps its own cache (display_cache.hpp: until a
// change signal, at most a minute while ready). A report with nothing
// changed: 0-3 wakeups (session_stop_test --capability-cost, two runs: 19-23 with
// the inputs kept, less ~15 for the three SCM reads and ~4 for the v1 recovery
// check that a test process makes and the service does not; 42-44 read afresh).
#include "stream_console_recovery.hpp"
#include "native_display_files.hpp"
#include "signal_cache.hpp"
#include <functional>

namespace host {
// The report's one read of the interactive session (interactiveSession() and
// its lock state), shared by its streaming, monitor and thumbnail fields.
struct CapabilitySessionView { DWORD session = 0xffffffff; bool unlocked = false; };
// sameInteractiveConsole() for a view; the console session ID costs no wakeup.
inline bool capabilityConsole(const CapabilitySessionView& view) {
  return view.session != 0 && view.session != 0xffffffff && WTSGetActiveConsoleSessionId() == view.session;
}
struct CapabilityInputAccess {
  std::function<CapabilitySessionView()> session;
  std::function<StreamConsoleIdentity(DWORD)> identity;
  std::function<std::optional<MachineFileStamp>()> account;
};
class CapabilityInputs {
#ifdef MULTITAP_SESSION_STOP_TEST
  friend struct SessionStopTestAccess;
#endif
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::seconds sessionMaxAge{30}, identityMaxAge{60};
  explicit CapabilityInputs(CapabilityInputAccess access) : access_(std::move(access)) {}
  CapabilitySessionView session(Clock::time_point now = Clock::now()) { return session_.get(now, hostSessionChangeEpoch().load(), access_.session); }
  StreamConsoleIdentity identity(DWORD console, Clock::time_point now = Clock::now()) {
    const IdentityKey key{hostSessionChangeEpoch().load(), console, access_.account()};
    return identity_.get(now, key, [&] { return access_.identity(console); });
  }
  void invalidate() noexcept { session_.invalidate(); identity_.invalidate(); invalidations_.fetch_add(1); }
  uint64_t invalidations() const noexcept { return invalidations_.load(); }
 private:
  struct IdentityKey { uint64_t sessions = 0; DWORD console = 0xffffffff; std::optional<MachineFileStamp> account; bool operator==(const IdentityKey&) const = default; };
  CapabilityInputAccess access_;
  SignalCache<CapabilitySessionView, uint64_t> session_{sessionMaxAge};
  SignalCache<StreamConsoleIdentity, IdentityKey> identity_{identityMaxAge};
  std::atomic<uint64_t> invalidations_{0};
};
}
