#pragma once
#include "support.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace host {
inline constexpr const char* streamObservabilityProtocol = "stream-observability-v1";

// This helper is also used below Session0's service.hpp serializer (Agent and
// track tunnel). Only an opted-in, bounded statistics object reaches it.
inline std::string streamObservationJson(const Json& value) {
  if(const auto* rows=std::get_if<Json::Array>(&value.value)){std::string out="[";for(const auto& child:*rows){if(out.size()>1)out+=',';out+=streamObservationJson(child);}return out+']';}
  if(const auto* fields=std::get_if<Json::Object>(&value.value)){std::string out="{";for(const auto& [key,child]:*fields){if(out.size()>1)out+=',';out+=jsonQuote(key)+':'+streamObservationJson(child);}return out+'}';}
  if(const auto* text=std::get_if<std::string>(&value.value))return jsonQuote(*text);
  if(const auto* number=std::get_if<double>(&value.value)){std::ostringstream out;out<<std::setprecision(17)<<*number;return out.str();}
  if(const auto* flag=std::get_if<bool>(&value.value))return *flag?"true":"false";
  return "null";
}

// Whole-machine process CPU, in tenths of a percent. The first observation or
// a reset/failure is unknown, never a fabricated zero. Counter units are 100 ns.
class ProcessCpuObservation {
 public:
  std::optional<uint32_t> observe(uint64_t wall, uint64_t cpu, unsigned processors) {
    const auto before = baseline_; baseline_ = std::pair{wall, cpu};
    if (!before || !processors || wall <= before->first || cpu < before->second) return {};
    const double share = double(cpu - before->second) / double(wall - before->first) / processors;
    if (!std::isfinite(share) || share < 0) return {};
    return uint32_t(std::llround(std::clamp(share * 1000.0, 0.0, 1000.0)));
  }
  void reset() { baseline_.reset(); }
  std::optional<uint32_t> sample() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) { reset(); return {}; }
    // GetSystemTimeAsFileTime would move with wall-clock adjustments. Use the
    // monotonic performance clock so only CPU counter and interval deltas mix.
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    const auto ticks = std::chrono::duration_cast<std::chrono::duration<uint64_t, std::ratio<1, 10000000>>>(elapsed).count();
    const auto value = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    const auto count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return observe(ticks, value(kernel) + value(user), count);
#else
    return {};
#endif
  }
 private:
  std::optional<std::pair<uint64_t, uint64_t>> baseline_;
};

struct ObservedSendQueue {
  uint64_t packets = 0, bytes = 0, oldestUs = 0;
  std::optional<uint64_t> writeUs;
};

// A window of CPU-observed WGC CopyResource + Map waits. This never forces a
// GPU flush or waits for anything capture would not already wait for.
class ReadbackObservation {
 public:
  void observe(std::optional<double> ms) {
    if (!ms || !std::isfinite(*ms) || *ms < 0 || *ms > 60000) return;
    if (count_ >= 1024) return;
    total_ += *ms; maximum_ = std::max(maximum_, *ms); ++count_;
  }
  std::optional<std::pair<uint64_t, uint64_t>> take() {
    if (!count_) return {};
    const auto value = std::pair{uint64_t(std::llround(total_ * 1000 / count_)), uint64_t(std::llround(maximum_ * 1000))};
    total_ = maximum_ = 0; count_ = 0; return value;
  }
 private:
  double total_ = 0, maximum_ = 0; unsigned count_ = 0;
};

inline std::optional<uint64_t> hostObservationMaximum(const std::string& name) {
  if (name == "hostQueuePackets") return 4096;
  if (name == "hostQueueBytes") return 16777216;
  if (name == "hostQueueOldestUs" || name == "hostWriteUs" || name == "readbackUs" || name == "readbackMaxUs") return 60000000;
  if (name == "serviceCpuPct10" || name == "workerCpuPct10") return 1000;
  return {};
}

