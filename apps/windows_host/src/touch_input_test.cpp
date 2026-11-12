// touch-input-v1 on the host: frame validation and bounds, coordinate mapping
// onto the streamed rectangle (any monitor, negative origins, a dual-output
// composite), the injected POINTER_TOUCH_INFO frames for every phase, the
// Windows frame rules (all contacts per frame, UP at the previous position,
// keep-alive refresh, NOT_READY retry), release on reset/failure/hold limit,
// session gating on the actual worker Agent path and Session0 forwarding.
// The injection backend is substituted: no OS touch input is emitted.
#include "agent.hpp"
#include <iostream>
using namespace host;
namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
const std::string session = "00000000-0000-4000-8000-000000000081";
const RECT screen{0, 0, 1920, 1080};
std::string start(const std::string& extra) {
  return "{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"" + session + "\",\"resizeProtocol\":\"epoch-v1\",\"epoch\":0,\"monitor\":\"0\"" + extra + '}';
}
const std::string granted = ",\"touchInputProtocol\":\"touch-input-v1\"";
std::string contact(int id, const char* phase, double x = 0.5, double y = 0.5, const std::string& extra = "") {
  std::ostringstream out; out.precision(17);
  out << "{\"id\":" << id << ",\"phase\":\"" << phase << "\",\"x\":" << x << ",\"y\":" << y << extra << '}';
  return out.str();
}
std::string frame(const std::vector<std::string>& contacts) {
  std::string out = "{\"type\":\"input\",\"event\":\"touch\",\"contacts\":[";
  for (size_t n = 0; n < contacts.size(); ++n) out += (n ? "," : "") + contacts[n];
  return out + "]}";
}
std::optional<std::vector<TouchContactInput>> parse(const std::string& json) { return touchInputFrame(JsonParser::parse(json)); }

// Substituted InitializeTouchInjection/InjectTouchInput.
struct Fake {
  std::vector<std::vector<POINTER_TOUCH_INFO>> frames;
  std::vector<DWORD> results;  // per inject call; 0 when exhausted
  int initializations = 0; bool initializeFails = false;
  std::mutex mutex;
  TouchInjectionBackend backend() {
    return {[this] { std::lock_guard lock(mutex); ++initializations; return !initializeFails; },
            [this](const std::vector<POINTER_TOUCH_INFO>& value) -> DWORD {
              std::lock_guard lock(mutex); frames.push_back(value);
              if (results.empty()) return 0;
              const DWORD result = results.front(); results.erase(results.begin()); return result;
            }};
  }
  size_t count() { std::lock_guard lock(mutex); return frames.size(); }
  std::vector<POINTER_TOUCH_INFO> last() { std::lock_guard lock(mutex); return frames.back(); }
  std::vector<POINTER_TOUCH_INFO> at(size_t n) { std::lock_guard lock(mutex); return frames.at(n); }
};
constexpr DWORD downFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
constexpr DWORD updateFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
constexpr DWORD cancelFlags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
bool is(const POINTER_TOUCH_INFO& info, UINT32 pointer, DWORD flags, LONG x, LONG y) {
  return info.pointerInfo.pointerType == PT_TOUCH && info.pointerInfo.pointerId == pointer && info.pointerInfo.pointerFlags == flags &&
         info.pointerInfo.ptPixelLocation.x == x && info.pointerInfo.ptPixelLocation.y == y && !info.pointerInfo.dwTime && !info.pointerInfo.PerformanceCount;
}
using Time = RemoteTouch::TimePoint;
const Time t0 = Time{} + std::chrono::hours(1);
std::vector<TouchContactInput> contacts(const std::vector<std::string>& values) { return *parse(frame(values)); }

