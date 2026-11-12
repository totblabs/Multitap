// The secure desktop on independent outputs (secure_desktop_tracks.hpp) with fakes: the worker's per-output
// switches fed by sections the test writes like the helpers do, the service's per-output dispatch with a fake
// launch, and the input path (the worker places, the service writes to the helper of the output the pixel is on).
// No SYSTEM process, no secure desktop, no policy: the real launch and the real injection are only exercised live.
#include "secure_desktop_tracks.hpp"
#include "system_input.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures = 0, checks = 0;
void expect(bool value, const char* what) { ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; } }
using namespace host::secure;
using host::Json;
using host::JsonParser;
using Clock = WorkerSwitch::Clock;
const std::string session = "12345678-1234-4234-8234-0000000000f1";

constexpr uint32_t W = 32, H = 16;
std::vector<uint8_t> picture(uint8_t value) { return std::vector<uint8_t>(ipc::frameBytes(W, H), value); }
struct FakeSection {
  std::shared_ptr<std::vector<uint8_t>> bytes = std::make_shared<std::vector<uint8_t>>(ipc::sectionBytes(W, H), uint8_t(0));
  FakeSection() { ipc::writerInit(bytes->data(), W, H); }
  uint8_t* data() { return bytes->data(); }
  std::shared_ptr<const uint8_t> view() const { return std::shared_ptr<const uint8_t>(bytes, bytes->data()); }
};
bool has(const std::vector<std::string>& lines, const char* type, int output) {
  for (const auto& line : lines) { const auto value = JsonParser::parse(line); if (value.get("type").string() == type && value.get("output").number(-1) == output) return true; }
  return false;
}
unsigned count(const std::vector<uint32_t>& records, uint32_t record) { unsigned n = 0; for (const auto r : records) if (r == record) ++n; return n; }

struct Worker {
  std::vector<std::string> sent;
  std::vector<uint32_t> records;
  unsigned counted = 0, converted = 0;
  bool ownedKnown = true, convertFails = false;
  TrackSwitches::Env env() {
    TrackSwitches::Env e;
    e.send = [this](const std::string& line) { sent.push_back(line); };
    e.owned = [this](size_t n) -> std::optional<OwnedOutput> { if (!ownedKnown) return {}; OwnedOutput o; o.adapterLow = 7; o.width = W; o.height = H; o.left = n ? 0 : -int32_t(W); o.top = 0; return o; };
    e.record = [this](uint32_t error) { records.push_back(error); };
    e.counts = [this](const ipc::InputCounts&) { ++counted; };
    e.toNv12 = [this](const uint8_t* bgra, uint8_t* nv12, int width, int height) { if (convertFails) throw std::runtime_error("convert"); ++converted; std::fill(nv12, nv12 + size_t(width) * height * 3 / 2, bgra[0]); };
    return e;
  }
};

