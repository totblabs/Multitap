// caret-v1 on the host: request/session gating, the screen-to-output caret
// mapping (offset origin, clipping, outside drop), the fixed report message,
// the 15/s change-only throttle with newest-wins coalescing, the Session0
// Agent handing watch requests to its worker, and a start/stop smoke run of
// the real WinEvent tracker (it must report once, with no text content).
#include "agent.hpp"
#include <iostream>
using namespace host;
namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
const std::string session = "00000000-0000-4000-8000-000000000072";
std::string start(const std::string& extra) {
  return "{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"" + session + "\",\"resizeProtocol\":\"epoch-v1\",\"epoch\":0,\"monitor\":\"0\"" + extra + '}';
}
const std::string granted = ",\"caretProtocol\":\"caret-v1\"";

void gating() {
  require(caretSessionRequested(JsonParser::parse(start(granted))), "A granted start was not recognized");
  require(!caretSessionRequested(JsonParser::parse(start(""))), "An ungranted start enabled caret");
  require(!caretSessionRequested(JsonParser::parse(start(",\"caretProtocol\":\"caret-v2\""))), "Another protocol value enabled caret");
  require(!caretSessionRequested(JsonParser::parse(start(granted + ",\"mediaProtocol\":\"tracks-v1\""))), "Independent outputs enabled caret");
  require(std::string(caretProtocol) == "caret-v1", "Protocol marker changed");
  require(caretWatchRequest(JsonParser::parse(R"({"type":"caret.watch","on":true})")) == std::optional<bool>(true), "watch on refused");
  require(caretWatchRequest(JsonParser::parse(R"({"type":"caret.watch","on":false,"sessionId":"x"})")) == std::optional<bool>(false), "watch off with sessionId refused");
  require(!caretWatchRequest(JsonParser::parse(R"({"type":"caret.watch"})")), "watch without on accepted");
  require(!caretWatchRequest(JsonParser::parse(R"({"type":"caret.watch","on":1})")), "numeric on accepted");
  require(!caretWatchRequest(JsonParser::parse(R"({"type":"caret.watch","on":true,"title":"x"})")), "extra field accepted");
}

void mapping() {
  const RECT area{1920, 0, 1920 + 1080, 2400};  // an owned output right of the primary
  const auto inside = caretToOutput(RECT{1920 + 100, 1200, 1920 + 102, 1230}, area);
  require(inside && inside->has && inside->x == 100 && inside->y == 1200 && inside->w == 2 && inside->h == 30 && inside->sw == 1080 && inside->sh == 2400, "Offset origin mapping");
  const auto thin = caretToOutput(RECT{2000, 10, 2000, 10}, area);
  require(thin && thin->w == 1 && thin->h == 1, "An empty caret rectangle was not made 1x1");
  const auto swapped = caretToOutput(RECT{2022, 1230, 2020, 1200}, area);
  require(swapped && swapped->w == 2 && swapped->h == 30, "A reversed rectangle was not normalized");
  const auto clipped = caretToOutput(RECT{1920 + 1070, 2390, 1920 + 1200, 2500}, area);
  require(clipped && clipped->x + clipped->w == 1080 && clipped->y + clipped->h == 2400, "A caret crossing the edge was not clipped");
  require(!caretToOutput(RECT{100, 100, 102, 120}, area), "A caret on the primary display mapped into the owned output");
  require(!caretToOutput(RECT{1920 + 1080, 5, 1920 + 1082, 20}, area), "A caret at the right edge was inside");
  require(!caretToOutput(RECT{2000, -5, 2002, 20}, area), "A caret above the output was inside");
  require(!caretToOutput(RECT{0, 0, 2, 2}, RECT{0, 0, 1, 1}), "An empty output mapped");
}

void message() {
  const auto none = caretMessage(session, CaretReport{});
  require(none == "{\"type\":\"caret\",\"sessionId\":\"" + session + "\",\"has\":false}", "The no-caret message changed");
  CaretReport r; r.has = true; r.x = 1; r.y = 2; r.w = 3; r.h = 4; r.sw = 5; r.sh = 6;
  const auto json = JsonParser::parse(caretMessage(session, r));
  const auto* fields = std::get_if<Json::Object>(&json.value);
  require(fields && fields->size() == 9 && json.get("has").boolean(false) && json.get("sw").number() == 5 && json.get("h").number() == 4, "The report fields changed");
}

