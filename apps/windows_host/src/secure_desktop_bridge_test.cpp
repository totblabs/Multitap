// The Role::secure bridge with a fake helper: the frame section (seqlock,
// heartbeat, last word), the BGRA scaling, the worker's source switch fed by a
// section the test writes like the helper does, the service's dispatch and input
// routing with a fake launch, the helper's control loop with fake capture and
// injection, and one end-to-end run of all of them over a real unnamed section
// and the service's real input pipe. No SYSTEM process, no secure desktop, no
// policy: the real launch is only exercised live.
#include "secure_desktop_helper.hpp"
#include "secure_desktop_ipc.hpp"
#include "secure_desktop_launch.hpp"
#include "secure_desktop_scale.hpp"
#include "secure_desktop_scope.hpp"
#include "secure_desktop_service.hpp"
#include "secure_desktop_switch.hpp"
#include "secure_desktop_transport.hpp"
#include "secure_desktop_worker.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {
int failures = 0, checks = 0;
void expect(bool value, const char* what) { ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; } }
using namespace host::secure;
using host::Json;
using host::JsonParser;
using Clock = WorkerSwitch::Clock;

constexpr uint32_t W = 32, H = 16;
std::vector<uint8_t> picture(uint8_t value) { return std::vector<uint8_t>(ipc::frameBytes(W, H), value); }
// A buffer that plays the shared section; the test writes it like the helper.
struct FakeSection {
  std::shared_ptr<std::vector<uint8_t>> bytes = std::make_shared<std::vector<uint8_t>>(ipc::sectionBytes(W, H), uint8_t(0));
  FakeSection() { ipc::writerInit(bytes->data(), W, H); }
  uint8_t* data() { return bytes->data(); }
  std::shared_ptr<const uint8_t> view() const { return std::shared_ptr<const uint8_t>(bytes, bytes->data()); }
};
bool hasRecord(const WorkerSwitch::Tick& tick, uint32_t record) {
  for (unsigned i = 0; i < tick.recordCount; ++i) if (tick.records[i] == record) return true;
  return false;
}

void sectionLayout() {
  expect(sizeof(ipc::FrameHeader) == 64 && ipc::sectionBytes(W, H) == 64 + size_t(W) * H * 4, "section = 64-byte header + BGRA");
  expect(ipc::sizeValid(16, 16) && ipc::sizeValid(8192, 8192) && !ipc::sizeValid(15, 100) && !ipc::sizeValid(100, 8193), "side bounds");
  FakeSection section;
  ipc::Reader reader{section.data(), W, H, 0};
  std::vector<uint8_t> frame, scratch;
  expect(reader.ready() && !reader.read(frame, scratch), "no frame before the helper wrote one");
  ipc::writeFrame(section.data(), picture(7).data(), W, H);
  expect(reader.read(frame, scratch) && frame == picture(7), "a complete frame is read");
  expect(!reader.read(frame, scratch), "the same frame is not read twice");
  ipc::writeFrame(section.data(), picture(9).data(), W, H);
  expect(reader.read(frame, scratch) && frame == picture(9), "the next frame is read");
  // A frame being written (odd seq) is never handed out; the last good one stays.
  auto* header = reinterpret_cast<ipc::FrameHeader*>(section.data());
  header->seq = header->seq | 1u;
  std::memset(section.data() + sizeof(ipc::FrameHeader), 0x55, 100);
  expect(!reader.read(frame, scratch) && frame == picture(9), "a frame being written is not read");
  header->seq = header->seq + 1u;
  expect(reader.read(frame, scratch), "it is read once complete");
  // Geometry the worker did not ask for is not a frame.
  ipc::Reader other{section.data(), W + 2, H, 0};
  expect(!other.ready() && !other.read(frame, scratch), "another size is refused");
  ipc::Reader none;
  expect(!none.ready() && none.alive() == 0 && none.state() == ipc::stateRunning, "no view reads nothing");
  const uint32_t before = reader.alive();
  ipc::writeAlive(section.data());
  expect(reader.alive() == before + 1, "heartbeat advances");
  ipc::writeState(section.data(), ipc::stateEnded);
  expect(reader.state() == ipc::stateEnded, "the helper's last word is read");
}

void scaling() {
  std::vector<uint8_t> src(2 * 2 * 4), dst(2 * 2 * 4, 0);
  for (size_t i = 0; i < src.size(); ++i) src[i] = uint8_t(i + 1);
  scaleBgra(src.data(), 2, 2, dst.data(), 2, 2);
  expect(dst == src, "same-size scale copies exactly");
  // 4x2 -> 2x1: every destination pixel is the average of the 2x2 block it covers.
  std::vector<uint8_t> big(4 * 2 * 4), shrunk(2 * 1 * 4, 0);
  for (uint32_t p = 0; p < 8; ++p) { big[p * 4 + 0] = uint8_t(p * 10); big[p * 4 + 1] = 100; big[p * 4 + 2] = 0; big[p * 4 + 3] = 255; }
  scaleBgra(big.data(), 4, 2, shrunk.data(), 2, 1);
  expect(shrunk[0] == (0 + 10 + 40 + 50 + 2) / 4 && shrunk[4] == (20 + 30 + 60 + 70 + 2) / 4 && shrunk[1] == 100 && shrunk[3] == 255, "shrinking averages the covered block");
  // 1x1 -> 2x2 repeats the pixel.
  std::vector<uint8_t> one{9, 8, 7, 255}, four(2 * 2 * 4, 0);
  scaleBgra(one.data(), 1, 1, four.data(), 2, 2);
  bool replicated = true;
  for (int q = 0; q < 4; ++q) replicated &= four[q * 4] == 9 && four[q * 4 + 1] == 8 && four[q * 4 + 2] == 7;
  expect(replicated, "enlarging repeats the pixel");
  // An encoder size that differs from the output in both directions and by odd
  // ratios: a flat picture stays flat and nothing is written past the buffer.
  for (const auto& [sw, sh, dw, dh] : {std::tuple{1920u, 1080u, 1280u, 720u}, std::tuple{1366u, 768u, 1920u, 1080u}, std::tuple{2560u, 1600u, 1706u, 1066u}, std::tuple{33u, 17u, 16u, 16u}}) {
    std::vector<uint8_t> a(size_t(sw) * sh * 4, 200), b(size_t(dw) * dh * 4 + 16, 0x11);
    scaleBgra(a.data(), sw, sh, b.data(), dw, dh);
    bool flat = true;
    for (size_t i = 0; i < size_t(dw) * dh * 4; ++i) flat &= b[i] == 200;
    bool guard = true;
    for (size_t i = size_t(dw) * dh * 4; i < b.size(); ++i) guard &= b[i] == 0x11;
    expect(flat && guard, "scaled picture is complete and stays inside its buffer");
  }
  // A recognizable edge survives a 3:2 shrink at the right place.
  std::vector<uint8_t> edge(48 * 16 * 4, 0), out(32 * 16 * 4, 0);
  for (uint32_t y = 0; y < 16; ++y) for (uint32_t x = 24; x < 48; ++x) std::memset(edge.data() + (size_t(y) * 48 + x) * 4, 255, 4);
  scaleBgra(edge.data(), 48, 16, out.data(), 32, 16);
  expect(out[(8 * 32 + 10) * 4] == 0 && out[(8 * 32 + 20) * 4] == 255, "the picture keeps its geometry when the encoder is smaller");
}