void validation() {
  require(parse(frame({contact(0, "down")})).has_value(), "A down was refused");
  require(parse(frame({contact(1, "move", 0, 0), contact(2, "up", 1, 1), contact(3, "cancel")})).has_value(), "Phases or edges were refused");
  require(parse(frame({contact(2147483647, "down", 0.1, 0.2, ",\"pressure\":1,\"w\":1,\"h\":1")})).has_value(), "The largest id or full size was refused");
  require(parse(R"({"type":"input","event":"touch","contacts":[{"id":0,"phase":"down","x":0.5,"y":0.5}],"sessionId":"s"})").has_value(), "A forwarded sessionId was refused");
  std::vector<std::string> ten; for (int n = 0; n < 10; ++n) ten.push_back(contact(n, "move", n / 10.0, 1 - n / 10.0));
  require(parse(frame(ten)) && parse(frame(ten))->size() == 10, "Ten contacts were refused");
  auto eleven = ten; eleven.push_back(contact(10, "move"));
  const auto parsed = parse(frame({contact(5, "move", 0.25, 0.75, ",\"pressure\":0.5,\"w\":0.01,\"h\":0.02")}));
  require(parsed && parsed->at(0).id == 5 && parsed->at(0).phase == TouchPhase::move && parsed->at(0).x == 0.25 && parsed->at(0).y == 0.75 &&
          parsed->at(0).pressure == 0.5 && parsed->at(0).width == 0.01 && parsed->at(0).height == 0.02, "Fields were not parsed exactly");
  for (const auto& bad : {frame({}), frame(eleven), frame({contact(1, "down"), contact(1, "move")}), frame({contact(-1, "down")}),
                          frame({"{\"id\":2147483648,\"phase\":\"down\",\"x\":0.5,\"y\":0.5}"}), frame({"{\"id\":1.5,\"phase\":\"down\",\"x\":0.5,\"y\":0.5}"}),
                          frame({"{\"id\":\"1\",\"phase\":\"down\",\"x\":0.5,\"y\":0.5}"}), frame({contact(1, "press")}), frame({contact(1, "down", -0.01)}),
                          frame({contact(1, "down", 0.5, 1.01)}), frame({"{\"id\":1,\"phase\":\"down\",\"x\":\"0.5\",\"y\":0.5}"}), frame({contact(1, "down", 0.5, 0.5, ",\"pressure\":1.5")}),
                          frame({contact(1, "down", 0.5, 0.5, ",\"w\":0.1")}), frame({contact(1, "down", 0.5, 0.5, ",\"h\":0.1")}), frame({contact(1, "down", 0.5, 0.5, ",\"w\":0,\"h\":0.1")}),
                          frame({contact(1, "down", 0.5, 0.5, ",\"orientation\":90")}), frame({"{\"id\":1,\"phase\":\"down\",\"x\":0.5}"}), frame({"[1]"}), frame({"null"}),
                          std::string(R"({"type":"input","event":"touch","contacts":[{"id":0,"phase":"down","x":0.5,"y":0.5}],"sample":1})"),
                          std::string(R"({"type":"input","event":"touch","contacts":{}})"), std::string(R"({"type":"input","event":"touch"})"),
                          std::string(R"({"type":"input","event":"move","contacts":[{"id":0,"phase":"down","x":0.5,"y":0.5}]})"), std::string("[]")})
    require(!parse(bad), "A malformed touch frame was accepted");
  require(touchInputSessionRequested(JsonParser::parse(start(granted))), "A granted start was not recognized");
  require(!touchInputSessionRequested(JsonParser::parse(start(""))), "An ungranted start enabled touch");
  require(!touchInputSessionRequested(JsonParser::parse(start(",\"touchInputProtocol\":\"touch-input-v2\""))), "Another protocol value enabled touch");
  require(!touchInputSessionRequested(JsonParser::parse(start(granted + ",\"mediaProtocol\":\"tracks-v1\""))), "Independent outputs enabled touch");
  require(std::string(touchInputProtocol) == "touch-input-v1", "Protocol marker changed");
}

void mapping() {
  const auto point = [](double x, double y, RECT area) { const auto value = touchInputPoint(x, y, area); return value ? std::pair{value->x, value->y} : std::pair{LONG(-99999), LONG(-99999)}; };
  require(point(0, 0, screen) == std::pair{LONG(0), LONG(0)} && point(1, 1, screen) == std::pair{LONG(1919), LONG(1079)}, "Corners did not map to the first/last pixel");
  require(point(0.5, 0.5, screen) == std::pair{LONG(960), LONG(540)}, "The centre did not map like the mouse path");
  // Monitor 1 right of the primary, an output left of it (negative origin),
  // and the dual-output composite: the position lands on the right output.
  require(point(0, 0, RECT{1920, 0, 3200, 720}) == std::pair{LONG(1920), LONG(0)} && point(1, 1, RECT{1920, 0, 3200, 720}) == std::pair{LONG(3199), LONG(719)}, "A second output was not mapped onto itself");
  require(point(0, 0.5, RECT{-1280, -100, 0, 620}) == std::pair{LONG(-1280), LONG(260)}, "A negative origin was not kept");
  require(point(0.75, 0.5, RECT{0, 0, 3840, 1080}).first == 2879 && point(0.25, 0.5, RECT{0, 0, 3840, 1080}).first == 960, "A composite position did not land on its output");
  require(!touchInputPoint(0.5, 0.5, RECT{}) && !touchInputPoint(0.5, 0.5, RECT{0, 0, 1, 1080}) && !touchInputPoint(1.5, 0.5, screen), "An empty area or out-of-range position mapped");
}

