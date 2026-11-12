#pragma once
// System input on independent outputs: the worker's part (system_input.hpp has
// the service's). The service keeps the client's event and asks the worker to
// place it; the worker's track plan admits the input exactly as it would before
// injecting it (session, topology, the output and its epoch, the client's
// acknowledgement of the plan, the event's own fields) and answers with the
// desktop pixel of its position, or with a refusal. The worker injects nothing
// and supplies nothing but that position.
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "system_input.hpp"
#include "track_input.hpp"

namespace host::sysinput {

// `asked`: the input as the service forwarded it (its marker and sequence
// number are removed here). `send` gets exactly one answer; for an admitted
// input it is called under the plan's lock, so the answer is ordered with every
// change of the outputs. true: the input was admitted.
inline bool answerTrackInput(TrackSourceInput* plan, const Json& asked, uint32_t seq, const std::string& session,
                             const std::function<void(const std::string&)>& send) {
  auto input = asked;
  if (auto* fields = std::get_if<Json::Object>(&input.value)) { fields->erase(workerMarker); fields->erase(workerSeq); }
  const bool placed = plan && plan->applyPlaced(input, [&](const Json&, const std::optional<POINT>& desktop) {
    send(trackAnswerJson(session, seq, true, desktop ? std::optional<std::pair<int32_t, int32_t>>{std::pair<int32_t, int32_t>{int32_t(desktop->x), int32_t(desktop->y)}} : std::nullopt));
  });
  if (!placed) send(trackAnswerJson(session, seq, false, std::nullopt));
  return placed;
}

}  // namespace host::sysinput
