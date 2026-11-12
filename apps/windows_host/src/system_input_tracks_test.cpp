// System input on independent outputs (system_input.hpp, system_input_tracks.hpp)
// with the real track plan and fakes for the injector: the service keeps the
// client's event, the worker's plan places it, the injector gets the client's
// event at that desktop pixel. Geometry for two and three outputs (negative
// origins, different sizes), the hand-over between outputs, a plan that changes
// (1 -> 2 -> 1 outputs), input for an output that just went away, answers the
// worker may not give, and the counts. No SYSTEM process, no input to Windows.
#include "remote_pointer.hpp"
#include "secure_desktop_capture.hpp"
#include "system_input.hpp"
#include "system_input_tracks.hpp"
#include "track_input.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
int failures = 0, checks = 0;
void expect(bool value, const char* what) { ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; } }
using namespace host;
using namespace host::sysinput;

const std::string kSession = "00000000-0000-4000-8000-0000000000d1";

struct FakeHelper final : secure::ServiceHelper {
  bool running = true, accepts = true, closed = false;
  std::vector<std::string> sent;
  bool alive() override { return running && !closed; }
  bool send(const std::string& json) override { if (!accepts) return false; sent.push_back(json); return true; }
  void close() override { closed = true; }
};

struct Output { uint8_t id; RECT rect; uint32_t epoch; };
TrackSourcePlan makePlan(uint32_t topology, const std::vector<Output>& outputs) {
  TrackSourcePlan plan;
  plan.sessionId = kSession; plan.topology = topology;
  std::vector<RECT> rects;
  for (const auto& output : outputs) {
    OwnedTrackSource source;
    source.media.id = output.id; source.media.epoch = output.epoch;
    source.capture.bounds = output.rect;
    plan.outputs.push_back(source);
    rects.push_back(output.rect);
  }
  plan.inputArea = secure::unionOf(rects);
  return plan;
}
std::unique_ptr<TrackSourceInput> ready(const TrackSourcePlan& plan, bool relative = false) {
  auto input = std::make_unique<TrackSourceInput>(plan, relative);
  input->encodedReady();
  std::string tracks;
  for (const auto& output : plan.outputs) { if (!tracks.empty()) tracks += ','; tracks += "{\"id\":\"" + std::to_string(output.media.id) + "\",\"epoch\":" + std::to_string(output.media.epoch) + "}"; }
  const bool ok = input->acknowledge(JsonParser::parse("{\"type\":\"stream.tracks.ready\",\"sessionId\":\"" + kSession + "\",\"topologyEpoch\":" + std::to_string(plan.topology) + ",\"tracks\":[" + tracks + "]}"));
  expect(ok, "fixture: the client acknowledged the plan");
  return input;
}
std::string head(uint32_t topology) { return "{\"type\":\"input\",\"sessionId\":\"" + kSession + "\",\"topologyEpoch\":" + std::to_string(topology) + ","; }
std::string at(uint32_t topology, const char* event, int track, uint32_t epoch, double x, double y, const std::string& more = "") {
  return head(topology) + "\"event\":\"" + event + "\",\"track\":\"" + std::to_string(track) + "\",\"epoch\":" + std::to_string(epoch) + ",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + more + "}";
}

