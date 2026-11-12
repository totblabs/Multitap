// input-latency-v1 on the host: the pure frame tagger and session math, the
// actual worker Agent control/injection path (substituted injector, no OS
// input, no capture) and the actual Session0 Agent publication path (injected
// bridge, recorded sink). No network, display or VM is involved.
#include "agent.hpp"
#include <iostream>
using namespace host;
namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
const std::string session = "00000000-0000-4000-8000-000000000001";
std::string start(bool marker, const std::string& extra = "") {
  return "{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"" + session + "\",\"resizeProtocol\":\"epoch-v1\",\"epoch\":0,\"monitor\":\"0\"" +
         (marker ? ",\"inputLatencyProtocol\":\"input-latency-v1\"" : "") + extra + '}';
}
std::set<std::string> keys(const Json& value) { std::set<std::string> out; for (const auto& [key, item] : std::get<Json::Object>(value.value)) { (void)item; out.insert(key); } return out; }

void taggerFirstChangedFrameAfterInjection() {
  InputFrameTagger tagger;
  tagger.beginCapture(1000); tagger.submitted(1000, true);
  require(!tagger.output(1000), "A frame without a preceding injection was tagged");
  // The capture that began before the injection cannot carry it.
  tagger.beginCapture(2000); tagger.injected(7, 2100); tagger.submitted(2000, true);
  require(!tagger.output(2000), "A frame captured before the injection was tagged");
  // Unchanged frames (idle ticks, refreshes, refinement) keep it waiting.
  tagger.beginCapture(3000); tagger.submitted(3000, false); require(!tagger.output(3000), "An unchanged frame was tagged");
  tagger.beginCapture(4000); tagger.submitted(4000, true);
  const auto tag = tagger.output(4000);
  require(tag && tag->sample == 7 && tag->pts == 4000 && tagger.tagged() == 1, "The first changed frame after the injection was not tagged");
  require(!tagger.output(5000), "One injection tagged two frames");
}
void taggerLatestAndBounds() {
  InputFrameTagger tagger;
  tagger.injected(1, 10); tagger.injected(2, 20);
  tagger.beginCapture(30); tagger.submitted(30, true);
  auto tag = tagger.output(30); require(tag && tag->sample == 2 && tagger.superseded() == 1, "The latest injected input did not name the frame");
  // A candidate older than the window without a change: no visible frame.
  tagger.injected(3, 100); tagger.beginCapture(110);
  tagger.beginCapture(110 + inputLatencyFrameWindowUs + 1); tagger.submitted(120 + inputLatencyFrameWindowUs, true);
  require(!tagger.output(120 + inputLatencyFrameWindowUs) && tagger.unchanged() == 1, "An input without a visible change was tagged after the window");
  // The encoder dropped the tagged input: its next later output carries the change.
  tagger.injected(4, 5000000); tagger.beginCapture(5000010); tagger.submitted(5000010, true);
  require(!tagger.output(5000000), "An earlier output claimed a later frame's tag");
  tag = tagger.output(5000050); require(tag && tag->sample == 4 && tag->pts == 5000010, "A dropped tagged frame lost its tag");
  // Assigned tags awaiting encoder output are bounded.
  for (uint32_t n = 0; n < 10; ++n) { tagger.injected(10 + n, 6000000 + n * 10); tagger.beginCapture(6000001 + n * 10); tagger.submitted(6000001 + n * 10, true); }
  tag = tagger.output(7000000); require(tag && tag->sample == 19, "A burst lost the newest tag");
  require(!tagger.output(8000000), "The bounded tag queue retained stale tags");
}
void taggerHostTiming() {
  InputFrameTagger tagger;
  // Injection 1000 -> capture tick 1500 (changed) -> encoder output 4000.
  tagger.injected(7, 1000); tagger.beginCapture(1500); tagger.submitted(1500, true);
  require(tagger.output(1500, 4000) && tagger.timings().size() == 1, "A tagged output recorded no timing");
  require(tagger.timings().back().injectToCaptureUs == 500 && tagger.timings().back().captureToEncodedUs == 2500, "Host timing split is wrong");
  // The tick that captures the change counts, not an earlier unchanged one.
  tagger.injected(8, 10000); tagger.beginCapture(12000); tagger.submitted(12000, false); tagger.beginCapture(28000); tagger.submitted(28000, true);
  require(tagger.output(28000, 30000) && tagger.timings().back().injectToCaptureUs == 18000 && tagger.timings().back().captureToEncodedUs == 2000, "Waiting ticks were misattributed");
  // An output without a time (tests, callers without a clock) records nothing.
  tagger.injected(9, 40000); tagger.beginCapture(41000); tagger.submitted(41000, true);
  require(tagger.output(41000) && tagger.timings().size() == 2, "An unknown output time produced a timing");
  for (uint32_t n = 0; n < 100; ++n) { tagger.injected(100 + n, 100000 + n * 1000); tagger.beginCapture(100100 + n * 1000); tagger.submitted(100100 + n * 1000, true); (void)tagger.output(100100 + n * 1000, 100200 + n * 1000); }
  require(tagger.timings().size() == InputFrameTagger::timingLimit, "Timings are not bounded");
  // Fixed fields, nearest-rank percentiles, samples older than 30 s left out.
  std::deque<InputFrameTagger::Timing> timings{{20000, 3000, 1000000}, {10000, 1000, 1000000}, {30000, 5000, 1000000}, {90000, 9000, 1}};
  const auto json = inputFrameTimingJson(timings, 31000000);
  require(json == ",\"inputFrame\":{\"samples\":3,\"injectToCaptureP50Ms\":20.00,\"injectToCaptureP95Ms\":30.00,\"captureToEncodedP50Ms\":3.00,\"captureToEncodedP95Ms\":5.00,\"expired\":0,\"ambient\":0}", "Timing report fields differ");
  // Inputs without a tagged frame (last 30 s only), also when nothing was tagged.
  std::deque<InputFrameTagger::MissEvent> misses{{InputFrameTagger::Miss::expired, 1000000}, {InputFrameTagger::Miss::ambient, 1000000}, {InputFrameTagger::Miss::ambient, 1000000}, {InputFrameTagger::Miss::expired, 1}};
  require(inputFrameTimingJson(timings, 31000000, misses).ends_with(",\"expired\":1,\"ambient\":2}"), "Miss counts are not reported with the percentiles");
  require(inputFrameTimingJson({}, 31000000, misses) == ",\"inputFrame\":{\"samples\":0,\"expired\":1,\"ambient\":2}", "Misses alone are not reported");
  require(inputFrameTimingJson({}, 99000000, misses).empty(), "Stale misses were reported");
  require(inputFrameTimingJson(timings, 99000000).empty(), "Stale timings were reported");
  InputLatencySession unnegotiated; require(unnegotiated.frameTimingJson(1).empty(), "An unnegotiated session reported timings");
}
// Which injected inputs may name a frame, and which unsampled ones drop a
// pending candidate (the next change may be theirs).
void candidateRules() {
  const auto is = [](const char* input, bool separated, bool held) { return inputFrameCandidate(JsonParser::parse(input), separated, held); };
  require(is(R"({"type":"input","event":"key","vk":65,"scan":30,"down":true})", true, false), "A key press is not a candidate");
  require(!is(R"({"type":"input","event":"key","vk":65,"scan":30,"down":false})", true, false), "A key release named a frame");
  for (const int vk : {0x10, 0x11, 0x12, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0x5B, 0x5C}) {
    const auto press = "{\"type\":\"input\",\"event\":\"key\",\"vk\":" + std::to_string(vk) + ",\"scan\":42,\"down\":true}";
    require(!inputFrameCandidate(JsonParser::parse(press), false, false), "A modifier press named a frame");
  }
  // IME mode keys change the visible input indicator; Enter, Backspace and F-keys change content.
  for (const int vk : {0x15, 0x19, 0x0D, 0x08, 0x70, 0x20}) {
    const auto press = "{\"type\":\"input\",\"event\":\"key\",\"vk\":" + std::to_string(vk) + ",\"scan\":28,\"down\":true}";
    require(inputFrameCandidate(JsonParser::parse(press), true, false), "A content key press is not a candidate");
  }
  for (const bool separated : {false, true}) {
    require(is(R"({"type":"input","event":"button","button":"left","down":true,"x":0.5,"y":0.5})", separated, false), "A button press is not a candidate");
    require(is(R"({"type":"input","event":"button","button":"left","down":false,"x":0.5,"y":0.5})", separated, true), "A button release is not a candidate");
    require(is(R"({"type":"input","event":"wheel","delta":-120,"x":0.5,"y":0.5})", separated, false), "A wheel step is not a candidate");
  }
  // A move changes the frame when the cursor is composed, or while a button is held (a drag).
  const char* move = R"({"type":"input","event":"move","x":0.5,"y":0.5})";
  require(is(move, false, false) && is(move, false, true), "A composed-cursor move is not a candidate");
  require(!is(move, true, false), "A cursor-v1 hover move named a frame");
  require(is(move, true, true), "A cursor-v1 drag move is not a candidate");
  require(!is(R"({"type":"input","event":"text","text":"a"})", false, false), "Committed text became a candidate");
  for (const auto* interrupting : {R"({"type":"input","event":"button","button":"left","down":false,"x":0.5,"y":0.5})", R"({"type":"input","event":"text","text":"a"})"})
    require(inputFrameInterrupts(JsonParser::parse(interrupting)), "A button change or committed text did not interrupt");
  for (const auto* quiet : {R"({"type":"input","event":"key","vk":66,"scan":48,"down":true})", R"({"type":"input","event":"wheel","delta":-120,"x":0.5,"y":0.5})", move})
    require(!inputFrameInterrupts(JsonParser::parse(quiet)), "A key press, wheel step or move interrupted");
}
// An input that changes nothing must not be credited with the next unrelated
// change, whether it is far away (window) or a beat of a slow rhythm (caret blink).
void taggerNoVisibleChange() {
  constexpr uint64_t ms = 1000;
  InputFrameTagger tagger;
  tagger.injected(1, 100 * ms); tagger.beginCapture(110 * ms); tagger.submitted(110 * ms, false);
  tagger.beginCapture(100 * ms + inputLatencyFrameWindowUs + ms); tagger.submitted(100 * ms + inputLatencyFrameWindowUs + ms, true);
  require(!tagger.output(100 * ms + inputLatencyFrameWindowUs + ms) && tagger.misses().size() == 1 && tagger.misses().back().kind == InputFrameTagger::Miss::expired, "An input with no change inside the window was tagged or not counted");
  require(inputLatencyFrameWindowUs <= 500000, "The attribution window is wider than 500 ms");
  // A caret that toggles every 530 ms while nobody injects; then a key that does nothing.
  InputFrameTagger blink;
  uint64_t at = 10000 * ms;
  for (int beat = 0; beat < 5; ++beat, at += 530 * ms) { blink.beginCapture(at); blink.submitted(at, true); }
  blink.activity(at - 100 * ms); blink.injected(2, at - 100 * ms);  // the next beat is 100 ms later
  blink.beginCapture(at); blink.submitted(at, true);
  require(!blink.output(at) && blink.misses().size() == 1 && blink.misses().back().kind == InputFrameTagger::Miss::ambient, "The next caret beat was credited to an input");
  // The real reaction after it is still tagged.
  blink.beginCapture(at + 200 * ms); blink.submitted(at + 200 * ms, true);
  const auto tag = blink.output(at + 200 * ms);
  require(tag && tag->sample == 2, "A reaction between two beats was not tagged");
  // The previous behaviour (no filter) credits the beat; the option exists for comparison only.
  InputFrameTagger plain({inputLatencyFrameWindowUs, false});
  at = 10000 * ms; for (int beat = 0; beat < 5; ++beat, at += 530 * ms) { plain.beginCapture(at); plain.submitted(at, true); }
  plain.injected(3, at - 100 * ms); plain.beginCapture(at); plain.submitted(at, true);
  require(plain.output(at).has_value(), "The unfiltered tagger did not take the beat");
  // Typing at a steady pace is a rhythm, but it is never input-free: no ambient evidence.
  InputFrameTagger typing; at = 20000 * ms;
  for (int key = 0; key < 8; ++key, at += 250 * ms) { typing.activity(at); typing.beginCapture(at + 30 * ms); typing.submitted(at + 30 * ms, true); }
  typing.injected(4, at); typing.activity(at); typing.beginCapture(at + 30 * ms); typing.submitted(at + 30 * ms, true);
  require(typing.output(at + 30 * ms).has_value() && typing.misses().empty(), "A typing rhythm was mistaken for an ambient one");
  // Irregular input-free changes are no rhythm.
  InputFrameTagger irregular; at = 30000 * ms;
  for (const uint64_t gap : {0, 530, 1700, 310}) { at += gap * ms; irregular.beginCapture(at); irregular.submitted(at, true); }
  irregular.injected(5, at + 100 * ms); irregular.beginCapture(at + 400 * ms); irregular.submitted(at + 400 * ms, true);
  require(irregular.output(at + 400 * ms).has_value() && irregular.misses().empty(), "Irregular changes were mistaken for a rhythm");
}
void taggerInterruption() {
  InputFrameTagger tagger;
  // Not yet tied to a capture tick: dropped.
  tagger.injected(1, 100); tagger.interrupted(); tagger.beginCapture(200); tagger.submitted(200, true);
  require(!tagger.output(200) && tagger.superseded() == 1, "An interrupted pending input named a frame");
  // Waiting for its change after a tick began: dropped as well.
  tagger.injected(2, 300); tagger.beginCapture(400); tagger.submitted(400, false); tagger.interrupted(); tagger.beginCapture(500); tagger.submitted(500, true);
  require(!tagger.output(500) && tagger.superseded() == 2, "An interrupted candidate named the next change");
  // Already tied to its changed frame: the tag is kept. Nothing pending: no count.
  tagger.injected(3, 600); tagger.beginCapture(700); tagger.submitted(700, true); tagger.interrupted();
  const auto tag = tagger.output(700); require(tag && tag->sample == 3 && tagger.superseded() == 2, "An interruption removed an assigned tag");
  // The next sampled input after an interruption is measured normally.
  tagger.injected(4, 800); tagger.beginCapture(900); tagger.submitted(900, true);
  require(tagger.output(900) && tagger.tagged() == 2, "A sample after an interruption was not tagged");
}
void sessionNegotiationAndMath() {
  InputLatencySession state;
  const auto sampled = JsonParser::parse(R"({"type":"input","event":"button","button":"left","down":true,"x":0.25,"y":0.75,"sample":5})");
  state.configure(JsonParser::parse(start(false)));
  require(!state.enabled() && !state.stamp(sampled, 100), "An unnegotiated session measured input");
  for (const auto& other : {start(true, ",\"mediaProtocol\":\"tracks-v1\""), std::string("{\"type\":\"session.start\",\"mode\":\"rdp\",\"sessionId\":\"x\",\"inputLatencyProtocol\":\"input-latency-v1\"}"),
                            std::string("{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"x\",\"inputLatencyProtocol\":\"input-latency-v2\"}")}) {
    state.configure(JsonParser::parse(other)); require(!state.enabled(), "Tracks, RDP or another marker enabled measurement");
  }
  state.configure(JsonParser::parse(start(true)));
  require(state.enabled(), "The exact marker did not enable measurement");
  for (const auto* bad : {R"({"type":"input","event":"move","x":0.5,"y":0.5})", R"({"type":"input","event":"move","x":0.5,"y":0.5,"sample":0})",
                          R"({"type":"input","event":"move","x":0.5,"y":0.5,"sample":2147483648})", R"({"type":"input","event":"move","x":0.5,"y":0.5,"sample":1.5})",
                          R"({"type":"input","event":"move","x":0.5,"y":0.5,"sample":"3"})"})
    require(!state.stamp(JsonParser::parse(bad), 100), "An unsampled or malformed input was stamped");
  const auto stamped = state.stamp(sampled, 1000000);
  require(stamped && stamped->get("latencyReceivedUs").number() == 1000000 && keys(*stamped).size() == keys(sampled).size() + 1, "The service stamp was not exactly one private field");
  require(!state.injected(sampled, true, 1000400), "A worker reported an input without the service receipt");
  const auto report = state.injected(*stamped, true, 1000400);
  require(report.has_value(), "A stamped injection was not reported");
  const auto parsed = JsonParser::parse(*report);
  require(keys(parsed) == std::set<std::string>{"type", "sessionId", "sample", "ok", "receivedUs", "injectedUs"} && parsed.get("type").string() == "worker.input.latency", "The private report has unexpected fields");
  require(!state.injected(*stamped, true, 999999), "Injection before receipt was reported");
  // Acknowledgement math at the send: injectUs = injected - received, holdUs = send - received.
  const auto ack = state.acknowledge(parsed, 1002500);
  require(ack.has_value(), "A valid report was not acknowledged");
  const auto value = JsonParser::parse(*ack);
  require(keys(value) == std::set<std::string>{"type", "sessionId", "sample", "ok", "injectUs", "holdUs"} && value.get("type").string() == "input.ack" && value.get("sample").number() == 5 &&
          value.get("injectUs").number() == 400 && value.get("holdUs").number() == 2500 && value.get("ok").boolean(), "Acknowledgement math or shape changed");
  require(!state.acknowledge(parsed, 1000300), "A send before the injection was acknowledged");
  require(!state.acknowledge(parsed, 1000000 + inputLatencyMaxUs + 1), "A hold beyond the bound was acknowledged");
  auto foreign = parsed; std::get<Json::Object>(foreign.value)["sessionId"].value = std::string("other");
  require(!state.acknowledge(foreign, 1002500), "Another session's report was acknowledged");
  const auto failed = state.injected(*stamped, false, 1000500);
  require(failed && !JsonParser::parse(*state.acknowledge(JsonParser::parse(*failed), 1003000)).get("ok").boolean(), "A failed injection was not reported as such");
  // Frame tags: exact rebuild, own session only.
  const auto tag = JsonParser::parse("{\"type\":\"input.frame\",\"sessionId\":\"" + session + "\",\"epoch\":3,\"sample\":5,\"pts\":44}");
  require(state.frame(tag) && *state.frame(tag) == "{\"type\":\"input.frame\",\"sessionId\":\"" + session + "\",\"epoch\":3,\"sample\":5,\"pts\":44}", "A frame tag was not forwarded exactly");
  auto extraTag = tag; std::get<Json::Object>(extraTag.value)["x"].value = 0.5;
  require(state.frame(extraTag) && JsonParser::parse(*state.frame(extraTag)).get("x").number(-1) < 0, "A frame tag forwarded an extra field");
  state.stop();
  require(!state.acknowledge(parsed, 1002500) && !state.frame(tag) && !state.injected(*stamped, true, 1000400), "A stopped session still reported");
}
void sessionFrameTagMessage() {
  InputLatencySession state; state.configure(JsonParser::parse(start(true)));
  const auto stamped = state.stamp(JsonParser::parse(R"({"type":"input","event":"key","vk":65,"scan":30,"down":true,"extended":false,"sample":9})"), 100);
  state.beginCapture(150);
  require(state.injected(*stamped, true, 200).has_value(), "Injection report missing");
  state.submitted(150, true);
  require(!state.output(150, 0), "The capture that began before injection was tagged");
  state.beginCapture(300); state.submitted(300, true);
  const auto message = state.output(310, 2);
  require(message && *message == "{\"type\":\"input.frame\",\"sessionId\":\"" + session + "\",\"epoch\":2,\"sample\":9,\"pts\":310}", "The frame tag does not name the actual output packet");
  for (const auto* field : {"vk", "scan", "x", "y", "button", "delta", "down", "event"}) require(message->find(std::string("\"") + field + "\"") == std::string::npos, "A frame tag carried input content");
}

