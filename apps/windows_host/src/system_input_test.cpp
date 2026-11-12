// System input (system_input.hpp) with fakes: when a stream gets the SYSTEM
// injector, what the injector does on the user's desktop, on the secure desktop,
// on a lock, a sign-out and a session change, how the service routes and marks
// input, the geometry of pointer positions, that the injector's pipe cannot be
// opened by a process that is not SYSTEM, and the time one input takes from the
// service's write to the injection call over the real pipe. No SYSTEM process is
// started and no input is sent to Windows.
#include "remote_pointer.hpp"
#include "secure_desktop_capture.hpp"
#include "secure_desktop_launch.hpp"
#include "secure_desktop_scope.hpp"
#include "secure_desktop_transport.hpp"
#include "secure_desktop_worker.hpp"
#include "stream_prepare_diagnostics.hpp"
#include "system_input.hpp"

#include <algorithm>
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
#include <vector>

namespace {
int failures = 0, checks = 0;
void expect(bool value, const char* what) { ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; } }
using namespace host;
using namespace host::sysinput;
using secure::ConsoleView;

const std::string kUser = "S-1-5-21-1-2-3-1001";

void decision() {
  using secure::integrityUnknown;
  // A standard user's worker (medium) always needs it; so does a low one.
  for (uint32_t consent : {0u, 2u, 5u}) for (bool secureDesktop : {false, true}) {
    expect(systemInputNeed(2, secureDesktop, consent) == Need::workerNotElevated, "a medium worker gets the injector whatever the UAC policy");
    expect(systemInputNeed(1, secureDesktop, consent) == Need::workerNotElevated, "a low worker gets the injector");
  }
  // An administrator's elevated worker (high): only when UAC prompts show on the user's desktop.
  expect(systemInputNeed(3, false, 5) == Need::promptOnUserDesktop, "elevated worker, prompts on the user's desktop: the injector");
  expect(systemInputNeed(3, false, 2) == Need::promptOnUserDesktop, "elevated worker, credential prompts on the user's desktop: the injector");
  expect(systemInputNeed(3, true, 5) == Need::none, "elevated worker, secure desktop on (Windows default): unchanged, the helper answers prompts");
  expect(systemInputNeed(3, false, 0) == Need::none, "elevated worker, no prompts at all: unchanged");
  expect(systemInputNeed(3, true, 0) == Need::none, "elevated worker, no prompts: unchanged");
  expect(systemInputNeed(4, false, 5) == Need::none, "a SYSTEM worker reaches everything");
  expect(systemInputNeed(integrityUnknown, false, 5) == Need::none, "an unreadable worker token changes nothing");
  // Independent outputs are decided the same way (the decision does not ask about them).

  // Integrity levels and the "cannot reach" record.
  expect(secure::integrityLevel(0x1000) == 1 && secure::integrityLevel(0x2000) == 2 && secure::integrityLevel(0x2100) == 2 && secure::integrityLevel(0x3000) == 3 &&
         secure::integrityLevel(0x4000) == 4 && secure::integrityLevel(0xffffffffu) == 14, "integrity RIDs map to small levels");
  expect(secure::inputUnreachable(2, 3) && secure::inputUnreachable(2, 4) && secure::inputUnreachable(3, 4) && secure::inputUnreachable(2, integrityUnknown), "a window above the injector is unreachable (UIPI)");
  expect(!secure::inputUnreachable(2, 2) && !secure::inputUnreachable(3, 2) && !secure::inputUnreachable(3, 3) && !secure::inputUnreachable(2, 0) && !secure::inputUnreachable(4, integrityUnknown), "same or lower, nothing in front, or a SYSTEM injector: reachable");
  expect(secure::input246Reach(2, 4) == (10u | (0x42u << 8)) && secure::input246Reach(3, 15) == (10u | (0xf3u << 8)), "record 246 kind 10 packs worker and window level");
  expect(secure::input246(1, 0) == 1 && secure::input246(12, 300) == (12u | (300u << 8)) && secure::input246(6, 0xffffffffu) == (6u | (0xffffffu << 8)), "record 246 = kind | value << 8, value capped");
  expect(streamObservationOk(46, secure::input246Reach(2, 4)) && streamObservationOk(46, secure::input246(record246StartFailed, 2)) && streamObservationOk(44, 5) && !streamObservationOk(44, 3) && !streamObservationOk(47, 1), "stage 46 records are observations; 44 and unknown stages as before");
}