void phases() {
  Fake fake; RemoteTouch touch(fake.backend(), [] { return screen; }, false);
  require(touch.apply(contacts({contact(7, "down", 0.25, 0.5)}), screen, t0), "A down was refused");
  require(fake.initializations == 1 && fake.count() == 1 && fake.last().size() == 1 && is(fake.last()[0], 0, downFlags, 480, 540), "The down frame differs");
  require(touch.apply(contacts({contact(7, "move", 0.5, 0.5)}), screen, t0), "A move was refused");
  require(fake.count() == 2 && is(fake.last()[0], 0, updateFlags, 960, 540), "The move frame differs");
  // A second finger: every held contact is in every frame.
  require(touch.apply(contacts({contact(9, "down", 0.75, 0.25)}), screen, t0), "A second down was refused");
  require(fake.last().size() == 2 && is(fake.last()[0], 0, updateFlags, 960, 540) && is(fake.last()[1], 1, downFlags, 1439, 270), "The second down frame differs");
  // Pinch: both move in one frame.
  require(touch.apply(contacts({contact(7, "move", 0.4, 0.5), contact(9, "move", 0.6, 0.5)}), screen, t0), "A pinch frame was refused");
  require(fake.count() == 4 && fake.last().size() == 2 && is(fake.last()[0], 0, updateFlags, 768, 540) && is(fake.last()[1], 1, updateFlags, 1151, 540), "The pinch frame differs");
  // Up where it was: one frame, the other contact UPDATE.
  require(touch.apply(contacts({contact(9, "up", 0.6, 0.5)}), screen, t0) && fake.count() == 5, "An up at its position took more than one frame");
  require(fake.last().size() == 2 && is(fake.last()[0], 0, updateFlags, 768, 540) && is(fake.last()[1], 1, POINTER_FLAG_UP, 1151, 540) && touch.held() == 1, "The up frame differs");
  // Up somewhere else: an UPDATE there first, then UP at that position.
  require(touch.apply(contacts({contact(7, "up", 0.5, 0.25)}), screen, t0) && fake.count() == 7, "An up at a new position was not preceded by an update");
  require(is(fake.at(5)[0], 0, updateFlags, 960, 270) && is(fake.at(6)[0], 0, POINTER_FLAG_UP, 960, 270) && touch.held() == 0, "The moved up differs");
  // The slot is free again; a cancel ends where the contact was.
  require(touch.apply(contacts({contact(3, "down", 0.1, 0.1)}), screen, t0) && is(fake.last()[0], 0, downFlags, 192, 108), "A reused slot differs");
  require(touch.apply(contacts({contact(3, "cancel", 0.9, 0.9)}), screen, t0) && is(fake.last()[0], 0, cancelFlags, 192, 108) && touch.held() == 0, "A cancel did not end at the previous position");
  require(fake.initializations == 1, "The injection context was initialized more than once");
}

