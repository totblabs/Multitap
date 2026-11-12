// text-input-v1 on the host: message validation and bounds, the exact Unicode
// INPUT sequence (surrogate pairs included), session gating by the granted
// session.start marker on the actual worker Agent path (substituted injector,
// no OS input) and the Session0 Agent forwarding text unchanged to its worker.
#include "agent.hpp"
#include <iostream>
#include <tuple>
using namespace host;
namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
const std::string testSession = "00000000-0000-4000-8000-000000000071";
std::string start(const std::string& extra) {
  return "{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"" + testSession + "\",\"resizeProtocol\":\"epoch-v1\",\"epoch\":0,\"monitor\":\"0\"" + extra + '}';
}
const std::string granted = ",\"textInputProtocol\":\"text-input-v1\"";
std::string message(const std::string& text) { return "{\"type\":\"input\",\"event\":\"text\",\"text\":" + jsonQuote(text) + '}'; }
std::optional<std::wstring> units(const std::string& json) { return textInputUnits(JsonParser::parse(json)); }

void validation() {
  require(units(message("hello")) == std::wstring(L"hello"), "ASCII text was refused");
  require(units(message("\xec\x95\x88\xeb\x85\x95\xed\x95\x98\xec\x84\xb8\xec\x9a\x94")) == std::wstring(L"안녕하세요"), "Korean text was not decoded as UTF-16");
  // U+1F600 as raw UTF-8 and as a JSON surrogate escape: the same two units.
  require(units(message("\xf0\x9f\x98\x80")) == std::wstring(L"\xD83D\xDE00"), "An emoji was not one surrogate pair");
  require(units(R"({"type":"input","event":"text","text":"😀"})") == std::wstring(L"\xD83D\xDE00"), "An escaped surrogate pair was refused");
  require(units(R"({"type":"input","event":"text","text":"a","sessionId":"x"})").has_value(), "A forwarded sessionId was refused");
  // Exactly 256 units (128 emoji are 256 units) pass, one more does not.
  std::string emoji; for (int n = 0; n < 128; ++n) emoji += "\xf0\x9f\x98\x80";
  require(units(message(emoji)) && units(message(emoji))->size() == textInputMaxUnits, "The 256-unit limit was not accepted");
  require(!units(message(emoji + "a")), "257 units were accepted");
  require(!units(message(std::string(257, 'a'))), "257 ASCII characters were accepted");
  for (const auto& bad : {std::string(""), std::string("a\nb"), std::string("\t"), std::string("\r"), std::string("a\x7f"), std::string("\xc2\x85"), std::string("\xff"), std::string("\xed\xa0\xbd")})
    require(!units(message(bad)), "Empty, control or malformed text was accepted");
  for (const auto& bad : {R"({"type":"input","event":"text","text":"a","sample":1})", R"({"type":"input","event":"text","text":1})",
                          R"({"type":"input","event":"text"})", R"({"type":"input","event":"key","text":"a"})", R"({"type":"control","event":"text","text":"a"})",
                          R"({"type":"input","event":"text","text":["a"]})"})
    require(!units(bad), "A malformed text message was accepted");
  bool lone = false; try { (void)JsonParser::parse(R"({"type":"input","event":"text","text":"\ud83d"})"); } catch (...) { lone = true; }
  require(lone, "A lone surrogate escape reached the validator");
}

void events() {
  const auto planned = textInputEvents(L"A\xD83D\xDE00한");
  require(planned.size() == 8, "Not one down and one up per unit");
  const WORD expected[] = {L'A', 0xD83D, 0xDE00, 0xD55C};
  for (size_t n = 0; n < planned.size(); ++n) {
    const auto& input = planned[n];
    require(input.type == INPUT_KEYBOARD && input.ki.wVk == 0 && input.ki.wScan == expected[n / 2], "Unit order or virtual key differs");
    require(input.ki.dwFlags == DWORD(KEYEVENTF_UNICODE | (n % 2 ? KEYEVENTF_KEYUP : 0)), "Down/up or Unicode flag differs");
  }
}