// The send path after the encoder: a worker tag carries the private encoder
// output time; the service rebuilds the public tag without it and times the
// write of the video packet that tag names.
void sessionSendTiming() {
  InputLatencySession state; state.configure(JsonParser::parse(start(true)));
  const auto t0 = hostMonotonicUs();
  const auto stamped = state.stamp(JsonParser::parse(R"({"type":"input","event":"key","vk":65,"scan":30,"down":true,"extended":false,"sample":3})"), t0 - 5000);
  require(state.injected(*stamped, true, t0 - 4900).has_value(), "Injection report missing");
  state.beginCapture(t0 - 4700); state.submitted(300, true);
  const auto before = hostMonotonicUs();
  const auto workerTag = state.output(300, 1, true);
  require(workerTag.has_value(), "No worker tag");
  const auto parsed = JsonParser::parse(*workerTag);
  require(keys(parsed) == std::set<std::string>{"type", "sessionId", "epoch", "sample", "pts", "outputUs", "captureUs", "encodeUs"}, "The worker tag fields differ");
  const auto output = uint64_t(parsed.get("outputUs").number());
  require(output >= before && output <= hostMonotonicUs(), "The output time is not the host clock at output");
  require(parsed.get("captureUs").number() == 200 && uint64_t(parsed.get("encodeUs").number()) == output - (t0 - 4700), "Worker tag durations differ");
  // Without frame-timing-v1 the public tag is the released five fields.
  require(keys(JsonParser::parse(*state.frame(parsed, output + 300))) == std::set<std::string>{"type", "sessionId", "epoch", "sample", "pts"}, "The public tag carried private fields");
  require(!state.output(400, 1, false), "A tag without a candidate");
  // With it, the host's durations and encoder output -> this write.
  InputLatencySession timed; timed.configure(JsonParser::parse(start(true, ",\"inputFrameTimingProtocol\":\"frame-timing-v1\"")));
  const auto rebuilt = JsonParser::parse(*timed.frame(parsed, output + 300));
  require(keys(rebuilt) == std::set<std::string>{"type", "sessionId", "epoch", "sample", "pts", "captureUs", "encodeUs", "queueUs"} && rebuilt.get("queueUs").number() == 300 &&
          rebuilt.get("captureUs").number() == 200, "The timed public tag differs");
  require(keys(JsonParser::parse(*timed.frame(parsed))) .size() == 5, "A tag without a write time carried durations");
  auto stale = parsed; std::get<Json::Object>(stale.value)["outputUs"].value = double(output + 1000);
  require(keys(JsonParser::parse(*timed.frame(stale, output + 300))).size() == 5, "A future output time was published");
  auto slow = parsed; std::get<Json::Object>(slow.value)["encodeUs"].value = double(inputLatencyMaxUs + 1);
  require(keys(JsonParser::parse(*timed.frame(slow, output + 300))).size() == 5, "An out-of-range duration was published");
  // The write of exactly that packet is timed; an earlier packet (a tile at pts-1) is not.
  state.expectVideo(parsed, output + 1000);
  state.videoWritten(299, output + 1500, output + 1600);
  require(state.sendTimingJson(output + 2000).empty(), "An earlier packet was timed");
  state.videoWritten(300, output + 2500, output + 4000);
  require(state.sendTimingJson(output + 5000) == ",\"inputSend\":{\"samples\":1,\"queueP50Ms\":2.50,\"queueP95Ms\":2.50,\"writeP50Ms\":1.50,\"writeP95Ms\":1.50}", "Send timing fields or math differ");
  state.videoWritten(300, output + 6000, output + 7000);
  require(state.sendTimingJson(output + 8000).find("\"samples\":1,") != std::string::npos, "One tag was timed twice");
  // A later packet first: the named one was not written; nothing is recorded.
  state.expectVideo(parsed, output + 9000); state.videoWritten(316, output + 9500, output + 9600); state.videoWritten(300, output + 9700, output + 9800);
  require(state.sendTimingJson(output + 10000).find("\"samples\":1,") != std::string::npos, "A skipped packet was timed");
  // No private time, a future one, another session's tag: nothing waits.
  for (const auto& extra : {std::string(), std::string(",\"outputUs\":") + std::to_string(output + 999999), std::string(",\"outputUs\":1")}) {
    auto tag = "{\"type\":\"input.frame\",\"sessionId\":\"" + session + "\",\"epoch\":1,\"sample\":3,\"pts\":500" + extra + '}';
    state.expectVideo(JsonParser::parse(tag), output + 20000); state.videoWritten(500, output + 20100, output + 20200);
  }
  state.expectVideo(JsonParser::parse("{\"type\":\"input.frame\",\"sessionId\":\"other\",\"epoch\":1,\"sample\":3,\"pts\":600,\"outputUs\":" + std::to_string(output) + '}'), output + 20000);
  state.videoWritten(600, output + 20100, output + 20200);
  require(state.sendTimingJson(output + 21000).find("\"samples\":1,") != std::string::npos, "An invalid tag was timed");
  require(state.sendTimingJson(output + 31000000).empty(), "Stale send timings were reported");
  state.stop(); require(state.sendTimingJson(output + 5000).empty(), "A stopped session reported send timings");
  InputLatencySession unnegotiated; unnegotiated.configure(JsonParser::parse(start(false)));
  unnegotiated.expectVideo(parsed, output + 1000); unnegotiated.videoWritten(300, output + 2000, output + 3000);
  require(unnegotiated.sendTimingJson(output + 4000).empty(), "An unnegotiated session timed a write");
}

