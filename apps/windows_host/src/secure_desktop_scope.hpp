#pragma once
// Role::secure — a SYSTEM helper that ONLY captures and injects on the UAC secure
// desktop (the Winlogon input desktop) of an UNLOCKED session, during an already
// authorized stream of the signed-in user. It never encodes, never reaches the
// network, and gains no control when no secure desktop shows. The user-session
// worker keeps the encoder, the stream session and the media epoch and feeds the
// helper's frames where the notice picture is fed today (secure_desktop_notice.hpp).
//
// This is DISTINCT from Role::signin (the dropped sign-in-screen stream). The
// sign-in role still must NEVER accept an unlocked session: the sign-in test
// "Unlocked UAC must not gain SYSTEM control" stays as it is. Role::secure is the
// opposite case, a product requirement:
// the local user is signed in and unlocked, a UAC prompt is on the secure desktop,
// and the remote user must see and answer it exactly as RDP/Parsec/Sunshine allow.
//
// Everything here is pure (no Windows calls), so the scope matrix and the
// source-switch state machine are unit-tested without the VM. The integration
// populates ConsoleView/HelperView from the existing native observers
// (signin_worker_native.cpp observeConsole/observeWorker).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace host::secure {

// hostAction 200 stage 244 error values. 1..4 are the existing notice records
// (secure_desktop_notice.hpp); 5..8 are the SYSTEM-helper outcomes. 1,2,5,6,7,8
// continue the stream; 3,4 end it (locked / 10-minute limit).
inline constexpr uint32_t record244NoticeShown      = 1;  // notice shown (a stream without a helper)
inline constexpr uint32_t record244Resumed          = 2;  // desktop returned (seconds << 8)
inline constexpr uint32_t record244EndedLocked      = 3;  // ended: session locked
inline constexpr uint32_t record244EndedLimit       = 4;  // ended: 10-minute limit
inline constexpr uint32_t record244HelperActive     = 5;  // SYSTEM helper streaming real secure-desktop frames
inline constexpr uint32_t record244HelperLaunchFail = 6;  // launch/scope denied -> notice fallback
inline constexpr uint32_t record244HelperLost       = 7;  // helper lost while active -> notice fallback
inline constexpr uint32_t record244HelperNoFrame    = 8;  // no frame within the deadline -> notice fallback

// True for a 244 error that continues the stream (an observation, not an end).
inline constexpr bool record244Continues(uint32_t what) noexcept {
  return what == record244NoticeShown || what == record244Resumed || what == record244HelperActive ||
         what == record244HelperLaunchFail || what == record244HelperLost || what == record244HelperNoFrame;
}

// hostAction 200 stage 246: where input went while a prompt showed, as counts and
// levels only (never a key, a text or a position). error = kind | value << 8.
//   1 received by the helper      2 accepted by Windows (SendInput)
//   3 dropped by the allowlist    4 not accepted (malformed, outside the owned output, or 5)
//   5 SendInput failed (count)    6 last SendInput error (the Win32 code)
//   7 written to the helper by the service
//   8 kept for the worker while a helper was held (not yet showing, or exited)
//   9 the service's write to the helper failed
//  10 the worker's input cannot reach the window in front: its process is above
//     the worker's integrity (Windows UIPI drops such input without an error).
//     value = worker level | window level << 4 (2 medium, 3 high, 4 system,
//     15 not readable). Seen with a prompt that is NOT on the secure desktop.
// 1..6 are written when a helper ends, 7..9 by the service at the same time, 10
// when the window comes to the front (at most every 10 s). All are observations.
inline constexpr uint32_t inputRecordStage = 46;
inline constexpr uint32_t input246Received = 1, input246Injected = 2, input246Dropped = 3, input246Refused = 4,
    input246SendFailed = 5, input246LastError = 6, input246Routed = 7, input246HeldBack = 8, input246WriteFailed = 9,
    input246Unreachable = 10;
inline constexpr uint32_t input246(uint32_t kind, uint32_t value) noexcept {
  return (kind & 0xffu) | ((value > 0xffffffu ? 0xffffffu : value) << 8);
}

// Windows integrity as a small level: the last sub-authority of the label SID
// (0x2000 medium, 0x3000 high, 0x4000 system) in units of 0x1000.
inline constexpr uint32_t integrityUnknown = 15;
inline constexpr uint32_t integrityLevel(uint32_t rid) noexcept { return rid >= 0xf000u ? 14u : rid >> 12; }
// UIPI: SendInput from a process reaches a window only when the window's process
// is not above it. A SYSTEM injector reaches everything. A window whose process
// cannot be read is treated as above (a process of the same user at the same or a
// lower level can always be read).
inline constexpr bool inputUnreachable(uint32_t own, uint32_t window) noexcept {
  return own < 4 && (window == integrityUnknown || window > own);
}
inline constexpr uint32_t input246Reach(uint32_t own, uint32_t window) noexcept {
  return input246(input246Unreachable, (own & 15u) | ((window & 15u) << 4));
}