// The injector's loop with fake Windows.
struct World {
  InjectorConfig config;
  ConsoleView console{1, true, false, kUser, 0};
  InjectorView self{true, 1, true, true};
  uint64_t clock = 100000;
  std::deque<Json> inbox;
  std::vector<std::string> injected, texts, touches;
  int consoleReads = 0, selfReads = 0, attaches = 0, releases = 0, areas = 0, waits = 0;
  int received = 0, done = 0, dropped = 0, refused = 0, offDesktop = 0;
  bool attachWorks = true, injectWorks = true, stop = false;
  std::function<void(World&)> onWait;  // before each wait: change the world
  World() { config.session = 1; config.userSid = kUser; }
  InjectorIo io() {
    InjectorIo io;
    io.console = [this] { ++consoleReads; auto view = console; view.observedAt = ++clock; return view; };
    io.self = [this] { ++selfReads; return self; };
    io.nowMs = [this] { return ++clock; };
    io.next = [this](uint32_t) -> std::optional<Json> {
      ++waits;
      if (onWait) onWait(*this);
      if (inbox.empty()) { stop = true; return {}; }
      auto value = inbox.front(); inbox.pop_front(); return value;
    };
    io.attach = [this] { ++attaches; return attachWorks; };
    io.inject = [this](const Json& message) { injected.push_back(message.get("event").string()); return injectWorks; };
    io.text = [this](const Json& message) { texts.push_back(message.get("text").string()); return true; };
    io.touch = [this](const Json&) { touches.push_back("touch"); return true; };
    io.area = [this](const Json& message) { ++areas; return parseArea(message).has_value(); };
    io.release = [this] { ++releases; };
    io.stopped = [this] { return stop; };
    io.note = [this](Note what) {
      if (what == Note::received) ++received; else if (what == Note::injected) ++done; else if (what == Note::dropped) ++dropped;
      else if (what == Note::refused) ++refused; else ++offDesktop;
    };
    return io;
  }
};
Json input(const char* event, const char* more = "") { return JsonParser::parse(std::string("{\"type\":\"input\",\"event\":\"") + event + "\"" + more + "}"); }

void injectorLoop() {
  // On the user's desktop every kind the client sends is injected, in order.
  {
    World w; w.config.grants = {true, true, true};
    w.inbox = {input("key", ",\"vk\":65,\"down\":true"), input("move", ",\"x\":0.5,\"y\":0.5"), input("button", ",\"button\":\"left\",\"down\":true"), input("wheel", ",\"delta\":120"),
               input("moveRelative", ",\"dx\":3,\"dy\":-2"), input("text", ",\"text\":\"a\""), input("touch", ",\"contacts\":[]"), input("key", ",\"vk\":65,\"down\":false")};
    const auto exit = runSystemInput(w.config, w.io());
    expect(exit == InjectorExit::transportClosed, "the injector ends when the service's pipe closes");
    expect(w.injected == std::vector<std::string>({"key", "move", "button", "wheel", "moveRelative", "key"}) && w.texts.size() == 1 && w.touches.size() == 1, "keys, absolute and relative moves, buttons, wheel, text and touch are injected in order");
    expect(w.received == 8 && w.done == 8 && w.dropped == 0 && w.refused == 0 && w.offDesktop == 0, "counts: all received, all injected");
    expect(w.attaches == 1 && w.releases == 1, "the thread joins the user's desktop once; held input is released at the end");
  }
  // The session's grants decide, exactly as in the worker.
  {
    World w;  // no grant
    w.inbox = {input("moveRelative", ",\"dx\":3,\"dy\":1"), input("text", ",\"text\":\"a\""), input("touch"), input("key", ",\"vk\":65,\"down\":true"), input("clipboard"),
               JsonParser::parse(R"({"type":"input.fence"})"), JsonParser::parse(R"({"type":"clip.packet","event":"key"})"), JsonParser::parse(R"({"event":"key"})")};
    (void)runSystemInput(w.config, w.io());
    expect(w.injected == std::vector<std::string>({"key"}) && w.texts.empty() && w.touches.empty(), "relative mouse, text and touch need the session's grant; unknown kinds and types never pass");
    expect(w.received == 8 && w.done == 1 && w.dropped == 7, "what the grants and the allowlist drop is counted as dropped");
  }
  // An input the injector cannot place (malformed, outside the output) is refused, and counted.
  {
    World w; w.injectWorks = false;
    w.inbox = {input("move", ",\"x\":7,\"y\":0.5")};
    (void)runSystemInput(w.config, w.io());
    expect(w.received == 1 && w.done == 0 && w.refused == 1, "a refused input is counted as refused");
  }
  // input.reset releases what is held; the stream's rectangle is taken without being counted as input.
  {
    World w;
    w.inbox = {JsonParser::parse(R"({"type":"system.area","left":0,"top":0,"right":1920,"bottom":1080})"), JsonParser::parse(R"({"type":"input.reset"})"), input("key", ",\"vk\":65,\"down\":true")};
    (void)runSystemInput(w.config, w.io());
    expect(w.areas == 1 && w.releases == 2 && w.received == 2 && w.done == 1, "area is applied, reset releases, only inputs are counted");
  }
  // The secure desktop (UAC, Ctrl+Alt+Del): nothing is injected there; back on the user's desktop the
  // injector lets go of what it held and goes on.
  {
    World w;
    w.inbox = {input("key", ",\"vk\":13,\"down\":true"), input("key", ",\"vk\":13,\"down\":false"), input("move", ",\"x\":0.1,\"y\":0.1"), input("key", ",\"vk\":89,\"down\":true"),
               input("key", ",\"vk\":65,\"down\":true")};
    w.onWait = [](World& world) {
      if (world.waits == 2) world.self.inputDefault = false;   // the prompt appears after the first key
      if (world.waits == 5) world.self.inputDefault = true;    // answered
    };
    const auto exit = runSystemInput(w.config, w.io());
    expect(exit == InjectorExit::transportClosed, "a secure desktop does not end the injector");
    expect(w.injected == std::vector<std::string>({"key", "key"}) && w.offDesktop == 3, "three inputs arrived while the secure desktop showed: none injected, all counted");
    expect(w.attaches == 2 && w.releases == 2, "on the way back it joins the user's desktop again and releases what it held before (and once more at the end)");
  }
  // The desktop is back but cannot be joined yet: nothing is injected.
  {
    World w; w.attachWorks = false;
    w.inbox = {input("key", ",\"vk\":65,\"down\":true")};
    (void)runSystemInput(w.config, w.io());
    expect(w.injected.empty() && w.offDesktop == 1 && w.releases == 0, "without the input desktop nothing is injected and nothing released");
  }
  // Lock, sign-out, another user, another session, not SYSTEM, not WinSta0: the injector ends, injecting nothing more.
  for (int variant = 0; variant < 7; ++variant) {
    World w;
    w.inbox = {input("key", ",\"vk\":65,\"down\":true"), input("key", ",\"vk\":66,\"down\":true"), input("key", ",\"vk\":67,\"down\":true")};
    w.onWait = [variant](World& world) {
      if (world.waits != 2) return;
      world.clock += 1000;  // past the console cache
      if (variant == 0) world.console.unlocked = false;
      if (variant == 1) { world.console.noUser = true; world.console.signedInSid.clear(); }
      if (variant == 2) world.console.signedInSid = "S-1-5-21-9-9-9-500";
      if (variant == 3) world.console.session = 2;
      if (variant == 4) world.self.session = 2;
      if (variant == 5) world.self.isSystem = false;
      if (variant == 6) world.self.winSta0 = false;
    };
    const auto exit = runSystemInput(w.config, w.io());
    expect(exit == InjectorExit::scopeLost && w.injected.size() == 1 && w.releases == 1, "outside the scope the injector releases what it holds and ends; the next input is not injected");
  }
  // A lock is seen within the cache time even while input keeps arriving (the cache is 50 ms).
  {
    World w;
    for (int i = 0; i < 200; ++i) w.inbox.push_back(input("move", ",\"x\":0.5,\"y\":0.5"));
    w.onWait = [](World& world) { if (world.waits == 20) world.console.unlocked = false; };
    const auto exit = runSystemInput(w.config, w.io());
    // Every loop turn advances the fake clock by 3 ms at most (two clock reads, one observation).
    expect(exit == InjectorExit::scopeLost && int(w.injected.size()) >= 19 && int(w.injected.size()) <= 19 + int(consoleCacheMs), "a lock ends the injector within the cache time");
    expect(w.consoleReads < w.selfReads && w.selfReads >= int(w.injected.size()), "the input desktop is looked at for every input; the console facts at most every 50 ms");
  }
  // Configuration.
  {
    World w; auto io = w.io(); io.inject = nullptr;
    expect(runSystemInput(w.config, io) == InjectorExit::badConfig, "a missing callback is a bad configuration");
    World zero; zero.config.session = 0;
    expect(runSystemInput(zero.config, zero.io()) == InjectorExit::badConfig, "session 0 is refused");
    World empty; empty.config.userSid.clear();
    expect(runSystemInput(empty.config, empty.io()) == InjectorExit::badConfig, "no user is refused");
    World quiet; auto silent = quiet.io(); silent.note = nullptr; quiet.inbox = {input("key", ",\"vk\":65,\"down\":true")};
    expect(runSystemInput(quiet.config, silent) == InjectorExit::transportClosed && quiet.injected.size() == 1, "counting is optional");
  }
}