void sourceSwitch() {
  const auto t0 = Clock::now();
  const auto ms = [&](int value) { return t0 + std::chrono::milliseconds(value); };
  // notice -> helper frames -> the desktop returns.
  {
    WorkerSwitch s; FakeSection section;
    auto tick = s.tick(ms(0), true);
    expect(tick.request && tick.seq == 1 && tick.hold && tick.recordCount == 0 && !tick.stop && !tick.activated, "first tick: one request, the picture is held (no notice flash), no record yet");
    tick = s.tick(ms(16), true);
    expect(!tick.request && tick.hold && tick.recordCount == 0, "waiting for the service: still held, nothing repeated");
    expect(s.attach(section.view(), W, H, 1), "the service's section is attached");
    tick = s.tick(ms(32), true);
    expect(tick.hold && !tick.activated, "an attached section without a frame is still held");
    ipc::writeFrame(section.data(), picture(42).data(), W, H);
    tick = s.tick(ms(48), true);
    expect(!tick.hold && !tick.notice && tick.fresh && tick.activated && tick.sourceChanged && hasRecord(tick, record244HelperActive), "the first helper frame switches the picture (record 5, IDR, active)");
    expect(s.frame() == picture(42) && s.frameWidth() == W && s.frameHeight() == H, "the picture is the helper's frame");
    ipc::writeAlive(section.data());
    tick = s.tick(ms(64), true);
    expect(!tick.notice && !tick.fresh && !tick.activated && !tick.sourceChanged && tick.recordCount == 0, "a still prompt keeps the last frame");
    ipc::writeFrame(section.data(), picture(43).data(), W, H);
    tick = s.tick(ms(80), true);
    expect(!tick.notice && tick.fresh && s.frame() == picture(43), "the next frame follows");
    expect(s.reset(), "the desktop returned: the helper is told to stop");
    expect(!s.reset() && s.phase() == HelperPhase::idle && s.frame().empty(), "nothing is left of the episode");
    tick = s.tick(ms(5000), true);
    expect(tick.request && tick.seq == 2 && tick.recordCount == 0, "a later prompt starts a new episode with a new request");
  }
  // A still prompt: no frames for a long time, but the heartbeat goes on.
  {
    WorkerSwitch s; FakeSection section;
    (void)s.tick(ms(0), true); s.attach(section.view(), W, H, 1);
    ipc::writeFrame(section.data(), picture(1).data(), W, H);
    (void)s.tick(ms(10), true);
    bool stayed = true;
    for (int at = 100; at <= 9000; at += 100) { ipc::writeAlive(section.data()); stayed &= !s.tick(ms(at), true).notice; }
    expect(stayed, "a prompt nobody touches for 9 s still shows (heartbeat, not frames)");
  }
  // helper lost (killed: the heartbeat stops) -> the notice, no end, no relaunch.
  {
    WorkerSwitch s; FakeSection section;
    (void)s.tick(ms(0), true); s.attach(section.view(), W, H, 1);
    ipc::writeFrame(section.data(), picture(1).data(), W, H);
    (void)s.tick(ms(10), true);
    auto tick = s.tick(ms(1900), true);
    expect(!tick.notice, "within the heartbeat limit the frame stays");
    tick = s.tick(ms(2100), true);
    expect(tick.notice && !tick.hold && tick.stop && tick.sourceChanged && hasRecord(tick, record244HelperLost), "a silent helper falls back to the notice (record 7, stop, IDR)");
    ipc::writeFrame(section.data(), picture(2).data(), W, H);
    tick = s.tick(ms(2200), true);
    expect(tick.notice && !tick.request && !tick.stop && tick.recordCount == 0 && s.phase() == HelperPhase::failed, "a failed episode stays on the notice");
  }
  // The helper's own last word "failed" (scope lost) while it streamed.
  {
    WorkerSwitch s; FakeSection section;
    (void)s.tick(ms(0), true); s.attach(section.view(), W, H, 1);
    ipc::writeFrame(section.data(), picture(1).data(), W, H);
    (void)s.tick(ms(10), true);
    ipc::writeState(section.data(), ipc::stateFailed);
    const auto tick = s.tick(ms(20), true);
    expect(tick.notice && !tick.hold && tick.stop && hasRecord(tick, record244HelperLost), "a helper that gave up is the notice at once (record 7)");
  }
  // no frame within the deadline -> the notice (record 8).
  {
    WorkerSwitch s; FakeSection section;
    (void)s.tick(ms(0), true); s.attach(section.view(), W, H, 1);
    auto tick = s.tick(ms(1900), true);
    expect(tick.hold && !tick.stop && tick.recordCount == 0, "still waiting inside the deadline");
    tick = s.tick(ms(2000), true);
    expect(tick.notice && !tick.hold && !tick.sourceChanged && tick.stop && hasRecord(tick, record244HelperNoFrame), "no frame in 2 s: the notice (record 8) and the helper is stopped");
  }
  // The service never answered at all: the same deadline.
  {
    WorkerSwitch s;
    (void)s.tick(ms(0), true);
    const auto tick = s.tick(ms(2000), true);
    expect(tick.notice && !tick.hold && tick.stop && hasRecord(tick, record244HelperNoFrame), "no answer in 2 s: the notice (record 8)");
  }
  // The service refused (worker.secure.failed) -> record 6.
  {
    WorkerSwitch s;
    (void)s.tick(ms(0), true);
    s.denied(7);
    expect(s.tick(ms(10), true).recordCount == 0, "a refusal of another request changes nothing");
    s.denied(1);
    const auto tick = s.tick(ms(20), true);
    expect(tick.notice && !tick.hold && tick.stop && hasRecord(tick, record244HelperLaunchFail), "a refused launch is the notice (record 6)");
  }
  // Not the service's worker (or several outputs): the notice alone, no request.
  {
    WorkerSwitch s;
    const auto tick = s.tick(ms(0), false);
    expect(tick.notice && !tick.hold && !tick.request && !tick.stop && tick.recordCount == 1 && hasRecord(tick, record244NoticeShown), "no helper for this stream: the notice at once with record 1, as before the helper existed");
    const auto next = s.tick(ms(200), false);
    expect(next.notice && !next.hold && next.recordCount == 0 && !next.request, "and it stays the notice without repeating the record");
    expect(!s.reset(), "nothing to stop");
  }
  // Answers to an older request are dropped.
  {
    WorkerSwitch s; FakeSection old, current;
    (void)s.tick(ms(0), true); (void)s.reset();
    const auto tick = s.tick(ms(100), true);
    expect(tick.request && tick.seq == 2, "second episode asks with seq 2");
    expect(!s.attach(old.view(), W, H, 1), "a late answer to request 1 is refused");
    expect(s.attach(current.view(), W, H, 2), "the answer to request 2 is taken");
    expect(!s.attach(current.view(), W, 4, 2), "a size out of bounds is refused");
  }
  // A new capture loop of the same worker (a resize) goes on counting, so an
  // answer to the old loop's request is never taken.
  {
    WorkerSwitch s(41); FakeSection stale;
    const auto tick = s.tick(ms(0), true);
    expect(tick.request && tick.seq == 42 && s.seq() == 42, "request numbers continue across capture loops");
    expect(!s.attach(stale.view(), W, H, 41) && !s.attach(stale.view(), W, H, 1), "answers to the earlier loop are refused");
  }
  // The helper ended by itself (it saw the Default desktop) while this worker
  // still sees a secure desktop (two prompts in a row): one more helper.
  {
    WorkerSwitch s; FakeSection first, second;
    (void)s.tick(ms(0), true); s.attach(first.view(), W, H, 1);
    ipc::writeFrame(first.data(), picture(1).data(), W, H);
    (void)s.tick(ms(10), true);
    ipc::writeState(first.data(), ipc::stateEnded);
    auto tick = s.tick(ms(20), true);
    expect(tick.hold && tick.stop && tick.recordCount == 0, "a helper that ended by itself: stop, no failure record, the picture is held for the next one");
    tick = s.tick(ms(30), true);
    expect(tick.request && tick.seq == 2 && tick.hold && tick.recordCount == 0, "the next tick asks again (the notice record is not repeated)");
    s.attach(second.view(), W, H, 2);
    ipc::writeFrame(second.data(), picture(5).data(), W, H);
    tick = s.tick(ms(40), true);
    expect(!tick.notice && tick.activated && s.frame() == picture(5), "the second prompt streams too");
  }
}