enum class Denial {
  none, staleScope, wrongSession, locked, noUser, accountMismatch,
  helperNotSystem, helperWrongSession, helperNotWinSta0,
  helperThreadNotWinlogon, helperInputNotWinlogon, helperForegroundNotPrompt
};

// The window in front on the Winlogon desktop belongs to a security prompt:
// %SystemRoot%\System32\consent.exe (UAC) or
// CredentialUIBroker.exe (a credential prompt on the secure desktop). The
// requirement covers those prompts ("UAC 같은 보안 확인 창"), not the
// Ctrl+Alt+Del options screen or anything else Winlogon may show.
inline bool sameAsciiText(std::wstring_view a, std::wstring_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    wchar_t x = a[i], y = b[i];
    if (x >= L'A' && x <= L'Z') x = wchar_t(x - L'A' + L'a');
    if (y >= L'A' && y <= L'Z') y = wchar_t(y - L'A' + L'a');
    if (x != y) return false;
  }
  return true;
}
inline bool securityPromptImage(std::wstring_view image, std::wstring_view system32) noexcept {
  while (!system32.empty() && system32.back() == L'\\') system32.remove_suffix(1);
  if (system32.empty() || image.size() <= system32.size() + 1 || image[system32.size()] != L'\\' || !sameAsciiText(image.substr(0, system32.size()), system32)) return false;
  const auto name = image.substr(system32.size() + 1);
  return sameAsciiText(name, L"consent.exe") || sameAsciiText(name, L"CredentialUIBroker.exe");
}

// A fresh WTS console observation of the streamed session (never a peer field).
struct ConsoleView {
  uint32_t session = 0;
  bool unlocked = false;    // WTS_SESSIONSTATE_UNLOCK
  bool noUser = false;      // no logged-on user
  std::string signedInSid;  // the logged-on user's SID (empty when noUser)
  uint64_t observedAt = 0;  // GetTickCount64 ms
};

// A fresh observation of the helper process itself (signin observeWorker fields).
struct HelperView {
  bool isSystem = false;
  uint32_t session = 0;
  bool winSta0 = false;
  bool threadWinlogon = false;
  bool inputWinlogon = false;
  bool promptForeground = false;  // the window in front is a security prompt's (securityPromptImage)
};

inline constexpr uint64_t scopeMaxAgeMs = 2000;

// The session must be the stream's session, freshly observed, UNLOCKED, with the
// stream's user signed in. A locked or no-user session is NEVER allowed for
// Role::secure (those end the stream as before). expectedSid must be a real
// account SID (never a service SID); the caller supplies the stream's user SID.
inline Denial consoleAllowed(uint32_t expectedSession, std::string_view expectedSid,
                             const ConsoleView& v, uint64_t now) {
  if (!expectedSession || expectedSession == 0xffffffffu || expectedSid.empty()) return Denial::wrongSession;
  if (v.session != expectedSession) return Denial::wrongSession;
  if (!v.observedAt || v.observedAt > now || now - v.observedAt > scopeMaxAgeMs) return Denial::staleScope;
  if (v.noUser) return Denial::noUser;
  if (!v.unlocked) return Denial::locked;
  if (v.signedInSid != expectedSid) return Denial::accountMismatch;
  return Denial::none;
}

// The helper must be SYSTEM, in the stream's session, on WinSta0, with its thread
// desktop AND the input desktop both Winlogon, and the window in front must be a
// security prompt's. The console scope must also hold. Enforced before every
// captured frame and before every injected input event.
inline Denial helperAllowed(uint32_t expectedSession, std::string_view expectedSid,
                            const ConsoleView& c, const HelperView& h, uint64_t now) {
  const auto console = consoleAllowed(expectedSession, expectedSid, c, now);
  if (console != Denial::none) return console;
  if (!h.isSystem) return Denial::helperNotSystem;
  if (h.session != expectedSession) return Denial::helperWrongSession;
  if (!h.winSta0) return Denial::helperNotWinSta0;
  if (!h.threadWinlogon) return Denial::helperThreadNotWinlogon;
  if (!h.inputWinlogon) return Denial::helperInputNotWinlogon;
  if (!h.promptForeground) return Denial::helperForegroundNotPrompt;
  return Denial::none;
}

// Input allowlist for the secure desktop: key, mouse move/button/wheel and text
// only. No touch (Windows draws no cursor for it), no relative mouse
// (moveRelative), no clipboard/drive/USB redirection (those are not input events
// and never reach the helper). Ctrl+Alt+Del is blocked at the injector on every
// desktop, as today.
inline bool inputEventAllowed(std::string_view event) {
  return event == "key" || event == "move" || event == "button" || event == "wheel" || event == "text";
}
inline bool inputTypeAllowed(std::string_view type) {
  // input.reset releases held keys; input.fence/input.direct never reach the helper.
  return type == "input" || type == "input.reset";
}

