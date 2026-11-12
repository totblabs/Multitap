#include "host_service_restart.hpp"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

using namespace host::service_restart;
namespace {
unsigned checks = 0;
void expect(bool value, const char* message) {
  ++checks;
  if (!value) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
}
struct Fake final : Access {
  Admission admission{true, true, {41, 1000}, 0};
  Process successor{42, 2000};
  uint64_t tick = 100, exitsAt = 500, stoppedAt = 500, runningAt = 1000, startedAt = 0;
  uint32_t stopError = 0, startError = 0;
  unsigned stops = 0, starts = 0, observations = 0, ownershipReads = 0;
  bool configOwned = true, exitUnknown = false, statusBroken = false, neverRunning = false, uiPresent = true;
  std::function<void(Fake&)> onStop, onStart, onObserve;
  std::vector<char> operations;
  Admission admit() noexcept override { operations.push_back('A'); return admission; }
  bool owned(uint32_t& error) noexcept override {
    ++ownershipReads;
    operations.push_back('C');
    error = configOwned ? 0 : ERROR_ACCESS_DENIED;
    return configOwned;
  }
  Observation observe() noexcept override {
    ++observations;
    operations.push_back('Q');
    if (onObserve) onObserve(*this);
    if (statusBroken) return {State::unknown, {}, ERROR_ACCESS_DENIED};
    if (starts && !neverRunning && tick >= runningAt) return {State::running, successor, 0};
    if (stops && tick >= stoppedAt) return {State::stopped, {}, 0};
    return {stops ? State::pending : State::running, admission.old, 0};
  }
  uint32_t stop() noexcept override {
    ++stops; operations.push_back('T');
    if (onStop) onStop(*this);
    return stopError;
  }
  Exit oldExit() noexcept override {
    operations.push_back('X');
    return exitUnknown ? Exit::unknown : tick >= exitsAt ? Exit::exited : Exit::alive;
  }
  uint32_t start() noexcept override {
    ++starts; startedAt = tick; operations.push_back('S');
    if (onStart) onStart(*this);
    return startError;
  }
  uint64_t now() noexcept override { return tick; }
  void wait(uint32_t milliseconds) noexcept override { tick += milliseconds; }
};
constexpr Limits shortLimits{1000, 1000, 2500, 100};
void completion() {
  Fake access;
  const auto result = perform(access, shortLimits);
  expect(result.outcome == Outcome::completed, "normal exact old exit and fresh Running completes");
  expect(result.stage == Stage::completed && result.error == 0, "completed fixed result");
  expect(result.stopRequested && result.startRequested && result.observedRunning, "actual operation flags");
  expect(result.oldExitRequired && result.exactOldExit, "old lifetime proof required");
  expect(access.stops == 1 && access.starts == 1, "one stop and one start only");
  expect(access.tick == 1000 && access.observations > 1, "real bounded observation loop runs");
  expect(access.ownershipReads >= 3, "ownership refreshed before stop/start/completion");
  const auto text = json(result);
  expect(text.find("\"protocol\":\"service-restart-v1\"") != std::string::npos, "stable protocol");
  expect(text.find("\"outcome\":\"completed\"") != std::string::npos, "fixed terminal outcome");
  expect(text.find("pid") == std::string::npos && text.find("path") == std::string::npos, "no process/path metadata in result");
}
void statusFailureRetainsStart() {
  Fake access;
  access.onStop = [](Fake& value) { value.statusBroken = true; value.uiPresent = false; };
  const auto result = perform(access, shortLimits);
  expect(access.starts == 1, "persistent post-stop query failure cannot discard Start obligation");
  expect(access.stops == 1 && !access.uiPresent, "UI disposal never cancels native transaction");
  expect(result.exactOldExit && result.stopRequested && result.startRequested, "physical exit and attempted startup reported");
  expect(result.outcome == Outcome::uncertain && !result.observedRunning, "failed status cannot manufacture Running success");
  expect(result.queryErrors > 1 && result.firstError == ERROR_ACCESS_DENIED, "persistent errors remain observable");
  expect(result.stage == Stage::waitRunning && result.error == ERROR_TIMEOUT, "bounded terminal uncertainty");
  expect(access.tick <= 2100, "logical loop bounded without retries");
}
void initialStopped() {
  Fake access;
  access.admission = {true, false, {}, 0};
  access.runningAt = access.tick;
  const auto result = perform(access, shortLimits);
  expect(result.outcome == Outcome::completed, "initially stopped owned service starts");
  expect(!result.stopRequested && access.stops == 0 && access.starts == 1, "initially stopped no fake stop");
  expect(!result.oldExitRequired && result.exactOldExit, "no old owner exit required when verified stopped");
}
void refusesBeforeMutation() {
  for (const uint32_t error : {uint32_t(ERROR_ACCESS_DENIED), uint32_t(ERROR_SERVICE_DISABLED), uint32_t(ERROR_FILE_NOT_FOUND), uint32_t(ERROR_BUSY)}) {
    Fake access;
    access.admission = {false, false, {}, error};
    const auto result = perform(access, shortLimits);
    expect(result.outcome == Outcome::refused && result.error == error, "failed ownership/disabled/missing/serialization refusal");
    expect(access.starts == 0 && access.stops == 0, "preflight refusal has no service mutation");
  }
  Fake missing;
  missing.admission.old.created = 0;
  expect(perform(missing, shortLimits).outcome == Outcome::refused && !missing.stops, "unknown process lifetime refuses before stop");
  Fake changed;
  changed.configOwned = false;
  expect(perform(changed, shortLimits).outcome == Outcome::refused && !changed.stops, "configuration recheck before Stop");
  for (const auto limits : {Limits{0, 1, 1, 1}, Limits{1, 0, 1, 1}, Limits{1, 1, 0, 1}, Limits{1, 1, 1, 0},
                            Limits{65001, 1, 1, 1}, Limits{1, 30001, 1, 1}, Limits{1, 1, 100001, 1}, Limits{1, 1, 1, 251}}) {
    Fake access;
    expect(perform(access, limits).outcome == Outcome::refused && !access.stops && !access.starts, "invalid/expanded logical budgets refused");
  }
}
void physicalExitFence() {
  for (bool unknown : {false, true}) {
    Fake access;
    access.exitsAt = 999999;
    access.exitUnknown = unknown;
    const auto result = perform(access, shortLimits);
    expect(result.outcome == Outcome::uncertain && !result.exactOldExit, "alive or unreadable old process remains uncertain");
    expect(access.stops == 1 && access.starts == 0, "never overlap an unproven old lifetime");
    expect(access.tick == 1100, "exit wait bounded");
  }
  Fake same;
  same.successor = same.admission.old;
  const auto result = perform(same, shortLimits);
  expect(result.outcome == Outcome::uncertain && !result.observedRunning, "same PID+creation cannot be fresh restart");
  Fake reused;
  reused.successor.pid = reused.admission.old.pid;
  expect(perform(reused, shortLimits).outcome == Outcome::completed, "reused numeric PID with distinct created time can be fresh");
}
void errorsAndOwnership() {
  Fake failedStop;
  failedStop.stopError = ERROR_SERVICE_NOT_ACTIVE;
  const auto recovered = perform(failedStop, shortLimits);
  expect(recovered.outcome == Outcome::completed && recovered.firstError == ERROR_SERVICE_NOT_ACTIVE, "Stop error does not preempt physical exit and recovery proof");
  Fake failedStart;
  failedStart.startError = ERROR_ACCESS_DENIED;
  failedStart.neverRunning = true;
  const auto failed = perform(failedStart, shortLimits);
  expect(failed.outcome == Outcome::uncertain && failed.error == ERROR_ACCESS_DENIED && failed.startRequested, "Start rejection preserved without false success");
  Fake already;
  already.startError = ERROR_SERVICE_ALREADY_RUNNING;
  expect(perform(already, shortLimits).outcome == Outcome::completed, "already running needs real fresh proof");
  Fake foreign;
  foreign.onStop = [](Fake& value) { value.configOwned = false; };
  const auto changed = perform(foreign, shortLimits);
  expect(changed.outcome == Outcome::uncertain && changed.exactOldExit && foreign.starts == 0, "changed post-stop configuration must not start foreign executable");
  Fake lateForeign;
  lateForeign.onStart = [](Fake& value) { value.configOwned = false; };
  expect(perform(lateForeign, shortLimits).outcome == Outcome::uncertain, "configuration loss after Start cannot report success");
  Fake lostProof;
  lostProof.onStart = [](Fake& value) { value.exitUnknown = true; };
  expect(perform(lostProof, shortLimits).outcome == Outcome::uncertain, "final old lifetime proof still mandatory");
  Fake invalid;
  invalid.successor = {42, 0};
  expect(perform(invalid, shortLimits).outcome == Outcome::uncertain, "fresh PID without creation evidence is not Running proof");
}
void blockingCallBudget() {
  Fake access;
  access.onStop = [](Fake& value) { value.tick += 10000; value.statusBroken = true; };
  const auto result = perform(access, shortLimits);
  expect(access.starts == 1 && result.exactOldExit, "blocking SCM duration never erases owned Start obligation");
  expect(result.outcome == Outcome::uncertain && result.startRequested, "expired total budget performs one final proof then returns uncertain");
  expect(access.tick == 10100, "no artificial hard wall-clock claim or extra observation sleeps after expiry");
}
void physicalExitBeforeScmStopped() {
  Fake access;
  access.stoppedAt = 900;
  access.onStart = [](Fake& value) {
    if (value.tick < value.stoppedAt) { value.startError = ERROR_SERVICE_CANNOT_ACCEPT_CTRL; value.neverRunning = true; }
  };
  const auto result = perform(access, shortLimits);
  expect(result.outcome == Outcome::completed, "physical exit before SCM STOPPED does not strand service on early Start");
  expect(access.startedAt >= access.stoppedAt && access.starts == 1, "wait through positively known stop-pending then one Start");
  Fake stale;
  stale.stoppedAt = 999999;
  const auto uncertain = perform(stale, shortLimits);
  expect(stale.startedAt == 1100 && stale.starts == 1 && uncertain.exactOldExit, "expired stop-pending status still discharges one owned Start obligation");
}
void pairedVersion() {
  VS_FIXEDFILEINFO value{};
  value.dwSignature = VS_FFI_SIGNATURE; value.dwFileType = VFT_APP;
  value.dwFileVersionMS = value.dwProductVersionMS = MAKELONG(0, 1);
  value.dwFileVersionLS = value.dwProductVersionLS = MAKELONG(0, 14);
  expect(versionMatches(value, L"1.0.14"), "paired exact Host and Tool fixed version accepted");
  expect(!versionMatches(value, L"1.0.13"), "older installed service refuses newer Tool mutation");
  expect(!versionMatches(value, L"1.0.15"), "newer service refuses stale Tool mutation");
  auto changed = value; changed.dwFileVersionLS |= 1;
  expect(!versionMatches(changed, L"1.0.14"), "nonzero hidden fourth file version refused");
  changed = value; changed.dwProductVersionLS = MAKELONG(0, 13);
  expect(!versionMatches(changed, L"1.0.14"), "file/product version disagreement refused");
  changed = value; changed.dwSignature = 0;
  expect(!versionMatches(changed, L"1.0.14"), "untrusted fixed info shape refused");
  changed = value; changed.dwFileType = VFT_DLL;
  expect(!versionMatches(changed, L"1.0.14"), "DLL named executable does not satisfy application version");
}
}
int main() {
#ifndef MULTITAP_RESTART_ISOLATED_TEST
  // Force production NativeAccess and its Win32 imports to link, without
  // constructing the adapter or calling any service API in this test process.
  Result (*volatile productionEndpoint)() noexcept = &native;
  expect(productionEndpoint != nullptr, "production endpoint linked without invocation");
#endif
  completion(); statusFailureRetainsStart(); initialStopped(); refusesBeforeMutation(); physicalExitFence(); errorsAndOwnership(); blockingCallBudget(); physicalExitBeforeScmStopped(); pairedVersion();
  std::printf("{\"kind\":\"host-service-restart-injected-tests\",\"checks\":%u,\"nativeAdapterInvocations\":0,\"actualScmMutations\":0,\"runtimeQualified\":false}\n", checks);
}