void throttle() {
  using Clock = CaretThrottle::Clock;
  CaretThrottle gate(std::chrono::milliseconds(100));
  const auto t0 = Clock::now();
  CaretReport a; a.has = true; a.x = 10; a.w = 2; a.h = 20; a.sw = 1000; a.sh = 1000;
  auto b = a; b.y = 40; auto c = a; c.y = 80;
  require(gate.offer(a, t0) == a, "The first report was not sent at once");
  require(!gate.offer(a, t0 + std::chrono::milliseconds(150)), "An unchanged report was resent");
  require(gate.offer(b, t0 + std::chrono::milliseconds(200)) == b, "A change after the interval was not sent");
  require(!gate.offer(c, t0 + std::chrono::milliseconds(230)) && gate.pending(), "A change inside the interval was not held");
  require(gate.wait(t0 + std::chrono::milliseconds(230)).count() == 70, "The wait until the next send is wrong");
  auto d = a; d.y = 120;
  require(!gate.offer(d, t0 + std::chrono::milliseconds(250)), "A second change inside the interval was sent");
  require(!gate.due(t0 + std::chrono::milliseconds(290)), "A held change was sent early");
  require(gate.due(t0 + std::chrono::milliseconds(300)) == d, "The newest held change did not win");
  require(!gate.pending(), "Nothing should be pending after the send");
  // A change back to the sent value while held is dropped.
  require(!gate.offer(a, t0 + std::chrono::milliseconds(310)) && gate.pending(), "Held change not kept");
  require(gate.offer(d, t0 + std::chrono::milliseconds(320)) == std::nullopt && !gate.pending(), "A change reverting to the sent value was sent");
  // The no-caret state is a change like any other.
  require(gate.offer(CaretReport{}, t0 + std::chrono::milliseconds(500)) == CaretReport{}, "The no-caret report was not sent");
  // Sustained movement never exceeds the rate: 10 s of changes every 5 ms.
  CaretThrottle fast;
  unsigned sent = 0;
  for (int ms = 0; ms < 10000; ms += 5) { auto r = a; r.y = ms; if (fast.offer(r, t0 + std::chrono::milliseconds(ms))) ++sent; }
  require(sent <= 10000 / 66 + 1 && sent >= 100, "The throttle rate is outside ~15/s");
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
  agent.localControl("{\"type\":\"caret.watch\",\"on\":true}");
  require(bridge.controls.size() == 1 && bridge.controls[0].get("type").string() == "caret.watch" && bridge.controls[0].get("on").boolean(false), "The service did not hand the watch request to its worker");
}

void trackerSmoke() {
  std::mutex mutex; std::vector<std::string> reports;
  {
    CaretTracker tracker([&](const std::string& m) { std::lock_guard lock(mutex); reports.push_back(m); });
    tracker.configure(session, RECT{0, 0, 1920, 1080});
    tracker.watch(true);
    require(tracker.watching(), "The tracker did not start");
    for (int n = 0; n < 50; ++n) { { std::lock_guard lock(mutex); if (!reports.empty()) break; } std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    tracker.watch(true);  // idempotent
    tracker.watch(false);
    require(!tracker.watching(), "The tracker did not stop");
    tracker.watch(true); tracker.watch(false);  // restartable
  }
  std::lock_guard lock(mutex);
  require(!reports.empty(), "The tracker reported nothing");
  const auto json = JsonParser::parse(reports.front());
  require(json.get("type").string() == "caret" && json.get("sessionId").string() == session && std::get_if<bool>(&json.get("has").value), "The first report is malformed");
  const auto* fields = std::get_if<Json::Object>(&json.value);
  require(fields && fields->size() <= 9, "The report carries extra fields");
}
// A cross-process accessibility call that never returns (a hung focused application) must not hang the stop.
void trackerStopsDuringHungCall() {
  std::mutex mutex; std::vector<std::string> reports;
  std::atomic<bool> entered{false}, release{false}; std::atomic<int> finishedCalls{0};
  CaretTracker::setAccessibleQueryForTest([&](HWND, LONG) -> std::optional<RECT> {
    entered = true; while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ++finishedCalls; return RECT{10, 10, 12, 30};
  });
  size_t before = 0;
  const int abandonedBefore = CaretTracker::abandoned();
  {
    CaretTracker tracker([&](const std::string& m) { std::lock_guard lock(mutex); reports.push_back(m); }, std::chrono::milliseconds(200));
    tracker.configure(session, RECT{0, 0, 1920, 1080});
    tracker.watch(true);
    for (int n = 0; n < 100 && !CaretTracker::raiseLocationEventForTest(); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (int n = 0; n < 200 && !entered; ++n) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(entered, "The simulated hung call was never reached");
    const auto started = std::chrono::steady_clock::now();
    tracker.watch(false);
    const auto took = std::chrono::steady_clock::now() - started;
    require(took < std::chrono::seconds(3), "Stopping the tracker waited for the hung call");
    require(!tracker.watching() && CaretTracker::abandoned() == abandonedBefore + 1, "The hung tracker thread was not abandoned");
    { std::lock_guard lock(mutex); before = reports.size(); }
  }  // the tracker is gone while its old thread is still inside the call
  release = true;
  for (int n = 0; n < 200 && finishedCalls == 0; ++n) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  { std::lock_guard lock(mutex); require(reports.size() == before, "An abandoned tracker thread reported after it was stopped"); }
  CaretTracker::setAccessibleQueryForTest(nullptr);
  // A new tracker works after an abandoned one.
  {
    CaretTracker tracker([&](const std::string&) {}); tracker.configure(session, RECT{0, 0, 1920, 1080}); tracker.watch(true); tracker.watch(false);
  }
}
}  // namespace

int main() {
  const struct { const char* name; void (*run)(); } cases[] = {{"gating", gating}, {"mapping", mapping}, {"message", message}, {"throttle", throttle}, {"service-forwarding", serviceForwarding}, {"tracker-smoke", trackerSmoke}, {"tracker-hung-call", trackerStopsDuringHungCall}};
  unsigned passed = 0, failed = 0;
  for (const auto& value : cases) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
  std::cout << "{\"kind\":\"caret-v1\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << ",\"productionAgent\":true,\"osInputEmitted\":false}\n";
  return failed ? 1 : 0;
}