struct FakeHelper final : secure::ServiceHelper {
  bool running = true, accepts = true, closed = false;
  std::vector<std::string> sent;
  bool alive() override { return running && !closed; }
  bool send(const std::string& json) override { if (!accepts) return false; sent.push_back(json); return true; }
  void close() override { closed = true; }
};
bool marked(const Json& command) { const auto* flag = std::get_if<bool>(&command.get(workerMarker).value); return flag && *flag; }
bool hasMarker(const Json& command) { const auto* fields = std::get_if<Json::Object>(&command.value); return fields && fields->contains(workerMarker); }

void serviceRouting() {
  const std::string key = R"({"type":"input","sessionId":"s1","event":"key","vk":65,"down":true})";
  // An elevated administrator's stream (no injector): nothing changes, and a marker of the client's own never passes.
  {
    ServiceSystemInput service;
    auto command = JsonParser::parse(key);
    expect(!service.route(command) && !hasMarker(command), "without an injector the worker injects, unmarked");
    auto forged = JsonParser::parse(R"({"type":"input","sessionId":"s1","event":"key","vk":65,"down":true,"systemInput":true})");
    expect(!service.route(forged) && !hasMarker(forged), "a client's own marker is removed");
    const auto counts = service.close();
    expect(counts.routed == 0 && counts.fellBack == 0 && counts.writeFailed == 0, "no injector, no counts");
  }
  // A standard user's stream: input goes to the injector, the worker gets the marked copy.
  {
    ServiceSystemInput service; auto helper = std::make_shared<FakeHelper>();
    service.attach(helper);
    expect(service.held() && service.routing(), "the injector is attached");
    auto command = JsonParser::parse(key);
    expect(service.route(command) && marked(command), "an input is written to the injector and marked for the worker");
    expect(helper->sent.size() == 1 && !hasMarker(JsonParser::parse(helper->sent[0])) && JsonParser::parse(helper->sent[0]).get("vk").number() == 65, "the injector gets the input itself, without the marker");
    auto forged = JsonParser::parse(R"({"type":"input","sessionId":"s1","event":"move","x":0.5,"y":0.5,"systemInput":false})");
    expect(service.route(forged) && marked(forged) && !hasMarker(JsonParser::parse(helper->sent[1])), "the marker is the service's, whatever the client sent");
    auto reset = JsonParser::parse(R"({"type":"input.reset","sessionId":"s1"})");
    expect(service.route(reset) && marked(reset) && helper->sent.size() == 3, "input.reset reaches the injector (it holds the keys) and the worker, marked");
    for (const char* text : {R"({"type":"ping","id":"1"})", R"({"type":"stream.resize"})", R"({"type":"clip.packet","sessionId":"s1"})", R"({"type":"input.fence"})", R"([1,2])"}) {
      auto other = JsonParser::parse(text);
      expect(!service.route(other) && !hasMarker(other), "only input and input.reset are routed");
    }
    expect(helper->sent.size() == 3, "nothing else reached the injector");
    // The worker's rectangle goes to the injector; every worker.input.* message is consumed.
    expect(service.consume(JsonParser::parse(R"({"type":"worker.input.area","sessionId":"s1","left":0,"top":0,"right":1920,"bottom":1080})"), "s1") && helper->sent.size() == 4 &&
           JsonParser::parse(helper->sent[3]).get("type").string() == injectorArea && JsonParser::parse(helper->sent[3]).get("right").number() == 1920, "the stream's rectangle reaches the injector");
    for (const char* text : {R"({"type":"worker.input.area","sessionId":"other","left":0,"top":0,"right":10,"bottom":10})", R"({"type":"worker.input.area","sessionId":"s1","left":0,"top":0,"right":1,"bottom":1080})",
                             R"({"type":"worker.input.area","sessionId":"s1","left":"0","top":0,"right":10,"bottom":10})", R"({"type":"worker.input.future","sessionId":"s1"})"})
      expect(service.consume(JsonParser::parse(text), "s1"), "worker.input.* is consumed (never forwarded), valid or not");
    expect(helper->sent.size() == 4, "a foreign, empty or malformed rectangle reaches nobody");
    for (const char* text : {R"({"type":"stream.info"})", R"({"type":"worker.secure.request"})", R"({"type":"input","event":"key"})", R"({"type":"worker.inputs"})"})
      expect(!service.consume(JsonParser::parse(text), "s1"), "other worker messages are not this feature's");
    // The injector ended by itself (lock, session change): the worker injects again, unmarked.
    helper->running = false;
    auto after = JsonParser::parse(key);
    expect(!service.route(after) && !hasMarker(after) && !service.routing(), "without a live injector the worker injects, as before");
    const auto counts = service.close();
    expect(helper->closed && counts.routed == 3 && counts.fellBack == 1 && counts.writeFailed == 0, "closing the relay ends the injector and returns the counts");
    auto late = JsonParser::parse(key);
    expect(!service.route(late) && !hasMarker(late), "after the relay nothing is routed");
  }
  // A write that fails: no more input for that injector, the worker takes over.
  {
    ServiceSystemInput service; auto helper = std::make_shared<FakeHelper>();
    service.attach(helper);
    helper->accepts = false;
    auto command = JsonParser::parse(key);
    expect(!service.route(command) && !hasMarker(command), "a failed write leaves the input with the worker");
    helper->accepts = true;
    auto next = JsonParser::parse(key);
    expect(!service.route(next) && helper->sent.empty(), "a broken injector gets nothing more");
    const auto counts = service.close();
    expect(counts.writeFailed == 1 && counts.fellBack == 1, "both are counted");
  }
  // The rectangle the worker reported before the injector was up is handed over when it starts.
  {
    ServiceSystemInput service;
    expect(service.consume(JsonParser::parse(R"({"type":"worker.input.area","sessionId":"s1","left":-1920,"top":0,"right":0,"bottom":1200})"), "s1"), "area before the injector is kept");
    auto helper = std::make_shared<FakeHelper>();
    service.attach(helper);
    expect(helper->sent.size() == 1 && JsonParser::parse(helper->sent[0]).get("left").number() == -1920, "the injector gets the rectangle first");
    auto second = std::make_shared<FakeHelper>();
    service.attach(second);
    expect(helper->closed && second->sent.size() == 1, "a new injector replaces and ends the old one");
    (void)service.close();
    auto late = std::make_shared<FakeHelper>();
    service.attach(late);
    expect(second->closed && late->closed && !service.held(), "an injector that arrives after the relay ended is ended at once");
  }
  // The relay just goes away.
  {
    auto helper = std::make_shared<FakeHelper>();
    { ServiceSystemInput service; service.attach(helper); }
    expect(helper->closed, "the injector never outlives its relay");
  }
  expect(parseArea(JsonParser::parse(R"({"left":10,"top":20,"right":30,"bottom":40})")).has_value() && !parseArea(JsonParser::parse(R"({"left":10,"top":20,"right":11,"bottom":40})")).has_value() &&
         !parseArea(JsonParser::parse(R"({"left":1.5,"top":20,"right":30,"bottom":40})")).has_value() && !parseArea(JsonParser::parse(R"({"left":10,"top":20,"right":300000,"bottom":40})")).has_value(), "the injector's area parser takes whole numbers within bounds only");
}

