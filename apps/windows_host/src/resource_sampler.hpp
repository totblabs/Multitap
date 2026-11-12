#pragma once
// In-guest CPU and memory for the Proxmox tile (host-resources-v1). Proxmox
// sees a Windows guest without the VirtIO balloon service at ~100 % memory;
// the Host runs inside the VM and measures the real figures. Sent as the
// optional "resources" object of host.heartbeat (released brokers ignore it).
// host-resources-v2: a broker that names it in host.capabilities.ack also gets
// uptimeSeconds there, for a physical PC's tile (a PC has no Proxmox uptime).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <powrprof.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#ifdef _MSC_VER
#pragma comment(lib, "powrprof.lib")
#endif
namespace host {
inline constexpr const char* hostResourcesProtocol = "host-resources-v2";
struct CpuTimes { uint64_t idle = 0, kernel = 0, user = 0; };  // GetSystemTimes: kernel includes idle
struct MemoryStatus { uint64_t total = 0, available = 0; };
struct ResourceReading { double cpuPercent = 0; unsigned cpuCount = 0; uint64_t memoryUsed = 0, memoryTotal = 0; std::optional<uint64_t> uptimeSeconds;
  double memoryPercent() const { return memoryTotal ? 100.0 * double(memoryUsed) / double(memoryTotal) : 0; } };

// Whole seconds Windows has run since it last started or woke (sleep,
// hibernation and a Fast Startup boot are wakes). Both are interrupt-time
// counts in 100 ns, which keep counting through sleep; lastWake 0: never woke
// since boot. A wake "after" now is no reading. Not yet observed on a PC that
// slept: that LastWakeTime includes the sleep and that Fast Startup sets it.
inline std::optional<uint64_t> awakeSeconds(uint64_t interruptTime, uint64_t lastWake) {
  if (lastWake > interruptTime) return std::nullopt;
  return (interruptTime - lastWake) / 10000000ull;
}
// GetTickCount64 counts the same interrupt time (through sleep) in ms.
inline std::optional<uint64_t> windowsAwakeSeconds() {
  ULONGLONG lastWake = 0;
  if (CallNtPowerInformation(LastWakeTime, nullptr, 0, &lastWake, sizeof(lastWake)) != 0) return std::nullopt;
  return awakeSeconds(GetTickCount64() * 10000ull, lastWake);
}

class ResourceSampler {
 public:
  using TimesSource = std::function<bool(CpuTimes&)>;
  using MemorySource = std::function<bool(MemoryStatus&)>;
  using UptimeSource = std::function<std::optional<uint64_t>()>;
  ResourceSampler(TimesSource times, MemorySource memory, unsigned cpuCount, UptimeSource uptime = {}) : times_(std::move(times)), memory_(std::move(memory)), cpuCount_(cpuCount), uptime_(std::move(uptime)) {}
  static ResourceSampler System() {
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return ResourceSampler([](CpuTimes& out) {
      FILETIME idle{}, kernel{}, user{}; if (!GetSystemTimes(&idle, &kernel, &user)) return false;
      const auto value = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
      out = {value(idle), value(kernel), value(user)}; return true;
    }, [](MemoryStatus& out) {
      MEMORYSTATUSEX status{}; status.dwLength = sizeof(status); if (!GlobalMemoryStatusEx(&status)) return false;
      out = {status.ullTotalPhys, status.ullAvailPhys}; return true;
    }, logical ? logical : std::max<DWORD>(1, info.dwNumberOfProcessors), windowsAwakeSeconds);
  }
  // Nullopt until a CPU baseline exists (the first call) or a source fails.
  // CPU is the busy share of all logical CPUs since the previous call.
  std::optional<ResourceReading> Sample() {
    CpuTimes now; MemoryStatus memory;
    if (!times_(now) || !memory_(memory) || memory.total == 0 || cpuCount_ == 0) return std::nullopt;
    const bool hadBaseline = baseline_.has_value();
    const CpuTimes before = baseline_.value_or(now);
    // Times only move forward; a backwards step (counter reset) restarts the baseline.
    const bool forward = now.kernel >= before.kernel && now.user >= before.user && now.idle >= before.idle;
    baseline_ = now;
    if (!hadBaseline || !forward) return std::nullopt;
    const uint64_t total = (now.kernel - before.kernel) + (now.user - before.user), idle = now.idle - before.idle;
    if (total > 0) lastCpu_ = std::clamp(100.0 * double(total - std::min(idle, total)) / double(total), 0.0, 100.0);
    else if (!lastCpu_) return std::nullopt;
    ResourceReading reading; reading.cpuPercent = *lastCpu_; reading.cpuCount = cpuCount_;
    reading.memoryTotal = memory.total; reading.memoryUsed = memory.total - std::min(memory.available, memory.total);
    if (uptime_) reading.uptimeSeconds = uptime_();
    return reading;
  }
 private:
  TimesSource times_; MemorySource memory_; unsigned cpuCount_; UptimeSource uptime_;
  std::optional<CpuTimes> baseline_; std::optional<double> lastCpu_;
};

// When the reading goes out: always with the regular 10 s heartbeat; earlier
// (but at most every 3 s) when CPU or memory moved by >= 5 percentage points
// since the last one sent.
class ResourceReportPolicy {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr double kEarlyDelta = 5.0;
  static constexpr std::chrono::seconds kMinInterval{3};
  bool EarlyDue(const ResourceReading& reading, Clock::time_point now) const {
    if (!sent_) return false;  // the regular heartbeat carries the first one
    if (now - sentAt_ < kMinInterval) return false;
    return std::abs(reading.cpuPercent - sent_->cpuPercent) >= kEarlyDelta || std::abs(reading.memoryPercent() - sent_->memoryPercent()) >= kEarlyDelta;
  }
  void Sent(const ResourceReading& reading, Clock::time_point now) { sent_ = reading; sentAt_ = now; }
  void Reset() { sent_.reset(); }
 private:
  std::optional<ResourceReading> sent_; Clock::time_point sentAt_{};
};

// The broker channel's sender (agent.hpp sendLoop) without a packet to send:
// sleep until the next resource sample or heartbeat is due, instead of waking
// every second to find, every other time, nothing due. A queued packet still
// ends the wait at once. The 10 ms slack makes the due check true on waking;
// whatever else falls due within 250 ms is done on the same wake, so the late
// wakes' drift never splits a heartbeat from its sample.
inline constexpr std::chrono::seconds hostResourceSampleInterval{2}, hostHeartbeatInterval{10};
inline constexpr std::chrono::milliseconds hostSenderCoalesce{250};
inline bool hostResourceSampleDue(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point sampledAt) { return now - sampledAt >= hostResourceSampleInterval - hostSenderCoalesce; }
inline bool hostHeartbeatDue(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point heartbeatAt) { return now - heartbeatAt >= hostHeartbeatInterval - hostSenderCoalesce; }
inline std::chrono::milliseconds hostSenderIdleWait(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point sampledAt, std::chrono::steady_clock::time_point heartbeatAt) {
  if (hostResourceSampleDue(now, sampledAt) || hostHeartbeatDue(now, heartbeatAt)) return std::chrono::milliseconds(0);
  const auto due = std::min(sampledAt + hostResourceSampleInterval, heartbeatAt + hostHeartbeatInterval);
  return std::chrono::ceil<std::chrono::milliseconds>(due - now) + std::chrono::milliseconds(10);
}
// The capability report a heartbeat carries. One report reads WTS sessions, SCM
// status, driver and file state: 70-96 context switches of the broker channel's
// sender each (measured). A heartbeat sent early for a CPU or memory
// change, every 3 s on a desktop in use, recomputed it each time and kept that
// idle sender at ~1,600 wakeups a minute. It now repeats the newest report the
// channel wrote (heartbeat or announcement) while that is younger than a regular
// heartbeat interval, as on a Host whose load does not move; a result carrying
// its own report (host.display.result) makes the next heartbeat read again.
struct HostHeartbeatCapabilities { std::string report; std::chrono::steady_clock::time_point at{}; };
template<class Read> const std::string& hostHeartbeatCapabilities(HostHeartbeatCapabilities& last, std::chrono::steady_clock::time_point now, Read&& read) {
  if (last.report.empty() || hostHeartbeatDue(now, last.at)) { last.report = read(); last.at = now; }
  return last.report;
}
// uptime: the broker negotiated host-resources-v2 (a released one never gets the field).
inline std::string ResourcesJson(const ResourceReading& reading, bool uptime = false) {
  char text[240];
  std::snprintf(text, sizeof(text), "{\"cpuPercent\":%.1f,\"cpuCount\":%u,\"memoryUsedBytes\":%llu,\"memoryTotalBytes\":%llu", std::clamp(reading.cpuPercent, 0.0, 100.0), reading.cpuCount,
                (unsigned long long)reading.memoryUsed, (unsigned long long)reading.memoryTotal);
  std::string json = text;
  if (uptime && reading.uptimeSeconds) json += ",\"uptimeSeconds\":" + std::to_string(*reading.uptimeSeconds);
  return json + '}';
}
}