void sequencing() {
  Fake fake; RemoteTouch touch(fake.backend(), [] { return screen; }, false);
  require(touch.apply(contacts({contact(1, "down")}), screen, t0), "A down was refused");
  const auto before = fake.count();
  require(!touch.apply(contacts({contact(1, "down")}), screen, t0) && fake.count() == before && touch.held() == 1, "A second down of a held contact was injected");
  require(!touch.apply(contacts({contact(2, "down"), contact(1, "down")}), screen, t0) && fake.count() == before && touch.held() == 1, "A refused frame was partly injected");
  // A reset the client has not seen yet: its move/up/cancel are skipped.
  require(touch.apply(contacts({contact(42, "move"), contact(43, "up"), contact(44, "cancel")}), screen, t0) && fake.count() == before, "Unknown contacts were injected");
  // Ten held; an eleventh is refused, but up and down in one frame fit.
  for (int n = 2; n <= 10; ++n) require(touch.apply(contacts({contact(n, "down", n / 20.0, 0.5)}), screen, t0), "A contact up to ten was refused");
  require(touch.held() == 10 && fake.last().size() == 10, "Ten contacts were not all in the frame");
  require(!touch.apply(contacts({contact(11, "down")}), screen, t0) && touch.held() == 10, "An eleventh contact was accepted");
  const auto ten = fake.count();
  require(touch.apply(contacts({contact(11, "down", 0.9, 0.9), contact(4, "up", 0.2, 0.5)}), screen, t0), "Up and down in one frame were refused");
  require(fake.count() == ten + 2 && fake.at(ten).size() == 10 && fake.at(ten + 1).size() == 10, "Up/down was not ended first, then started");
  require(fake.at(ten)[3].pointerInfo.pointerFlags == POINTER_FLAG_UP && fake.at(ten + 1)[3].pointerInfo.pointerFlags == downFlags && is(fake.at(ten + 1)[3], 3, downFlags, 1727, 971), "The freed slot was not reused for the new contact");
  // Areas the desktop does not contain, and no area at all, are refused.
  RemoteTouch outside(fake.backend(), [] { return screen; }, false);
  require(!outside.apply(contacts({contact(1, "down")}), RECT{1920, 0, 3840, 1080}, t0) && !outside.apply(contacts({contact(1, "down")}), RECT{}, t0), "A frame outside the desktop was accepted");
  RemoteTouch empty(TouchInjectionBackend{}, [] { return screen; }, false);
  require(!empty.apply(contacts({contact(1, "down")}), screen, t0), "An empty backend accepted a frame");
}

void attributes() {
  Fake fake; RemoteTouch touch(fake.backend(), [] { return screen; }, false);
  require(touch.apply(contacts({contact(1, "down", 0.5, 0.5, ",\"pressure\":0.5,\"w\":0.01,\"h\":0.02")}), screen, t0), "A sized down was refused");
  auto info = fake.last()[0];
  require(info.touchMask == (TOUCH_MASK_PRESSURE | TOUCH_MASK_CONTACTAREA) && info.pressure == 512, "Pressure was not scaled to 0..1024");
  require(info.rcContact.left == 960 - 10 && info.rcContact.right == 960 + 11 && info.rcContact.top == 540 - 11 && info.rcContact.bottom == 540 + 12, "The contact rectangle differs");
  // A move without them keeps both; at the edge the rectangle stays inside.
  require(touch.apply(contacts({contact(1, "move", 0, 0)}), screen, t0), "A move was refused");
  info = fake.last()[0];
  require(info.pressure == 512 && info.rcContact.left == 0 && info.rcContact.top == 0 && info.rcContact.right == 11 && info.rcContact.bottom == 12, "Kept size or edge clamping differs");
  Fake plain; RemoteTouch bare(plain.backend(), [] { return screen; }, false);
  require(bare.apply(contacts({contact(1, "down")}), screen, t0) && plain.last()[0].touchMask == TOUCH_MASK_NONE, "A contact without attributes got a mask");
}