// Pointer positions: a client position is a fraction of the stream's rectangle,
// whatever size the client shows the picture at, and SendInput's absolute
// position is a fraction of the physical virtual desktop.
void geometry() {
  const auto plan = [](double x, double y, RECT area, RECT desktop) {
    return planRemotePointerInput(JsonParser::parse("{\"type\":\"input\",\"event\":\"move\",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + "}"), area, [desktop] { return desktop; });
  };
  const auto pixelX = [](const RemotePointerInput& p, RECT desktop) { return desktop.left + double(p.input.mi.dx) * double(desktop.right - desktop.left - 1) / 65535.0; };
  const auto pixelY = [](const RemotePointerInput& p, RECT desktop) { return desktop.top + double(p.input.mi.dy) * double(desktop.bottom - desktop.top - 1) / 65535.0; };
  const auto about = [](double a, double b) { return a - b < 1.01 && b - a < 1.01; };
  // One output (the stream's): a scaled client window sends the same fractions as a full-size one.
  {
    const RECT output{0, 0, 1266, 800};
    const auto centre = plan(0.5, 0.5, output, output);
    expect(centre && (centre->input.mi.dwFlags & (MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE)) == (MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE), "a position is an absolute move on the virtual desktop");
    expect(centre && about(pixelX(*centre, output), 632.5) && about(pixelY(*centre, output), 399.5), "the centre of the picture is the centre of the output");
    const auto corner = plan(1.0, 1.0, output, output), origin = plan(0.0, 0.0, output, output);
    expect(corner && origin && about(pixelX(*corner, output), 1265) && about(pixelY(*corner, output), 799) && origin->input.mi.dx == 0 && origin->input.mi.dy == 0, "the corners are the output's corners");
    expect(!plan(1.2, 0.5, output, output) && !plan(-0.1, 0.5, output, output), "a position outside the picture is refused");
  }
  // Two outputs, the stream's is the right one; then a left one at negative coordinates.
  {
    const RECT left{0, 0, 1920, 1080}, right{1920, 0, 1920 + 2560, 1440};
    const RECT desktop = secure::unionOf({left, right});
    expect(desktop.left == 0 && desktop.top == 0 && desktop.right == 4480 && desktop.bottom == 1440, "the virtual desktop is the union of the outputs");
    const auto p = plan(0.5, 0.5, right, desktop);
    expect(p && about(pixelX(*p, desktop), 1920 + 1279.5) && about(pixelY(*p, desktop), 719.5), "a position on the second output lands on the second output");
    const RECT negative{-1920, 0, 0, 1200}, primary{0, 0, 2560, 1440};
    const RECT both = secure::unionOf({primary, negative});
    expect(both.left == -1920 && both.right == 2560 && both.bottom == 1440, "an output left of the primary one extends the desktop to negative coordinates");
    const auto q = plan(0.25, 0.75, negative, both);
    expect(q && about(pixelX(*q, both), -1920 + 0.25 * 1919) && about(pixelY(*q, both), 0.75 * 1199), "a position on an output at negative coordinates lands there");
    expect(!plan(0.5, 0.5, RECT{0, 0, 5000, 1440}, both), "an area that is not inside the desktop is refused");
    expect(secure::unionOf({}).right == 0 && secure::unionOf({RECT{5, 5, 5, 9}}).right == 0, "no output, no desktop");
  }
  // Display scaling. DXGI's rectangle of a 1920x1080 output at 125 % is 1920x1080; the system metrics of a process
  // Windows treats as DPI-unaware say 1536x864. With the metrics every position of the real output is refused
  // (the area is not inside that desktop); with the physical bounds it is placed.
  {
    const RECT output{0, 0, 1920, 1080}, virtualized{0, 0, 1536, 864};
    expect(!plan(0.5, 0.5, output, virtualized), "DPI-virtualized bounds refuse the real output (what the helper did before)");
    const auto p = plan(0.9, 0.9, output, secure::unionOf({output}));
    expect(p && about(pixelX(*p, output), 0.9 * 1919) && about(pixelY(*p, output), 0.9 * 1079), "physical bounds place the position whatever the display scale");
  }
  // A button and a wheel carry their position in the same event; bare ones act where the pointer is.
  {
    const RECT output{0, 0, 1920, 1080};
    const auto click = planRemotePointerInput(JsonParser::parse(R"({"type":"input","event":"button","button":"left","down":true,"x":0.5,"y":0.5})"), output, [output] { return output; });
    expect(click && (click->input.mi.dwFlags & MOUSEEVENTF_LEFTDOWN) && (click->input.mi.dwFlags & MOUSEEVENTF_ABSOLUTE), "a click carries its position");
    const auto relative = planRemotePointerInput(JsonParser::parse(R"({"type":"input","event":"moveRelative","dx":5,"dy":-3})"), output, [output] { return output; });
    expect(relative && relative->input.mi.dx == 5 && relative->input.mi.dy == -3 && !(relative->input.mi.dwFlags & MOUSEEVENTF_ABSOLUTE), "game mouse movement stays relative");
  }
}

// What the service hands the injector cannot be opened by a process that is not
// SYSTEM, and the launch does nothing outside the service.
void reach() {
  HANDLE raw = nullptr;
  bool system = false;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) { WinHandle token(raw); system = secure::detail::tokenUserIsSystem(token.value); }
  if (!system) {
    // The pipe's DACL admits SYSTEM only: this process can create it (it is the creator) but cannot open its
    // other end, exactly as any other process of the user could not.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    expect(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr) != FALSE, "the pipe's security descriptor");
    SECURITY_ATTRIBUTES systemOnly{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const std::wstring name = L"\\\\.\\pipe\\MultitapHost.SecureInputTest." + std::to_wstring(GetCurrentProcessId()) + L"." + randomName();
    WinHandle server(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 0, 0, &systemOnly));
    expect(server.value && server.value != INVALID_HANDLE_VALUE, "the service's end of the pipe");
    SetLastError(0);
    const HANDLE client = CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    const DWORD error = GetLastError();
    expect(client == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED, "a process that is not SYSTEM cannot open the injector's pipe (access denied)");
    if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    SetLastError(0);
    const HANDLE writer = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    expect(writer == INVALID_HANDLE_VALUE, "nor open it for writing");
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    if (descriptor) LocalFree(descriptor);
    expect(!secure::detail::createInputPipe().has_value(), "the product's pipe pair is refused to this process for the same reason");
  }
  // One instance only: once both ends are open (as the service leaves it) nobody else gets in, whoever asks.
  {
    const std::wstring name = L"\\\\.\\pipe\\MultitapHost.SecureInputTest." + std::to_wstring(GetCurrentProcessId()) + L"." + randomName();
    WinHandle server(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 0, 0, nullptr));
    WinHandle first(CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    SetLastError(0);
    const HANDLE second = CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    const DWORD error = GetLastError();
    expect(first.value != INVALID_HANDLE_VALUE && second == INVALID_HANDLE_VALUE && error == ERROR_PIPE_BUSY, "the pipe has one instance: a second open finds it busy");
    if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
    SetLastError(0);
    const HANDLE squat = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536, 0, 0, nullptr);
    expect(squat == INVALID_HANDLE_VALUE, "and no second instance of that name can be made");
    if (squat != INVALID_HANDLE_VALUE) CloseHandle(squat);
  }
  wchar_t image[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, image, MAX_PATH);
  expect(!secure::launchSystemInput(image, 1, GetCurrentProcess(), kUser, 0).has_value(), "no injector is started outside the SYSTEM service");
  expect(!secure::launchSystemInput(image, 0, GetCurrentProcess(), kUser, 0).has_value() && !secure::launchSystemInput(L"", 1, GetCurrentProcess(), kUser, 0).has_value() &&
         !secure::launchSystemInput(image, 1, GetCurrentProcess(), "Everyone", 0).has_value() && !secure::launchSystemInput(image, 1, GetCurrentProcess(), kUser, 8).has_value(), "session 0, no image, no user and unknown grants are refused");
  // The injector's command line.
  const auto parse = [](std::vector<const wchar_t*> args) { return secure::entry::parseInjectorCommand(int(args.size()), args.data()); };
  const auto ok = parse({L"MultitapHost.exe", L"--system-input-v1", L"--session", L"2", L"--sid", L"S-1-5-21-1-2-3-1001", L"--grants", L"5", L"--input", L"400", L"--worker", L"404", L"--status", L"408"});
  expect(ok && ok->session == 2 && ok->sid == kUser && ok->grants == 5 && ok->input == reinterpret_cast<HANDLE>(uintptr_t(400)) && ok->status == reinterpret_cast<HANDLE>(uintptr_t(408)), "the service's command line parses");
  expect(!parse({L"MultitapHost.exe", L"--system-input-v1", L"--session", L"0", L"--sid", L"S-1-5-21-1-2-3-1001", L"--grants", L"0", L"--input", L"400", L"--worker", L"404", L"--status", L"408"}), "session 0 is refused");
  expect(!parse({L"MultitapHost.exe", L"--system-input-v1", L"--session", L"2", L"--sid", L"S-1-5-21-1-2-3-1001", L"--grants", L"9", L"--input", L"400", L"--worker", L"404", L"--status", L"408"}), "unknown grants are refused");
  expect(!parse({L"MultitapHost.exe", L"--system-input-v1", L"--session", L"2", L"--sid", L"S-1-5-21-1-2-3-1001", L"--grants", L"0", L"--input", L"400", L"--worker", L"404"}), "a missing handle is refused");
  expect(!parse({L"MultitapHost.exe", L"--system-input-v1", L"--session", L"2", L"--sid", L"S-1-5-21-1-2-3-1001", L"--grants", L"0", L"--input", L"400", L"--worker", L"404", L"--status", L"408", L"--pipe", L"x"}), "an unknown argument is refused");
  // This process's own level reads as a level; the window in front reads as a level or as unreadable.
  const uint32_t own = secure::worker::processLevel(GetCurrentProcess());
  expect(own >= 1 && own <= 4, "the worker's own integrity level is readable");
  const uint32_t front = secure::worker::foregroundLevel();
  expect(front <= 4 || front == secure::integrityUnknown, "the window in front has a level, or cannot be read");
}

