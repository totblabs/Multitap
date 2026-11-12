// channel-resume-v1 on the Host (channel_redial.hpp): which ends of a print,
// drive or USB channel may be dialled again, whether the stream is away (this
// Host's Broker channel or the client), and the redial itself: three fresh
// pairs, nothing spent while the stream is away, one fresh set when it
// continues, a refusal that no resume brings back, and no dial after the
// session ended. Real threads, delays divided by 50; nothing is dialled for real.
#include "channel_redial.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace host;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool eventually(const std::function<bool()>& condition, int ms = 3000) {
  for (int i = 0; i < ms / 5; ++i) { if (condition()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
  return condition();
}
// Nothing more happens for a while (longer than every scaled delay: 20 + 40 + 80 ms).
void quiet() { std::this_thread::sleep_for(std::chrono::milliseconds(300)); }
struct Channel {
  ChannelRedial redial{50};
  std::atomic<unsigned> dials{0}; std::atomic<bool> refuse{false};
  void begin() { redial.begin([this] { ++dials; if (refuse) throw std::runtime_error("refused"); }); }
  bool dialled(unsigned count) { return eventually([&] { return dials == count && redial.idle(); }); }
};
}
int main() {
  try {
    int passed = 0;
    { // Only a lost transport or an absent stream is a transient end, for each channel by its own name.
      for (const std::string kind : {"print", "drive", "usb"}) {
        for (const char* suffix : {"_transport_closed", "_pair_timeout", "_stream_away", "_timeout"}) require(channelEndTransient(kind, kind + suffix), "A transient end was final");
        for (const std::string reason : {kind + "_policy_changed", kind + "_invalid_message", kind + "_rate_limit", kind + "_unavailable", std::string("authorization_revoked"), std::string("session_ended"), std::string("server_stopping"), std::string()})
          require(!channelEndTransient(kind, reason), "A final end was transient");
      }
      require(!channelEndTransient("print", "usb_stream_away") && !channelEndTransient("usb", "drive_transport_closed"), "Another channel's reason was taken");
      // The clipboard channel knows the new reason too (ClipboardReconnect).
      require(ClipboardReconnect::transientReason("clipboard_stream_away") && !ClipboardReconnect::transientReason("clipboard_policy_changed"), "The clipboard does not wait on an absent stream");
      ++passed;
    }
    { // A dial: 101 pairs, no answer and "not now" are transient, every other answer is a refusal.
      for (const unsigned long status : {0ul, 101ul, 408ul, 502ul, 503ul, 504ul}) require(!channelDialRefused(status), "A transient dial result was a refusal");
      for (const unsigned long status : {200ul, 400ul, 401ul, 403ul, 404ul, 409ul, 500ul}) require(channelDialRefused(status), "A refusal was retried");
      ++passed;
    }
    { // The stream is away while this Host's channel or the client is; each change is reported once.
      ChannelPresence presence;
      require(!presence.away() && !presence.host(false) && !presence.peer(false), "A fresh session was away");
      require(presence.peer(true) && presence.away(), "The client's absence was not noticed");
      require(!presence.host(true) && presence.away(), "A second absence was reported as a change");
      require(!presence.peer(false) && presence.away(), "The client's return ended the Host's own absence");
      require(presence.host(false) && !presence.away(), "Both back was not reported");
      require(presence.host(true) && !presence.peer(true), "The Host's absence was not noticed");
      presence.reset();
      require(!presence.away(), "A new session inherited an absence");
      ++passed;
    }
    { // A transient end is dialled again after the first delay; an end that keeps coming gets three, then waits.
      Channel channel; channel.begin();
      quiet(); require(channel.dials == 0, "A live channel was dialled");
      for (unsigned n = 1; n <= 3; ++n) { channel.redial.ended(true); require(channel.dialled(n), "A transient end was not dialled again"); }
      channel.redial.ended(true); quiet();
      require(channel.dials == 3 && channel.redial.waiting() && !channel.redial.stopped(), "A fourth attempt was made by itself");
      // The stream continues after an outage: one fresh set.
      channel.redial.away(); channel.redial.resumed();
      require(channel.dialled(4), "The resumed stream did not restart the waiting channel");
      ++passed;
    }
    { // While the stream is away nothing is dialled, however often the channel ends; then one fresh set of three.
      Channel channel; channel.begin();
      channel.redial.away();
      for (int n = 0; n < 5; ++n) channel.redial.ended(true);
      quiet(); require(channel.dials == 0 && channel.redial.waiting(), "A dial was made while the stream was away");
      channel.redial.resumed();
      for (unsigned n = 1; n <= 3; ++n) { require(channel.dialled(n), "The resumed channel was not dialled"); channel.redial.ended(true); }
      quiet(); require(channel.dials == 3 && channel.redial.waiting(), "The resumed channel got more than three attempts");
      ++passed;
    }
    { // The stream goes away during the delay: no dial then, and the resume starts over.
      Channel channel; channel.begin();
      channel.redial.ended(true); channel.redial.away();
      quiet(); require(channel.dials == 0, "A dial was made after the stream went away");
      channel.redial.resumed();
      require(channel.dialled(1), "The channel did not come back with the stream");
      ++passed;
    }
    { // A final end (a violation, a revoked permission, a refused dial) is never dialled, away or not, resumed or not.
      for (const bool away : {false, true}) {
        Channel channel; channel.begin();
        if (away) channel.redial.away();
        channel.redial.ended(false);
        require(eventually([&] { return channel.redial.stopped(); }), "A final end was kept open");
        channel.redial.resumed(); channel.redial.ended(true);
        quiet(); require(channel.dials == 0 && !channel.redial.waiting(), "A final end was dialled again");
      }
      ++passed;
    }
    { // A dial that cannot even start (a URL this Host refuses, a service that is closing) ends the channel.
      Channel channel; channel.refuse = true; channel.begin();
      channel.redial.ended(true);
      require(channel.dialled(1) && eventually([&] { return channel.redial.stopped(); }), "A dial that threw was not final");
      channel.refuse = false; channel.redial.ended(true); channel.redial.resumed();
      quiet(); require(channel.dials == 1, "A refused channel was dialled again");
      ++passed;
    }
    { // A resume with a live channel changes nothing, and a later end is an ordinary first attempt.
      Channel channel; channel.begin();
      channel.redial.away(); channel.redial.resumed();
      quiet(); require(channel.dials == 0, "A resume dialled a live channel");
      channel.redial.ended(true);
      require(channel.dialled(1), "An end after a resume was not dialled");
      ++passed;
    }
    { // The session ended: a scheduled dial is dropped, nothing is dialled later, and the next session starts clean.
      Channel channel; channel.begin();
      channel.redial.ended(true); channel.redial.end();
      channel.redial.ended(true); channel.redial.resumed();
      quiet(); require(channel.dials == 0, "A dial ran after the session ended");
      channel.begin(); channel.redial.ended(true);
      require(channel.dialled(1), "The next session did not start clean");
      // A session that never negotiated: nothing was begun, and reports change nothing.
      ChannelRedial idle{50};
      idle.away(); idle.ended(true); idle.resumed();
      require(idle.redials() == 0 && !idle.waiting(), "An unstarted redial acted");
      ++passed;
    }
    std::cout << "{\"kind\":\"host-channel-redial\",\"passed\":" << passed << "}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
