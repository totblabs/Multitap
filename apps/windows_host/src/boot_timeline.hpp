#pragma once
#include "operational_log.hpp"
#include <array>
#include <atomic>
#include <optional>

namespace host {
// boot-ready-v1 (backlog #41): where a service start's time goes until the
// client can show the desktop as 연결 준비됨. Each stage is the first time it
// was reached, in ms after the service started. One hostAction 274 event per
// service run: on the first streaming-capable report, or after ten minutes
// without one (ok false, only the stages reached). History only, never
// readiness authority.
inline constexpr uint32_t bootReadyAction = 274;
enum class BootStage : size_t {
  config = 0,  // Host configuration (broker credentials) loaded, broker agent started
  broker,      // broker channel open: the Host token was accepted (WebSocket upgrade)
  display,     // virtual display controller running with an active lease (service side)
  logon,       // an unlocked interactive Windows session first observed
  worker,      // the session worker first answered a probe with its desktop available
  ready,       // the first capabilities report with "streaming":true written to the broker
  count
};
// Field per stage (operational_log.hpp hostLogField; broker host-diagnostics.mjs).
inline constexpr std::array<const char*, size_t(BootStage::count)> bootStageFields{"configMs", "brokerMs", "virtualDisplayMs", "logonMs", "sessionWorkerMs", "readyMs"};
// Windows uptime when the service started: VM power-on to service start
// (firmware time is not included).
inline constexpr char bootUptimeField[] = "uptimeMs";
class BootTimeline {
 public:
  static constexpr uint64_t windowMs = 600000;
  explicit BootTimeline(uint64_t startMs = GetTickCount64()) : start_(startMs) { for (auto& at : at_) at = unset; }
  // First time only; later marks of a stage are ignored.
  void mark(BootStage stage, uint64_t nowMs = GetTickCount64()) noexcept {
    auto expected = unset;
    at_[size_t(stage)].compare_exchange_strong(expected, nowMs > start_ ? nowMs - start_ : 0);
  }
  bool reached(BootStage stage) const noexcept { return at_[size_t(stage)].load() != unset; }
  // Until the first ready report, at most ten minutes.
  bool window(uint64_t nowMs = GetTickCount64()) const noexcept { return !reached(BootStage::ready) && within(nowMs); }
  struct Record { bool ready = false; uint64_t uptimeMs = 0; std::array<int64_t, size_t(BootStage::count)> at{}; };
  // Once per run: when ready, or at the end of the window without it.
  std::optional<Record> take(uint64_t nowMs = GetTickCount64()) noexcept {
    const bool ready = reached(BootStage::ready);
    if ((!ready && within(nowMs)) || taken_.exchange(true)) return {};
    Record record; record.ready = ready; record.uptimeMs = start_;
    for (size_t i = 0; i < at_.size(); ++i) { const auto at = at_[i].load(); record.at[i] = at == unset ? -1 : int64_t(std::min<uint64_t>(at, 4294967295ULL)); }
    return record;
  }
 private:
  static constexpr uint64_t unset = UINT64_MAX;
  bool within(uint64_t nowMs) const noexcept { return nowMs < start_ || nowMs - start_ < windowMs; }
  uint64_t start_;
  std::array<std::atomic<uint64_t>, size_t(BootStage::count)> at_;
  std::atomic<bool> taken_{false};
};
// The service process's timeline: serviceMain's first use starts it.
inline BootTimeline& bootTimeline() { static BootTimeline value; return value; }
// stage 1 ready, 2 not ready after ten minutes; unreached stages are left out (-1).
// An uptime past 49.7 days does not fit the log's 32-bit values and is left out.
inline void recordBootTimeline(const BootTimeline::Record& record) noexcept {
  const auto& at = record.at;
  recordHostEvent("hostAction", {{"action", bootReadyAction}, {"stage", record.ready ? 1 : 2}, {"ok", record.ready}, {bootUptimeField, int64_t(std::min<uint64_t>(record.uptimeMs, uint64_t(INT64_MAX)))},
    {bootStageFields[0], at[0]}, {bootStageFields[1], at[1]}, {bootStageFields[2], at[2]}, {bootStageFields[3], at[3]}, {bootStageFields[4], at[4]}, {bootStageFields[5], at[5]}});
}
// The boot service loop's wait: none before its first pass. The virtual
// display start and the broker connection used to wait one full second after
// SERVICE_RUNNING for a timeout nothing needed.
inline DWORD serviceLoopWaitMs(bool firstPass) { return firstPass ? 0 : 1000; }
}
