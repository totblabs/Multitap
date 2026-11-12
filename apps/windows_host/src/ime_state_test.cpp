// ime-state-v1 (ime_state.hpp): the rules of the reading, the wire, and when
// the session user's helper runs. Memory only: no process, window or IME.
#include "ime_state.hpp"
#include <iostream>

using namespace host;
using namespace host::ime;
namespace {
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
constexpr LANGID korean = MAKELANGID(LANG_KOREAN, SUBLANG_KOREAN), english = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), japanese = MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT);
Reading reading(LANGID language, std::optional<bool> open, std::optional<DWORD> conversion) {
  Reading r; r.window = true; r.reachable = true; r.language = language;
  if (open) { r.openKnown = true; r.open = *open; }
  if (conversion) { r.conversionKnown = true; r.conversion = *conversion; }
  return r;
}
void decisions() {
  require(decide(reading(korean, true, IME_CMODE_HANGUL)) == Mode::korean, "Korean IME in Hangul mode is not 한글");
  require(decide(reading(korean, true, IME_CMODE_HANGUL | IME_CMODE_FULLSHAPE)) == Mode::korean, "Other conversion bits hid Hangul mode");
  require(decide(reading(korean, true, IME_CMODE_ALPHANUMERIC)) == Mode::latin, "Korean IME in English mode is not Latin");
  require(decide(reading(korean, false, IME_CMODE_HANGUL)) == Mode::latin, "A closed Korean IME is not Latin");
  require(decide(reading(korean, std::nullopt, IME_CMODE_HANGUL)) == Mode::unknown, "A missing open status was guessed");
  require(decide(reading(korean, true, std::nullopt)) == Mode::unknown, "A missing conversion mode was guessed");
  require(decide(reading(english, false, std::nullopt)) == Mode::latin, "A US layout without an open IME is not Latin");
  require(decide(reading(english, std::nullopt, std::nullopt)) == Mode::unknown, "A layout without an IME answer was guessed");
  require(decide(reading(japanese, true, IME_CMODE_NATIVE)) == Mode::unknown, "An open Japanese IME was taken for Latin or 한글");
  auto elevated = reading(korean, true, IME_CMODE_HANGUL); elevated.reachable = false;
  require(decide(elevated) == Mode::unknown, "A window above the helper's integrity level was read");
  auto none = reading(korean, true, IME_CMODE_HANGUL); none.window = false;
  require(decide(none) == Mode::unknown, "No focus window (a secure desktop, a lock) was read");
  for (const auto mode : {Mode::korean, Mode::latin, Mode::unknown}) require(modeFromLetter(modeLetter(mode)) == mode, "Helper letter did not round-trip");
  require(!modeFromLetter('x') && !modeFromLetter('\n'), "A stray helper byte was accepted");
}
void wire() {
  const std::string id = "0f8fad5b-d9cb-469f-a165-70867728950e";
  const auto query = [&](const std::string& extra, const std::string& value) { return JsonParser::parse("{\"type\":\"host.ime.query\",\"protocol\":\"ime-state-v1\",\"id\":" + jsonQuote(value) + extra + '}'); };
  require(queryId(query("", id)) == id, "An exact query was refused");
  require(!queryId(query(",\"window\":\"x\"", id)), "A query with an unknown field was accepted");
  require(!queryId(query("", "0F8FAD5B-D9CB-469F-A165-70867728950E")) && !queryId(query("", "short")) && !queryId(query("", id + "0")), "A malformed id was accepted");
  require(!queryId(JsonParser::parse("{\"type\":\"host.ime.query\",\"protocol\":\"ime-state-v2\",\"id\":" + jsonQuote(id) + '}')), "Another protocol was accepted");
  require(statePacket(id, Mode::korean) == "{\"type\":\"host.ime.state\",\"protocol\":\"ime-state-v1\",\"id\":\"" + id + "\",\"imeKorean\":true}", "Korean answer changed");
  require(statePacket(id, Mode::latin) == "{\"type\":\"host.ime.state\",\"protocol\":\"ime-state-v1\",\"id\":\"" + id + "\",\"imeKorean\":false}", "Latin answer changed");
  require(statePacket(id, Mode::unknown) == "{\"type\":\"host.ime.state\",\"protocol\":\"ime-state-v1\",\"id\":\"" + id + "\"}", "Unknown must carry no imeKorean");
}
struct Fake {
  std::optional<DWORD> session = 2; uint64_t now = 1000;
  int starts = 0, asks = 0, alive = 0; bool failStart = false;
  std::deque<std::optional<Mode>> answers; std::vector<uint32_t> timeouts;
  std::function<void()> duringAsk;
  struct FakeChannel final : Channel {
    Fake& f; explicit FakeChannel(Fake& value) : f(value) { ++f.alive; }
    ~FakeChannel() override { --f.alive; }
    std::optional<Mode> ask(uint32_t timeout) override {
      ++f.asks; f.timeouts.push_back(timeout); if (f.duringAsk) f.duringAsk();
      if (f.answers.empty()) return std::nullopt; auto value = f.answers.front(); f.answers.pop_front(); return value;
    }
  };
  WatchAccess access() {
    WatchAccess value;
    value.session = [this] { return session; };
    value.start = [this](DWORD id) -> std::unique_ptr<Channel> { require(id == *session, "Helper started in another session"); ++starts; if (failStart) throw std::runtime_error("no token"); return std::make_unique<FakeChannel>(*this); };
    value.now = [this] { return now; };
    return value;
  }
};
void watchLifetime() {
  Fake f; Watch watch(f.access());
  require(!watch.running() && f.starts == 0, "A helper ran before any question");
  f.answers = {Mode::korean, Mode::latin};
  require(watch.query() == Mode::korean && f.starts == 1 && f.timeouts.back() == firstAnswerMs, "First question did not start one helper");
  require(watch.query() == Mode::latin && f.starts == 1 && f.timeouts.back() == answerMs, "Second question did not reuse the helper with a fresh sample");
  // Each question is a new sample: nothing is answered from memory.
  require(f.asks == 2, "A question was answered without asking the helper");
  f.now += idleMs - 1; watch.idle(); require(watch.running(), "Helper ended before a quiet minute");
  f.now += 1; watch.idle(); require(!watch.running() && f.alive == 0, "Helper outlived a quiet minute");
}
void watchSessions() {
  Fake f; Watch watch(f.access());
  f.session.reset();
  require(watch.query() == Mode::unknown && f.starts == 0, "A signed-out or locked console started a helper");
  f.session = 2; f.answers = {Mode::korean};
  require(watch.query() == Mode::korean, "Unlocked session was not read");
  f.session = 3; f.answers = {Mode::latin};
  require(watch.query() == Mode::latin && f.starts == 2 && f.alive == 1, "A new console session kept the old session's helper");
  f.session.reset(); watch.idle(); require(!watch.running(), "Helper kept running after the session locked");
  // A sample taken while the session locked is not this session's state.
  f.session = 3; f.answers = {Mode::korean}; f.duringAsk = [&] { f.session.reset(); };
  require(watch.query() == Mode::unknown && !watch.running(), "A sample across a lock was reported");
  f.duringAsk = {};
}
void watchFailures() {
  Fake f; Watch watch(f.access());
  f.answers = {Mode::latin}; require(watch.query() == Mode::latin, "Setup failed");
  // The helper stopped answering (its user signed out and in again): one new helper.
  f.answers = {std::nullopt, Mode::korean};
  require(watch.query() == Mode::korean && f.starts == 2 && f.alive == 1, "A dead helper was not replaced once");
  f.answers = {std::nullopt, std::nullopt};
  require(watch.query() == Mode::unknown && f.starts == 3 && !watch.running(), "A new helper that does not answer was retried or kept");
  f.failStart = true;
  require(watch.query() == Mode::unknown && !watch.running(), "A helper that cannot start was not unknown");
  f.failStart = false; f.answers = {Mode::unknown};
  require(watch.query() == Mode::unknown && watch.running(), "The helper's own unknown was changed");
}
void serviceAnswers() {
  Fake f; f.answers = {Mode::korean, Mode::latin};
  std::mutex mutex; std::condition_variable done; std::vector<std::string> sent;
  {
    Service service(f.access());
    const auto send = [&](const std::string& packet) { std::lock_guard lock(mutex); sent.push_back(packet); done.notify_all(); };
    require(service.post("0f8fad5b-d9cb-469f-a165-70867728950e", send), "First question refused");
    require(service.post("1f8fad5b-d9cb-469f-a165-70867728950e", send), "Second question refused");
    std::unique_lock lock(mutex);
    require(done.wait_for(lock, std::chrono::seconds(5), [&] { return sent.size() == 2; }), "Questions were not answered");
    require(sent[0].find("0f8fad5b") != std::string::npos && sent[0].find("\"imeKorean\":true") != std::string::npos, "First answer lost its id or state");
    require(sent[1].find("1f8fad5b") != std::string::npos && sent[1].find("\"imeKorean\":false") != std::string::npos, "Second answer lost its id or state");
  }
  require(f.alive == 0, "The helper outlived the service");
  Service stopped(f.access()); stopped.stop();
  require(!stopped.post("0f8fad5b-d9cb-469f-a165-70867728950e", [](const std::string&) {}), "A stopped service took a question");
}
}  // namespace
int main() {
  try { decisions(); wire(); watchLifetime(); watchSessions(); watchFailures(); serviceAnswers(); std::cout << "ime state tests passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