void workerSwitches() {
  Worker w;
  TrackSwitches switches(session, {4, 9}, w.env());
  auto now = Clock::now();
  // Both outputs ask for their own helper and code nothing while it has not answered.
  auto a = switches.tick(0, W, H, now, true), b = switches.tick(1, 16, 8, now, true);
  expect(a.hold && b.hold && !a.nv12 && !b.nv12, "an output whose helper was asked for holds its picture");
  expect(w.sent.size() == 2 && has(w.sent, controlRequest, 0) && has(w.sent, controlRequest, 1), "one request per output, each naming its output");
  const auto first = JsonParser::parse(w.sent[0]), second = JsonParser::parse(w.sent[1]);
  expect(first.get("seq").number() == 5 && second.get("seq").number() == 10 && first.get("sessionId").string() == session, "each output continues its own request numbers");
  expect(first.get("rect").get("left").number() == -double(W) && second.get("rect").get("left").number() == 0 && first.get("adapter").get("low").number() == 7, "the request carries that output's rectangle");
  expect(parseHelperRequest(first).has_value() && trackOutput(first) == size_t(0) && trackOutput(second) == size_t(1), "the service can read the request");
  // Output 1 is answered and streams; output 0 never is.
  FakeSection one;
  expect(!switches.attach(1, one.view(), W, H, 9), "a late answer to an older request is dropped");
  expect(switches.attach(1, one.view(), W, H, 10), "the answer to the current request is taken");
  ipc::writeFrame(one.data(), picture(90).data(), W, H);
  now += std::chrono::milliseconds(100);
  b = switches.tick(1, 16, 8, now, true);
  expect(b.real && b.key && !b.hold && b.nv12 && b.nv12->size() == size_t(16) * 8 * 3 / 2 && (*b.nv12)[0] == 90, "the helper's frame is scaled to the output's coded size");
  expect(has(w.sent, controlActive, 1) && !has(w.sent, controlActive, 0) && count(w.records, record244HelperActive) == 1, "the service is told which output shows its helper");
  a = switches.tick(0, W, H, now, true);
  expect(a.hold, "the other output still waits");
  // A still prompt: the same picture, not converted again, no IDR.
  const unsigned convertedBefore = w.converted;
  ipc::writeAlive(one.data());
  now += std::chrono::milliseconds(100);
  b = switches.tick(1, 16, 8, now, true);
  expect(b.real && !b.key && w.converted == convertedBefore, "an unchanged prompt is not converted again");
  // 2 s without an answer: the notice on output 0 only.
  now += std::chrono::milliseconds(2100);
  ipc::writeAlive(one.data());
  a = switches.tick(0, W, H, now, true); b = switches.tick(1, 16, 8, now, true);
  expect(!a.hold && !a.real && !a.nv12 && b.real, "an output whose helper never answered shows the notice; the other keeps the prompt");
  expect(count(w.records, record244HelperNoFrame) == 1 && has(w.sent, controlStop, 0) && !has(w.sent, controlStop, 1), "its helper is given up, the other one is not");
  // The helper of output 1 stops beating.
  now += std::chrono::milliseconds(2100);
  b = switches.tick(1, 16, 8, now, true);
  expect(!b.real && !b.nv12 && b.key && count(w.records, record244HelperLost) == 1 && has(w.sent, controlStop, 1), "a lost helper is the notice on its output, with an IDR");
  expect(w.counted == 1, "the lost helper's input counts are taken once");
  // The desktop returns.
  const size_t sentBefore = w.sent.size();
  switches.ended();
  expect(w.sent.size() == sentBefore && switches.seq(0) == 5 && switches.seq(1) == 10, "nothing is stopped twice, and the request numbers stay");
  // The next prompt asks again, with the next numbers.
  w.sent.clear();
  a = switches.tick(0, W, H, now, true);
  expect(a.hold && w.sent.size() == 1 && JsonParser::parse(w.sent[0]).get("seq").number() == 6, "the next prompt asks again");
  switches.ended();
  expect(has(w.sent, controlStop, 0), "a helper that was asked for is stopped when the desktop returns");
}

void workerRefusals() {
  {  // The service refuses.
    Worker w; TrackSwitches switches(session, {0, 0}, w.env()); auto now = Clock::now();
    (void)switches.tick(0, W, H, now, true); switches.denied(0, 1);
    const auto a = switches.tick(0, W, H, now, true);
    expect(!a.hold && !a.real && count(w.records, record244HelperLaunchFail) == 1, "a refused helper is the notice at once");
  }
  {  // The output is not an attached DXGI output as described.
    Worker w; w.ownedKnown = false; TrackSwitches switches(session, {0, 0}, w.env()); auto now = Clock::now();
    auto a = switches.tick(1, W, H, now, true); a = switches.tick(1, W, H, now, true);
    expect(!a.hold && !a.real && !has(w.sent, controlRequest, 1) && count(w.records, record244HelperLaunchFail) == 1, "an output that cannot be named asks for nothing");
  }
  {  // Not the service's worker.
    Worker w; TrackSwitches switches(session, {0, 0}, w.env()); auto now = Clock::now();
    const auto a = switches.tick(0, W, H, now, false), b = switches.tick(1, W, H, now, false);
    expect(!a.hold && !b.hold && w.sent.empty() && count(w.records, record244NoticeShown) == 2, "a worker that may not ask shows the notice on both outputs");
  }
  {  // A frame that cannot be used: the notice for the rest of this secure desktop, the stream goes on.
    Worker w; TrackSwitches switches(session, {0, 0}, w.env()); auto now = Clock::now();
    (void)switches.tick(0, W, H, now, true);
    FakeSection zero; expect(switches.attach(0, zero.view(), W, H, 1), "attached");
    ipc::writeFrame(zero.data(), picture(50).data(), W, H);
    w.convertFails = true;
    auto a = switches.tick(0, W, H, now, true);
    expect(!a.real && !a.hold && a.key && has(w.sent, controlStop, 0) && w.counted == 1, "a failure inside the tick is the notice, and the helper is stopped");
    const size_t sent = w.sent.size(); w.convertFails = false;
    a = switches.tick(0, W, H, now, true);
    expect(!a.real && !a.hold && w.sent.size() == sent, "it stays the notice until the desktop returns");
    switches.ended();
    a = switches.tick(0, W, H, now, true);
    expect(a.hold && has(w.sent, controlRequest, 0), "and asks again at the next prompt");
    // Out of range.
    const auto none = switches.tick(5, W, H, now, true);
    expect(!none.hold && !none.real && !switches.attach(5, zero.view(), W, H, 1), "an output that does not exist is the notice");
  }
}

