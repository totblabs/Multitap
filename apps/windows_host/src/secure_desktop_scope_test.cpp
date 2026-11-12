// secure_desktop_scope.hpp: the Role::secure scope matrix, the input allowlist,
// and the helper source-switch state machine. Pure; no Windows, no VM.
#include "secure_desktop_scope.hpp"

#include <iostream>

namespace {
int failures = 0, checks = 0;
void expect(bool value, const char* what) { ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; } }

using namespace host::secure;
const std::string kUser = "S-1-5-21-1-2-3-1001";
const uint32_t kSession = 1;

ConsoleView okConsole() { return {kSession, /*unlocked*/ true, /*noUser*/ false, kUser, /*observedAt*/ 10000}; }
HelperView okHelper() { return {/*isSystem*/ true, kSession, /*winSta0*/ true, /*threadWinlogon*/ true, /*inputWinlogon*/ true, /*promptForeground*/ true}; }

void consoleMatrix() {
  const uint64_t now = 10000;
  expect(consoleAllowed(kSession, kUser, okConsole(), now) == Denial::none, "unlocked, same user, same session passes");

  auto wrongSession = okConsole(); wrongSession.session = 2;
  expect(consoleAllowed(kSession, kUser, wrongSession, now) == Denial::wrongSession, "another session denied");

  auto locked = okConsole(); locked.unlocked = false;
  expect(consoleAllowed(kSession, kUser, locked, now) == Denial::locked, "locked session denied (ends the stream the old way)");

  auto noUser = okConsole(); noUser.noUser = true; noUser.signedInSid.clear();
  expect(consoleAllowed(kSession, kUser, noUser, now) == Denial::noUser, "no-user session denied");

  auto otherUser = okConsole(); otherUser.signedInSid = "S-1-5-21-9-9-9-1001";
  expect(consoleAllowed(kSession, kUser, otherUser, now) == Denial::accountMismatch, "another signed-in account denied");

  expect(consoleAllowed(kSession, kUser, okConsole(), 12000) == Denial::none, "two-second scope boundary inclusive");
  expect(consoleAllowed(kSession, kUser, okConsole(), 12001) == Denial::staleScope, "stale scope denied");
  expect(consoleAllowed(kSession, kUser, okConsole(), 9999) == Denial::staleScope, "future scope denied");

  expect(consoleAllowed(0, kUser, okConsole(), now) == Denial::wrongSession, "zero expected session denied");
  expect(consoleAllowed(kSession, "", okConsole(), now) == Denial::wrongSession, "empty expected SID denied");
}

