#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <vector>

// udp-media-v1 probe schedule (docs/remote-session-protocol.md, "UDP media"): BIND through
// the relays, the PING burst, keepalives and the report. Pure and clock-injected.
// The socket that sends BIND is the one the relay pins, so this runs on the leg that
// goes on to carry media. Any failure ends the probe only.
namespace multitap::udp {
constexpr size_t proofBytes = 41, probeBytes = 25, legIdSize = 8;
constexpr int probeReachable = 1, probeSocketError = 2, probeLookupFailed = 3, probeBindTimeout = 4, probeNoPong = 5;
constexpr uint64_t bindIntervalUs = 200000, bindBudgetUs = 5000000;
constexpr unsigned bindsPerRelay = 3;
constexpr uint32_t burstPings = 25, hostWaitPings = 15;  // 3 s at the burst's rate
constexpr uint64_t burstIntervalUs = 200000, pongWindowUs = 1000000, keepaliveIntervalUs = 20000000;

class Probe {
 public:
  using Proof = std::array<uint8_t, proofBytes>;
  using Send = std::function<bool(size_t relay, const uint8_t* data, size_t size)>;
  // Fixed numbers only; -1 is an absent field, code 0 an open outcome.
  struct Report { int code = 0, relay = -1, rttP50Ms = -1, rttP95Ms = -1, lossPct = -1; uint32_t keepSent = 0, keepLost = 0; };

  Probe(size_t relays, const std::array<uint8_t, legIdSize>& id, const Proof& bind, const Proof& ack, uint64_t nowUs, Send send)
      : id_(id), bind_(bind), ack_(ack), send_(std::move(send)), ready_(relays, false), timerAt_(nowUs), deadlineAt_(nowUs + bindBudgetUs) {}

  const Report& report() const { return report_; }
  bool over() const { return stage_ == Stage::over; }
  bool bound() const { return report_.relay >= 0; }
  size_t chosen() const { return size_t(report_.relay); }

  // A relay's name resolved: it takes its turn, at once if this is its turn.
  void resolved(size_t relay) {
    ready_[relay] = true;
    if (stage_ == Stage::binding && ticks_ && relay == current_) sendBind(relay);
  }

  void tick(uint64_t nowUs) {
    if (stage_ == Stage::binding && nowUs >= deadlineAt_) return bindExpired();
    while (stage_ != Stage::over && nowUs >= timerAt_) {
      const uint64_t at = timerAt_;
      fire(at);
      if (timerAt_ <= at) break;
    }
  }

  void received(size_t relay, const uint8_t* data, size_t size, uint64_t nowUs) {
    if (stage_ == Stage::over) return;
    if (!bound()) { if (size == ack_.size() && matches(data)) start(relay, nowUs); return; }
    if (relay == chosen()) pong(data, size, nowUs);
  }

  // The socket is gone: what was measured stays.
  void socketLost() {
    if (stage_ == Stage::over) return;
    if (!report_.code) report_.code = probeSocketError; else report_.keepLost += uint32_t(pending_.size());
    stage_ = Stage::over;
  }

 private:
  enum class Stage { binding, pinging, draining, idle, answering, over };