// One stream: the service, the worker's plan and the injector's link, wired as the product wires them.
struct Stream {
  ServiceSystemInput service;
  std::shared_ptr<FakeHelper> injector = std::make_shared<FakeHelper>();
  TrackSourceInput* plan = nullptr;
  std::vector<std::string> answers;
  Stream() { service.tracks(true); service.attach(injector); }
  // The client's message through the tunnel: service -> worker -> service -> injector. Returns what the
  // injector got for it (empty: nothing).
  std::optional<Json> send(const std::string& client) {
    const size_t before = injector->sent.size();
    auto command = JsonParser::parse(client);
    if (!service.routeTrack(command)) return {};
    if (command.get("type").string() == "input") {
      const auto seq = uint32_t(command.get(workerSeq).number());
      (void)answerTrackInput(plan, command, seq, kSession, [&](const std::string& answer) { answers.push_back(answer); (void)service.consume(JsonParser::parse(answer), kSession); });
    }
    if (injector->sent.size() == before) return {};
    return JsonParser::parse(injector->sent.back());
  }
};
// Where the injector puts a placed input: desktop pixel -> fractions of the physical desktop -> SendInput's
// absolute position -> pixel again.
std::optional<POINT> landed(const Json& injected, const RECT& desktop) {
  const auto placed = placeOnDesktop(injected, Area{desktop.left, desktop.top, desktop.right, desktop.bottom});
  if (!placed) return {};
  const auto input = planRemotePointerInput(*placed, desktop, [desktop] { return desktop; });
  if (!input || !(input->input.mi.dwFlags & MOUSEEVENTF_ABSOLUTE)) return {};
  return POINT{LONG(std::lround(desktop.left + double(input->input.mi.dx) * double(desktop.right - desktop.left - 1) / 65535.0)),
               LONG(std::lround(desktop.top + double(input->input.mi.dy) * double(desktop.bottom - desktop.top - 1) / 65535.0))};
}
bool lands(const std::optional<POINT>& point, LONG x, LONG y) { return point && std::abs(point->x - x) <= 1 && std::abs(point->y - y) <= 1; }

void twoOutputs() {
  // Different sizes, side by side.
  const RECT left{0, 0, 1920, 1080}, right{1920, 0, 1920 + 2560, 1440};
  const auto plan = makePlan(1, {{0, left, 1}, {1, right, 1}});
  const RECT desktop = plan.inputArea;
  auto input = ready(plan);
  Stream stream; stream.plan = input.get();
  auto got = stream.send(at(1, "move", 0, 1, 0.5, 0.5));
  expect(got && got->get("type").string() == "input" && got->get("event").string() == "move" && got->get(placedX).number() == 960 && got->get(placedY).number() == 540, "a move on the first output is that output's pixel");
  expect(got && !std::get_if<Json::Object>(&got->value)->contains("track") && !std::get_if<Json::Object>(&got->value)->contains("x") && !std::get_if<Json::Object>(&got->value)->contains(workerSeq), "the injector gets the event and a pixel, nothing of the track protocol");
  expect(got && lands(landed(*got, desktop), 960, 540), "and the injector lands on it");
  got = stream.send(at(1, "move", 1, 1, 0.5, 0.5));
  expect(got && lands(landed(*got, desktop), 1920 + 1280, 720), "a move on the second output lands on the second output");
  got = stream.send(at(1, "move", 1, 1, 1.0, 1.0));
  expect(got && lands(landed(*got, desktop), 1920 + 2559, 1439), "its far corner is its far corner");
  // The hand-over between the outputs: the last pixel of one and the first of the next are neighbours.
  const auto edgeA = stream.send(at(1, "move", 0, 1, 1.0, 0.25)), edgeB = stream.send(at(1, "move", 1, 1, 0.0, 0.25 * 1079 / 1439));
  expect(edgeA && edgeB && edgeA->get(placedX).number() == 1919 && edgeB->get(placedX).number() == 1920 && std::abs(edgeA->get(placedY).number() - edgeB->get(placedY).number()) <= 1, "crossing from one output to the next does not jump");
  // A click and a wheel carry the client's own content and the worker's pixel.
  got = stream.send(at(1, "button", 1, 1, 0.25, 0.75, ",\"button\":\"left\",\"down\":true"));
  expect(got && got->get("event").string() == "button" && got->get("button").string() == "left" && got->get("down").boolean() && lands(landed(*got, desktop), 1920 + LONG(std::lround(0.25 * 2559)), LONG(std::lround(0.75 * 1439))), "a click is the client's button at the placed pixel");
  got = stream.send(at(1, "wheel", 0, 1, 0.5, 0.5, ",\"delta\":-120"));
  expect(got && got->get("event").string() == "wheel" && got->get("delta").number() == -120 && got->get(placedX).number() == 960, "a wheel is the client's delta at the placed pixel");
  // A key has no position.
  got = stream.send(head(1) + R"("event":"key","vk":65,"scan":30,"down":true,"extended":false})");
  expect(got && got->get("event").string() == "key" && got->get("vk").number() == 65 && got->get("scan").number() == 30 && got->get("down").boolean() && !std::get_if<Json::Object>(&got->value)->contains(placedX), "a key goes through with the client's own fields and no position");
  // Text and touch are not input of independent outputs: the plan refuses them, nothing is injected.
  expect(!stream.send(head(1) + R"("event":"text","text":"a"})") && !stream.send(head(1) + R"("event":"touch","contacts":[]})"), "text and touch are refused on independent outputs, as before");
  // Without the game-mouse grant relative movement is refused.
  expect(!stream.send(head(1) + R"("event":"moveRelative","dx":4,"dy":2})"), "relative movement needs the session's grant");
  const auto counts = stream.service.close();
  expect(counts.trackAsked == 11 && counts.trackDropped == 3 && counts.routed == 8, "counts: 11 asked, 8 injected, 3 dropped");
}

