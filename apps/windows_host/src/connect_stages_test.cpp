// connect-timing-v1 (connect_stages.hpp): the stages a stream start reports,
// their stream.info member and their Host log fields.
#include "connect_stages.hpp"
#include "operational_log.hpp"
#include <iostream>
#include <stdexcept>
using namespace host;
namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
using std::chrono::milliseconds;

void stagesFromOneClock() {
  ConnectStages stages;
  const auto t0 = ConnectStages::Clock::now();
  stages.received = t0; stages.admitted = t0 + milliseconds(3); stages.worker = t0 + milliseconds(143);
  stages.dispatched = t0 + milliseconds(363); stages.info = t0 + milliseconds(458); stages.frame = t0 + milliseconds(500);
  require(stages.admitMs() == 3u && stages.workerMs() == 140u && stages.displayMs() == 220u && stages.mediaMs() == 95u && stages.frameMs() == 42u, "Stage arithmetic is wrong");
  require(stages.json() == R"({"admitMs":3,"workerMs":140,"displayMs":220,"mediaMs":95})", "stream.info carries exactly the four service stages");
  // Independent outputs announce no stream.info: their first frame counts from the dispatch.
  stages.info.reset();
  require(stages.frameMs() == 137u && !stages.mediaMs() && stages.json() == R"({"admitMs":3,"workerMs":140,"displayMs":220})", "A missing end leaves its stage out");
  require(ConnectStages::logged(stages.mediaMs()) < 0 && ConnectStages::logged(stages.frameMs()) == 137, "A missing stage must be left out of the log record");
  ConnectStages late; late.received = t0 + milliseconds(10); late.admitted = t0;
  require(!late.admitMs(), "A reversed interval is not a stage");
  ConnectStages hung; hung.received = t0; hung.admitted = t0 + std::chrono::hours(2);
  require(hung.admitMs() == ConnectStages::maxMs, "A stage is capped at its maximum");
}

void startInfoMember() {
  ConnectStages stages; const auto t0 = ConnectStages::Clock::now();
  stages.received = t0; stages.admitted = t0 + milliseconds(1); stages.worker = t0 + milliseconds(2); stages.dispatched = t0 + milliseconds(4); stages.info = t0 + milliseconds(8);
  const std::string info = R"({"type":"stream.info","codec":"h264","width":1280,"height":720,"audio":{"sampleRate":48000,"channels":2,"format":"s16le","available":true},"sessionId":"s","epoch":1})";
  const auto out = withStartStages(std::vector<uint8_t>(info.begin(), info.end()), stages);
  const std::string text(out.begin(), out.end());
  const auto parsed = JsonParser::parse(text);
  require(parsed.get("type").string() == "stream.info" && parsed.get("width").number() == 1280 && parsed.get("audio").get("channels").number() == 2 && parsed.get("epoch").number() == 1, "The worker's own fields must stay");
  require(parsed.get("startStages").get("admitMs").number() == 1 && parsed.get("startStages").get("workerMs").number() == 1 && parsed.get("startStages").get("displayMs").number() == 2 && parsed.get("startStages").get("mediaMs").number() == 4, "startStages missing from the start's stream.info");
  require(text.rfind(info.substr(0, info.size() - 1), 0) == 0, "Only a last member is appended; nothing else is rewritten");
  const std::string spaced = "{\"type\":\"stream.info\"} \r\n";
  const auto trimmed = withStartStages({spaced.begin(), spaced.end()}, stages);
  require(JsonParser::parse(std::string(trimmed.begin(), trimmed.end())).get("startStages").get("mediaMs").number() == 4, "Trailing whitespace");
  const std::string empty = "{}";
  const auto filled = withStartStages({empty.begin(), empty.end()}, stages);
  require(JsonParser::parse(std::string(filled.begin(), filled.end())).get("startStages").get("admitMs").number() == 1, "An empty object gets no leading comma");
  for (const std::string other : {std::string("[1,2]"), std::string(""), std::string("{\"a\":1")}) {
    const auto unchanged = withStartStages({other.begin(), other.end()}, stages);
    require(std::string(unchanged.begin(), unchanged.end()) == other, "Anything but one object stays unchanged");
  }
}

void hostLogKeepsStages() {
  for (const char* name : {"admitMs", "workerMs", "displayMs", "mediaMs", "frameMs"})
    require(hostLogField(name) && !hostLogBoolean(name), "A stage is not a Host log number");
  const auto record = safeHostLogRecord(JsonParser::parse(R"({"time":"2026-11-02T23:00:00.000Z","event":"hostAction","values":{"action":200,"ok":true,"stage":10,"error":0,"admitMs":3,"workerMs":140,"displayMs":220,"mediaMs":95,"frameMs":42,"secret":"x"}})"));
  const auto parsed = JsonParser::parse(record);
  require(parsed.get("values").get("workerMs").number() == 140 && parsed.get("values").get("frameMs").number() == 42 && parsed.get("values").get("stage").number() == 10, "The stage 10 record lost its stages");
  require(record.find("secret") == std::string::npos, "Only listed fields are kept");
}
}  // namespace
int main() {
  try {
    stagesFromOneClock(); startInfoMember(); hostLogKeepsStages();
    std::cout << "connect stages: 3 groups passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