struct FakeHelper final : ServiceHelper {
  bool up = true, closed = false, fail = false;
  std::vector<std::string> got;
  bool alive() override { return up && !closed; }
  bool send(const std::string& json) override { if (fail) return false; got.push_back(json); return true; }
  void close() override { closed = true; }
};
struct Service {
  bool eligible = true;
  std::vector<std::shared_ptr<FakeHelper>> helpers;
  std::vector<HelperArgs> launched;
  std::vector<std::string> toWorker;
  unsigned revoked = 0;
  ServiceSecure::Env env() {
    ServiceSecure::Env e;
    e.eligible = [this] { return eligible; };
    e.launch = [this](const HelperArgs& args) -> std::optional<HelperGrant> {
      auto helper = std::make_shared<FakeHelper>(); helpers.push_back(helper); launched.push_back(args);
      return HelperGrant{helper, 1000 + helpers.size(), 2000 + helpers.size(), [this] { ++revoked; }};
    };
    e.toWorker = [this](const std::string& line) { toWorker.push_back(line); return true; };
    e.nowMs = [] { return uint64_t(1); };
    return e;
  }
};
// Output 0 is left of output 1; both 32x16.
Json request(size_t output, uint32_t seq) {
  OwnedOutput owned; owned.adapterLow = 7; owned.width = W; owned.height = H; owned.left = output ? 0 : -int32_t(W); owned.top = 0;
  return JsonParser::parse(trackRequestJson(session, output, seq, owned));
}
Json control(const char* type, size_t output) { return JsonParser::parse(trackControlJson(type, session, output)); }
const char* keyDown = "{\"type\":\"input\",\"event\":\"key\",\"vk\":65,\"scan\":30,\"down\":true,\"extended\":false}";