void threeOutputs() {
  // Negative origins, three sizes; one is above the others' top edge.
  const RECT a{-1280, 0, 0, 1024}, b{0, 0, 2560, 1440}, c{2560, -200, 2560 + 3840, 1960};
  const auto plan = makePlan(4, {{0, a, 2}, {1, b, 2}, {2, c, 2}});
  const RECT desktop = plan.inputArea;
  expect(desktop.left == -1280 && desktop.top == -200 && desktop.right == 6400 && desktop.bottom == 1960, "the desktop is the union of the three outputs");
  auto input = ready(plan);
  Stream stream; stream.plan = input.get();
  struct Case { int track; RECT rect; };
  bool all = true;
  for (const Case& item : {Case{0, a}, Case{1, b}, Case{2, c}}) {
    for (const auto& [x, y] : std::vector<std::pair<double, double>>{{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {1.0, 1.0}, {0.5, 0.5}, {0.123, 0.877}}) {
      const auto got = stream.send(at(4, "move", item.track, 2, x, y));
      const LONG px = LONG(std::lround(item.rect.left + x * (item.rect.right - item.rect.left - 1))), py = LONG(std::lround(item.rect.top + y * (item.rect.bottom - item.rect.top - 1)));
      all &= got && got->get(placedX).number() == px && got->get(placedY).number() == py && lands(landed(*got, desktop), px, py);
    }
  }
  expect(all, "every corner and inner point of three outputs (negative origins, different sizes) lands on its own output");
  expect(stream.service.close().trackDropped == 0, "nothing was dropped");
}

void gameMouse() {
  const RECT left{0, 0, 1920, 1080}, right{1920, 0, 3840, 1080};
  const auto plan = makePlan(1, {{0, left, 1}, {1, right, 1}});
  auto input = ready(plan, true);
  Stream stream; stream.plan = input.get();
  auto got = stream.send(head(1) + R"("event":"moveRelative","dx":7,"dy":-3})");
  expect(got && got->get("event").string() == "moveRelative" && got->get("dx").number() == 7 && got->get("dy").number() == -3 && !std::get_if<Json::Object>(&got->value)->contains(placedX), "game mouse movement is the client's own, without a position");
  got = stream.send(head(1) + R"("event":"button","button":"right","down":true})");
  expect(got && got->get("button").string() == "right" && !std::get_if<Json::Object>(&got->value)->contains(placedX), "a button at the current cursor has no position");
  got = stream.send(head(1) + R"("event":"wheel","delta":240})");
  expect(got && got->get("delta").number() == 240, "a wheel at the current cursor");
  got = stream.send(at(1, "move", 1, 1, 0.5, 0.5));
  expect(got && got->get(placedX).number() == 1920 + 960, "positions still work while the grant is there");
  expect(!stream.send(head(1) + R"("event":"moveRelative","dx":0,"dy":0})") && !stream.send(head(1) + R"("event":"moveRelative","dx":99999,"dy":0})"), "empty or oversized movement is refused by the plan");
}

void planChanges() {
  const RECT one{0, 0, 1920, 1080}, second{1920, 0, 3840, 1080};
  Stream stream;
  // 1 output.
  auto planA = makePlan(1, {{0, one, 1}});
  auto inputA = ready(planA);
  stream.plan = inputA.get();
  expect(stream.send(at(1, "move", 0, 1, 0.5, 0.5)).has_value(), "one output: placed");
  expect(!stream.send(at(1, "move", 1, 1, 0.5, 0.5)), "an output the plan does not have is refused, not guessed");
  // -> 2 outputs: a new plan with a new topology. The old plan is closed first, as the worker does.
  inputA->close();
  auto planB = makePlan(2, {{0, one, 2}, {1, second, 1}});
  auto inputB = std::make_unique<TrackSourceInput>(planB);
  inputB->encodedReady();
  stream.plan = inputB.get();
  expect(!stream.send(at(2, "move", 1, 1, 0.5, 0.5)), "the new plan places nothing before the client acknowledged it");
  expect(inputB->acknowledge(JsonParser::parse("{\"type\":\"stream.tracks.ready\",\"sessionId\":\"" + kSession + "\",\"topologyEpoch\":2,\"tracks\":[{\"id\":\"0\",\"epoch\":2},{\"id\":\"1\",\"epoch\":1}]}")), "the client acknowledges two outputs");
  auto got = stream.send(at(2, "move", 1, 1, 0.5, 0.5));
  expect(got && got->get(placedX).number() == 1920 + 960, "two outputs: the second one is placed");
  expect(!stream.send(at(1, "move", 0, 1, 0.5, 0.5)), "an input still carrying the old topology is refused");
  expect(!stream.send(at(2, "move", 0, 1, 0.5, 0.5)), "an input carrying the output's old epoch is refused");
  // An application changes the second output's mode and position: positions follow at once.
  inputB->follow(1, RECT{1920, 0, 1920 + 1280, 720}, {});
  got = stream.send(at(2, "move", 1, 1, 1.0, 1.0));
  expect(got && got->get(placedX).number() == 1920 + 1279 && got->get(placedY).number() == 719, "after a mode change the same fraction lands on the output's new rectangle");
  // -> 1 output again. An input for the output that just went away is on its way.
  inputB->close();
  expect(!stream.send(at(2, "move", 1, 1, 0.5, 0.5)), "a closed plan places nothing");
  auto planC = makePlan(3, {{0, one, 3}});
  auto inputC = ready(planC);
  stream.plan = inputC.get();
  expect(!stream.send(at(2, "move", 1, 1, 0.5, 0.5)) && !stream.send(at(3, "move", 1, 1, 0.5, 0.5)), "input for the output that disappeared is dropped, under the old and the new topology");
  got = stream.send(at(3, "button", 0, 3, 0.5, 0.5, ",\"button\":\"left\",\"down\":false"));
  expect(got && got->get(placedX).number() == 960 && !got->get("down").boolean(), "the remaining output works on");
  // No plan at all (the stream between two plans): refused.
  stream.plan = nullptr;
  expect(!stream.send(at(3, "move", 0, 3, 0.5, 0.5)), "without a plan nothing is placed");
  const auto counts = stream.service.close();
  expect(counts.trackAsked == 12 && counts.routed == 4 && counts.trackDropped == 8, "every refused input is counted as dropped (8 of 12)");
  // Every answer the worker gave is positions only.
  bool positionsOnly = true;
  for (const auto& answer : stream.answers) {
    const auto value = JsonParser::parse(answer);
    for (const auto& [name, field] : *std::get_if<Json::Object>(&value.value)) { (void)field; positionsOnly &= name == "type" || name == "sessionId" || name == "seq" || name == "ok" || name == placedX || name == placedY; }
  }
  expect(positionsOnly && stream.answers.size() == 12, "the worker's answers carry a sequence number, ok and a pixel, nothing else");
}

// What a worker may not do, whatever it sends.
void workerCannot() {
  using Clock = ServiceSystemInput::Clock;
  const auto answer = [](uint32_t seq, bool ok, const std::string& more = "") { return JsonParser::parse("{\"type\":\"worker.input.track\",\"sessionId\":\"" + kSession + "\",\"seq\":" + std::to_string(seq) + ",\"ok\":" + (ok ? "true" : "false") + more + "}"); };
  const auto ask = [](ServiceSystemInput& service, const std::string& client, Clock::time_point now = Clock::now()) { auto command = JsonParser::parse(client); return service.routeTrack(command, now) ? uint32_t(command.get(workerSeq).number()) : 0u; };
  // Choose the event: an answer's own "event", "vk" or "button" is never used.
  {
    ServiceSystemInput service; auto injector = std::make_shared<FakeHelper>(); service.tracks(true); service.attach(injector);
    const auto seq = ask(service, at(1, "move", 0, 1, 0.5, 0.5));
    expect(service.consume(answer(seq, true, ",\"px\":10,\"py\":20,\"event\":\"key\",\"vk\":13,\"down\":true,\"button\":\"left\""), kSession), "an answer is consumed");
    const auto got = injector->sent.size() == 1 ? JsonParser::parse(injector->sent[0]) : Json{};
    const auto* fields = std::get_if<Json::Object>(&got.value);
    expect(fields && fields->size() == 4 && got.get("event").string() == "move" && got.get(placedX).number() == 10 && got.get(placedY).number() == 20, "the injected event is the client's move at the answered pixel; nothing else of the answer is used");
    // Answer twice, or for nothing that waits.
    (void)service.consume(answer(seq, true, ",\"px\":900,\"py\":900"), kSession);
    (void)service.consume(answer(seq + 5, true, ",\"px\":900,\"py\":900"), kSession);
    (void)service.consume(answer(0, true, ",\"px\":1,\"py\":1"), kSession);
    expect(injector->sent.size() == 1, "a second answer, an answer for an input that does not wait, and seq 0 inject nothing");
    // Turn a key into a click, or a click into a move elsewhere without the client's content.
    const auto key = ask(service, head(1) + R"("event":"key","vk":65,"scan":30,"down":true,"extended":false})");
    (void)service.consume(answer(key, true, ",\"px\":5,\"py\":5"), kSession);
    const auto injectedKey = JsonParser::parse(injector->sent.back());
    expect(injector->sent.size() == 2 && injectedKey.get("event").string() == "key" && !std::get_if<Json::Object>(&injectedKey.value)->contains(placedX), "a position answered for a key is ignored: the key stays a key");
    // Leave out the position of a positional event, or give one that is not a whole number or far away.
    for (const char* more : {"", ",\"px\":10", ",\"px\":10.5,\"py\":3", ",\"px\":\"10\",\"py\":3", ",\"px\":900000,\"py\":3"}) {
      const auto next = ask(service, at(1, "button", 0, 1, 0.5, 0.5, ",\"button\":\"left\",\"down\":true"));
      (void)service.consume(answer(next, true, more), kSession);
    }
    expect(injector->sent.size() == 2, "a positional event without a usable position is dropped, never injected at the cursor");
    // Skip ahead: answering a later input drops the earlier ones it skipped.
    const auto first = ask(service, at(1, "move", 0, 1, 0.1, 0.1)), second = ask(service, at(1, "move", 0, 1, 0.2, 0.2)), third = ask(service, at(1, "move", 0, 1, 0.3, 0.3));
    (void)first; (void)second;
    (void)service.consume(answer(third, true, ",\"px\":33,\"py\":33"), kSession);
    expect(injector->sent.size() == 3 && JsonParser::parse(injector->sent.back()).get(placedX).number() == 33 && service.waiting() == 0, "answers are taken in order: skipped inputs are dropped, not injected later");
    (void)service.consume(answer(first, true, ",\"px\":11,\"py\":11"), kSession);
    expect(injector->sent.size() == 3, "an answer that comes after its turn injects nothing");
    // Another session's answer, a malformed one.
    const auto waiting = ask(service, at(1, "move", 0, 1, 0.5, 0.5));
    expect(service.consume(JsonParser::parse("{\"type\":\"worker.input.track\",\"sessionId\":\"other\",\"seq\":" + std::to_string(waiting) + ",\"ok\":true,\"px\":1,\"py\":1}"), kSession) &&
           service.consume(JsonParser::parse("{\"type\":\"worker.input.track\",\"sessionId\":\"" + kSession + "\",\"seq\":\"x\",\"ok\":true}"), kSession) &&
           service.consume(JsonParser::parse("{\"type\":\"worker.input.track\",\"sessionId\":\"" + kSession + "\",\"seq\":" + std::to_string(waiting) + ",\"ok\":\"yes\",\"px\":1,\"py\":1}"), kSession), "foreign and malformed answers are consumed (never forwarded)");
    expect(injector->sent.size() == 3, "and inject nothing");
    const auto counts = service.close();
    expect(counts.routed == 3 && counts.trackAsked == 11 && counts.trackDropped == 8, "counts: 11 asked, 3 injected, 8 dropped");
  }
  // The client's own marker fields never reach the worker as the service's.
  {
    ServiceSystemInput service; auto injector = std::make_shared<FakeHelper>(); service.tracks(true);
    auto forged = JsonParser::parse(head(1) + R"("event":"key","vk":65,"scan":30,"down":true,"extended":false,"systemInput":true,"systemSeq":7})");
    expect(!service.routeTrack(forged) && !std::get_if<Json::Object>(&forged.value)->contains(workerMarker) && !std::get_if<Json::Object>(&forged.value)->contains(workerSeq), "without an injector the worker injects, and the client's markers are removed");
    service.attach(injector);
    auto again = JsonParser::parse(head(1) + R"("event":"key","vk":65,"scan":30,"down":true,"extended":false,"systemSeq":7})");
    expect(service.routeTrack(again) && again.get(workerSeq).number() == 1 && again.get(workerMarker).boolean(), "with an injector the sequence number is the service's own");
    // The single-stream route leaves a stream of independent outputs alone.
    auto plain = JsonParser::parse(R"({"type":"input","event":"move","x":0.5,"y":0.5})");
    expect(!service.route(plain) && injector->sent.empty(), "the single-stream route is not used for independent outputs");
    // input.reset goes to the injector at once (it holds the keys) and on to the worker, marked.
    auto reset = JsonParser::parse("{\"type\":\"input.reset\",\"sessionId\":\"" + kSession + "\"}");
    expect(service.routeTrack(reset) && reset.get(workerMarker).boolean() && injector->sent.size() == 1 && JsonParser::parse(injector->sent[0]).get("type").string() == "input.reset", "input.reset reaches the injector");
    auto ping = JsonParser::parse(R"({"type":"ping","id":"1"})");
    expect(!service.routeTrack(ping) && !std::get_if<Json::Object>(&ping.value)->contains(workerMarker), "only input and input.reset are routed");
    // The injector is gone: the worker injects as before (unmarked), and placed answers that still arrive are dropped.
    injector->running = false;
    auto late = JsonParser::parse(head(1) + R"("event":"key","vk":65,"scan":30,"down":false,"extended":false})");
    expect(!service.routeTrack(late) && !std::get_if<Json::Object>(&late.value)->contains(workerMarker), "without a live injector the worker injects, unmarked");
    (void)service.consume(JsonParser::parse("{\"type\":\"worker.input.track\",\"sessionId\":\"" + kSession + "\",\"seq\":1,\"ok\":true}"), kSession);
    expect(injector->sent.size() == 1, "an answer for a dead injector injects nothing");
    const auto counts = service.close();
    expect(counts.fellBack == 1 && counts.trackDropped == 1, "both are counted");
  }
  // A worker that never answers: the service does not wait for it for ever.
  {
    ServiceSystemInput service; auto injector = std::make_shared<FakeHelper>(); service.tracks(true); service.attach(injector);
    const auto t0 = Clock::now();
    for (size_t i = 0; i < ServiceSystemInput::pendingLimit + 40; ++i) (void)ask(service, at(1, "move", 0, 1, 0.5, 0.5), t0);
    expect(service.waiting() == ServiceSystemInput::pendingLimit, "inputs waiting for a placement are bounded");
    (void)ask(service, at(1, "move", 0, 1, 0.5, 0.5), t0 + ServiceSystemInput::pendingAge);
    expect(service.waiting() == 1, "placements older than 2 s are not waited for");
    const auto counts = service.close();
    expect(counts.trackAsked == ServiceSystemInput::pendingLimit + 41 && counts.trackDropped == ServiceSystemInput::pendingLimit + 41 && counts.routed == 0, "every unanswered input is counted as dropped");
  }
}

// The injector's side: a pixel on the physical desktop, or nothing.
void injectorPlacement() {
  const Area desktop{-1280, -200, 6400, 1960};
  const auto place = [&](int px, int py) { return placeOnDesktop(JsonParser::parse("{\"type\":\"input\",\"event\":\"move\",\"px\":" + std::to_string(px) + ",\"py\":" + std::to_string(py) + "}"), desktop); };
  const auto corner = place(-1280, -200), opposite = place(6399, 1959);
  expect(corner && corner->get("x").number() == 0 && corner->get("y").number() == 0 && opposite && opposite->get("x").number() == 1 && opposite->get("y").number() == 1, "the desktop's corners are 0 and 1");
  expect(corner && !std::get_if<Json::Object>(&corner->value)->contains(placedX), "the pixel fields are replaced by fractions");
  expect(!place(6400, 0) && !place(0, 1960) && !place(-1281, 0) && !place(0, -201), "a pixel that is not on the desktop is refused, not moved onto it");
  expect(!placeOnDesktop(JsonParser::parse(R"({"type":"input","event":"move","px":1.5,"py":2})"), desktop) && !placeOnDesktop(JsonParser::parse(R"({"type":"input","event":"move","px":1})"), desktop) &&
         !placeOnDesktop(JsonParser::parse(R"({"type":"input","event":"move","px":1,"py":2})"), Area{0, 0, 1, 1}), "malformed pixels and an empty desktop are refused");
  // The assembled message for each kind.
  const auto client = JsonParser::parse(at(1, "button", 1, 1, 0.5, 0.5, ",\"button\":\"left\",\"down\":true"));
  const auto message = trackInjectorMessage(client, std::pair<int32_t, int32_t>{100, 200});
  expect(message && *message == R"({"button":"left","down":true,"event":"button","px":100,"py":200,"type":"input"})", "the injector's message is the client's content and the pixel, exactly");
  expect(!trackInjectorMessage(client, std::nullopt) && !trackInjectorMessage(JsonParser::parse(R"({"type":"input","event":"text","text":"a"})"), std::nullopt) &&
         !trackInjectorMessage(JsonParser::parse(R"({"type":"ping"})"), std::nullopt) && !trackInjectorMessage(JsonParser::parse(R"([1])"), std::nullopt), "no position, another kind or another type: no message");
  expect(trackAnswerJson("s", 7, true, std::pair<int32_t, int32_t>{-5, 9}) == R"({"type":"worker.input.track","sessionId":"s","seq":7,"ok":true,"px":-5,"py":9})" &&
         trackAnswerJson("s", 8, false, std::pair<int32_t, int32_t>{1, 1}) == R"({"type":"worker.input.track","sessionId":"s","seq":8,"ok":false})" &&
         trackAnswerJson("s", 9, true, std::nullopt) == R"({"type":"worker.input.track","sessionId":"s","seq":9,"ok":true})", "the worker's answer is a sequence number, ok and at most a pixel");
}

}  // namespace

int main() {
  twoOutputs();
  threeOutputs();
  gameMouse();
  planChanges();
  workerCannot();
  injectorPlacement();
  if (failures) { std::cerr << failures << " failure(s) of " << checks << " checks\n"; return 1; }
  std::cout << "system input tracks: ok (" << checks << " checks)\n";
  return 0;
}
