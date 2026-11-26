// A session worker's stream start that fails in its own checks, before its capture thread runs (agent.hpp, stream):
// it is recorded (stage streamStartCheckStage) and told to the client like a capture that failed, and it does not
// reach the worker's command loop as an exception. On the actual worker Agent path; no capture, encoder or display
// change is started (the commands here fail before any of them).
#include "agent.hpp"
#include <iostream>
#include <mutex>
using namespace host;
namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
struct Seen { std::vector<std::string> sent; std::vector<std::pair<uint32_t, uint32_t>> records; bool threw = false; };
Seen start(const std::string& command) {
  Seen seen; std::mutex mutex;
  {
    Agent worker([](const std::wstring&) {}, nullptr, [&](bool text, std::vector<uint8_t> bytes) { std::lock_guard lock(mutex); if (text) seen.sent.emplace_back(bytes.begin(), bytes.end()); });
    worker.captureDiagnostic([&](uint32_t stage, uint32_t error) { std::lock_guard lock(mutex); seen.records.push_back({stage, error}); });
    try { worker.localControl(command); } catch (...) { seen.threw = true; }
    // The worker is stopped in order afterwards: its loop still takes commands.
    if (!seen.threw) worker.localControl("{\"type\":\"session.stop\",\"sessionId\":\"s-1\"}");
  }
  return seen;
}
const std::string geometry = ",\"width\":1280,\"height\":720,\"captureLeft\":0,\"captureTop\":0,\"captureWidth\":1280,\"captureHeight\":720";

void resizeRestart() {
  // A resize restart whose command carries no capture geometry.
  const auto seen = start("{\"type\":\"session.start\",\"sessionId\":\"s-1\",\"resizeProtocol\":\"epoch-v1\",\"epoch\":3,\"requestId\":\"r\",\"resizeId\":\"z\"}");
  require(!seen.threw, "The failed start reached the worker's command loop as an exception");
  require(seen.sent.size() == 1 && seen.sent[0] == "{\"type\":\"stream.resize.failed\",\"reason\":\"resize_failed\",\"sessionId\":\"s-1\",\"epoch\":3,\"requestId\":\"r\",\"resizeId\":\"z\"}", "The resize was not reported as failed");
  require(seen.records.size() == 1 && seen.records[0].first == streamStartCheckStage && seen.records[0].second == 21, "The failed check was not recorded");
}
void monitorsChanged() {
  // The monitor the resize bound is not the one Windows lists when the stream starts (it re-laid its displays late).
  const auto seen = start("{\"type\":\"session.start\",\"sessionId\":\"s-1\",\"resizeProtocol\":\"epoch-v1\",\"epoch\":4,\"requestId\":\"r\",\"resizeId\":\"y\"" + geometry +
                          ",\"captureDisplays\":[{\"name\":\"\\\\\\\\.\\\\DISPLAY250\",\"identity\":\"MONITOR\\\\NONE|none\",\"left\":0,\"top\":0,\"right\":1280,\"bottom\":720}]}");
  require(!seen.threw && seen.sent.size() == 1 && seen.sent[0].find("\"type\":\"stream.resize.failed\"") != std::string::npos && seen.sent[0].find("\"resizeId\":\"y\"") != std::string::npos, "A changed monitor did not fail the resize");
  // 14: the selected monitor changed. 16: this session has no monitor to list at all.
  require(seen.records.size() == 1 && seen.records[0].first == streamStartCheckStage && (seen.records[0].second == 14 || seen.records[0].second == 16), "The monitor check was not recorded with its code");
}
void firstStart() {
  const auto seen = start("{\"type\":\"session.start\",\"sessionId\":\"s-1\"}");
  require(!seen.threw && seen.sent.size() == 1, "A failed first start said nothing");
  const auto notice = JsonParser::parse(seen.sent[0]);
  require(notice.get("type").string() == "stream.unavailable" && notice.get("sessionId").string() == "s-1", "A failed first start was not reported as an unavailable stream");
  require(notice.get("reason").string() == "capture_failed" || notice.get("reason").string() == "no_interactive_session", "The reason is not one a client knows");
  require(seen.records.size() == 1 && seen.records[0].second == 21, "The failed check was not recorded");
}
void unnamedSession() {
  // No notice can name this session: the exception stays the caller's, as before.
  const auto seen = start("{\"type\":\"session.start\",\"sessionId\":\"not a session\"}");
  require(seen.threw && seen.sent.empty() && seen.records.empty(), "A start without a usable session ID was answered");
}
}

int main() {
  const struct { const char* name; void (*run)(); } cases[] = {{"resize-restart", resizeRestart}, {"monitors-changed", monitorsChanged}, {"first-start", firstStart}, {"unnamed-session", unnamedSession}};
  unsigned passed = 0, failed = 0;
  for (const auto& value : cases) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
  std::cout << "{\"kind\":\"stream-start-failure\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << ",\"captureStarted\":false,\"displayChanged\":false}\n";
  return failed ? 1 : 0;
}