  void fire(uint64_t at) {
    switch (stage_) {
      case Stage::binding: {
        const unsigned tick = ticks_++;
        current_ = (tick / bindsPerRelay) % ready_.size();
        if (ready_[current_]) sendBind(current_);
        timerAt_ = at + bindIntervalUs;
        break;
      }
      case Stage::pinging:
        if (waiting_ && next_ == hostWaitPings) { noHost(at); break; }
        ping(at);
        timerAt_ = at + (next_ < burstPings ? burstIntervalUs : pongWindowUs);
        if (next_ >= burstPings) stage_ = Stage::draining;
        break;
      case Stage::draining: burstDone(at); break;
      case Stage::idle:
        ++report_.keepSent; ping(at);
        stage_ = Stage::answering; timerAt_ = at + pongWindowUs;
        break;
      case Stage::answering:
        report_.keepLost += uint32_t(pending_.size()); pending_.clear();
        stage_ = Stage::idle; timerAt_ = at + keepaliveIntervalUs - pongWindowUs;
        break;
      default: break;
    }
  }
  bool matches(const uint8_t* data) const {
    uint8_t difference = 0;
    for (size_t n = 0; n < ack_.size(); ++n) difference |= uint8_t(data[n] ^ ack_[n]);
    return !difference;
  }
  void sendBind(size_t relay) { if (send_(relay, bind_.data(), bind_.size())) bindSent_ = true; else socketFailed_ = true; }
  void bindExpired() {
    report_.code = bindSent_ ? probeBindTimeout : socketFailed_ ? probeSocketError : probeLookupFailed;
    stage_ = Stage::over;
  }
  void start(size_t relay, uint64_t nowUs) {
    report_.relay = int(relay);
    waiting_ = true; stage_ = Stage::pinging;
    ping(nowUs); timerAt_ = nowUs + burstIntervalUs;
  }
  void ping(uint64_t nowUs) {
    const uint32_t sequence = next_++;
    std::array<uint8_t, probeBytes> datagram{};
    datagram[0] = 3; std::memcpy(&datagram[1], id_.data(), legIdSize); datagram[9] = 1;
    for (unsigned n = 0; n < 4; ++n) datagram[13 + n] = uint8_t(sequence >> (24 - 8 * n));
    for (unsigned n = 0; n < 8; ++n) datagram[17 + n] = uint8_t(nowUs >> (56 - 8 * n));
    pending_[sequence] = nowUs;
    send_(chosen(), datagram.data(), datagram.size());
  }
  // The DATA header carries the Host's leg ID, which this leg never learns.
  void pong(const uint8_t* data, size_t size, uint64_t nowUs) {
    if (size != probeBytes || data[0] != 3 || data[9] != 2 || data[10] || data[11] || data[12]) return;
    uint32_t sequence = 0; uint64_t stamp = 0;
    for (unsigned n = 0; n < 4; ++n) sequence = sequence << 8 | data[13 + n];
    for (unsigned n = 0; n < 8; ++n) stamp = stamp << 8 | data[17 + n];
    const auto found = pending_.find(sequence);
    if (found == pending_.end() || found->second != stamp || nowUs - stamp > pongWindowUs) return;
    pending_.erase(found);
    if (waiting_) {
      waiting_ = false;
      // A later PING was the first to reach the Host, so the ones before it say
      // nothing about the path. Clearing them leaves a late PONG of the old
      // numbering nothing to match: no entry, or a new entry's stamp.
      if (sequence) { pending_.clear(); next_ = 0; stage_ = Stage::pinging; ping(nowUs); timerAt_ = nowUs + burstIntervalUs; return; }
    }
    if (sequence < burstPings) rttsMs_.push_back(double(nowUs - stamp) / 1000);
  }
  // No burst ran, so no share of 25 means anything. Keepalives go on, numbered
  // as they would be after one.
  void noHost(uint64_t at) {
    waiting_ = false; pending_.clear(); next_ = burstPings;
    report_.lossPct = 100; report_.code = probeNoPong;
    stage_ = Stage::idle; timerAt_ = at + keepaliveIntervalUs - burstIntervalUs;
  }
  void burstDone(uint64_t at) {
    report_.lossPct = int(pending_.size() * 100 / burstPings); pending_.clear();
    if (!rttsMs_.empty()) { report_.rttP50Ms = percentile(0.5); report_.rttP95Ms = percentile(0.95); }
    report_.code = rttsMs_.empty() ? probeNoPong : probeReachable;
    stage_ = Stage::idle; timerAt_ = at + keepaliveIntervalUs - pongWindowUs;
  }
  int percentile(double fraction) {
    std::sort(rttsMs_.begin(), rttsMs_.end());
    const size_t rank = size_t(std::ceil(fraction * double(rttsMs_.size())));
    return int(std::llround(rttsMs_[std::clamp<size_t>(rank, 1, rttsMs_.size()) - 1]));
  }

  std::array<uint8_t, legIdSize> id_;
  Proof bind_, ack_;
  Send send_;
  std::vector<bool> ready_;
  Stage stage_ = Stage::binding;
  uint64_t timerAt_, deadlineAt_;
  unsigned ticks_ = 0;
  size_t current_ = 0;
  bool bindSent_ = false, socketFailed_ = false, waiting_ = false;
  uint32_t next_ = 0;
  std::map<uint32_t, uint64_t> pending_;  // PINGs awaiting a PONG: sequence -> stamp
  std::vector<double> rttsMs_;
  Report report_;
};
}