void serviceTracks() {
  Service s;
  ServiceSecureTracks tracks(session, s.env());
  expect(!tracks.consume(JsonParser::parse("{\"type\":\"stream.tracks.stats\"}")), "other messages are not this feature's");
  expect(tracks.consume(JsonParser::parse(std::string("{\"type\":\"") + controlRequest + "\",\"sessionId\":\"" + session + "\",\"seq\":1}")) && s.helpers.empty(), "a request that names no output is consumed and launches nothing");
  expect(tracks.consume(JsonParser::parse(std::string("{\"type\":\"") + controlRequest + "\",\"sessionId\":\"" + session + "\",\"output\":2,\"seq\":1}")) && s.helpers.empty(), "an output that does not exist launches nothing");
  expect(tracks.consume(request(0, 5)) && tracks.consume(request(1, 10)), "requests are consumed");
  expect(s.helpers.size() == 2 && s.launched[0].left == -int32_t(W) && s.launched[1].left == 0, "one helper per output, bound to that output's rectangle");
  expect(s.toWorker.size() == 2 && has(s.toWorker, controlReady, 0) && has(s.toWorker, controlReady, 1), "each answer names its output");
  expect(JsonParser::parse(s.toWorker[1]).get("seq").number() == 10 && JsonParser::parse(s.toWorker[1]).get("section").number() == 1002, "with its own request number and handles");
  // Nothing is routed before the worker says the stream shows a helper's frames.
  expect(!tracks.routing() && !tracks.send(keyDown, 0, 0, false) && !tracks.send("{\"type\":\"input\",\"event\":\"move\",\"px\":3,\"py\":3}", 3, 3, true), "no input before a helper shows the prompt");
  expect(tracks.consume(control(controlActive, 1)) && tracks.routing(), "the worker reported output 1");
  // Positions go to the helper of the output the pixel is on, or nowhere.
  expect(tracks.send("{\"type\":\"input\",\"event\":\"move\",\"px\":3,\"py\":3}", 3, 3, true) && s.helpers[1]->got.size() == 1, "a pixel on output 1 goes to its helper");
  expect(!tracks.send("{\"type\":\"input\",\"event\":\"move\",\"px\":-3,\"py\":3}", -3, 3, true) && s.helpers[0]->got.empty(), "a pixel on an output whose helper does not show the prompt goes nowhere");
  expect(!tracks.send("{\"type\":\"input\",\"event\":\"move\",\"px\":40,\"py\":3}", 40, 3, true) && !tracks.send("{}", 3, int32_t(H), true), "a pixel on no output goes nowhere");
  // Keys go to one helper, the same one while it shows the prompt.
  expect(tracks.send(keyDown, 0, 0, false) && s.helpers[1]->got.size() == 2, "a key goes to the helper that shows the prompt");
  expect(tracks.consume(control(controlActive, 0)), "output 0 shows its helper too");
  expect(tracks.send(keyDown, 0, 0, false) && s.helpers[1]->got.size() == 3 && s.helpers[0]->got.empty(), "keys stay with the helper that took the last one");
  expect(tracks.send("{\"type\":\"input\",\"event\":\"move\",\"px\":-3,\"py\":3}", -3, 3, true) && s.helpers[0]->got.size() == 1, "positions follow the pixel");
  expect(tracks.reset("{\"type\":\"input.reset\"}") && s.helpers[0]->got.size() == 2 && s.helpers[1]->got.size() == 4, "a reset reaches every helper that shows the prompt");
  // Output 1's helper ends: keys move to the other one; nothing else changes.
  expect(tracks.consume(control(controlStop, 1)) && s.helpers[1]->closed && !s.helpers[0]->closed, "a stop ends only that output's helper");
  expect(tracks.send(keyDown, 0, 0, false) && s.helpers[0]->got.size() == 3, "keys go to the helper that is left");
  expect(!tracks.send("{\"type\":\"input\",\"event\":\"move\",\"px\":3,\"py\":3}", 3, 3, true), "the ended helper's output takes no input");
  // A new request replaces only its own output's helper; a dead helper routes nothing.
  expect(tracks.consume(request(0, 6)) && s.helpers.size() == 3 && s.helpers[0]->closed && !tracks.routing(), "a newer request ends the older helper of the same output");
  expect(tracks.consume(control(controlActive, 0)) && tracks.routing(), "the new helper shows");
  s.helpers[2]->up = false;
  expect(!tracks.routing() && !tracks.send(keyDown, 0, 0, false), "a helper that exited routes nothing");
  // Not eligible (a resize in flight, a locked session, a stream that is ending): refused, with the output.
  s.eligible = false; s.toWorker.clear();
  expect(tracks.consume(request(1, 11)) && s.helpers.size() == 3 && has(s.toWorker, controlFailed, 1), "an ineligible request is refused and says which output");
  // The relay ends.
  s.eligible = true;
  expect(tracks.consume(request(1, 12)) && s.helpers.size() == 4, "launched again");
  tracks.close();
  expect(s.helpers[2]->closed && s.helpers[3]->closed && !tracks.helperHeld(0) && !tracks.helperHeld(1), "no helper outlives the relay");
  expect(tracks.consume(request(0, 7)) && s.helpers.size() == 4, "nothing launches after the relay ended");
}