struct Recorded { std::vector<INPUT> injected; std::vector<std::string> sent; };
Recorded runWorker(const std::string& startExtra, const std::vector<std::string>& inputs, int failAt = -1) {
  Recorded log; std::mutex mutex; int count = 0;
  {
    Agent agent([](const std::wstring&) {}, [&](bool text, std::vector<uint8_t> bytes) { std::lock_guard lock(mutex); if (text) log.sent.emplace_back(bytes.begin(), bytes.end()); },
                [&](INPUT& value) -> UINT { std::lock_guard lock(mutex); log.injected.push_back(value); return count++ == failAt ? 0U : 1U; }, [] { return RECT{0, 0, 1920, 1080}; });
    agent.inputLatencyTestStream(JsonParser::parse(start(startExtra)), RECT{0, 0, 1920, 1080});
    for (const auto& input : inputs) agent.localControl(input);
    agent.localControl("{\"type\":\"session.stop\",\"sessionId\":\"" + testSession + "\"}");
  }
  return log;
}

void sessionGating() {
  const auto text = message("\xed\x95\x9c\xea\xb8\x80");  // 한글
  require(runWorker("", {text}).injected.empty(), "An ungranted session injected text");
  require(runWorker(granted + ",\"mediaProtocol\":\"tracks-v1\"", {text}).injected.empty(), "Independent outputs injected text");
  const auto withText = runWorker(granted, {text});
  require(withText.injected.size() == 4, "A granted session did not inject both syllables");
  require(withText.injected[0].ki.wScan == 0xD55C && withText.injected[2].ki.wScan == 0xAE00, "The syllables were reordered");
  // Keys and text interleave in arrival order; text holds no key.
  const auto mixed = runWorker(granted, {R"({"type":"input","event":"key","vk":13,"scan":28,"down":true,"extended":false})",
                                         R"({"type":"input","event":"key","vk":13,"scan":28,"down":false,"extended":false})", message("ok")});
  require(mixed.injected.size() == 6 && !(mixed.injected[0].ki.dwFlags & KEYEVENTF_UNICODE) && (mixed.injected[2].ki.dwFlags & KEYEVENTF_UNICODE), "Keys and text were not injected in order");
  // A refused up stops the message; its release precedes the next message.
  const auto failed = runWorker(granted, {message("abc"), message("d")}, 1);
  require(failed.injected.size() == 3 + 2, "A failed Unicode release was lost or replayed its text");
  require(failed.injected[2].ki.wScan == L'a' && failed.injected[2].ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP), "The next message did not release the old Unicode unit first");
  require(failed.sent.empty(), "Text input produced a latency report");
  // Invalid text is refused whole: nothing of it is injected.
  require(runWorker(granted, {message("a\nb")}).injected.empty(), "Partially valid text was injected");
}

