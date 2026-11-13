#pragma once
// settings-progress-v1: what a long settings request is doing, while it does it.
//
// The settings RPC (settings_rpc.hpp) is one short elevated process per request:
// Host settings writes one JSON request to its stdin and reads one JSON reply
// from its stdout when it ends. A request that installs something can take
// minutes, and until now the window had nothing to show for all of them.
//
// When Host settings asks for it (MULTITAP_SETTINGS_PROGRESS=1 in the
// helper's environment; an older window never sets it and reads one line as
// before) the helper writes one line per step BEFORE the reply:
//   {"progress":{"step":"winfsp_install"}}
//   {"progress":{"step":"winfsp_install","done":1840,"total":5200}}
// `step` is a fixed code (lower-case letters, digits, "_"): the window has the
// sentence for it in its own language. `done`/`total` are present only when the
// work has a real measure (Windows Installer's own progress count); a step
// without them has no known length and the window must not pretend it has.
// The reply is always the last line and is unchanged.
//
// Nothing here decides anything: a sink that is not set, or that fails, changes
// no result. No path, name, account or other machine detail is ever written.
#include <functional>
#include <string>

namespace host::settings_progress {
inline bool validStep(const std::string& step) {
  if (step.empty() || step.size() > 48) return false;
  for (const char ch : step) if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
  return true;
}
// One line without its newline, or "" for a step that is not a fixed code.
// A measure that is not one (nothing to do, or more done than there is) is left out.
inline std::string line(const std::string& step, long long done = -1, long long total = -1) {
  if (!validStep(step)) return {};
  std::string out = "{\"progress\":{\"step\":\"" + step + "\"";
  if (total > 0 && done >= 0 && done <= total) out += ",\"done\":" + std::to_string(done) + ",\"total\":" + std::to_string(total);
  return out + "}}";
}
using Sink = std::function<void(const std::string& line)>;
inline Sink& sink() { static Sink value; return value; }
inline void emit(const std::string& text) noexcept {
  if (text.empty()) return;
  try { if (sink()) sink()(text); } catch (...) {}
}
// The step that starts now (its length is not known).
inline void step(const char* code) noexcept { try { emit(line(code ? code : "")); } catch (...) {} }
// How far the step is, by its own measure.
inline void measure(const char* code, long long done, long long total) noexcept { try { emit(line(code ? code : "", done, total)); } catch (...) {} }
}  // namespace host::settings_progress
