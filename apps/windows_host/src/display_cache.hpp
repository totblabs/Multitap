#pragma once
#include "machine.hpp"
#include "signal_cache.hpp"
#include <mutex>
#include <algorithm>
namespace host {
struct SessionEvent { DWORD type = 0, session = 0xffffffff; };
inline bool workerSessionMustRestart(DWORD workerSession, DWORD selectedSession, bool unlocked, const std::vector<SessionEvent>& events) {
  if (!unlocked || workerSession != selectedSession) return true;
  for (const auto& event : events) if (event.type && (event.session == workerSession || event.session == 0xffffffff)) return true;
  return false;
}
// Why a session change ended a stream. Only a still-unlocked console session
// of the same worker is a capture problem; a lock, sign-out, user switch or
// the console moving to RDP leaves no interactive console to stream (the
// takeover by RDP used to be reported as capture_failed).
inline const char* workerSessionStopReason(DWORD workerSession, DWORD selectedSession, bool unlocked, bool console) {
  return unlocked && console && workerSession == selectedSession ? "capture_failed" : "no_interactive_session";
}
// The change signals a display probe's worker would see (signal_cache.hpp): WTS
// session notifications and display changes (display_change_signals.hpp).
struct DisplayProbeKey { uint64_t sessions = 0, displays = 0; bool operator==(const DisplayProbeKey&) const = default; };
inline DisplayProbeKey displayProbeKey() { return {hostSessionChangeEpoch().load(), hostDisplayChangeEpoch().load()}; }
class DisplayReadinessCache {
 public:
  // Each probe starts a worker process in the user's session. A ready
  // observation was probed again every 30 s; now it is kept until its key
  // changes, at most a minute (an unsignalled change: a UAC secure desktop,
  // an unsaved display change). A not-ready one is probed again after 5 s.
  static constexpr std::chrono::seconds readyMaxAge{60}, notReadyMaxAge{5};
  // `key`: the signals when the observation began; a change after it makes the
  // observation due (a display operation's own change, too).
  struct Ticket { uint64_t generation; DWORD session; DisplayProbeKey key; };
  void invalidate() { std::lock_guard lock(mutex_); generation_++; session_ = 0xffffffff; display_ = {}; checked_ = {}; desktopAvailable_=false;refreshPending_=false;key_={}; }
  Ticket begin(DWORD session, DisplayProbeKey key = displayProbeKey()) { std::lock_guard lock(mutex_); return {generation_, session, key}; }
  // `changed`: what a capability report shows differs from the kept
  // observation (none, another session, another state).
  bool complete(Ticket ticket, DisplayState display, Clock::time_point now = Clock::now(), bool desktopAvailable = true, bool refreshPending = false, bool* changed = nullptr) {
    std::lock_guard lock(mutex_); if (ticket.generation != generation_) return false;
    if (changed) *changed = checked_ == Clock::time_point{} || session_ != ticket.session || desktopAvailable_ != desktopAvailable || !(display_ == display);
    session_ = ticket.session; display_ = std::move(display); checked_ = now;desktopAvailable_=desktopAvailable;refreshPending_=refreshPending;key_=ticket.key; return true;
  }
  DisplayState snapshot(DWORD session, bool unlocked, bool* desktopAvailable = nullptr,bool* observed=nullptr,int64_t* ageMs=nullptr) { std::lock_guard lock(mutex_);const bool matches=unlocked&&session_==session;const bool sampled=matches&&checked_!=Clock::time_point{};if(desktopAvailable)*desktopAvailable=matches&&desktopAvailable_;if(observed)*observed=sampled;if(ageMs)*ageMs=sampled?std::clamp<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-checked_).count(),0,4294967295LL):-1;return matches ? display_ : DisplayState{}; }
  bool captureAllowed(DWORD session, bool unlocked) { std::lock_guard lock(mutex_);return unlocked&&session_==session&&desktopAvailable_; }
  bool due(DWORD session, const DisplayProbeKey& key, Clock::time_point now = Clock::now()) {
    // Activation's worker sample precedes the service's durable ACK. Keep its
    // actual observation age, but re-read capabilities after that exact ACK
    // instead of retaining incomplete viewport/count flags until the next signal.
    std::lock_guard lock(mutex_); return refreshPending_ || session_ != session || key_ != key || now - checked_ >= (display_.ready()&&desktopAvailable_ ? readyMaxAge : notReadyMaxAge);
  }
 private: std::mutex mutex_; uint64_t generation_ = 0; DWORD session_ = 0xffffffff; DisplayState display_; Clock::time_point checked_{};bool desktopAvailable_=false,refreshPending_=false;DisplayProbeKey key_;
};
}