// The whole way of one input over the real objects: the service's link writes to
// the real pipe, the injector's reader thread takes it, the injector's loop
// checks its scope and calls the (fake) injection. Measured per input.
void throughput() {
  const std::wstring name = L"\\\\.\\pipe\\MultitapHost.SecureInputTest." + std::to_wstring(GetCurrentProcessId()) + L"." + randomName();
  WinHandle write(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 0, 0, nullptr));
  WinHandle read(CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
  HANDLE process = CreateEventW(nullptr, TRUE, FALSE, nullptr), processForLink = nullptr;
  DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &processForLink, 0, FALSE, DUPLICATE_SAME_ACCESS);
  auto link = std::make_shared<secure::HelperLink>(WinHandle(), WinHandle(processForLink), std::move(write));
  ServiceSystemInput service;
  service.attach(link);

  LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
  const auto ticks = [] { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; };
  std::atomic<long long> injectedAt{0};
  std::atomic<int> injectedCount{0};
  std::atomic<bool> lock{false};
  std::atomic<int> exitCode{-1};
  secure::ipc::InputCounts counts;
  std::thread injector([&] {
    secure::entry::InputReader reader(read.value);
    InjectorConfig config; config.session = 1; config.userSid = kUser;
    InjectorIo io;
    // The observations cost what they cost in the product only on Windows itself; here they are in-process,
    // so the measured time is the transport and the loop.
    io.console = [&] { ConsoleView v{1, !lock.load(), false, kUser, 0}; v.observedAt = GetTickCount64(); return v; };
    io.self = [] { return InjectorView{true, 1, true, true}; };
    io.nowMs = [] { return uint64_t(GetTickCount64()); };
    io.next = [&](uint32_t milliseconds) { return reader.wait(milliseconds); };
    io.attach = [] { return true; };
    io.inject = [&](const Json&) { injectedAt = ticks(); ++injectedCount; return true; };
    io.text = [](const Json&) { return true; };
    io.touch = [](const Json&) { return true; };
    io.area = [](const Json&) { return true; };
    io.release = [] {};
    io.stopped = [&] { return reader.closed(); };
    io.note = [&](Note what) { if (what == Note::received) ++counts.received; else if (what == Note::injected) ++counts.injected; };
    exitCode = int(runSystemInput(config, io));
  });
  std::vector<double> micros;
  const int total = 2000;
  for (int i = 0; i < total; ++i) {
    auto command = JsonParser::parse("{\"type\":\"input\",\"sessionId\":\"s1\",\"event\":\"move\",\"x\":0.5,\"y\":0.5,\"n\":" + std::to_string(i) + "}");
    const int before = injectedCount.load();
    const long long sentAt = ticks();
    if (!service.route(command)) break;
    for (int spin = 0; spin < 2000000 && injectedCount.load() == before; ++spin) { if (spin > 20000) std::this_thread::yield(); }
    if (injectedCount.load() == before) break;
    micros.push_back(double(injectedAt.load() - sentAt) * 1000000.0 / double(frequency.QuadPart));
  }
  expect(int(micros.size()) == total, "every input written by the service reached the injection call, one by one");
  std::sort(micros.begin(), micros.end());
  double sum = 0; for (const double v : micros) sum += v;
  const double mean = micros.empty() ? 0 : sum / double(micros.size());
  const double median = micros.empty() ? 0 : micros[micros.size() / 2], p99 = micros.empty() ? 0 : micros[micros.size() * 99 / 100], worst = micros.empty() ? 0 : micros.back();
  expect(median < 2000.0, "service write to injection call: under 2 ms at the median");
  // A burst: 500 inputs written without waiting arrive in order and none is lost below the reader's bound for moves.
  const int burstBefore = injectedCount.load();
  int written = 0;
  for (int i = 0; i < 200; ++i) { auto command = JsonParser::parse(R"({"type":"input","sessionId":"s1","event":"key","vk":65,"down":true})"); if (service.route(command)) ++written; }
  for (int spin = 0; spin < 400 && injectedCount.load() < burstBefore + written; ++spin) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  expect(written == 200 && injectedCount.load() == burstBefore + 200, "a burst of 200 key events is injected completely");
  // The session locks: the injector ends by itself, and the service gives the next input to the worker.
  lock = true;
  for (int spin = 0; spin < 600 && exitCode.load() < 0; ++spin) { auto poke = JsonParser::parse(R"({"type":"input","sessionId":"s1","event":"key","vk":65,"down":false})"); (void)service.route(poke); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
  injector.join();
  expect(exitCode.load() == int(InjectorExit::scopeLost), "a lock ends the injector over the real transport too");
  SetEvent(process);  // the process is gone
  auto after = JsonParser::parse(R"({"type":"input","sessionId":"s1","event":"key","vk":65,"down":true})");
  expect(!service.route(after) && !hasMarker(after), "after the injector ended the worker gets the input, unmarked");
  const auto serviceCounts = service.close();
  expect(serviceCounts.routed >= uint32_t(total + 200) && serviceCounts.fellBack >= 1, "the service counted what it wrote and what fell back");
  CloseHandle(process);
  std::cout << "{\"kind\":\"system-input-transport\",\"inputs\":" << micros.size() << ",\"meanUs\":" << mean << ",\"medianUs\":" << median << ",\"p99Us\":" << p99 << ",\"maxUs\":" << worst
            << ",\"injectorReceived\":" << counts.received << ",\"injectorInjected\":" << counts.injected << ",\"osInputEmitted\":false}\n";
}