// One owning Agent/tunnel, reset on session replacement. Disabled sessions do
// not parse/copy messages or read process counters. Every optional failure is
// swallowed by augment(): diagnostics cannot reject valid media/control.
class StreamObservability {
 public:
  template<class JsonValue> void configure(const JsonValue& start) {
    std::lock_guard lock(mutex_);
    session_ = start.get("sessionId").string();
    enabled_ = !session_.empty() && start.get("type").string() == "session.start" &&
      start.get("mode").string() != "rdp" && start.get("observabilityProtocol").string() == streamObservabilityProtocol;
    tracks_ = start.get("mediaProtocol").string() == "tracks-v1";
    const auto epoch = start.get("epoch").number(0);
    epoch_ = std::isfinite(epoch) && epoch >= 0 && epoch <= 65535 && std::floor(epoch) == epoch ? uint16_t(epoch) : 0;
    cpu_.reset();
  }
  void reset() { std::lock_guard lock(mutex_); enabled_ = false; session_.clear(); cpu_.reset(); }
  bool enabled() const { return enabled_.load(); }

  template<class JsonValue, class Parser, class Serializer>
  std::string augment(const std::string& raw, bool service, const std::optional<ObservedSendQueue>& queue,
                      std::optional<std::pair<uint64_t, uint64_t>> readback, Parser parse, Serializer serialize) noexcept {
    try {
      std::lock_guard lock(mutex_);
      if (!enabled_) return raw;
      auto value = parse(raw);
      const auto type = value.get("type").string();
      if (type != (tracks_ ? "stream.tracks.stats" : "stream.stats")) return raw;
      auto* object = std::get_if<typename JsonValue::Object>(&value.value);
      if (!object) return raw;
      const auto sourceSession = value.get("sessionId").string();
      if ((!sourceSession.empty() && sourceSession != session_) || (service && sourceSession != session_)) return raw;
      const auto* observedEpoch = std::get_if<double>(&value.get("epoch").value);
      if (!tracks_ && service && (!observedEpoch || !std::isfinite(*observedEpoch) || *observedEpoch < 0 || *observedEpoch > 65535 || std::floor(*observedEpoch) != *observedEpoch)) return raw;
      if (!tracks_ && !service && observedEpoch && *observedEpoch != epoch_) return raw;
      std::map<std::string, uint64_t> fields;
      const auto& previous = value.get("observability");
      if (previous.get("protocol").string() == streamObservabilityProtocol) {
        if (const auto* metrics = std::get_if<typename JsonValue::Object>(&previous.get("metrics").value)) {
          for (const auto& [key, item] : *metrics) {
            const auto maximum = hostObservationMaximum(key);
            const auto* number = std::get_if<double>(&item.value);
            // Service-owned queue/CPU measurements can never come from worker.
            if (!maximum || !number || !std::isfinite(*number) || *number < 0 || *number > *maximum || std::floor(*number) != *number ||
                (service && key != "workerCpuPct10" && key != "readbackUs" && key != "readbackMaxUs")) continue;
            fields[key] = uint64_t(*number);
          }
        }
      }
      if (const auto cpu = cpu_.sample()) fields[service ? "serviceCpuPct10" : "workerCpuPct10"] = *cpu;
      if (readback) { fields["readbackUs"] = readback->first; fields["readbackMaxUs"] = readback->second; }
      if (queue) {
        fields["hostQueuePackets"] = queue->packets; fields["hostQueueBytes"] = queue->bytes;
        fields["hostQueueOldestUs"] = queue->oldestUs;
        if (queue->writeUs) fields["hostWriteUs"] = *queue->writeUs;
      }
      typename JsonValue::Object metrics;
      for (const auto& [key, number] : fields) if (const auto maximum = hostObservationMaximum(key); maximum && number <= *maximum) {
        JsonValue item; item.value = double(number); metrics.emplace(key, std::move(item));
      }
      JsonValue observation, protocol, payload;
      protocol.value = std::string(streamObservabilityProtocol); payload.value = std::move(metrics);
      typename JsonValue::Object snapshot; snapshot.emplace("protocol", std::move(protocol)); snapshot.emplace("metrics", std::move(payload));
      observation.value = std::move(snapshot); (*object)["observability"] = std::move(observation);
      JsonValue sessionId; sessionId.value = session_; (*object)["sessionId"] = std::move(sessionId);
      if (!tracks_ && !observedEpoch) { JsonValue epoch; epoch.value = double(epoch_); (*object)["epoch"] = std::move(epoch); }
      return serialize(value);
    } catch (...) { return raw; }
  }
 private:
  mutable std::mutex mutex_; std::atomic<bool> enabled_{false}; bool tracks_ = false; uint16_t epoch_ = 0;
  std::string session_; ProcessCpuObservation cpu_;
};
}
