#pragma once
#include <cstddef>
namespace host::service_workers {
// A timeout, a missing exit code, or a closed observer is never an exit witness.
enum class ProcessObservation { live, exited, unknown };
constexpr bool physicalExit(ProcessObservation value) noexcept {
  return value == ProcessObservation::exited;
}
struct DrainFacts {
  bool admissionClosed = false;
  std::size_t creationsInFlight = 0, retainedProcesses = 0, unknownWaits = 0;
  std::size_t retainedJobs = 0;
  std::size_t outstandingCleanupOwners = 0;
};
constexpr bool mayReportStopped(const DrainFacts& facts) noexcept {
  return facts.admissionClosed && facts.creationsInFlight == 0 &&
         facts.retainedProcesses == 0 && facts.unknownWaits == 0 && facts.retainedJobs == 0 &&
         facts.outstandingCleanupOwners == 0;
}
} // namespace host::service_workers