// A helper the service "launched": records what it was sent and whether it was closed.
struct FakeHelper final : ServiceHelper {
  bool running = true, accepts = true, closed = false;
  std::vector<std::string> sent;
  bool alive() override { return running && !closed; }
  bool send(const std::string& json) override { if (!accepts) return false; sent.push_back(json); return true; }
  void close() override { closed = true; }
};
struct FakeService {
  bool eligible = true, launchWorks = true, workerTakes = true;
  uint64_t now = 1000;
  int revoked = 0;
  std::vector<std::shared_ptr<FakeHelper>> helpers;
  std::vector<std::string> toWorker;
  std::vector<HelperArgs> launched;
  ServiceSecure::Env env() {
    ServiceSecure::Env e;
    e.eligible = [this] { return eligible; };
    e.launch = [this](const HelperArgs& args) -> std::optional<HelperGrant> {
      if (!launchWorks) return {};
      launched.push_back(args);
      helpers.push_back(std::make_shared<FakeHelper>());
      HelperGrant grant{helpers.back(), 0x1230, 0x4560, {}};
      grant.revoke = [this] { ++revoked; };
      return grant;
    };
    e.toWorker = [this](const std::string& line) { if (!workerTakes) return false; toWorker.push_back(line); return true; };
    e.nowMs = [this] { return now; };
    return e;
  }
};
Json request(const std::string& session = "s1", int seq = 1) {
  OwnedOutput output; output.adapterLow = 77; output.adapterHigh = -3; output.left = 0; output.top = 0; output.width = 1920; output.height = 1080;
  return JsonParser::parse(worker::requestJson(session, uint32_t(seq), output));
}
Json control(const char* type, const std::string& session = "s1") { return JsonParser::parse(worker::controlJson(type, session)); }
const Json key = JsonParser::parse(R"({"type":"input","sessionId":"s1","event":"key","vk":89,"down":true})");
const Json reset = JsonParser::parse(R"({"type":"input.reset","sessionId":"s1"})");

