#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
// Restart recovery. The wait before the Host dials its broker
// channel again. It used to be a fixed 2 s after any end. A broker update
// restarts the broker process (field: 13 forced stream ends in a
// day), and every stream of that broker then waited for its Host's next dial
// before it could start again. The broker now closes the channel with 1012
// Service Restart (server/src/relay.mjs); a released broker only sends a Host
// with a stream session.stop "server_stopping" before it drops the channel.
// After such an end the waits are 250 ms, 500 ms, then 1 s, each 80-120 %
// (jitter: Hosts of one broker do not all arrive at once), for at most
// restartMs.size() dials; then the released 2 s. Every other end keeps the
// released fixed 2 s. A restart sequence starts at most once per rearmMs, so a
// broker that keeps accepting and closing with 1012 is not dialled faster.
namespace host {
class BrokerReconnectWait {
 public:
  static constexpr uint32_t normalMs = 2000;
  static constexpr std::array<uint32_t, 6> restartMs{250, 500, 1000, 1000, 1000, 1000};
  static constexpr uint64_t rearmMs = 15000;
  // The channel, or a dial, ended. restart: the broker said it restarts.
  // random: any value (the jitter). The wait in milliseconds.
  uint32_t next(bool restart, uint64_t nowMs, uint32_t random) {
    if (restart && (!armed_ || nowMs - armedAt_ >= rearmMs)) { armed_ = fast_ = true; armedAt_ = nowMs; step_ = 0; }
    if (!fast_ || step_ >= restartMs.size()) { fast_ = false; return normalMs; }
    const uint32_t base = restartMs[step_++];
    return base * 4 / 5 + uint32_t(uint64_t(random % 1001) * (base * 2 / 5) / 1000);
  }
  // The channel is up again: a restart sequence is over.
  void connected() { fast_ = false; }
  bool restarting() const { return fast_; }
 private:
  uint64_t armedAt_ = 0; size_t step_ = 0; bool armed_ = false, fast_ = false;
};
// The end of a channel was a broker restart: its close frame said 1012, or the
// broker's session.stop said server_stopping at most 5 s before.
inline bool brokerRestartClose(uint32_t remoteCloseCode, uint64_t stoppingAtMs, uint64_t nowMs) {
  return remoteCloseCode == 1012 || (stoppingAtMs != 0 && nowMs >= stoppingAtMs && nowMs - stoppingAtMs <= 5000);
}
}