void keepAlive() {
  Fake fake; RemoteTouch touch(fake.backend(), [] { return screen; }, false);
  touch.tick(t0);
  require(fake.count() == 0, "A keep-alive without contacts injected");
  require(touch.apply(contacts({contact(1, "down", 0.5, 0.5), contact(2, "down", 0.25, 0.25)}), screen, t0), "Downs were refused");
  touch.tick(t0 + std::chrono::milliseconds(49));
  require(fake.count() == 1, "A keep-alive came before its period");
  touch.tick(t0 + std::chrono::milliseconds(50));
  require(fake.count() == 2 && fake.last().size() == 2 && is(fake.last()[0], 0, updateFlags, 960, 540) && is(fake.last()[1], 1, updateFlags, 480, 270), "A held press was not refreshed at its position");
  // A client frame resets the hold limit; without one the contacts end.
  require(touch.apply(contacts({contact(1, "move", 0.5, 0.5)}), screen, t0 + std::chrono::seconds(20)), "A move was refused");
  touch.tick(t0 + std::chrono::seconds(49));
  require(touch.held() == 2, "Contacts ended before the hold limit");
  touch.tick(t0 + std::chrono::seconds(50));
  require(touch.held() == 0 && fake.last().size() == 2 && fake.last()[0].pointerInfo.pointerFlags == cancelFlags && fake.last()[1].pointerInfo.pointerFlags == cancelFlags, "The hold limit did not cancel every contact");
  require(touch.stats().keepAlives >= 2, "Keep-alives were not counted");
  // The automatic thread refreshes a held press on its own and stops with it.
  Fake automatic;
  {
    RemoteTouch timed(automatic.backend(), [] { return screen; }, true);
    require(timed.apply(contacts({contact(1, "down")}), screen, std::chrono::steady_clock::now()), "A timed down was refused");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    require(automatic.count() >= 3, "The keep-alive thread did not refresh a held press");
    timed.release();
    const auto released = automatic.count();
    require(automatic.last()[0].pointerInfo.pointerFlags == cancelFlags, "release() did not cancel");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    require(automatic.count() == released, "Keep-alives continued after release");
    require(timed.apply(contacts({contact(2, "down")}), screen, std::chrono::steady_clock::now()), "A down after release was refused");
  }
  require(automatic.last()[0].pointerInfo.pointerFlags == cancelFlags, "Destruction did not cancel a held contact");
}

void failures() {
  // ERROR_NOT_READY is retried; the frame then succeeds.
  Fake busy; busy.results = {ERROR_NOT_READY};
  RemoteTouch retried(busy.backend(), [] { return screen; }, false);
  require(retried.apply(contacts({contact(1, "down")}), screen, t0) && busy.count() == 2 && retried.held() == 1, "NOT_READY was not retried");
  // Any other failure cancels and forgets every contact; the next frame starts fresh.
  Fake denied; RemoteTouch touch(denied.backend(), [] { return screen; }, false);
  require(touch.apply(contacts({contact(1, "down"), contact(2, "down", 0.1, 0.1)}), screen, t0), "Downs were refused");
  denied.results = {ERROR_ACCESS_DENIED};
  require(!touch.apply(contacts({contact(1, "move", 0.6, 0.6)}), screen, t0), "A refused injection was reported as applied");
  require(touch.held() == 0 && denied.last().size() == 2 && denied.last()[0].pointerInfo.pointerFlags == cancelFlags && touch.stats().lastError == ERROR_ACCESS_DENIED, "A failure did not cancel every contact");
  require(touch.apply(contacts({contact(1, "down")}), screen, t0) && touch.held() == 1 && is(denied.last()[0], 0, downFlags, 960, 540), "A new press after a failure was refused");
  // A failed keep-alive (a desktop switch or display change) does the same.
  denied.results = {ERROR_INVALID_PARAMETER};
  touch.tick(t0 + std::chrono::seconds(1));
  require(touch.held() == 0, "A failed keep-alive kept its contacts");
  // An initialization failure refuses the frame and is tried again later.
  Fake uninitialized; uninitialized.initializeFails = true;
  RemoteTouch later(uninitialized.backend(), [] { return screen; }, false);
  require(!later.apply(contacts({contact(1, "down")}), screen, t0) && uninitialized.count() == 0, "A frame was injected without a context");
  uninitialized.initializeFails = false;
  require(later.apply(contacts({contact(1, "down")}), screen, t0) && uninitialized.initializations == 2, "Initialization was not retried");
  // release() with nothing held injects nothing.
  Fake idle; RemoteTouch quiet(idle.backend(), [] { return screen; }, false);
  quiet.release();
  require(idle.count() == 0 && idle.initializations == 0, "An idle release touched the injection API");
}

struct Recorded { std::vector<INPUT> keys; std::vector<std::vector<POINTER_TOUCH_INFO>> touches; size_t heldAtEnd = 0; };
Recorded runWorker(const std::string& startExtra, const std::vector<std::string>& inputs, bool stopAtEnd = true) {
  Recorded log; std::mutex mutex; Fake fake;
  {
    Agent agent([](const std::wstring&) {}, [](bool, std::vector<uint8_t>) {},
                [&](INPUT& value) -> UINT { std::lock_guard lock(mutex); log.keys.push_back(value); return 1U; }, [] { return screen; }, fake.backend());
    agent.inputLatencyTestStream(JsonParser::parse(start(startExtra)), screen);
    for (const auto& input : inputs) agent.localControl(input);
    log.heldAtEnd = agent.touchTestHeld();
    if (stopAtEnd) agent.localControl("{\"type\":\"session.stop\",\"sessionId\":\"" + session + "\"}");
  }
  std::lock_guard lock(fake.mutex); log.touches = fake.frames;
  return log;
}

