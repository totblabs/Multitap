// host-resume-v1 (host_resume.hpp): which lost channels keep the stream, what
// the next channel offers the Broker, and when the hold ends.
#include "host_resume.hpp"
#include "broker_reconnect.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace host;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
const std::string session = "0b5a8f0e-4c1f-4f7e-9c11-2f5a7d9e1c3b";
// A channel whose first ACK offered the protocol (or not).
void acknowledgedChannel(HostResumeHold& hold, bool resumable) { bool stop = false; hold.channel(); (void)hold.acknowledged(resumable, "", stop); }
}
int main() {
  try {
    int passed = 0;
    { // A transport loss of a granted stream is kept; a refusal or a deliberate end is not.
      for (const char* failure : {"transport_closed", "network_unavailable", "tls_failed", "server_unavailable"}) {
        HostResumeHold hold; acknowledgedChannel(hold, true);
        require(hold.lost(true, failure, 1000) && hold.held(), "A lost channel did not keep the stream");
      }
      for (const char* failure : {"protocol_rejected", "authorization_failed"}) {
        HostResumeHold hold; acknowledgedChannel(hold, true);
        require(!hold.lost(true, failure, 1000) && !hold.held(), "A refused channel kept the stream");
      }
      HostResumeHold idle; acknowledgedChannel(idle, true);
      require(!idle.lost(false, "transport_closed", 1000), "A Host without a stream held something");
      HostResumeHold released; acknowledgedChannel(released, false);
      require(!released.lost(true, "transport_closed", 1000), "A stream of a released Broker was kept");
      ++passed;
    }
    { // The next channel offers the kept stream once; the Broker's answer releases it.
      HostResumeHold hold; acknowledgedChannel(hold, true);
      require(hold.lost(true, "transport_closed", 1000), "Not held");
      hold.channel();
      require(hold.lost(true, "network_unavailable", 3000), "A failed reconnect ended the hold");
      hold.channel(); bool stop = true;
      const auto offer = hold.acknowledged(true, session, stop);
      require(!stop && offer && *offer == "{\"type\":\"host.session.resume\",\"sessions\":[\"" + session + "\"]}", "The kept stream was not offered");
      require(!hold.acknowledged(true, session, stop) && !stop, "A second ACK on the same channel offered it again");
      require(!hold.resumed("other", session) && hold.held(), "Another session's answer released the hold");
      require(hold.resumed(session, session) && !hold.held(), "The Broker's answer did not release the hold");
      ++passed;
    }
    { // With nothing kept the offer is empty, so a Broker ends a waiting session at once.
      HostResumeHold hold; bool stop = true;
      hold.channel();
      const auto offer = hold.acknowledged(true, session, stop);
      require(!stop && offer && *offer == "{\"type\":\"host.session.resume\",\"sessions\":[]}", "An idle Host did not say it kept nothing");
      ++passed;
    }
    { // A Broker that cannot resume ends the kept stream.
      HostResumeHold hold; acknowledgedChannel(hold, true);
      require(hold.lost(true, "transport_closed", 1000), "Not held");
      hold.channel(); bool stop = false;
      require(!hold.acknowledged(false, session, stop) && stop && !hold.held(), "A released Broker's ACK kept the stream");
      ++passed;
    }
    { // The hold ends by itself.
      HostResumeHold hold; acknowledgedChannel(hold, true);
      require(hold.lost(true, "transport_closed", 1000), "Not held");
      require(!hold.expired(1000 + HostResumeHold::holdMs - 1) && hold.expired(1000 + HostResumeHold::holdMs), "The hold did not end after holdMs");
      hold.clear();
      require(!hold.held() && !hold.expired(1000 + 2 * HostResumeHold::holdMs), "A cleared hold stayed");
      ++passed;
    }
    // Restart recovery (broker_reconnect.hpp): the waits between dials.
    { // Any end other than a broker restart keeps the released fixed 2 s.
      BrokerReconnectWait wait;
      for (uint64_t now = 0; now < 10; ++now) require(wait.next(false, now * 3000, uint32_t(now)) == 2000, "An ordinary end lost the released 2 s");
      ++passed;
    }
    { // A restart: 250 ms, 500 ms, then 1 s steps within 80-120 % (jitter), capped, then 2 s.
      for (const uint32_t random : {0u, 500u, 1000u, 123456789u}) {
        BrokerReconnectWait wait; uint64_t now = 1000;
        for (size_t step = 0; step < BrokerReconnectWait::restartMs.size(); ++step) {
          const uint32_t base = BrokerReconnectWait::restartMs[step], value = wait.next(step == 0, now, random);
          require(value >= base * 4 / 5 && value <= base * 6 / 5, "A restart step left its jitter band");
          now += value;
        }
        require(wait.next(false, now, random) == 2000 && !wait.restarting(), "The restart steps were not capped");
      }
      BrokerReconnectWait spread; require(spread.next(true, 0, 0) == 200 && BrokerReconnectWait().next(true, 0, 1000) == 300, "The jitter does not spread the first dial");
      ++passed;
    }
    { // A dial that got through ends the steps; a broker that keeps closing with 1012 is not dialled faster.
      BrokerReconnectWait wait;
      require(wait.next(true, 0, 500) == 250, "First restart step");
      wait.connected();
      require(wait.next(true, 5000, 500) == 2000, "A second restart close within rearmMs was dialled fast");
      require(wait.next(true, 5000 + BrokerReconnectWait::rearmMs, 500) == 250, "A later restart did not dial fast again");
      ++passed;
    }
    { // Which ends are restarts: 1012, or the broker's server_stopping at most 5 s before.
      require(brokerRestartClose(1012, 0, 100) && !brokerRestartClose(1000, 0, 100) && !brokerRestartClose(1001, 0, 100) && !brokerRestartClose(0, 0, 100), "Close status misread");
      require(brokerRestartClose(0, 1000, 6000) && !brokerRestartClose(0, 1000, 6001) && !brokerRestartClose(0, 7000, 6000), "server_stopping window misread");
      ++passed;
    }
    { // Simulated (no network): the broker is down for `down` ms after it closed the
      // channel; a failed dial costs 20 ms. The dial that reaches it, released 2 s
      // steps against the restart steps (jitter at its middle).
      const auto reconnectAt = [](uint64_t down, bool fast) {
        BrokerReconnectWait wait; uint64_t now = 0; bool restart = fast;
        for (;;) { now += wait.next(restart, now, 500); restart = false; if (now >= down) return now; now += 20; }
      };
      std::string rows;
      for (const uint64_t down : {300u, 800u, 1500u, 2500u, 3500u}) {
        const uint64_t old = reconnectAt(down, false), now = reconnectAt(down, true);
        require(now <= old, "The restart steps reconnected later than the released wait");
        rows += std::string(rows.empty() ? "" : ",") + "{\"downMs\":" + std::to_string(down) + ",\"beforeMs\":" + std::to_string(old) + ",\"afterMs\":" + std::to_string(now) + "}";
      }
      // The wait after the broker is back, averaged over outages of 10 ms to 4 s.
      uint64_t lateBefore = 0, lateAfter = 0, samples = 0;
      for (uint64_t down = 10; down <= 4000; down += 10, ++samples) { lateBefore += reconnectAt(down, false) - down; lateAfter += reconnectAt(down, true) - down; }
      require(lateAfter * 10 <= lateBefore * 6, "The restart steps did not cut the wait after the broker was back");
      std::cout << "{\"simulated\":true,\"hostReconnectAfterBrokerRestart\":[" << rows << "],\"meanLateMs\":{\"before\":" << lateBefore / samples << ",\"after\":" << lateAfter / samples << "}}\n";
      ++passed;
    }
    std::cout << "Host resume checks passed: " << passed << '\n';
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