// The actual worker Agent: same INPUT sequence with and without measurement,
// one report after each sampled injection, no report when unnegotiated.
struct Recorded { std::string kind; INPUT input{}; std::string text; };
std::vector<Recorded> runWorker(bool negotiated, const std::vector<std::string>& inputs, bool fail = false) {
  std::vector<Recorded> log; std::mutex mutex;
  {
    Agent agent([](const std::wstring&) {}, [&](bool text, std::vector<uint8_t> bytes) { std::lock_guard lock(mutex); if (text) log.push_back({"text", {}, std::string(bytes.begin(), bytes.end())}); },
                [&](INPUT& value) -> UINT { std::lock_guard lock(mutex); log.push_back({"inject", value, {}}); return fail ? 0U : 1U; }, [] { return RECT{0, 0, 1920, 1080}; });
    agent.inputLatencyTestStream(JsonParser::parse(start(negotiated)), RECT{0, 0, 1920, 1080});
    for (const auto& input : inputs) agent.localControl(input);
    agent.localControl("{\"type\":\"session.stop\",\"sessionId\":\"" + session + "\"}");
  }
  return log;
}
bool sameInput(const INPUT& a, const INPUT& b) {
  if (a.type != b.type) return false;
  if (a.type == INPUT_KEYBOARD) return a.ki.wVk == b.ki.wVk && a.ki.wScan == b.ki.wScan && a.ki.dwFlags == b.ki.dwFlags;
  return a.mi.dx == b.mi.dx && a.mi.dy == b.mi.dy && a.mi.dwFlags == b.mi.dwFlags && a.mi.mouseData == b.mi.mouseData;
}
void workerOrderingAndReports() {
  const std::vector<std::string> plain = {
    R"({"type":"input","event":"move","x":0.1,"y":0.2})",
    R"({"type":"input","event":"button","button":"left","down":true,"x":0.1,"y":0.2})",
    R"({"type":"input","event":"move","x":0.4,"y":0.5})",
    R"({"type":"input","event":"button","button":"left","down":false,"x":0.4,"y":0.5})",
    R"({"type":"input","event":"wheel","delta":-120,"x":0.4,"y":0.5})",
    R"({"type":"input","event":"key","vk":65,"scan":30,"down":true,"extended":false})",
    R"({"type":"input","event":"key","vk":65,"scan":30,"down":false,"extended":false})"};
  // The service-stamped form the worker receives for sampled events 2, 5 and 6.
  auto sampled = plain;
  const auto stamp = [&](size_t index, int sample, uint64_t received) { sampled[index].pop_back(); sampled[index] += ",\"sample\":" + std::to_string(sample) + ",\"latencyReceivedUs\":" + std::to_string(received) + '}'; };
  const auto now = hostMonotonicUs();
  stamp(1, 1, now); stamp(4, 2, now); stamp(5, 3, now);
  const auto reference = runWorker(false, plain), unmeasured = runWorker(false, sampled), measured = runWorker(true, sampled);
  std::vector<INPUT> expected; for (const auto& item : reference) { require(item.kind == "inject", "The reference session emitted a report"); expected.push_back(item.input); }
  require(expected.size() == plain.size() + 1 /* key release on stop is not pending */ || expected.size() == plain.size(), "Unexpected reference injection count");
  for (const auto* run : {&unmeasured, &measured}) {
    std::vector<INPUT> injected; for (const auto& item : *run) if (item.kind == "inject") injected.push_back(item.input);
    require(injected.size() == expected.size(), "Measurement changed the number of injections");
    for (size_t n = 0; n < expected.size(); ++n) require(sameInput(injected[n], expected[n]), "Measurement changed an injected INPUT or its order");
  }
  require(std::none_of(unmeasured.begin(), unmeasured.end(), [](const auto& item) { return item.kind == "text"; }), "An unnegotiated worker reported input");
  // Exactly one report per sampled injection, immediately after it, in order.
  std::vector<int> samples; size_t injections = 0;
  for (size_t n = 0; n < measured.size(); ++n) {
    if (measured[n].kind == "inject") { ++injections; continue; }
    const auto report = JsonParser::parse(measured[n].text);
    require(report.get("type").string() == "worker.input.latency" && report.get("ok").boolean(), "Unexpected worker message");
    require(n > 0 && measured[n - 1].kind == "inject", "A report did not follow its injection");
    require(report.get("injectedUs").number() >= report.get("receivedUs").number(), "Injection preceded the service receipt");
    samples.push_back(int(report.get("sample").number()));
    const std::set<size_t> sampledAfter = {2, 5, 6};
    require(sampledAfter.contains(injections), "A report followed an unsampled injection");
    for (const auto* field : {"vk", "scan", "x", "y", "button", "delta", "down", "event"}) require(measured[n].text.find(std::string("\"") + field + "\"") == std::string::npos, "A report carried input content");
  }
  require(samples == std::vector<int>{1, 2, 3}, "Reports were lost, duplicated or reordered");
  const auto rejected = runWorker(true, {sampled[1]}, true);
  const auto failed = std::find_if(rejected.begin(), rejected.end(), [](const auto& item) { return item.kind == "text"; });
  require(failed != rejected.end() && !JsonParser::parse(failed->text).get("ok").boolean(), "A rejected injection was not reported as not applied");
}