// The client's input on its way to a helper: the worker places, the service writes.
struct Sink {
  bool routing = false, accept = true;
  struct Sent { std::string message; int32_t x, y; bool positional; };
  std::vector<Sent> sent; std::vector<std::string> resets;
  host::sysinput::ServiceSystemInput::SecureSink sink() {
    return {[this] { return routing; },
            [this](const std::string& m, int32_t x, int32_t y, bool positional) { if (!accept) return false; sent.push_back({m, x, y, positional}); return true; },
            [this](const std::string& m) { resets.push_back(m); return true; }};
  }
};
Json move() { return JsonParser::parse("{\"type\":\"input\",\"sessionId\":\"" + session + "\",\"topologyEpoch\":1,\"event\":\"move\",\"track\":\"1\",\"epoch\":1,\"x\":0.5,\"y\":0.5}"); }
Json key() { return JsonParser::parse("{\"type\":\"input\",\"sessionId\":\"" + session + "\",\"topologyEpoch\":1,\"event\":\"key\",\"vk\":65,\"scan\":30,\"down\":true,\"extended\":false}"); }
Json answer(uint32_t seq, bool ok, int x, int y) { return JsonParser::parse(host::sysinput::trackAnswerJson(session, seq, ok, std::pair<int32_t, int32_t>{x, y})); }
uint32_t asked(const Json& command) { return uint32_t(command.get(host::sysinput::workerSeq).number(0)); }
bool marked(const Json& command) { const auto* b = std::get_if<bool>(&command.get(host::sysinput::workerMarker).value); return b && *b; }

void inputWithoutInjector() {
  // An elevated worker's stream has no injector: the worker injects for itself, except under the prompt.
  using namespace host::sysinput;
  ServiceSystemInput input; Sink sink; input.tracks(true); input.secureSink(sink.sink());
  auto command = move();
  expect(!input.routeTrack(command) && !marked(command) && !input.counted(), "without a prompt the worker injects, as before");
  sink.routing = true;
  command = move();
  expect(input.routeTrack(command) && marked(command) && asked(command) == 1, "under the prompt the worker is asked to place the input and injects nothing");
  expect(input.consume(answer(1, true, 16, 8), session) && sink.sent.size() == 1 && sink.sent[0].positional && sink.sent[0].x == 16 && sink.sent[0].y == 8, "the placed input goes to the helpers with its pixel");
  const auto written = JsonParser::parse(sink.sent[0].message);
  expect(written.get(placedX).number() == 16 && written.get(placedY).number() == 8 && written.get("event").string() == "move" && std::holds_alternative<std::nullptr_t>(written.get("track").value), "what is written is the client's event with the worker's pixel, nothing else of the worker's");
  command = key();
  expect(input.routeTrack(command) && asked(command) == 2 && input.consume(JsonParser::parse(trackAnswerJson(session, 2, true, std::nullopt)), session), "a key is placed too (no position)");
  expect(sink.sent.size() == 2 && !sink.sent[1].positional && JsonParser::parse(sink.sent[1].message).get("vk").number() == 65, "and goes to the helpers without one");
  // The worker refuses (an output that is gone, a plan the client has not acknowledged): dropped, never guessed.
  command = move();
  expect(input.routeTrack(command) && input.consume(answer(3, false, 0, 0), session) && sink.sent.size() == 2, "an input the worker did not place is dropped");
  // No helper shows that output: dropped and counted.
  sink.accept = false; command = move();
  expect(input.routeTrack(command) && input.consume(answer(4, true, 1, 1), session) && sink.sent.size() == 2, "an input no helper takes is dropped");
  // The prompt closes between the question and the answer: nothing injects it.
  sink.accept = true; command = move();
  expect(input.routeTrack(command), "asked while the prompt showed");
  sink.routing = false;
  expect(input.consume(answer(5, true, 1, 1), session) && sink.sent.size() == 2, "an answer after the prompt closed is dropped (there is no injector)");
  // input.reset: the helpers and, unmarked, the worker.
  sink.routing = true;
  auto reset = JsonParser::parse("{\"type\":\"input.reset\",\"sessionId\":\"" + session + "\"}");
  expect(!input.routeTrack(reset) && !marked(reset) && sink.resets.size() == 1, "a reset reaches the helpers and the worker");
  expect(input.counted(), "the service has counts to record");
  const auto counts = input.close();
  expect(counts.trackAsked == 5 && counts.secureRouted == 3 && counts.trackDropped == 3 && counts.routed == 0 && counts.fellBack == 0, "counts: asked 5, to helpers 2 + the reset, dropped 3");
}