void serviceDispatch() {
  // Every worker.secure.* message is consumed (never forwarded); nothing else is.
  {
    FakeService fake; ServiceSecure service("s1", fake.env());
    for (const char* text : {R"({"type":"worker.secure.request"})", R"({"type":"worker.secure.stop","sessionId":"s1"})", R"({"type":"worker.secure.active","sessionId":"other"})",
                             R"({"type":"worker.secure.ready","sessionId":"s1","section":4,"event":8})", R"({"type":"worker.secure.failed","sessionId":"s1"})",
                             R"({"type":"worker.secure.future","sessionId":"s1"})", R"({"type":"worker.secure.request","sessionId":"s1","seq":"x"})"})
      expect(service.consume(JsonParser::parse(text)), "worker.secure.* is consumed");
    for (const char* text : {R"({"type":"stream.info","sessionId":"s1"})", R"({"type":"worker.stream.failure","sessionId":"s1","stage":44,"error":5})", R"({"type":"worker.encoder.usage"})",
                             R"({"type":"input","sessionId":"s1","event":"key"})", R"({"type":"pong","id":"x"})", R"({"sessionId":"s1"})"})
      expect(!service.consume(JsonParser::parse(text)), "other worker messages are left to the relay");
    expect(fake.launched.empty() && fake.toWorker.empty(), "malformed and foreign messages launch nothing and answer nothing");
  }
  // A valid request: one launch, the ready answer carries the worker's handles.
  {
    FakeService fake; ServiceSecure service("s1", fake.env());
    expect(service.consume(request()), "request consumed");
    expect(fake.launched.size() == 1 && fake.launched[0].adapterLow == 77 && fake.launched[0].adapterHigh == -3 && fake.launched[0].width == 1920 && fake.launched[0].height == 1080, "the helper is launched for the asked output");
    expect(fake.toWorker.size() == 1, "one answer to the worker");
    const auto ready = JsonParser::parse(fake.toWorker[0]);
    expect(ready.get("type").string() == "worker.secure.ready" && ready.get("sessionId").string() == "s1" && ready.get("seq").number() == 1 &&
           ready.get("section").number() == 0x1230 && ready.get("event").number() == 0x4560 && ready.get("width").number() == 1920 && ready.get("height").number() == 1080, "ready names seq, handles and size");
    expect(service.helperHeld() && !service.routing(), "a launched helper gets no input yet");
    // Input: nothing before the worker shows the prompt.
    expect(!service.route(key) && fake.helpers[0]->sent.empty(), "input stays with the worker until the stream shows the prompt");
    expect(service.consume(control(controlActive)) && service.routing(), "active turns routing on");
    expect(service.route(key) && fake.helpers[0]->sent.size() == 1 && JsonParser::parse(fake.helpers[0]->sent[0]).get("vk").number() == 89, "input goes to the helper and not to the worker");
    expect(!service.route(reset) && fake.helpers[0]->sent.size() == 2, "input.reset goes to the helper and also on to the worker");
    for (const char* text : {R"({"type":"ping","id":"1"})", R"({"type":"input.fence","sessionId":"s1"})", R"({"type":"clip.packet","sessionId":"s1"})", R"({"type":"cursor.compose","on":true})", R"({"type":"stream.resize"})"})
      expect(!service.route(JsonParser::parse(text)), "only input and input.reset are routed");
    expect(fake.helpers[0]->sent.size() == 2, "nothing else reached the helper");
    const std::string big(ServiceSecure::inputLimit, 'a');
    expect(service.route(JsonParser::parse("{\"type\":\"input\",\"event\":\"text\",\"text\":\"" + big + "\"}")) && fake.helpers[0]->sent.size() == 2, "an oversized input reaches neither side");
    // The helper exited by itself (the desktop returned): input is the worker's again at once.
    fake.helpers[0]->running = false;
    expect(!service.route(key) && fake.helpers[0]->sent.size() == 2, "no input to a helper that exited");
    expect(service.consume(control(controlStop)) && fake.helpers[0]->closed && !service.helperHeld(), "stop ends the helper");
    expect(!service.route(key), "after stop input stays with the worker");
  }
  // A helper that does not take input gets no more of it.
  {
    FakeService fake; ServiceSecure service("s1", fake.env());
    (void)service.consume(request()); (void)service.consume(control(controlActive));
    fake.helpers[0]->accepts = false;
    expect(!service.route(key) && !service.routing(), "a failed write stops the routing (the worker's notice follows)");
    fake.helpers[0]->accepts = true;
    expect(!service.route(key) && fake.helpers[0]->sent.empty(), "routing does not come back by itself");
  }
  // Refusals: every one answers failed and launches nothing.
  {
    FakeService fake; fake.eligible = false; ServiceSecure service("s1", fake.env());
    expect(service.consume(request()) && fake.launched.empty(), "no launch while the stream is not eligible (locked, stopping, resizing)");
    expect(fake.toWorker.size() == 1 && JsonParser::parse(fake.toWorker[0]).get("type").string() == "worker.secure.failed" && JsonParser::parse(fake.toWorker[0]).get("seq").number() == 1, "the worker is told failed");
    fake.eligible = true; fake.launchWorks = false;
    expect(service.consume(request("s1", 2)) && fake.toWorker.size() == 2 && JsonParser::parse(fake.toWorker[1]).get("type").string() == "worker.secure.failed", "a failed launch answers failed");
    expect(!service.helperHeld() && !service.route(key), "no helper, no routing");
    fake.launchWorks = true;
    expect(service.consume(request("other", 3)) && fake.launched.empty() && fake.toWorker.size() == 2, "another session's request is consumed and ignored");
    auto bad = JsonParser::parse(R"({"type":"worker.secure.request","sessionId":"s1","seq":4,"adapter":{"low":1,"high":0},"rect":{"left":0,"top":0,"width":8,"height":1080}})");
    expect(service.consume(bad) && fake.launched.empty() && fake.toWorker.size() == 3, "an output out of bounds launches nothing and answers failed");
  }
  // A newer request ends the older helper; the worker that cannot be answered gets none.
  {
    FakeService fake; ServiceSecure service("s1", fake.env());
    (void)service.consume(request("s1", 1)); (void)service.consume(control(controlActive));
    (void)service.consume(request("s1", 2));
    expect(fake.helpers.size() == 2 && fake.helpers[0]->closed && !fake.helpers[1]->closed && !service.routing(), "one helper per stream: the older one is ended, routing starts over");
    fake.workerTakes = false;
    (void)service.consume(request("s1", 3));
    expect(fake.helpers.size() == 3 && fake.helpers[1]->closed && fake.helpers[2]->closed && !service.helperHeld() && fake.revoked == 1, "a helper whose section cannot be handed over is ended and the worker's handles are closed again");
  }
  // Launch budget for one stream.
  {
    FakeService fake; ServiceSecure service("s1", fake.env());
    for (unsigned i = 0; i < ServiceSecure::launchBudget + 3; ++i) (void)service.consume(request("s1", int(i + 1)));
    expect(fake.launched.size() == ServiceSecure::launchBudget, "launches per minute are bounded");
    fake.now += ServiceSecure::budgetWindowMs;
    (void)service.consume(request("s1", 100));
    expect(fake.launched.size() == ServiceSecure::launchBudget + 1, "the budget returns with time");
  }
  // The relay ends: the helper ends with it, and nothing works afterwards.
  {
    FakeService fake;
    {
      ServiceSecure service("s1", fake.env());
      (void)service.consume(request()); (void)service.consume(control(controlActive));
      service.close();
      expect(fake.helpers[0]->closed && !service.route(key), "closing the relay ends the helper and the routing");
      expect(service.consume(request("s1", 2)) && fake.launched.size() == 1, "a closed relay still consumes but launches nothing");
    }
    FakeService second;
    { ServiceSecure service("s1", second.env()); (void)service.consume(request()); }
    expect(second.helpers.size() == 1 && second.helpers[0]->closed, "a relay that just goes away ends its helper");
  }
  // parseHelperRequest bounds.
  expect(parseHelperRequest(request()).has_value(), "a worker's request parses");
  for (const char* text : {R"({"adapter":{"low":1,"high":0},"rect":{"left":0,"top":0,"width":1920}})", R"({"adapter":{"low":-1,"high":0},"rect":{"left":0,"top":0,"width":1920,"height":1080}})",
                           R"({"adapter":{"low":1,"high":0},"rect":{"left":0,"top":0,"width":1920.5,"height":1080}})", R"({"adapter":{"low":1,"high":0},"rect":{"left":0,"top":0,"width":9000,"height":1080}})",
                           R"({"adapter":{"low":1,"high":0},"rect":{"left":"0","top":0,"width":1920,"height":1080}})", R"({"rect":{"left":0,"top":0,"width":1920,"height":1080}})"})
    expect(!parseHelperRequest(JsonParser::parse(text)).has_value(), "a malformed output is refused");
}

// The helper's control loop with fake capture, injection and observations.
struct FakeHelperWorld {
  HelperConfig config;
  ConsoleView console{1, true, false, "S-1-5-21-1-2-3-1001", 0};
  HelperView self{true, 1, true, true, true, true};
  uint64_t clock = 50000;
  int scopeReads = 0, frames = 0, releases = 0, opens = 0, captures = 0, beats = 0;
  std::vector<int> scopeAtFrame, scopeAtInject;
  std::vector<std::string> injected;
  std::deque<Json> inputs;
  std::deque<CaptureStep> steps;
  CaptureStep openResult = CaptureStep::frame;
  std::function<void(FakeHelperWorld&)> onTick;
  bool stop = false, sinkAccepts = true;
  FakeHelperWorld() { config.session = 1; config.userSid = "S-1-5-21-1-2-3-1001"; config.output.width = W; config.output.height = H; }
  HelperIo io() {
    HelperIo io;
    // The clock moves with every observation: an observation read after the
    // clock would look like one from the future.
    io.console = [this] { ++scopeReads; auto view = console; view.observedAt = ++clock; return view; };
    io.self = [this] { ++clock; return self; };
    io.nowMs = [this] { return ++clock; };
    io.open = [this] { ++opens; return openResult; };
    io.capture = [this](std::vector<uint8_t>& bgra) {
      ++captures;
      if (onTick) onTick(*this);
      if (steps.empty()) { stop = true; return CaptureStep::none; }
      const auto step = steps.front(); steps.pop_front();
      if (step == CaptureStep::frame) bgra.assign(ipc::frameBytes(W, H), uint8_t(captures));
      return step;
    };
    io.frame = [this](const uint8_t*, uint32_t w, uint32_t h) { if (w != W || h != H) return false; ++frames; scopeAtFrame.push_back(scopeReads); return sinkAccepts; };
    io.alive = [this] { ++beats; };
    io.nextInput = [this]() -> std::optional<Json> { if (inputs.empty()) return {}; auto value = inputs.front(); inputs.pop_front(); return value; };
    io.inject = [this](const Json& event) { injected.push_back(event.get("event").string()); scopeAtInject.push_back(scopeReads); return true; };
    io.releaseKeys = [this] { ++releases; };
    io.stopped = [this] { return stop; };
    return io;
  }
};
Json input(const char* event) { return JsonParser::parse(std::string("{\"type\":\"input\",\"event\":\"") + event + "\"}"); }

