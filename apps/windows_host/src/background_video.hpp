#pragma once
// background-video-v1 (docs/remote-session-protocol.md "Background video"): while the client's picture cannot be
// seen (the Android app in the background, the Windows app minimised) it sends {"type":"stream.background","on":true}
// and the worker captures and encodes one picture a second instead of every frame, as for an idle screen (whose
// refreshes keep every watchdog of the stream satisfied). Sound, input, the cursor channel and the side channels
// go on. {"on":false} ends it at the next tick of the capture loop (at most `idleWait` later) with an IDR.
// Ordinary stream sessions only; independent outputs keep every frame. Standard C++ only.
#include <chrono>
#include <optional>
#include <string>
#include <variant>

namespace host {
inline constexpr const char* backgroundVideoProtocol = "background-video-v1";

// An ordinary stream session whose broker granted it.
template<class JsonValue> bool backgroundVideoGranted(const JsonValue& start) {
  return start.get("mode").string() == "stream" && start.get("backgroundVideoProtocol").string() == backgroundVideoProtocol;
}
// {"type":"stream.background","on":bool[,"sessionId":...]}; nothing else.
template<class JsonValue> std::optional<bool> backgroundRequest(const JsonValue& message) {
  const auto* fields = std::get_if<typename JsonValue::Object>(&message.value);
  if (!fields || message.get("type").string() != "stream.background") return std::nullopt;
  for (const auto& [key, item] : *fields) { (void)item; if (key != "type" && key != "on" && key != "sessionId") return std::nullopt; }
  const auto* on = std::get_if<bool>(&message.get("on").value);
  if (!on) return std::nullopt;
  return *on;
}

// The capture loop's gate. Shown: every tick captures. Hidden: one tick a second does; the others wait `idleWait`
// and ask again, so coming back is seen within that.
class BackgroundPace {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::seconds interval{1};
  static constexpr std::chrono::milliseconds idleWait{50};
  bool due(bool hidden, Clock::time_point now) {
    if (!hidden) { last_.reset(); return true; }
    if (last_ && now - *last_ < interval) return false;
    last_ = now;
    return true;
  }
 private:
  std::optional<Clock::time_point> last_;
};
}  // namespace host