// The actual worker Agent of a cursor-v1 session decides frame candidates with
// its own pressed-button state: a sampled key release, modifier press or hover
// move names no frame, a drag move does, and an unsampled button change drops
// a pending candidate. Every sampled input is still acknowledged.
void workerFrameCandidates() {
  std::vector<std::string> texts; std::mutex mutex;
  Agent agent([](const std::wstring&) {}, [&](bool text, std::vector<uint8_t> bytes) { std::lock_guard lock(mutex); if (text) texts.emplace_back(bytes.begin(), bytes.end()); },
              [](INPUT&) -> UINT { return 1U; }, [] { return RECT{0, 0, 1920, 1080}; });
  agent.inputLatencyTestStream(JsonParser::parse(start(true, ",\"cursorProtocol\":\"cursor-v1\"")), RECT{0, 0, 1920, 1080});
  auto& latency = agent.inputLatencyTestState();
  uint64_t pts = 1000; int sample = 0;
  const auto send = [&](std::string input, bool sampled) {
    if (sampled) { input.pop_back(); input += ",\"sample\":" + std::to_string(++sample) + ",\"latencyReceivedUs\":" + std::to_string(hostMonotonicUs()) + '}'; }
    agent.localControl(input);
  };
  // The next capture tick after the inputs has a changed frame: which sample names it?
  const auto named = [&]() -> int {
    pts += 1000; latency.beginCapture(hostMonotonicUs() + 10); latency.submitted(pts, true);
    const auto tag = latency.output(pts, 0); return tag ? int(JsonParser::parse(*tag).get("sample").number()) : 0;
  };
  const std::string keyDown = R"({"type":"input","event":"key","vk":65,"scan":30,"down":true,"extended":false})";
  const std::string keyUp = R"({"type":"input","event":"key","vk":65,"scan":30,"down":false,"extended":false})";
  const std::string shiftDown = R"({"type":"input","event":"key","vk":160,"scan":42,"down":true,"extended":false})";
  const std::string shiftUp = R"({"type":"input","event":"key","vk":160,"scan":42,"down":false,"extended":false})";
  const std::string press = R"({"type":"input","event":"button","button":"left","down":true,"x":0.3,"y":0.3})";
  const std::string release = R"({"type":"input","event":"button","button":"left","down":false,"x":0.6,"y":0.6})";
  const std::string move = R"({"type":"input","event":"move","x":0.5,"y":0.5})";
  send(keyDown, true); require(named() == 1, "A sampled key press did not name its change");
  send(keyUp, true); require(named() == 0, "A sampled key release named a frame");
  send(shiftDown, true); require(named() == 0, "A sampled modifier press named a frame");
  send(shiftUp, false);
  send(move, true); require(named() == 0, "A cursor-v1 hover move named a frame");
  send(press, false); send(move, true); require(named() == 5, "A cursor-v1 drag move did not name its change");
  send(release, false); send(move, true); require(named() == 0, "A move after the drag named a frame");
  // A sampled press whose change was not captured yet, then the unsampled release: dropped.
  send(press, true); send(release, false); require(named() == 0, "A press was credited with the change after its unsampled release");
  // Unsampled key presses do not interrupt: the sampled one keeps its change.
  send(keyDown, true); send(keyUp, false); send(keyDown, false); require(named() == 8, "An unsampled key press dropped the sampled one");
  send(keyUp, false);
  std::lock_guard lock(mutex);
  require(std::count_if(texts.begin(), texts.end(), [](const std::string& t) { return t.find("worker.input.latency") != std::string::npos; }) == sample, "A sampled input was not acknowledged");
}