void helperLoop() {
  // Frames and input flow; every frame and every injected event has its own fresh scope observation.
  {
    FakeHelperWorld world;
    world.steps = {CaptureStep::frame, CaptureStep::none, CaptureStep::frame, CaptureStep::frame};
    world.inputs = {input("key"), input("move"), input("button"), input("wheel"), input("text")};
    const auto exit = runSecureDesktopHelper(world.config, world.io());
    expect(exit == HelperExit::transportClosed && world.frames == 3 && world.opens == 1, "frames are delivered until the transport closes");
    expect(world.injected == std::vector<std::string>({"key", "move", "button", "wheel", "text"}), "key, move, button, wheel and text are injected in order");
    bool fresh = true; int last = 0;
    for (const int at : world.scopeAtInject) { fresh &= at > last; last = at; }
    expect(fresh, "a fresh scope observation precedes every injected event");
    fresh = true; last = world.scopeAtInject.back();
    for (const int at : world.scopeAtFrame) { fresh &= at > last; last = at; }
    expect(fresh, "a fresh scope observation precedes every delivered frame");
    expect(world.releases >= 1 && world.beats >= 4, "keys are released at the end; the heartbeat ran every tick");
  }
  // Only the allowlist reaches the injector.
  {
    FakeHelperWorld world;
    world.steps = {CaptureStep::none};
    world.inputs = {input("touch"), input("moveRelative"), input("clipboard"), JsonParser::parse(R"({"type":"input.fence","event":"key"})"), JsonParser::parse(R"({"type":"clip.packet","event":"key"})"),
                    JsonParser::parse(R"({"type":"drive.open","event":"key"})"), JsonParser::parse(R"({"event":"key"})"), JsonParser::parse(R"({"type":"input.reset"})"), input("key")};
    (void)runSecureDesktopHelper(world.config, world.io());
    expect(world.injected == std::vector<std::string>({"key"}), "touch, relative mouse, clipboard, drive, fences and untyped events are dropped");
    expect(world.releases == 2, "input.reset releases held keys (and the end releases again)");
  }
  // Only a security prompt in front. Something else on the Winlogon
  // desktop (the Ctrl+Alt+Del options screen) gets no frame and no input, and the helper ends after the grace.
  {
    FakeHelperWorld world; world.self.promptForeground = false;
    world.steps = {CaptureStep::frame, CaptureStep::frame};
    world.inputs = {input("key"), input("button")};
    const auto exit = runSecureDesktopHelper(world.config, world.io());
    expect(exit == HelperExit::scopeLost && world.frames == 0 && world.opens == 0 && world.injected.empty() && world.inputs.empty() && world.releases >= 1,
           "a Winlogon window that is not a security prompt got a frame or input, or the helper did not end");
  }
  {
    FakeHelperWorld world; world.self.promptForeground = false;
    world.steps = {CaptureStep::frame, CaptureStep::frame};
    world.inputs = {input("key")};
    auto io = world.io(); int idles = 0;
    io.idle = [&] { if (++idles == 5) { world.self.promptForeground = true; world.inputs.push_back(input("button")); } };
    const auto exit = runSecureDesktopHelper(world.config, io);
    expect(exit == HelperExit::transportClosed && world.frames == 2 && world.injected == std::vector<std::string>({"button"}),
           "a prompt that comes to the front within the grace is not streamed, or input queued before it was injected");
  }
  {
    FakeHelperWorld world;
    world.steps = {CaptureStep::frame, CaptureStep::frame, CaptureStep::frame};
    world.onTick = [](FakeHelperWorld& w) { if (w.captures == 2) { w.self.promptForeground = false; w.inputs.push_back(input("key")); } };
    const auto exit = runSecureDesktopHelper(world.config, world.io());
    expect(exit == HelperExit::scopeLost && world.frames == 1 && world.injected.empty(), "after the prompt left the front a frame or input still went through");
  }
  // The prompt closes: the input desktop is Default again. Nothing more is injected or delivered.
  {
    FakeHelperWorld world;
    world.steps = {CaptureStep::frame, CaptureStep::frame, CaptureStep::frame};
    world.onTick = [](FakeHelperWorld& w) { if (w.captures == 2) { w.self.inputWinlogon = false; w.inputs.push_back(input("key")); } };
    const auto exit = runSecureDesktopHelper(world.config, world.io());
    expect(exit == HelperExit::desktopReturned && world.frames == 1 && world.injected.empty() && world.releases == 1, "Default desktop: keys released, exit, no frame and no input after it");
  }
  // The desktop flips between the queue and the injection: the per-event check stops it.
  {
    FakeHelperWorld world;
    world.steps = {CaptureStep::none, CaptureStep::none};
    world.inputs = {input("key"), input("key"), input("key")};
    int reads = 0;
    auto io = world.io();
    const auto self = io.self;
    io.self = [&] { auto view = self(); if (++reads > 2) view.inputWinlogon = false; return view; };  // tick, first event, then gone
    const auto exit = runSecureDesktopHelper(world.config, io);
    expect(exit == HelperExit::desktopReturned && world.injected.size() == 1, "an event whose scope check fails is not injected, nor any after it");
  }
  // Locked, another account, another session, not SYSTEM: scope lost.
  for (int variant = 0; variant < 6; ++variant) {
    FakeHelperWorld world;
    world.steps = {CaptureStep::frame, CaptureStep::frame};
    world.inputs = {input("key")};
    if (variant == 0) world.console.unlocked = false;
    if (variant == 1) world.console.signedInSid = "S-1-5-21-9-9-9-500";
    if (variant == 2) world.console.noUser = true;
    if (variant == 3) world.self.isSystem = false;
    if (variant == 4) world.self.session = 2;
    if (variant == 5) world.self.winSta0 = false;
    const auto exit = runSecureDesktopHelper(world.config, world.io());
    expect(exit == HelperExit::scopeLost && world.opens == 0 && world.frames == 0 && world.injected.empty() && world.releases == 1, "outside the scope nothing is opened, captured or injected");
  }
  // A thread that is not on Winlogon is the desktop returning, not a failure.
  {
    FakeHelperWorld world; world.self.threadWinlogon = false; world.steps = {CaptureStep::frame};
    expect(runSecureDesktopHelper(world.config, world.io()) == HelperExit::desktopReturned && world.opens == 0, "thread desktop not Winlogon: exit before opening anything");
  }
  // A lost duplication is opened again, after another scope check.
  {
    FakeHelperWorld world;
    world.steps = {CaptureStep::frame, CaptureStep::lost, CaptureStep::frame};
    const auto exit = runSecureDesktopHelper(world.config, world.io());
    expect(exit == HelperExit::transportClosed && world.opens == 2 && world.frames == 2, "ACCESS_LOST: the duplication is opened again and frames go on");
  }
  // Capture failures end the helper (the worker then shows the notice).
  {
    FakeHelperWorld world; world.openResult = CaptureStep::mismatch;
    expect(runSecureDesktopHelper(world.config, world.io()) == HelperExit::outputMismatch && world.frames == 0, "another output is refused");
    FakeHelperWorld failed; failed.openResult = CaptureStep::failed;
    expect(runSecureDesktopHelper(failed.config, failed.io()) == HelperExit::duplicationFailed, "a duplication that cannot open ends the helper");
    FakeHelperWorld broke; broke.steps = {CaptureStep::frame, CaptureStep::failed};
    expect(runSecureDesktopHelper(broke.config, broke.io()) == HelperExit::duplicationFailed && broke.frames == 1 && broke.releases == 1, "a capture error ends the helper with keys released");
    FakeHelperWorld sink; sink.steps = {CaptureStep::frame, CaptureStep::frame}; sink.sinkAccepts = false;
    expect(runSecureDesktopHelper(sink.config, sink.io()) == HelperExit::transportClosed && sink.frames == 1, "a refused frame closes the transport");
    FakeHelperWorld lost; for (int i = 0; i < 100; ++i) lost.steps.push_back(CaptureStep::lost);
    expect(runSecureDesktopHelper(lost.config, lost.io()) == HelperExit::duplicationFailed, "a duplication that keeps getting lost does not spin forever");
  }
  // Configuration.
  {
    FakeHelperWorld world; auto io = world.io(); io.inject = nullptr;
    expect(runSecureDesktopHelper(world.config, io) == HelperExit::badConfig, "a missing callback is a bad configuration");
    FakeHelperWorld empty; empty.config.userSid.clear();
    expect(runSecureDesktopHelper(empty.config, empty.io()) == HelperExit::badConfig, "no user SID is a bad configuration");
    FakeHelperWorld zero; zero.config.session = 0;
    expect(runSecureDesktopHelper(zero.config, zero.io()) == HelperExit::badConfig, "session 0 is a bad configuration");
  }
}

