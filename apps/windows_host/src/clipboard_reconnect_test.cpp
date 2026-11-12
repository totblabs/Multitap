// ClipboardReconnect (clipboard_reconnect.hpp): three fresh pairs per session,
// no attempt spent while the stream is held (host-resume-v1), one fresh set for
// the resumed stream, and a refusal that no resume brings back.
#include "clipboard_reconnect.hpp"
#include <iostream>
#include <stdexcept>
using namespace host;
namespace {
using Step = ClipboardReconnect::Step;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
// The delays of the retries next() grants in a row, ending at the first other step.
struct Run { int retries = 0; long long total = 0; Step last = Step::retry; };
Run drain(ClipboardReconnect& reconnect) {
  Run run;
  for (;;) {
    std::chrono::milliseconds delay{};
    run.last = reconnect.next(true, delay);
    if (run.last != Step::retry) return run;
    ++run.retries; run.total += delay.count();
    if (run.retries > 16) throw std::runtime_error("The retries have no bound");
  }
}
}
int main() {
  try {
    int passed = 0;
    { // The released bound: 1, 2 and 4 seconds, then nothing more by itself.
      ClipboardReconnect reconnect;
      std::chrono::milliseconds delay{};
      require(reconnect.next(true, delay) == Step::retry && delay.count() == 1000, "The first retry is not after 1 s");
      require(reconnect.next(true, delay) == Step::retry && delay.count() == 2000, "The second retry is not after 2 s");
      require(reconnect.next(true, delay) == Step::retry && delay.count() == 4000, "The third retry is not after 4 s");
      require(reconnect.next(true, delay) == Step::wait && reconnect.waiting(), "A fourth attempt was granted");
      require(reconnect.next(true, delay) == Step::wait, "A spent channel dialled again without a resume");
      ++passed;
    }
    { // A refusal or a protocol failure ends the channel at once, and no resume brings it back.
      ClipboardReconnect reconnect;
      std::chrono::milliseconds delay{};
      require(reconnect.next(false, delay) == Step::stop && !reconnect.waiting(), "A refused channel was retried");
      require(!reconnect.resumed(), "A resume restarted a refused channel");
      ClipboardReconnect held;
      held.away();
      require(held.next(false, delay) == Step::stop && !held.waiting(), "A refusal during the hold was kept waiting");
      require(!held.resumed(), "A resume restarted a channel refused during the hold");
      ++passed;
    }
    { // While the stream is held nothing is spent: the channel waits, however often it fails.
      ClipboardReconnect reconnect;
      std::chrono::milliseconds delay{};
      reconnect.away();
      for (int i = 0; i < 8; ++i) require(reconnect.next(true, delay) == Step::wait, "A dial was granted while the stream was away");
      require(reconnect.waiting(), "The held channel is not waiting");
      // The stream continues: one fresh set of the same three.
      require(reconnect.resumed() && !reconnect.waiting(), "The resumed stream did not restart the waiting channel");
      const auto run = drain(reconnect);
      require(run.retries == 3 && run.total == 7000 && run.last == Step::wait, "The resumed channel did not get exactly three attempts");
      ++passed;
    }
    { // Attempts spent before the hold was noticed do not count against the resumed stream.
      ClipboardReconnect reconnect;
      std::chrono::milliseconds delay{};
      require(reconnect.next(true, delay) == Step::retry && reconnect.next(true, delay) == Step::retry, "The first two retries were not granted");
      reconnect.away();
      require(reconnect.next(true, delay) == Step::wait, "The third attempt was spent while the stream was away");
      require(reconnect.resumed(), "The resumed stream did not restart the channel");
      require(drain(reconnect).retries == 3, "The resumed channel kept the attempts spent before the hold");
      // The same for a channel whose three were gone before the Host knew its broker channel was lost.
      ClipboardReconnect spent;
      require(drain(spent).retries == 3 && spent.waiting(), "The released three were not spent");
      spent.away();
      require(spent.resumed() && drain(spent).retries == 3, "A channel spent before the hold was not restarted by the resume");
      ++passed;
    }
    { // A resume with nothing waiting changes nothing: a live channel keeps its own count.
      ClipboardReconnect reconnect;
      std::chrono::milliseconds delay{};
      require(reconnect.next(true, delay) == Step::retry, "The first retry was not granted");
      reconnect.away();
      require(!reconnect.resumed(), "A resume restarted a channel that was not waiting");
      require(drain(reconnect).retries == 2, "A resume without a waiting channel gave attempts back");
      ++passed;
    }
    { // Every resume gives one set, never more: a channel that is really broken ends each time.
      ClipboardReconnect reconnect;
      for (int resume = 0; resume < 3; ++resume) {
        require(drain(reconnect).retries == 3, "A set was not three attempts");
        require(reconnect.resumed(), "A waiting channel was not restarted");
      }
      require(drain(reconnect).retries == 3 && reconnect.waiting(), "The last set was not bounded");
      ++passed;
    }
    { // channel-resume-v1: a pair the Broker closed because the client (or this Host) is away is not a refusal.
      require(ClipboardReconnect::transientReason("clipboard_stream_away"), "An absent stream ended the clipboard for good");
      require(!ClipboardReconnect::transientReason("clipboard_unavailable") && !ClipboardReconnect::transientReason("clipboard_policy_changed") && !ClipboardReconnect::transientReason("authorization_revoked"), "A refusal was retried");
      ClipboardReconnect reconnect;
      std::chrono::milliseconds delay{};
      require(!reconnect.isAway(), "A fresh channel is away");
      // The client left: the pair ends with the reason, and the channel waits for it instead of dialling a refusal.
      reconnect.away();
      require(reconnect.isAway() && reconnect.next(ClipboardReconnect::transientReason("clipboard_stream_away"), delay) == Step::wait, "The channel dialled while the client was away");
      // The client is back: one fresh set.
      require(reconnect.resumed() && !reconnect.isAway() && drain(reconnect).retries == 3, "The returned client did not restart the channel");
      ++passed;
    }
    std::cout << "{\"kind\":\"host-clipboard-reconnect\",\"passed\":" << passed << "}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
