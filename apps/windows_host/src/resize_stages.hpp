#pragma once
// Where one viewport resize spends its time on the Host (tuning backlog #1,
// measured: client request -> first new frame p50 780 ms, p90 1.95 s, with no
// stage breakdown). Fixed millisecond fields only, diagnostics: nothing decides
// on them. The worker's resize handler measures the stages before the restart
// and hands them to the capture thread inside the restart command
// ("resizeStages"); the capture thread adds its own and reports them all in the
// resize's stream.info, which the relay forwards unchanged and the client adds
// to its viewportResult record.
#include "support.hpp"
#include <algorithm>
#include <chrono>
#include <string>

namespace host {
struct ResizeStages {
  // Worker, before the restart: the old capture/encoder stopped; the display
  // manager v2 change as a whole and, inside it, its SDK size call, Windows
  // settling after it (target arrival, apply, verification) and the capture
  // proof (confirm rounds).
  uint32_t suspendMs = 0, displayMs = 0, sdkMs = 0, settleMs = 0, confirmMs = 0;
  // Capture thread: restart command -> stream.info, and inside it the capture,
  // encoder and audio waits.
  uint32_t startMs = 0, captureMs = 0, encoderMs = 0, audioMs = 0;
};
inline constexpr uint32_t resizeStageMaxMs = 600000;
inline uint32_t resizeStageMs(std::chrono::steady_clock::duration elapsed) {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  return uint32_t(std::clamp<long long>(ms, 0, resizeStageMaxMs));
}
inline uint32_t resizeStageMs(uint64_t ms) { return uint32_t(std::min<uint64_t>(ms, resizeStageMaxMs)); }
inline uint32_t resizeStageMs(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) { return resizeStageMs(to - from); }

namespace detail {
template <class Each> void eachResizeStage(ResizeStages& stages, Each&& each) {
  each("suspendMs", stages.suspendMs); each("displayMs", stages.displayMs); each("sdkMs", stages.sdkMs);
  each("settleMs", stages.settleMs); each("confirmMs", stages.confirmMs); each("startMs", stages.startMs);
  each("captureMs", stages.captureMs); each("encoderMs", stages.encoderMs); each("audioMs", stages.audioMs);
}
}  // namespace detail

inline std::string resizeStagesJson(ResizeStages stages) {
  std::string out = "{";
  detail::eachResizeStage(stages, [&](const char* key, uint32_t value) { if (out.size() > 1) out += ','; out += '"'; out += key; out += "\":"; out += std::to_string(value); });
  return out + '}';
}
// The worker's stages from a restart command; anything missing or invalid is 0.
inline ResizeStages resizeStagesFrom(const Json& command) {
  ResizeStages stages;
  const auto& field = command.get("resizeStages");
  detail::eachResizeStage(stages, [&](const char* key, uint32_t& value) {
    const double n = field.get(key).number(-1);
    value = n >= 0 && n <= resizeStageMaxMs && n == double(uint32_t(n)) ? uint32_t(n) : 0;
  });
  return stages;
}
// A copy of the restart command carrying the worker's stages. The stored
// stream command stays without them, so no later restart reports stale ones.
inline Json withResizeStages(Json command, const ResizeStages& stages) {
  if (auto* object = std::get_if<Json::Object>(&command.value)) (*object)["resizeStages"] = JsonParser::parse(resizeStagesJson(stages));
  return command;
}
}  // namespace host
