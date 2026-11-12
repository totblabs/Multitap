#pragma once
// connect-timing-v1 (measured before it: client stream start ->
// first picture p50 0.95 s, p90 2.0 s, with no breakdown). Where the service
// spends a new stream's start, on one steady clock:
//   admitMs   session.start received -> its stream thread runs (the previous
//             stream's stop, a running display probe's end, admission)
//   workerMs  -> the interactive worker launched and bound (tracks: after
//             their tunnel)
//   displayMs -> the start handed to the worker (display preparation)
//   mediaMs   -> the worker's first stream.info (capture, encoder, audio)
//   frameMs   -> the first video packet relayed (Host log only)
// Fixed millisecond fields, diagnostics only: nothing decides on them. The
// first stream.info carries the first four as "startStages" (the relay
// forwards it unchanged; the client adds them to its streamReady record) and
// the first video packet's hostAction 200 stage 10 record carries all five.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace host {
struct ConnectStages {
  using Clock = std::chrono::steady_clock;
  static constexpr uint32_t maxMs = 600000;
  std::optional<Clock::time_point> received, admitted, worker, dispatched, info, frame;
  static std::optional<uint32_t> between(const std::optional<Clock::time_point>& from, const std::optional<Clock::time_point>& to) {
    if (!from || !to || *to < *from) return std::nullopt;
    return uint32_t(std::min<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(*to - *from).count(), maxMs));
  }
  std::optional<uint32_t> admitMs() const { return between(received, admitted); }
  std::optional<uint32_t> workerMs() const { return between(admitted, worker); }
  std::optional<uint32_t> displayMs() const { return between(worker, dispatched); }
  std::optional<uint32_t> mediaMs() const { return between(dispatched, info); }
  // Independent outputs announce no stream.info: from the start's dispatch.
  std::optional<uint32_t> frameMs() const { return between(info ? info : dispatched, frame); }
  // For recordHostEvent, which leaves out a negative value.
  static int64_t logged(std::optional<uint32_t> ms) { return ms ? int64_t(*ms) : -1; }
  // name -> milliseconds; a stage whose ends were not both seen is left out.
  std::vector<std::pair<const char*, uint32_t>> fields() const {
    std::vector<std::pair<const char*, uint32_t>> out;
    const auto add = [&](const char* name, std::optional<uint32_t> ms) { if (ms) out.emplace_back(name, *ms); };
    add("admitMs", admitMs()); add("workerMs", workerMs()); add("displayMs", displayMs()); add("mediaMs", mediaMs());
    return out;
  }
  std::string json() const {
    std::string out = "{";
    for (const auto& [name, ms] : fields()) { if (out.size() > 1) out += ','; out += '"'; out += name; out += "\":"; out += std::to_string(ms); }
    return out + '}';
  }
};

// The worker's stream.info text with "startStages" added as its last member;
// returned unchanged unless it is one JSON object (validated by the caller's
// parse) ending in '}'.
inline std::vector<uint8_t> withStartStages(std::vector<uint8_t> info, const ConnectStages& stages) {
  size_t end = info.size();
  while (end > 0 && (info[end - 1] == ' ' || info[end - 1] == '\n' || info[end - 1] == '\r' || info[end - 1] == '\t')) --end;
  if (end < 2 || info.front() != '{' || info[end - 1] != '}') return info;
  bool empty = true;
  for (size_t i = 1; i + 1 < end && empty; ++i) empty = info[i] == ' ' || info[i] == '\n' || info[i] == '\r' || info[i] == '\t';
  const std::string member = std::string(empty ? "" : ",") + "\"startStages\":" + stages.json();
  info.insert(info.begin() + std::ptrdiff_t(end - 1), member.begin(), member.end());
  return info;
}
}  // namespace host