// The actual Session0 Agent with an injected bridge: stamps only sampled input
// of a negotiated session, acknowledges at publication, never forwards a
// private report or an unnegotiated tag.
struct Bridge final : AgentBridge {
  Send output; std::vector<Json> controls;
  void start(const Json&, Send send) override { output = std::move(send); }
  void control(const Json& value) override { controls.push_back(value); }
  void stop() override {}
  std::string capabilities() override { return "{\"service\":true,\"streaming\":true,\"monitors\":[]}"; }
  void displayControl(const Json&, Send) override {}
  void text(const std::string& bytes) { require(bool(output), "Missing Agent bridge callback"); output(true, {bytes.begin(), bytes.end()}); }
  void media(std::vector<uint8_t> bytes) { require(bool(output), "Missing Agent bridge callback"); output(false, std::move(bytes)); }
};
std::vector<Json> drain(Agent& agent, size_t* binary = nullptr) {
  std::vector<Json> sent;
  require(agent.publicationTestDrain([&](bool text, const std::vector<uint8_t>& bytes) { if (text) sent.push_back(JsonParser::parse(std::string(bytes.begin(), bytes.end()))); else if (binary) ++*binary; return true; }), "Publication failed");
  return sent;
}
void servicePublication() {
  const std::string sampled = R"({"type":"input","event":"button","button":"left","down":true,"x":0.25,"y":0.75,"sample":4})";
  const std::string plain = R"({"type":"input","event":"move","x":0.25,"y":0.75})";
  for (const bool negotiated : {false, true}) {
    Bridge bridge; Agent agent([](const std::wstring&) {}, &bridge); agent.publicationTestOpen(); agent.localControl(start(negotiated));
    agent.localControl(plain); agent.localControl(sampled);
    require(bridge.controls.size() == 2, "Service did not forward every input exactly once");
    require(keys(bridge.controls[0]) == keys(JsonParser::parse(plain)), "An unsampled input was changed");
    require((bridge.controls[1].get("latencyReceivedUs").number(-1) > 0) == negotiated, "The receipt stamp did not follow negotiation");
    require(keys(bridge.controls[1]).size() == keys(JsonParser::parse(sampled)).size() + (negotiated ? 1 : 0), "The service added more than its private stamp");
    const auto received = uint64_t(negotiated ? bridge.controls[1].get("latencyReceivedUs").number() : double(hostMonotonicUs()));
    const auto report = "{\"type\":\"worker.input.latency\",\"sessionId\":\"" + session + "\",\"sample\":4,\"ok\":true,\"receivedUs\":" + std::to_string(received) + ",\"injectedUs\":" + std::to_string(received + 300) + '}';
    // The worker's tag carries its private encoder output time; its packet follows.
    const auto frame = "{\"type\":\"input.frame\",\"sessionId\":\"" + session + "\",\"epoch\":0,\"sample\":4,\"pts\":16667,\"outputUs\":" + std::to_string(hostMonotonicUs()) + '}';
    bridge.text(report); bridge.text(frame); bridge.media(mediaPacket(1, 16667, {0, 0, 0, 1, 0x65}));
    bridge.text("{\"type\":\"worker.input.latency\",\"sessionId\":\"other\",\"sample\":4,\"ok\":true,\"receivedUs\":1,\"injectedUs\":2}");
    bridge.text(R"({"type":"stream.stats","capturedFps":60,"encodedFps":60,"bitrateKbps":1000})");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));  // the write follows the reported injection
    size_t binary = 0; const auto sent = drain(agent, &binary);
    require(binary == 1, "The video packet was not written exactly once");
    // Statistics carry the send path of the tagged packet only when negotiated.
    const auto send = sent.back().get("inputSend");
    require(std::holds_alternative<Json::Object>(send.value) == negotiated, "Send timing did not follow negotiation");
    if (negotiated) require(keys(send) == std::set<std::string>{"samples", "queueP50Ms", "queueP95Ms", "writeP50Ms", "writeP95Ms"} && send.get("samples").number() == 1 &&
                            send.get("queueP50Ms").number(-1) >= 0 && send.get("writeP50Ms").number(-1) >= 0, "Send timing member differs");
    for (const auto& value : sent) require(value.get("type").string() != "worker.input.latency", "A private worker report reached the broker");
    const auto ack = std::find_if(sent.begin(), sent.end(), [](const auto& value) { return value.get("type").string() == "input.ack"; });
    const auto tag = std::find_if(sent.begin(), sent.end(), [](const auto& value) { return value.get("type").string() == "input.frame"; });
    require((ack != sent.end()) == negotiated && (tag != sent.end()) == negotiated, "Acknowledgement or tag did not follow negotiation");
    require(std::count_if(sent.begin(), sent.end(), [](const auto& value) { return value.get("type").string() == "input.ack"; }) <= 1, "Another session's report was acknowledged");
    require(sent.back().get("type").string() == "stream.stats", "Ordinary statistics changed");
    if (negotiated) {
      require(ack->get("injectUs").number() == 300 && ack->get("holdUs").number() >= 300 && ack->get("holdUs").number() < 300 + 5000000, "Acknowledgement hold/injection math changed");
      require(keys(*tag) == std::set<std::string>{"type", "sessionId", "epoch", "sample", "pts"} && tag->get("pts").number() == 16667, "Frame tag was not forwarded exactly");
      // After the session stops, late reports are dropped.
      agent.localControl("{\"type\":\"session.stop\",\"sessionId\":\"" + session + "\"}");
      agent.publicationTestOpen(); bridge.text(report); bridge.text(frame);
      require(drain(agent).empty(), "A retired session's report was forwarded");
    }
  }
}
}
int main() {
  const struct { const char* name; void (*run)(); } cases[] = {{"tagger-first-changed-frame", taggerFirstChangedFrameAfterInjection}, {"tagger-latest-and-bounds", taggerLatestAndBounds}, {"tagger-host-timing", taggerHostTiming},
    {"candidate-rules", candidateRules}, {"tagger-no-visible-change", taggerNoVisibleChange}, {"tagger-interruption", taggerInterruption},
    {"session-negotiation-and-math", sessionNegotiationAndMath}, {"session-frame-tag", sessionFrameTagMessage}, {"worker-ordering-and-reports", workerOrderingAndReports},
    {"worker-frame-candidates", workerFrameCandidates}, {"session-send-timing", sessionSendTiming}, {"service-publication", servicePublication}};
  unsigned passed = 0, failed = 0;
  for (const auto& value : cases) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
  std::cout << "{\"kind\":\"input-latency-v1\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << ",\"productionAgent\":true,\"injector\":\"substituted\",\"osInputEmitted\":false,\"capture\":false,\"network\":false}\n";
  return failed ? 1 : 0;
}
