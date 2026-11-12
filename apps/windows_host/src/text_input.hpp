#pragma once
// text-input-v1 (docs/remote-session-protocol.md "Text input"): committed text
// from a client's on-screen keyboard or IME, e.g. Korean from a phone. It is
// injected as Unicode key events (KEYEVENTF_UNICODE, one down/up pair per
// UTF-16 code unit; a surrogate pair is two units), so the result does not
// depend on the remote keyboard layout or on the remote IME being in Hangul
// mode. Enter, Tab, Backspace and editing keys stay ordinary `key` events.
#include "support.hpp"
#include <optional>
#include <vector>

namespace host {
inline constexpr const char* textInputProtocol = "text-input-v1";
// UTF-16 code units per message; the client splits longer commits.
inline constexpr size_t textInputMaxUnits = 256;

// session.start of an ordinary stream session whose broker granted it
// (independent outputs keep keys only).
inline bool textInputSessionRequested(const Json& start) {
  return start.get("type").string() == "session.start" && start.get("mode").string() == "stream" &&
         start.get("textInputProtocol").string() == textInputProtocol && start.get("mediaProtocol").string().empty();
}

// The UTF-16 units of one valid {"type":"input","event":"text","text":...}
// message (an optional matching sessionId is the only other field). Empty,
// over-long, malformed UTF-8, lone surrogates and control characters (C0,
// DEL, C1) are refused as a whole: nothing of them is injected.
inline std::optional<std::wstring> textInputUnits(const Json& message) {
  const auto* fields = std::get_if<Json::Object>(&message.value);
  if (!fields || message.get("type").string() != "input" || message.get("event").string() != "text") return {};
  for (const auto& [key, item] : *fields) {
    (void)item;
    if (key != "type" && key != "event" && key != "text" && key != "sessionId") return {};
  }
  const auto* text = std::get_if<std::string>(&message.get("text").value);
  if (!text || text->empty() || text->size() > textInputMaxUnits * 3) return {};
  std::wstring units;
  try { units = wide(*text); } catch (...) { return {}; }
  if (units.empty() || units.size() > textInputMaxUnits) return {};
  for (size_t at = 0; at < units.size(); ++at) {
    const auto unit = static_cast<unsigned>(units[at]);
    if (unit < 0x20 || (unit >= 0x7f && unit <= 0x9f)) return {};
    if (unit >= 0xdc00 && unit <= 0xdfff) return {};
    if (unit >= 0xd800 && unit <= 0xdbff) {
      if (at + 1 >= units.size()) return {};
      const auto low = static_cast<unsigned>(units[at + 1]);
      if (low < 0xdc00 || low > 0xdfff) return {};
      ++at;
    }
  }
  return units;
}

// Down then up for every unit, in order. The injector must retry a failed up.
inline std::vector<INPUT> textInputEvents(const std::wstring& units) {
  std::vector<INPUT> events;
  events.reserve(units.size() * 2);
  for (const wchar_t unit : units) {
    for (const bool up : {false, true}) {
      INPUT input{};
      input.type = INPUT_KEYBOARD;
      input.ki.wVk = 0;
      input.ki.wScan = WORD(unit);
      input.ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
      events.push_back(input);
    }
  }
  return events;
}
}  // namespace host