void sessionGating() {
  const auto press = frame({contact(1, "down", 0.5, 0.5)});
  require(runWorker("", {press}).touches.empty(), "An ungranted session injected touch");
  require(runWorker(granted + ",\"mediaProtocol\":\"tracks-v1\"", {press}).touches.empty(), "Independent outputs injected touch");
  const auto touched = runWorker(granted, {press, frame({contact(1, "move", 0.25, 0.25)})}, false);
  require(touched.heldAtEnd == 1 && touched.touches.size() >= 3 && is(touched.touches[0][0], 0, downFlags, 960, 540) && is(touched.touches[1][0], 0, updateFlags, 480, 270), "A granted session did not inject its frames");
  require(touched.touches.back()[0].pointerInfo.pointerFlags == cancelFlags, "Worker destruction did not cancel the held contact");
  // Keys keep working beside touch; an input.reset (focus loss) cancels contacts.
  const auto reset = runWorker(granted, {press, R"({"type":"input","event":"key","vk":13,"scan":28,"down":true,"extended":false})", R"({"type":"input.reset"})"});
  require(reset.keys.size() == 2 && !(reset.keys[0].ki.dwFlags & KEYEVENTF_KEYUP) && (reset.keys[1].ki.dwFlags & KEYEVENTF_KEYUP), "Keys were not injected and released beside touch");
  require(reset.heldAtEnd == 0 && reset.touches.back()[0].pointerInfo.pointerFlags == cancelFlags, "input.reset did not cancel the contact");
  // session.stop (disconnect, session end) cancels as well.
  const auto stopped = runWorker(granted, {press, "{\"type\":\"session.stop\",\"sessionId\":\"" + session + "\"}"}, false);
  require(stopped.heldAtEnd == 0 && stopped.touches.back()[0].pointerInfo.pointerFlags == cancelFlags, "session.stop did not cancel the contact");
  // A malformed frame injects nothing; a frame of another session is ignored.
  require(runWorker(granted, {frame({contact(1, "down", 1.5, 0.5)})}).touches.empty(), "A malformed frame was injected");
  require(runWorker(granted, {R"({"type":"input","event":"touch","sessionId":"another","contacts":[{"id":1,"phase":"down","x":0.5,"y":0.5}]})"}).touches.empty(), "Another session's frame was injected");
}

struct Bridge final : AgentBridge {
  std::vector<Json> controls;
  void start(const Json&, Send) override {}
  void control(const Json& value) override { controls.push_back(value); }
  void stop() override {}
  std::string capabilities() override { return "{\"service\":true}"; }
  void displayControl(const Json&, Send) override {}
};
void serviceForwarding() {
  Bridge bridge; Agent agent([](const std::wstring&) {}, &bridge); agent.publicationTestOpen();
  agent.localControl(start(granted));
  const auto press = frame({contact(5, "down", 0.125, 0.875, ",\"pressure\":0.25")});
  agent.localControl(press);
  require(bridge.controls.size() == 1 && bridge.controls[0].get("event").string() == "touch" && !bridge.controls[0].get("sample").number(0), "The service changed or dropped a touch frame before its worker");
  const auto* forwarded = std::get_if<Json::Array>(&bridge.controls[0].get("contacts").value);
  require(forwarded && forwarded->size() == 1 && (*forwarded)[0].get("x").number() == 0.125 && (*forwarded)[0].get("pressure").number() == 0.25, "Contacts were changed on the way");
}
}  // namespace

int main() {
  const struct { const char* name; void (*run)(); } cases[] = {{"validation", validation}, {"mapping", mapping}, {"phases", phases}, {"sequencing", sequencing}, {"attributes", attributes},
                                                                {"keep-alive", keepAlive}, {"failures", failures}, {"session-gating", sessionGating}, {"service-forwarding", serviceForwarding}};
  unsigned passed = 0, failed = 0;
  for (const auto& value : cases) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
  std::cout << "{\"kind\":\"touch-input-v1\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << ",\"productionAgent\":true,\"injector\":\"substituted\",\"osInputEmitted\":false}\n";
  return failed ? 1 : 0;
}