void helperMatrix() {
  const uint64_t now = 10000;
  expect(helperAllowed(kSession, kUser, okConsole(), okHelper(), now) == Denial::none, "SYSTEM on Winlogon in the unlocked user session passes");

  // Console deviations propagate through the helper check.
  auto locked = okConsole(); locked.unlocked = false;
  expect(helperAllowed(kSession, kUser, locked, okHelper(), now) == Denial::locked, "helper denied when the session is locked");
  auto otherUser = okConsole(); otherUser.signedInSid = "S-1-5-21-9-9-9-1001";
  expect(helperAllowed(kSession, kUser, otherUser, okHelper(), now) == Denial::accountMismatch, "helper denied for another account");

  // Each helper fact deviation is denied in order.
  auto notSystem = okHelper(); notSystem.isSystem = false;
  expect(helperAllowed(kSession, kUser, okConsole(), notSystem, now) == Denial::helperNotSystem, "a non-SYSTEM helper denied");
  auto wrongSession = okHelper(); wrongSession.session = 2;
  expect(helperAllowed(kSession, kUser, okConsole(), wrongSession, now) == Denial::helperWrongSession, "a helper in another session denied");
  auto notWinSta0 = okHelper(); notWinSta0.winSta0 = false;
  expect(helperAllowed(kSession, kUser, okConsole(), notWinSta0, now) == Denial::helperNotWinSta0, "a helper off WinSta0 denied");
  auto notThread = okHelper(); notThread.threadWinlogon = false;
  expect(helperAllowed(kSession, kUser, okConsole(), notThread, now) == Denial::helperThreadNotWinlogon, "a helper thread off Winlogon denied");
  auto notInput = okHelper(); notInput.inputWinlogon = false;
  expect(helperAllowed(kSession, kUser, okConsole(), notInput, now) == Denial::helperInputNotWinlogon, "input desktop not Winlogon denied (no injection into Default)");
  // Only a security prompt in front (not the Ctrl+Alt+Del options screen).
  auto notPrompt = okHelper(); notPrompt.promptForeground = false;
  expect(helperAllowed(kSession, kUser, okConsole(), notPrompt, now) == Denial::helperForegroundNotPrompt, "a Winlogon window that is not a security prompt denied");
  expect(securityPromptImage(L"C:\\Windows\\System32\\consent.exe", L"C:\\Windows\\system32") && securityPromptImage(L"C:\\WINDOWS\\system32\\CredentialUIBroker.exe", L"C:\\Windows\\System32\\"),
         "consent.exe and CredentialUIBroker.exe in System32 are security prompts");
  for (const wchar_t* image : {L"C:\\Windows\\System32\\LogonUI.exe", L"C:\\Windows\\System32\\winlogon.exe", L"C:\\Users\\a\\consent.exe", L"C:\\Windows\\System32\\sub\\consent.exe",
                               L"C:\\Windows\\SysWOW64\\consent.exe", L"C:\\Windows\\System32consent.exe", L"C:\\Windows\\System32\\consent.exe.bak", L"consent.exe", L""})
    expect(!securityPromptImage(image, L"C:\\Windows\\System32"), "another program, or consent.exe outside System32, is not a security prompt");
}

void inputAllowlist() {
  for (const char* e : {"key", "move", "button", "wheel", "text"}) expect(inputEventAllowed(e), "allowed input event");
  for (const char* e : {"touch", "moveRelative", "scrollbar", ""}) expect(!inputEventAllowed(e), "disallowed input event");
  expect(inputTypeAllowed("input") && inputTypeAllowed("input.reset"), "input and reset allowed");
  for (const char* t : {"input.fence", "input.direct", "clip.packet", "cursor.compose", "drive.packet", "usb.attach", ""}) expect(!inputTypeAllowed(t), "non-input types refused");
}

void routingAndControl() {
  // The service routes input to the helper only while the helper it launched is
  // alive AND the worker reported that the stream shows the helper's frames.
  expect(serviceRoutesInput("input", true, true), "input routed while the helper streams");
  expect(serviceRoutesInput("input.reset", true, true), "reset routed while the helper streams");
  expect(!serviceRoutesInput("input", true, false), "no input before the stream shows the prompt");
  expect(!serviceRoutesInput("input", false, true), "no input to a helper that exited");
  for (const char* t : {"ping", "input.fence", "input.direct", "clip.packet", "cursor.compose", "session.stop"})
    expect(!serviceRoutesInput(t, true, true), "only input and input.reset are routed");
  // Everything the worker sends under worker.secure. is consumed by the service.
  for (const char* t : {"worker.secure.request", "worker.secure.active", "worker.secure.stop", "worker.secure.ready", "worker.secure.failed", "worker.secure.anything-new"})
    expect(secureControlConsumed(t), "worker.secure.* is consumed, never forwarded");
  for (const char* t : {"stream.info", "worker.encoder.usage", "worker.profile.active", "input", "worker.stream.failure", "worker.secure", "worker.secured.x", ""})
    expect(!secureControlConsumed(t), "other worker messages are not this feature's");
}