// The SERVICE routes a client's input to the SYSTEM helper instead of the user
// worker (which cannot reach the Winlogon desktop) only for ordinary input and
// the held-key release, only while the helper it launched for this stream is
// alive, and only after the worker reported that the stream shows the helper's
// real frames (so nobody clicks a prompt they cannot see). The user worker never
// carries input to the helper: a process of the user cannot answer a prompt.
inline bool serviceRoutesInput(std::string_view type, bool helperAlive, bool workerShowsHelper) {
  return helperAlive && workerShowsHelper && inputTypeAllowed(type);
}

// The worker <-> service control messages of this feature. Everything the worker
// sends under this prefix is consumed by the service and never forwarded to the
// broker (the relay drops a Host that sends an unknown message), known or not.
inline constexpr char controlPrefix[] = "worker.secure.";
inline constexpr char controlRequest[] = "worker.secure.request";  // worker -> service: launch the helper
inline constexpr char controlActive[] = "worker.secure.active";    // worker -> service: the stream shows helper frames
inline constexpr char controlStop[] = "worker.secure.stop";        // worker -> service: end the helper
inline constexpr char controlReady[] = "worker.secure.ready";      // service -> worker: the section handles
inline constexpr char controlFailed[] = "worker.secure.failed";    // service -> worker: no helper
inline bool secureControlConsumed(std::string_view type) {
  return type.substr(0, sizeof(controlPrefix) - 1) == controlPrefix;
}

// One secure-desktop episode's source switch, used by the user-session worker
// (secure_desktop_switch.hpp). The 10-minute limit and the locked/resume
// lifecycle stay in SecureDesktopNotice; this only decides, while the secure
// desktop shows, whether the picture is the helper's real frame or the fallback
// notice, and which stage-244 record to emit on each transition. A failure
// sticks until the desktop returns. A helper that ended by itself because it saw
// the Default desktop (while this worker still sees the secure one: two prompts
// in a row) may be asked for again, at most maxLaunches times per episode, so a
// flapping prompt never thrashes SYSTEM process creation.
enum class HelperPhase { idle, starting, active, failed };

class HelperSession {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto firstFrameDeadline = std::chrono::seconds(2);
  static constexpr unsigned maxLaunches = 4;

  // The secure desktop shows and no helper runs: true when the service should
  // be asked for one now. false (and the notice for the rest of the episode)
  // once the launches of this episode are used up.
  bool enter(Clock::time_point now) {
    if (phase_ != HelperPhase::idle) return false;
    if (launches_ >= maxLaunches) { phase_ = HelperPhase::failed; record_ = record244HelperLaunchFail; return false; }
    ++launches_;
    phase_ = HelperPhase::starting;
    began_ = now;
    return true;
  }
  // This stream has no helper (not the service's worker, or several outputs):
  // the notice alone for this episode, as before the helper existed. No record.
  void unavailable() { if (phase_ == HelperPhase::idle) phase_ = HelperPhase::failed; }
  // The desktop returned, or the stream is ending: forget this episode.
  void reset() { phase_ = HelperPhase::idle; record_ = 0; began_ = {}; launches_ = 0; }

  // A fresh, complete helper frame arrived.
  void frame() {
    if (phase_ == HelperPhase::starting) { phase_ = HelperPhase::active; record_ = record244HelperActive; }
  }
  // The helper could not launch, or its scope was denied before any frame.
  void launchFailed() {
    if (phase_ == HelperPhase::starting) { phase_ = HelperPhase::failed; record_ = record244HelperLaunchFail; }
  }
  // The helper was active and then lost (killed, crashed, hung, scope changed).
  void lost() {
    if (phase_ == HelperPhase::active) { phase_ = HelperPhase::failed; record_ = record244HelperLost; }
    else if (phase_ == HelperPhase::starting) { phase_ = HelperPhase::failed; record_ = record244HelperLaunchFail; }
  }
  // The helper said it ended by itself because the input desktop was Default.
  void helperEnded() {
    if (phase_ == HelperPhase::active) phase_ = HelperPhase::idle;  // may be asked for again
    else if (phase_ == HelperPhase::starting) { phase_ = HelperPhase::failed; record_ = record244HelperLaunchFail; }
  }
  // Each tick while starting: if the first frame has not come within the
  // deadline, fall back to the notice for the rest of this episode.
  void checkDeadline(Clock::time_point now) {
    if (phase_ == HelperPhase::starting && now - began_ >= firstFrameDeadline) {
      phase_ = HelperPhase::failed;
      record_ = record244HelperNoFrame;
    }
  }

  HelperPhase phase() const { return phase_; }
  // A helper is wanted (asked for or streaming) while true.
  bool wantHelper() const { return phase_ == HelperPhase::starting || phase_ == HelperPhase::active; }
  // The picture is the helper's frame only while active; otherwise the notice.
  bool showNotice() const { return phase_ != HelperPhase::active; }
  // The stage-244 record to emit since the last read (0 if none).
  uint32_t takeRecord() { return std::exchange(record_, 0u); }

 private:
  HelperPhase phase_ = HelperPhase::idle;
  Clock::time_point began_{};
  uint32_t record_ = 0;
  unsigned launches_ = 0;
};

}  // namespace host::secure