struct Injector final : ServiceHelper {
  std::vector<std::string> got;
  bool alive() override { return true; }
  bool send(const std::string& json) override { got.push_back(json); return true; }
  void close() override {}
};
void inputWithInjector() {
  // A standard user's stream has the SYSTEM injector; under the prompt the helpers take its place.
  using namespace host::sysinput;
  ServiceSystemInput input; Sink sink; auto injector = std::make_shared<Injector>();
  input.tracks(true); input.attach(injector); input.secureSink(sink.sink());
  auto command = move();
  expect(input.routeTrack(command) && input.consume(answer(asked(command), true, 16, 8), session) && injector->got.size() == 1 && sink.sent.empty(), "on the user's desktop the injector takes the input");
  sink.routing = true; command = move();
  expect(input.routeTrack(command) && input.consume(answer(asked(command), true, 16, 8), session) && injector->got.size() == 1 && sink.sent.size() == 1, "under the prompt the helpers take it, not the injector");
  auto reset = JsonParser::parse("{\"type\":\"input.reset\",\"sessionId\":\"" + session + "\"}");
  expect(input.routeTrack(reset) && marked(reset) && sink.resets.size() == 1 && injector->got.size() == 2, "a reset reaches the helpers and the injector");
  sink.routing = false; command = key();
  expect(input.routeTrack(command) && input.consume(JsonParser::parse(trackAnswerJson(session, asked(command), true, std::nullopt)), session) && injector->got.size() == 3 && sink.sent.size() == 1, "after the prompt the injector again");
  const auto counts = input.close();
  expect(counts.routed == 3 && counts.secureRouted == 2 && counts.trackDropped == 0, "counts: injector 3, helpers 2");
}

// The whole path with the service's dispatch: the pixel the worker placed picks the helper.
void inputEndToEnd() {
  using namespace host::sysinput;
  Service s; auto tracks = std::make_shared<ServiceSecureTracks>(session, s.env());
  ServiceSystemInput input; input.tracks(true);
  input.secureSink({[tracks] { return tracks->routing(); },
                    [tracks](const std::string& m, int32_t x, int32_t y, bool positional) { return tracks->send(m, x, y, positional); },
                    [tracks](const std::string& m) { return tracks->reset(m); }});
  (void)tracks->consume(request(0, 1)); (void)tracks->consume(request(1, 1));
  (void)tracks->consume(control(controlActive, 0)); (void)tracks->consume(control(controlActive, 1));
  auto command = move();
  expect(input.routeTrack(command) && input.consume(answer(asked(command), true, -5, 4), session) && s.helpers[0]->got.size() == 1 && s.helpers[1]->got.empty(), "a pixel on output 0 reaches helper 0 only");
  command = move();
  expect(input.routeTrack(command) && input.consume(answer(asked(command), true, 5, 4), session) && s.helpers[1]->got.size() == 1, "a pixel on output 1 reaches helper 1 only");
  // What the helper then does with it: only a pixel on its own output becomes a position.
  const auto onOwn = placeOnDesktop(JsonParser::parse(s.helpers[1]->got[0]), Area{0, 0, int32_t(W), int32_t(H)});
  const auto onOther = placeOnDesktop(JsonParser::parse(s.helpers[0]->got[0]), Area{0, 0, int32_t(W), int32_t(H)});
  expect(onOwn && onOwn->get("x").number() == 5.0 / (W - 1) && !onOther, "a helper takes a pixel on its own output and refuses any other");
  tracks->close();
  command = move();
  expect(!input.routeTrack(command) && !marked(command), "after the helpers ended the worker injects again");
}
}  // namespace

int main() {
  workerSwitches();
  workerRefusals();
  serviceTracks();
  inputWithoutInjector();
  inputWithInjector();
  inputEndToEnd();
  if (failures) { std::cerr << "secure desktop tracks: " << failures << " of " << checks << " checks FAILED\n"; return 1; }
  std::cout << "secure desktop tracks: ok (" << checks << " checks)\n";
  return 0;
}
