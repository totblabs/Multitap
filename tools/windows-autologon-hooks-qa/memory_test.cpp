#include "../../apps/windows_host/src/windows_autologon_job.hpp"
#include "../../apps/windows_host/src/windows_autologon_update_gate.hpp"
#include <iostream>
using namespace host::autologon;
namespace { int checks = 0, failures = 0;
void check(bool ok, const char* label) { ++checks; if (!ok) { ++failures; std::cerr << "FAIL " << label << '\n'; } }
}
int main() {
  check(queuedRecoveryMayStart(10, 10, false), "fresh work item can start");
  check(!queuedRecoveryMayStart(10, 10, true), "closed service suppresses queued launch");
  check(!queuedRecoveryMayStart(10, 30010, false), "queued absolute deadline does not slide");
  check(!queuedRecoveryMayStart(10, 9, false), "invalid clock cannot admit stale queue");
  // Actual production gate: SCM stop can be signalled before the loop reaches
  // teardown/closeScheduling. A queued callback must reject it independently.
  check(!queuedRecoveryMayStart(10, 11, false, RecoveryStopState::requested), "signalled stop blocks queued work before explicit close");
  check(!queuedRecoveryMayStart(10, 11, false, RecoveryStopState::unknown), "unreadable owned stop signal fails closed");
  check(queuedRecoveryMayStart(10, 11, false, RecoveryStopState::running), "same queued work runs only with live verified stop signal");
  RecoveryObservation admittedBeforeStop; admittedBeforeStop.begin(10, 30);
  check(admittedBeforeStop.active() && !queuedRecoveryMayStart(10, 11, true, RecoveryStopState::requested), "closing stops future admission while actual admitted owner remains");
  admittedBeforeStop.observe(41, false, {}, true);
  check(admittedBeforeStop.active(), "shutdown plus observer timeout never retires actual owner");
  admittedBeforeStop.observe(42, true, uint32_t(0), true);
  check(!admittedBeforeStop.active() && admittedBeforeStop.code() == "completed", "admitted owner still requires actual terminal exit");
  RecoveryObservation job;
  check(job.begin(100, 30), "first start accepted");
  check(!job.begin(101, 30), "duplicate start denied before exit");
  job.observe(129, false, {}, true);
  check(job.active() && !job.expired(), "observing before deadline");
  job.observe(130, false, {}, true);
  check(job.active() && job.expired() && !job.exitCode(), "timeout retains admitted owner");
  check(!job.begin(200, 30), "timeout never permits retry");
  job.observe(201, true, {}, true);
  check(job.active(), "signalled without actual exit code does not release");
  job.observe(202, true, uint32_t(0), false);
  check(!job.active() && job.code() == "identity_unverified", "unknown identity never promoted to success");
  check(!job.begin(300, 30), "one child per service lifetime even after terminal");
  RecoveryObservation complete; complete.begin(0);
  complete.observe(3, true, uint32_t(0), true);
  check(!complete.active() && complete.code() == "completed", "actual exit required for completion");
  complete.observe(40000, false, {}, false);
  check(complete.code() == "completed", "late observation cannot undo terminal result");
  RecoveryObservation failed; failed.begin(0); failed.launchFailed();
  check(!failed.active() && !failed.begin(2), "launch failure bounded without retry loop");
  RecoveryObservation nonzero; nonzero.begin(0); nonzero.observe(1, true, uint32_t(20), true);
  check(nonzero.code() == "repair_required", "cleanup failure not Host failure or success");
  for (const auto* result : {"cleaned", "unchanged"}) {
    check(cleanupAllowsStateDeletion(true, true, true, uint32_t(0), result), "explicit complete cleanup permits own-state removal");
    check(!cleanupAllowsStateDeletion(false, true, true, uint32_t(0), result), "confirmation required");
    check(!cleanupAllowsStateDeletion(true, false, true, uint32_t(0), result), "observer timeout cannot remove metadata");
    check(!cleanupAllowsStateDeletion(true, true, false, uint32_t(0), result), "foreign process cannot authorize removal");
    check(!cleanupAllowsStateDeletion(true, true, true, {}, result), "null exit cannot authorize removal");
    check(!cleanupAllowsStateDeletion(true, true, true, uint32_t(20), result), "nonzero exit cannot authorize removal");
  }
  for (const auto* result : {"recovered", "repair_required", "unknown", "partial", "", "foreign"}) {
    check(!cleanupAllowsStateDeletion(true, true, true, uint32_t(0), result), "only uninstall terminal vocabulary permits removal");
  }
  Record valid; valid.operationId = "11111111-1111-4111-8111-111111111111";
  valid.action = "apply"; valid.phase = "owned"; valid.account = {L"fixture", L"FIXTURE-PC", L"S-1-5-21-1-2-3-1001", true};
  valid.expected.values[0] = textValue(L"1"); valid.expected.secretExists = true; valid.expected.secret = Secret{L"synthetic-password"}; valid.next = valid.expected;
  check(completedJournalForUpdate(valid), "completed owned metadata can be preserved without OS reads");
  { auto r = valid; r.intent = true; check(!completedJournalForUpdate(r), "inflight intent defers update"); }
  { auto r = valid; r.phase = "changing"; check(!completedJournalForUpdate(r), "unknown native outcome defers update"); }
  { auto r = valid; r.next.values[1] = textValue(L"different"); check(!completedJournalForUpdate(r), "uncommitted next image defers"); }
  { auto r = valid; r.baseline.secret.text = L"unexplained"; check(!completedJournalForUpdate(r), "inconsistent baseline secret defers"); }
  { auto r = valid; r.expected.values[5] = textValue(L"plaintext"); r.next = r.expected; check(!completedJournalForUpdate(r), "plaintext record rejected"); }
  { auto r = valid; r.expected.values[3] = textValue(L"1"); r.next = r.expected; check(!completedJournalForUpdate(r), "foreign forced setting rejected"); }
  { auto r = valid; r.phase = "disabled"; r.action = "disable"; r.expected = r.baseline; r.next = r.expected; check(completedJournalForUpdate(r), "completed disabled record preserved"); }
  { auto r = valid; r.schema = 2; check(completedJournalForUpdate(r), "completed schema2 owned record remains update compatible"); }
  { auto r = valid; r.schema = 2; r.phase = "disabled"; r.action = "disable"; r.account = {}; r.baseline = takeoverBaseline(r.baseline); r.expected = r.baseline; r.next = r.expected;
    check(completedJournalForUpdate(r), "external disable schema2 needs no invented SID to preserve completed metadata");
    r.phase = "changing"; check(!completedJournalForUpdate(r), "takeover partial still defers update"); }
  { auto r = valid; r.schema = 2; r.expected.plainPasswordBytes = 34; r.next = r.expected; check(!completedJournalForUpdate(r), "completed journal cannot conceal leftover plaintext metadata"); }
  { auto r = valid; r.schema = 99; check(!completedJournalForUpdate(r), "unknown schema is never adopted for update"); }
  std::cout << "{\"kind\":\"windows-autologon-hooks-memory\",\"checks\":" << checks << ",\"failures\":" << failures << ",\"nativeCalls\":false,\"processCalls\":false,\"filesChanged\":false}\n";
  return failures ? 1 : 0;
}