// The service stops (an update, a shutdown) as its own transition: the relay ends
// while the control path may still be routing, the injector lets go of what it
// holds and exits, and no new child may be started any more.
struct CountingHelper final : secure::ServiceHelper {
  std::atomic<bool> closed{false};
  std::atomic<int> sent{0}, sentAfterClose{0}, closes{0};
  bool alive() override { return !closed; }
  bool send(const std::string&) override { if (closed) ++sentAfterClose; ++sent; return true; }
  void close() override { closed = true; ++closes; }
};
void serviceStop() {
  // 1. The relay's end races the control path: one close, and nothing is written to the injector after it.
  {
    ServiceSystemInput service; auto helper = std::make_shared<CountingHelper>();
    service.attach(helper);
    std::atomic<bool> go{true};
    std::atomic<int> routed{0}, unrouted{0};
    std::thread control([&] {
      while (go) {
        auto command = JsonParser::parse(R"({"type":"input","sessionId":"s1","event":"key","vk":65,"down":true})");
        if (service.route(command)) ++routed; else ++unrouted;
      }
    });
    for (int spin = 0; spin < 2000 && routed.load() < 200; ++spin) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto counts = service.close();
    const int routedAtStop = routed.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    go = false;
    control.join();
    expect(routedAtStop >= 200 && helper->closes.load() == 1 && helper->sentAfterClose.load() == 0, "service stop: the injector is ended once and gets nothing after it");
    expect(routed.load() - routedAtStop <= 1 && unrouted.load() > 0, "service stop: the control path's next inputs are no longer routed");
    expect(int(counts.routed) >= 200 && int(counts.routed) <= routed.load(), "service stop: the counts are those of the ended relay");
    auto late = std::make_shared<CountingHelper>();
    service.attach(late);
    expect(late->closes.load() == 1 && !service.held(), "service stop: an injector that would attach afterwards is ended at once");
  }
  // 2. The injector's side over the real pipe: the service closes its end while a key is held; the injector
  //    releases it on the user's desktop and exits by itself.
  {
    const std::wstring name = L"\\\\.\\pipe\\MultitapHost.SecureInputTest." + std::to_wstring(GetCurrentProcessId()) + L"." + randomName();
    WinHandle write(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 0, 0, nullptr));
    WinHandle read(CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    HANDLE ended = CreateEventW(nullptr, TRUE, FALSE, nullptr), endedForLink = nullptr;
    DuplicateHandle(GetCurrentProcess(), ended, GetCurrentProcess(), &endedForLink, 0, FALSE, DUPLICATE_SAME_ACCESS);
    auto link = std::make_shared<secure::HelperLink>(WinHandle(), WinHandle(endedForLink), std::move(write));
    ServiceSystemInput service;
    service.attach(link);
    std::atomic<int> held{0}, releases{0}, exitCode{-1};
    std::thread injector([&] {
      {
        secure::entry::InputReader reader(read.value);
        InjectorConfig config; config.session = 1; config.userSid = kUser;
        InjectorIo io;
        io.console = [] { ConsoleView v{1, true, false, kUser, 0}; v.observedAt = GetTickCount64(); return v; };
        io.self = [] { return InjectorView{true, 1, true, true}; };
        io.nowMs = [] { return uint64_t(GetTickCount64()); };
        io.next = [&](uint32_t milliseconds) { return reader.wait(milliseconds); };
        io.attach = [] { return true; };
        io.inject = [&](const Json& message) { held += message.get("down").boolean() ? 1 : -1; return true; };
        io.text = [](const Json&) { return true; };
        io.touch = [](const Json&) { return true; };
        io.area = [](const Json&) { return true; };
        io.release = [&] { ++releases; held = 0; };
        io.stopped = [&] { return reader.closed(); };
        exitCode = int(runSystemInput(config, io));
      }
      SetEvent(ended);
    });
    auto down = JsonParser::parse(R"({"type":"input","sessionId":"s1","event":"key","vk":16,"down":true})");
    expect(service.route(down), "a key is pressed through the injector");
    for (int spin = 0; spin < 400 && held.load() != 1; ++spin) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    expect(held.load() == 1 && releases.load() == 0, "the injector holds the key");
    const auto began = std::chrono::steady_clock::now();
    (void)service.close();  // the service stops
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    injector.join();
    expect(exitCode.load() == int(InjectorExit::transportClosed) && releases.load() == 1 && held.load() == 0, "service stop: the injector releases what it holds and exits by itself");
    expect(took < 500, "service stop: the relay does not wait for the forced end (the injector left in time)");
    CloseHandle(ended);
  }
  // 3. A stopping service starts no child: the worker registry refuses the creation.
  {
    service_workers::ServiceLifetime lifetime;
    wchar_t image[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, image, MAX_PATH);
    HANDLE inheritable = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetHandleInformation(inheritable, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    bool started = false, refused = false;
    lifetime.closeAdmission();
    try { started = secure::detail::spawnSystemChild(image, 1, L"winsta0\\Default", L"--system-input-v1", {inheritable}, service_workers::Role::systemInput).has_value(); }
    catch (const std::exception& error) { refused = std::string(error.what()) == "service_worker_creation_closed"; }
    expect(!started && refused, "service stop: no SYSTEM child is created once the service stops admitting workers");
    expect(!secure::launchSystemInput(image, 1, GetCurrentProcess(), kUser, 0).has_value(), "service stop: the injector's launch answers no");
    CloseHandle(inheritable);
  }
}

}  // namespace

int main() {
  serviceStop();
  decision();
  injectorLoop();
  serviceRouting();
  geometry();
  reach();
  throughput();
  if (failures) { std::cerr << failures << " failure(s) of " << checks << " checks\n"; return 1; }
  std::cout << "system input: ok (" << checks << " checks)\n";
  return 0;
}