void switchStateMachine() {
  using Clock = HelperSession::Clock;
  const auto t0 = Clock::now();

  // notice -> helper -> resume (the desktop returns): a clean episode.
  {
    HelperSession s;
    expect(s.phase() == HelperPhase::idle && s.showNotice() && !s.wantHelper(), "idle shows notice, wants no helper");
    s.enter(t0);
    expect(s.wantHelper() && s.showNotice() && s.takeRecord() == 0, "starting: want helper, still notice until a frame");
    s.checkDeadline(t0 + std::chrono::seconds(1));
    expect(s.phase() == HelperPhase::starting, "within deadline stays starting");
    s.frame();
    expect(s.phase() == HelperPhase::active && !s.showNotice() && s.wantHelper(), "active: real frames, not notice");
    expect(s.takeRecord() == record244HelperActive && s.takeRecord() == 0, "active emits record 5 once");
    s.reset();  // desktop returned
    expect(s.phase() == HelperPhase::idle && s.showNotice() && !s.wantHelper(), "reset returns to idle");
  }
  // notice -> helper -> lost while active: fall back to the notice for the rest.
  {
    HelperSession s; s.enter(t0); s.frame();
    expect(s.takeRecord() == record244HelperActive, "active record");
    s.lost();
    expect(s.phase() == HelperPhase::failed && s.showNotice() && !s.wantHelper(), "lost -> notice, no relaunch");
    expect(s.takeRecord() == record244HelperLost, "lost emits record 7");
    s.frame();  // a late frame must not revive a failed episode
    expect(s.phase() == HelperPhase::failed && s.showNotice(), "no revival after failure");
  }
  // launch failed before any frame.
  {
    HelperSession s; s.enter(t0); s.launchFailed();
    expect(s.phase() == HelperPhase::failed && s.showNotice() && s.takeRecord() == record244HelperLaunchFail, "launch fail -> record 6, notice");
  }
  // no frame within the deadline.
  {
    HelperSession s; s.enter(t0);
    s.checkDeadline(t0 + HelperSession::firstFrameDeadline);
    expect(s.phase() == HelperPhase::failed && s.showNotice() && s.takeRecord() == record244HelperNoFrame, "no-frame deadline -> record 8, notice");
  }
  // The helper ended by itself (it saw the Default desktop) while this worker
  // still sees the secure one: it may be asked for again, a bounded number of times.
  {
    HelperSession s;
    expect(s.enter(t0), "first launch is asked for");
    expect(!s.enter(t0), "no second request while one is starting");
    s.frame(); (void)s.takeRecord();
    s.helperEnded();
    expect(s.phase() == HelperPhase::idle && s.showNotice() && s.takeRecord() == 0, "a clean helper end returns to idle without a failure record");
    unsigned launches = 1;
    while (s.enter(t0)) { ++launches; s.frame(); (void)s.takeRecord(); s.helperEnded(); }
    expect(launches == HelperSession::maxLaunches, "launches per episode are bounded");
    expect(s.phase() == HelperPhase::failed && s.takeRecord() == record244HelperLaunchFail, "after the bound the notice stays (record 6)");
    s.reset();
    expect(s.enter(t0), "a new episode may launch again");
  }
  // A stream without a helper: the notice for the episode, no helper record.
  {
    HelperSession s; s.unavailable();
    expect(s.phase() == HelperPhase::failed && s.showNotice() && !s.wantHelper() && s.takeRecord() == 0 && !s.enter(t0), "no helper for this stream: the notice, no record, no request");
    s.reset();
    expect(s.phase() == HelperPhase::idle, "the next episode starts over");
  }
  // A helper that ended before any frame is a failed launch.
  {
    HelperSession s; (void)s.enter(t0); s.helperEnded();
    expect(s.phase() == HelperPhase::failed && s.takeRecord() == record244HelperLaunchFail, "ended before a frame -> record 6");
  }
  // records that continue vs end the stream.
  for (uint32_t r : {record244NoticeShown, record244Resumed, record244HelperActive, record244HelperLaunchFail, record244HelperLost, record244HelperNoFrame})
    expect(record244Continues(r), "continue record");
  for (uint32_t r : {record244EndedLocked, record244EndedLimit}) expect(!record244Continues(r), "ending record");
}

}  // namespace

int main() {
  consoleMatrix();
  helperMatrix();
  inputAllowlist();
  routingAndControl();
  switchStateMachine();
  if (failures) { std::cerr << failures << " failure(s) of " << checks << " checks\n"; return 1; }
  std::cout << "secure desktop scope: ok (" << checks << " checks)\n";
  return 0;
}