void failedReleases() {
  const std::string emoji = "\xf0\x9f\x98\x80";
  for (const auto& [text, unit] : {std::pair{std::string("Ab"), WORD(L'A')}, std::pair{emoji + "Z", WORD(0xD83D)}, std::pair{emoji + "Z", WORD(0xDE00)}}) {
    std::vector<INPUT> sent; bool deny = true;
    RemoteInput input([&](INPUT& event) -> UINT {
      sent.push_back(event);
      return deny && event.type == INPUT_KEYBOARD && event.ki.wScan == unit && event.ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP) ? 0U : 1U;
    }, false, [] { return RECT{0, 0, 1920, 1080}; });
    input.area(RECT{0, 0, 1920, 1080});
    require(input.apply(JsonParser::parse(R"({"event":"key","vk":162,"scan":29,"down":true,"extended":false})")), "Modifier fixture failed");
    require(input.apply(JsonParser::parse(R"({"event":"button","button":"left","down":true})")), "Mouse fixture failed");
    require(!input.text(JsonParser::parse(message(text))), "Denied Unicode up was acknowledged");
    require(input.pending() == 3, "Failed Unicode unit was not retained beside key and button");
    input.reset();
    require(input.pending() == 1, "Reset lost Unicode release or stranded the ordinary key/button");
    require(!input.text(JsonParser::parse(message("n"))), "New text bypassed a pending Unicode release");
    require(!input.apply(JsonParser::parse(R"({"event":"key","vk":66,"scan":48,"down":true,"extended":false})")), "New key bypassed a pending Unicode release");
    const auto downCount = [&](WORD scan) { return std::count_if(sent.begin(), sent.end(), [&](const INPUT& event) { return event.type == INPUT_KEYBOARD && event.ki.wScan == scan && event.ki.dwFlags == KEYEVENTF_UNICODE; }); };
    require(downCount(unit) == 1 && downCount(L'n') == 0 && downCount(L'Z') == 0, "Failed text or a later unit was replayed");
    deny = false; input.retry();
    require(input.pending() == 0 && sent.back().ki.wScan == unit && sent.back().ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP), "Retry changed the Unicode unit or its release flags");
    const auto settled = sent.size(); input.retry();
    require(sent.size() == settled, "Recovered Unicode release continued retrying");
    require(input.text(JsonParser::parse(message("n"))) && downCount(L'n') == 1 && input.pending() == 0, "Text did not recover after release");
  }
  for (const auto& [text, failAt, unit] : {std::tuple{std::string("Ab"), 1, WORD(L'A')}, std::tuple{emoji + "Z", 1, WORD(0xD83D)}, std::tuple{emoji + "Z", 3, WORD(0xDE00)}}) {
    const auto log = runWorker(granted, {message(text)}, failAt);
    require(log.injected.size() == size_t(failAt + 2) && log.injected.back().ki.wScan == unit && log.injected.back().ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP), "Session stop did not release the exact failed Unicode unit");
  }
  unsigned calls = 0;
  RemoteInput refusedDown([&](INPUT&) -> UINT { ++calls; return 0; });
  require(!refusedDown.text(JsonParser::parse(message("A"))) && refusedDown.pending() == 0, "Refused Unicode down became a held unit");
  refusedDown.reset(); refusedDown.retry();
  require(calls == 1, "A refused down caused a fabricated release");
}

void idleUnicodeRelease() {
  std::atomic<bool> deny{true}; std::atomic<unsigned> ups{0}, downs{0};
  std::mutex wakeMutex; std::condition_variable wake;
  RemoteInput input([&](INPUT& event) -> UINT {
    if (event.ki.dwFlags == KEYEVENTF_UNICODE) { ++downs; return 1; }
    ++ups; const bool accepted = !deny.load(); wake.notify_all(); return accepted ? 1U : 0U;
  }, true, [] { return RECT{0, 0, 1920, 1080}; });
  require(!input.text(JsonParser::parse(message("A"))), "Idle retry fixture did not fail its up");
  std::unique_lock lock(wakeMutex);
  require(wake.wait_for(lock, std::chrono::seconds(5), [&] { return ups.load() >= 2; }), "Unicode release waited for another input or pipe command");
  deny = false;
  require(wake.wait_for(lock, std::chrono::seconds(5), [&] { return input.pending() == 0; }), "Idle Unicode release did not recover");
  require(downs == 1, "Idle retry replayed a Unicode press");
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
  agent.localControl(message("\xf0\x9f\x98\x80"));
  require(bridge.controls.size() == 1 && bridge.controls[0].get("text").string() == "\xf0\x9f\x98\x80" && bridge.controls[0].get("event").string() == "text", "The service changed or dropped text before its worker");
  require(std::string(textInputProtocol) == "text-input-v1", "Protocol marker changed");
}
}  // namespace

int main() {
  const struct { const char* name; void (*run)(); } cases[] = {{"validation", validation}, {"events", events}, {"session-gating", sessionGating}, {"failed-releases", failedReleases}, {"idle-unicode-release", idleUnicodeRelease}, {"service-forwarding", serviceForwarding}};
  unsigned passed = 0, failed = 0;
  for (const auto& value : cases) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
  std::cout << "{\"kind\":\"text-input-v1\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << ",\"productionAgent\":true,\"injector\":\"substituted\",\"osInputEmitted\":false}\n";
  return failed ? 1 : 0;
}