void commandLine() {
  const auto parse = [](std::vector<const wchar_t*> args) { return entry::parseCommand(int(args.size()), args.data()); };
  const auto ok = parse({L"MultitapHost.exe", L"--secure-desktop-helper-v1", L"--session", L"1", L"--sid", L"S-1-5-21-1-2-3-1001", L"--adapter", L"77:-3", L"--rect", L"0,0,1920,1080",
                         L"--section", L"400", L"--event", L"404", L"--input", L"408", L"--worker", L"412"});
  expect(ok && ok->session == 1 && ok->sid == "S-1-5-21-1-2-3-1001" && ok->output.adapterLow == 77 && ok->output.adapterHigh == -3 && ok->output.width == 1920 && ok->output.height == 1080 &&
         ok->section == reinterpret_cast<HANDLE>(uintptr_t(400)) && ok->worker == reinterpret_cast<HANDLE>(uintptr_t(412)), "the service's command line parses");
  expect(!parse({L"MultitapHost.exe", L"--secure-desktop-helper-v1", L"--session", L"1", L"--sid", L"S-1-5-21-1-2-3-1001", L"--adapter", L"77:-3", L"--rect", L"0,0,1920,1080", L"--section", L"400", L"--event", L"404", L"--input", L"408"}), "a missing handle is refused");
  expect(!parse({L"MultitapHost.exe", L"--secure-desktop-helper-v1", L"--session", L"0", L"--sid", L"S-1-5-21-1-2-3-1001", L"--adapter", L"77:-3", L"--rect", L"0,0,1920,1080", L"--section", L"400", L"--event", L"404", L"--input", L"408", L"--worker", L"412"}), "session 0 is refused");
  expect(!parse({L"MultitapHost.exe", L"--secure-desktop-helper-v1", L"--session", L"1", L"--sid", L"Everyone", L"--adapter", L"77:-3", L"--rect", L"0,0,1920,1080", L"--section", L"400", L"--event", L"404", L"--input", L"408", L"--worker", L"412"}), "a SID that is not a SID is refused");
  expect(!parse({L"MultitapHost.exe", L"--secure-desktop-helper-v1", L"--session", L"1", L"--sid", L"S-1-5-21-1-2-3-1001", L"--adapter", L"77:-3", L"--rect", L"0,0,4,1080", L"--section", L"400", L"--event", L"404", L"--input", L"408", L"--worker", L"412"}), "a rectangle out of bounds is refused");
  expect(!parse({L"MultitapHost.exe", L"--secure-desktop-helper-v1", L"--session", L"1", L"--sid", L"S-1-5-21-1-2-3-1001", L"--adapter", L"77:-3", L"--rect", L"0,0,1920,1080", L"--section", L"4x", L"--event", L"404", L"--input", L"408", L"--worker", L"412"}), "a handle that is not a number is refused");
  expect(!parse({L"MultitapHost.exe", L"--secure-desktop-helper-v1", L"--session", L"1", L"--sid", L"S-1-5-21-1-2-3-1001", L"--adapter", L"77:-3", L"--rect", L"0,0,1920,1080", L"--section", L"400", L"--event", L"404", L"--input", L"408", L"--worker", L"412", L"--nonce", L"x"}), "an unknown argument is refused");
}

// Everything but the SYSTEM launch, DXGI and SendInput, together: a real unnamed
// section and event (as the service creates them), the service's real pipe
// writer and the helper's real pipe reader, the helper's loop on its own thread
// with a fake capture and injector, the service dispatch with a launch that
// starts that thread, and the worker's switch reading the mapped section.
struct ThreadHelper final : ServiceHelper {
  std::shared_ptr<HelperLink> link;      // the real framed writer; "process" is an event set when the thread ends
  std::thread thread;
  std::atomic<bool> desktopWinlogon{true}, unlocked{true}, killed{false};  // killed: stops without a last word
  std::atomic<int> captured{0};
  std::mutex mutex;
  std::vector<std::string> injected;
  HANDLE done = nullptr;
  HelperExit exit = HelperExit::badConfig;
  bool alive() override { return link->alive(); }
  bool send(const std::string& json) override { return link->send(json); }
  void close() override { link->close(); if (thread.joinable()) thread.join(); }
  ~ThreadHelper() override { close(); }
};

void endToEnd() {
  const std::string sid = "S-1-5-21-1-2-3-1001";
  std::shared_ptr<ThreadHelper> helper;
  std::vector<std::string> toWorker;
  ServiceSecure::Env env;
  env.eligible = [] { return true; };
  env.nowMs = [] { return uint64_t(GetTickCount64()); };
  env.toWorker = [&](const std::string& line) { toWorker.push_back(line); return true; };
  env.launch = [&](const HelperArgs& args) -> std::optional<HelperGrant> {
    // What secure_desktop_launch.hpp creates, minus the process.
    const uint64_t bytes = ipc::sectionBytes(args.width, args.height);
    host::WinHandle section(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(bytes >> 32), DWORD(bytes), nullptr));
    host::WinHandle frameEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    const std::wstring name = L"\\\\.\\pipe\\MultitapHost.SecureInputTest." + std::to_wstring(GetCurrentProcessId()) + L"." + host::randomName();
    host::WinHandle write(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 0, 0, nullptr));
    HANDLE read = CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr), doneForLink = nullptr, workerSection = nullptr, workerEvent = nullptr, helperSection = nullptr, helperEvent = nullptr;
    if (!section.value || !frameEvent.value || !write.value || write.value == INVALID_HANDLE_VALUE || read == INVALID_HANDLE_VALUE || !done) return {};
    DuplicateHandle(GetCurrentProcess(), done, GetCurrentProcess(), &doneForLink, 0, FALSE, DUPLICATE_SAME_ACCESS);
    // The worker's duplicates are read-only / wait-only, exactly as in the product.
    DuplicateHandle(GetCurrentProcess(), section.value, GetCurrentProcess(), &workerSection, FILE_MAP_READ, FALSE, 0);
    DuplicateHandle(GetCurrentProcess(), frameEvent.value, GetCurrentProcess(), &workerEvent, SYNCHRONIZE, FALSE, 0);
    DuplicateHandle(GetCurrentProcess(), section.value, GetCurrentProcess(), &helperSection, 0, FALSE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(GetCurrentProcess(), frameEvent.value, GetCurrentProcess(), &helperEvent, 0, FALSE, DUPLICATE_SAME_ACCESS);
    helper = std::make_shared<ThreadHelper>();
    helper->done = done;
    helper->link = std::make_shared<HelperLink>(host::WinHandle(), host::WinHandle(doneForLink), std::move(write));
    HelperConfig config; config.session = 1; config.userSid = sid; config.output.width = args.width; config.output.height = args.height;
    auto* self = helper.get();
    helper->thread = std::thread([self, config, helperSection, helperEvent, read, done] {
      host::WinHandle ownedSection(helperSection), ownedEvent(helperEvent), ownedRead(read), ownedDone(done);
      auto* view = static_cast<uint8_t*>(MapViewOfFile(ownedSection.value, FILE_MAP_WRITE, 0, 0, ipc::sectionBytes(config.output.width, config.output.height)));
      if (!view) { SetEvent(ownedDone.value); return; }
      ipc::writerInit(view, config.output.width, config.output.height);
      {
        entry::InputReader reader(ownedRead.value);
        uint64_t clock = 1000;
        HelperIo io;
        io.console = [&] { ConsoleView v{1, self->unlocked.load(), false, config.userSid, 0}; v.observedAt = ++clock; return v; };
        io.self = [&] { ++clock; return HelperView{true, 1, true, true, self->desktopWinlogon.load(), true}; };
        io.nowMs = [&] { return ++clock; };
        io.open = [] { return CaptureStep::frame; };
        io.capture = [&](std::vector<uint8_t>& bgra) {
          std::this_thread::sleep_for(std::chrono::milliseconds(4));
          const int n = ++self->captured;
          if (n % 3) return CaptureStep::none;  // a mostly still prompt
          bgra.assign(ipc::frameBytes(config.output.width, config.output.height), uint8_t(n));
          return CaptureStep::frame;
        };
        io.frame = [&](const uint8_t* bgra, uint32_t w, uint32_t h) { ipc::writeFrame(view, bgra, w, h); SetEvent(ownedEvent.value); return true; };
        io.alive = [&] { ipc::writeAlive(view); };
        io.nextInput = [&] { return reader.pop(); };
        io.inject = [&](const Json& event) { std::lock_guard<std::mutex> lock(self->mutex); self->injected.push_back(host::jsonObject(event)); return true; };
        io.releaseKeys = [] {};
        io.stopped = [&] { return reader.closed() || self->killed.load(); };
        self->exit = runSecureDesktopHelper(config, io);
      }
      if (!self->killed) { ipc::writeState(view, self->exit == HelperExit::desktopReturned ? ipc::stateEnded : ipc::stateFailed); SetEvent(ownedEvent.value); }
      UnmapViewOfFile(view);
      SetEvent(ownedDone.value);
    });
    return HelperGrant{helper, uint64_t(reinterpret_cast<uintptr_t>(workerSection)), uint64_t(reinterpret_cast<uintptr_t>(workerEvent)), {}};
  };

  ServiceSecure service("s1", std::move(env));
  WorkerSwitch workerSwitch;
  std::shared_ptr<worker::Mapping> mapping;
  OwnedOutput output; output.adapterLow = 5; output.width = 64; output.height = 48;
  std::vector<uint32_t> records;
  bool sawPrompt = false, activated = false, stopSent = false;
  const auto workerTick = [&] {
    // The worker's capture-loop glue (agent.hpp secureFill), with the service in-process.
    while (!toWorker.empty()) {
      const auto message = JsonParser::parse(toWorker.front()); toWorker.erase(toWorker.begin());
      if (message.get("type").string() == controlReady) {
        auto grant = worker::adoptGrant(message);
        if (grant) { const uint32_t seq = grant->seq; if (auto mapped = worker::mapGrant(std::move(*grant))) { if (workerSwitch.attach(std::shared_ptr<const uint8_t>(mapped, mapped->view), mapped->width, mapped->height, seq)) mapping = std::move(mapped); } }
      } else if (const auto seq = worker::controlSeq(message)) workerSwitch.denied(*seq);
    }
    const auto tick = workerSwitch.tick(Clock::now(), true);
    for (unsigned i = 0; i < tick.recordCount; ++i) records.push_back(tick.records[i]);
    if (tick.request) (void)service.consume(JsonParser::parse(worker::requestJson("s1", tick.seq, output)));
    if (tick.activated) { activated = true; (void)service.consume(JsonParser::parse(worker::controlJson(controlActive, "s1"))); }
    if (tick.stop) { stopSent = true; mapping.reset(); (void)service.consume(JsonParser::parse(worker::controlJson(controlStop, "s1"))); }
    if (!tick.notice) sawPrompt = true;
    if (mapping && mapping->event.value) WaitForSingleObject(mapping->event.value, 5); else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return tick;
  };
  const auto until = [&](const std::function<bool()>& done, int limitMs) {
    const auto end = Clock::now() + std::chrono::milliseconds(limitMs);
    while (Clock::now() < end) { if (done()) return true; (void)workerTick(); }
    return done();
  };

  expect(until([&] { return activated; }, 3000), "end to end: the helper's frames reach the worker through the read-only section");
  expect(helper && mapping && mapping->width == 64 && mapping->height == 48 && workerSwitch.frame().size() == ipc::frameBytes(64, 48), "the worker reads the output's size");
  // The worker's view is read-only: it cannot write the prompt's pixels.
  MEMORY_BASIC_INFORMATION memory{};
  expect(mapping && VirtualQuery(mapping->view, &memory, sizeof(memory)) && memory.Protect == PAGE_READONLY, "the worker's view of the section is read-only");
  expect(service.routing(), "the service routes input once the worker shows the prompt");
  expect(service.route(JsonParser::parse(R"({"type":"input","sessionId":"s1","event":"key","vk":89,"down":true})")) &&
         service.route(JsonParser::parse(R"({"type":"input","sessionId":"s1","event":"touch","contacts":[]})")) &&
         service.route(JsonParser::parse(R"({"type":"input","sessionId":"s1","event":"key","vk":89,"down":false})")), "the service's pipe takes the input");
  expect(until([&] { std::lock_guard<std::mutex> lock(helper->mutex); return helper->injected.size() >= 2; }, 3000), "the helper injects what the service sent");
  {
    std::lock_guard<std::mutex> lock(helper->mutex);
    expect(helper->injected.size() == 2 && JsonParser::parse(helper->injected[0]).get("down").boolean() && !JsonParser::parse(helper->injected[1]).get("down").boolean(), "key down and up arrive in order; the touch event was dropped by the helper");
  }
  // A still prompt for longer than the heartbeat limit keeps streaming.
  const int capturedBefore = helper->captured.load();
  expect(!until([&] { return stopSent; }, 2600) && helper->captured.load() > capturedBefore && workerSwitch.phase() == HelperPhase::active, "after 2.6 s the helper still streams (no false loss)");
  // The prompt is answered: the helper sees the Default desktop, says so and exits by itself. (The worker's
  // loop sees the desktop too and resumes; it is not ticked here, or it would ask for a helper again as for
  // a second prompt.)
  const auto first = helper;
  first->desktopWinlogon = false;
  for (int i = 0; i < 600 && first->link->alive(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  expect(!first->link->alive(), "the helper exits by itself when the Default desktop returns");
  expect(!service.route(JsonParser::parse(R"({"type":"input","sessionId":"s1","event":"key","vk":13,"down":true})")), "input is the worker's again the moment the helper is gone");
  expect(first->exit == HelperExit::desktopReturned && mapping && ipc::Reader{mapping->view, 64, 48, 0}.state() == ipc::stateEnded, "the helper's last word is that the desktop returned");
  if (workerSwitch.reset()) (void)service.consume(JsonParser::parse(worker::controlJson(controlStop, "s1")));  // the worker's resume
  mapping.reset();
  expect(!service.helperHeld(), "after the worker's stop the service holds no helper");
  expect(sawPrompt && records.size() == 1 && records[0] == record244HelperActive, "the only record is 5 (the helper streamed the prompt; the notice never showed)");
  for (const uint32_t record : records) expect(record244Continues(record), "no record of this run ends the stream");

  // Second prompt: the helper is killed mid-prompt (it stops without a last word).
  records.clear(); activated = false; stopSent = false; sawPrompt = false;
  expect(until([&] { return activated; }, 3000), "a second prompt streams again");
  const auto second = helper;
  expect(second != first, "with a new helper");
  second->killed = true;
  const auto lostAt = Clock::now();
  expect(until([&] { return stopSent; }, 4000), "the worker notices the lost helper");
  { const auto after = workerTick(); expect(Clock::now() - lostAt < std::chrono::milliseconds(3500) && workerSwitch.phase() == HelperPhase::failed && after.notice && !after.hold, "and shows the notice within the heartbeat limit"); }
  bool lost = false;
  for (const uint32_t record : records) { lost |= record == record244HelperLost; expect(record244Continues(record), "a lost helper never ends the stream"); }
  expect(lost, "record 7 (helper lost)");
  expect(!service.helperHeld() && !service.route(JsonParser::parse(R"({"type":"input","sessionId":"s1","event":"key","vk":13,"down":true})")), "no helper and no routing are left");
  (void)workerSwitch.reset();
  service.close();
}

void launchGuards() {
  // Outside the SYSTEM service nothing is launched: there is no service worker
  // registry and the token is not SYSTEM.
  HelperArgs args; args.adapterLow = 1; args.width = 1920; args.height = 1080;
  wchar_t image[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, image, MAX_PATH);
  expect(!launchSecureDesktopHelper(image, 1, GetCurrentProcess(), "S-1-5-21-1-2-3-1001", args).has_value(), "no launch outside the SYSTEM service");
  expect(!launchSecureDesktopHelper(image, 0, GetCurrentProcess(), "S-1-5-21-1-2-3-1001", args).has_value(), "no launch into session 0");
  expect(!launchSecureDesktopHelper(L"", 1, GetCurrentProcess(), "S-1-5-21-1-2-3-1001", args).has_value(), "no launch without an image");
  args.width = 4;
  expect(!launchSecureDesktopHelper(image, 1, GetCurrentProcess(), "S-1-5-21-1-2-3-1001", args).has_value(), "no launch for an output out of bounds");
  // The service's framed writer and the helper's reader agree, and a link whose
  // process ended is not alive.
  HANDLE process = CreateEventW(nullptr, TRUE, FALSE, nullptr), processForLink = nullptr;
  DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &processForLink, 0, FALSE, DUPLICATE_SAME_ACCESS);
  const std::wstring name = L"\\\\.\\pipe\\MultitapHost.SecureInputTest." + std::to_wstring(GetCurrentProcessId()) + L"." + host::randomName();
  host::WinHandle write(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 0, 0, nullptr));
  host::WinHandle read(CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
  {
    HelperLink link(host::WinHandle(), host::WinHandle(processForLink), std::move(write));
    entry::InputReader reader(read.value);
    expect(link.alive() && link.send(R"({"type":"input","event":"key","vk":65,"down":true})") && link.send(R"({"type":"input.reset"})"), "the link writes framed input");
    std::optional<Json> first, second;
    for (int i = 0; i < 200 && !second; ++i) { if (!first) first = reader.pop(); else second = reader.pop(); if (!second) std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    expect(first && second && first->get("vk").number() == 65 && second->get("type").string() == "input.reset" && !reader.closed(), "the helper's reader gets the same messages in order");
    expect(!link.send("") && !link.send(std::string(70000, 'x')), "empty and oversized messages are refused");
    SetEvent(process);
    expect(!link.alive(), "a link whose process exited is not alive");
    link.close();
    for (int i = 0; i < 200 && !reader.closed(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    expect(reader.closed(), "closing the link closes the helper's pipe: the helper's loop stops");
    expect(!link.send(R"({"type":"input"})"), "a closed link sends nothing");
  }
  CloseHandle(process);
  // The user SID the service passes is the worker's real account, never SYSTEM.
  const auto own = processUserSid(GetCurrentProcess());
  expect(!own || own->rfind("S-1-5-21-", 0) == 0 || own->rfind("S-1-12-1-", 0) == 0, "processUserSid yields an account SID or nothing");
  // The worker's request and the service's parser agree.
  OwnedOutput output; output.adapterLow = 4000000000u; output.adapterHigh = -1; output.left = -1920; output.top = 0; output.width = 1920; output.height = 1200;
  const auto parsed = parseHelperRequest(JsonParser::parse(worker::requestJson("s1", 9, output)));
  expect(parsed && parsed->adapterLow == 4000000000u && parsed->adapterHigh == -1 && parsed->left == -1920 && parsed->width == 1920 && parsed->height == 1200, "the worker's request round-trips through the service's parser");
  expect(worker::controlSeq(JsonParser::parse(worker::requestJson("s1", 9, output))) == 9u && !worker::controlSeq(JsonParser::parse(R"({"seq":0})")) && !worker::controlSeq(JsonParser::parse(R"({"seq":1.5})")), "sequence numbers are whole and positive");
  expect(!worker::adoptGrant(JsonParser::parse(R"({"type":"worker.secure.ready","seq":1,"width":1920,"height":1080})")).has_value(), "a ready without handles is no grant");
}

}  // namespace

int main() {
  sectionLayout();
  scaling();
  sourceSwitch();
  serviceDispatch();
  helperLoop();
  commandLine();
  launchGuards();
  endToEnd();
  if (failures) { std::cerr << failures << " failure(s) of " << checks << " checks\n"; return 1; }
  std::cout << "secure desktop bridge: ok (" << checks << " checks)\n";
  return 0;
}
