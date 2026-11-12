// BitrateController on a simulated relay path (no network, no encoder, no
// sleeping): CBR-like encoder with a measured-style rate lag, 1.5 Mbps PCM
// audio, a host send queue (128 packets / 8 MiB, as Agent::enqueue for an
// adaptive session; fixed sessions keep 32), a FIFO
// bottleneck with propagation delay (the relay->client leg, 8 MiB relay
// limit), client arrival feedback every 100 ms on its own drifting clock, and
// 5 ms target delivery to the encoder. It asserts bounded queueing delay,
// convergence near capacity, quick backoff, no oscillation storm, bursts that
// are not congestion, and a stable rate on an idle desktop. A simulation, not a
// real relay/VM measurement.
#include "bitrate_controller.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using host::BitrateArrival;
using host::BitrateController;
void require(bool value, const std::string& reason) { if (!value) throw std::runtime_error(reason); }
constexpr uint64_t ms = 1000, second = 1000000;
bool traceRequested = false;

// light: typing / cursor-sized updates at 30 fps whose size does not depend on
// the target (application-limited, ~2.5 Mbps).
enum class Content { motion, idle, pages, light };
struct Scenario {
  std::string name;
  uint64_t durationUs = 40 * second;
  std::function<double(uint64_t)> capacity = [](uint64_t) { return 30e6; };  // bottleneck bps
  std::function<double(uint64_t)> hostLeg = [](uint64_t) { return 1e9; };  // VM -> relay bps
  uint64_t propagationUs = 20 * ms;
  std::function<Content(uint64_t)> content = [](uint64_t) { return Content::motion; };
  uint64_t pageEveryUs = 4 * second;
  int width = 1920, height = 1080, fps = 60, start = 20000000, maximum = 100000000;
  uint64_t stallEveryUs = 0, stallUs = 0;   // periodic link stalls (Wi-Fi scan)
  double spikeProbability = 0; uint64_t spikeUs = 0;  // single delayed packets (head-of-line)
  uint64_t feedbackStopsAtUs = UINT64_MAX;
  int encoderFloor = 0;  // the encoder never emits less (content at its quantiser limit)
  unsigned seed = 7;
};
struct Packet { uint64_t producedUs = 0, pts = 0, hostDoneUs = 0, exitUs = 0, arrivalUs = 0; size_t bytes = 0; bool video = false; };
struct Result {
  std::vector<std::pair<uint64_t, int>> targets;     // sampled every 100 ms
  std::vector<std::pair<uint64_t, double>> delays;   // video frame one-way delay above propagation (ms)
  std::vector<uint64_t> decreases, increases;
  std::vector<std::pair<uint64_t, size_t>> arrivedVideo;
  size_t hostOverflow = 0, relayOverflow = 0;
  int finalTarget = 0;
  double targetMean(uint64_t from, uint64_t to) const { double sum = 0; size_t n = 0; for (const auto& [at, value] : targets) if (at >= from && at < to) { sum += value; ++n; } return n ? sum / n : 0; }
  int targetMin(uint64_t from, uint64_t to) const { int value = INT32_MAX; for (const auto& [at, t] : targets) if (at >= from && at < to) value = std::min(value, t); return value; }
  int targetMax(uint64_t from, uint64_t to) const { int value = 0; for (const auto& [at, t] : targets) if (at >= from && at < to) value = std::max(value, t); return value; }
  int targetAt(uint64_t at) const { int value = 0; for (const auto& [when, t] : targets) if (when <= at) value = t; return value; }
  double delayPercentile(uint64_t from, uint64_t to, double fraction) const {
    std::vector<double> values; for (const auto& [at, d] : delays) if (at >= from && at < to) values.push_back(d);
    if (values.empty()) return 0; std::sort(values.begin(), values.end()); return values[std::min(values.size() - 1, size_t(fraction * double(values.size())))];
  }
  size_t count(const std::vector<uint64_t>& events, uint64_t from, uint64_t to) const { return size_t(std::count_if(events.begin(), events.end(), [&](uint64_t at) { return at >= from && at < to; })); }
  double videoMbps(uint64_t from, uint64_t to) const { double bytes = 0; for (const auto& [at, b] : arrivedVideo) if (at >= from && at < to) bytes += double(b); return bytes * 8 / double(to - from); }
};

Result simulate(const Scenario& s) {
  Result result;
  std::mt19937 random(s.seed); std::uniform_real_distribution<double> unit(0, 1);
  BitrateController controller(s.start, s.maximum, s.fps);
  if (const auto value = controller.geometry(s.width, s.height, s.fps)) (void)value;
  int applied = controller.target(); double effective = double(applied) / 8 / s.fps;
  std::deque<std::pair<uint64_t, int>> deliveries;  // target -> encoder after 5 ms
  std::deque<Packet> hostQueue, bottleneck; std::deque<std::pair<uint64_t, size_t>> writes;
  size_t hostQueuedBytes = 0, bottleneckBytes = 0;
  uint64_t hostBusyUs = 0, bottleneckBusyUs = 0, lastArrivalUs = 0;
  std::deque<Packet> inFlight;  // left the bottleneck, not yet arrived
  struct Feedback { uint64_t dueUs; std::vector<BitrateArrival> frames; uint64_t bytes, intervalUs, relayQueued; };
  std::deque<Feedback> uplink; std::vector<BitrateArrival> frames; uint64_t feedbackBytes = 0, lastReportUs = 0;
  const auto clientClock = [](uint64_t at) { return uint64_t(double(at) * (1 + 40e-6)) + 987654321ull; };
  int published = applied; uint64_t nextTick = 0, nextFrame = 0, nextAudio = 0, lastPageUs = 0; int frameIndex = 0, refinementLeft = 0;
  bool pageArmed = false;
  const auto decision = [&](uint64_t now, std::optional<int> value) {
    if (!value) return;
    if (*value < published) result.decreases.push_back(now); else if (*value > published) result.increases.push_back(now);
    published = *value; deliveries.push_back({now + 5 * ms, *value});
  };
  const auto produce = [&](uint64_t now, size_t bytes, bool video) {
    Packet packet; packet.producedUs = now; packet.pts = now; packet.bytes = bytes; packet.video = video;
    if (hostQueue.size() >= host::adaptiveHostQueuePackets || hostQueuedBytes + bytes > 8 * 1024 * 1024) { ++result.hostOverflow; return; }
    controller.sent(now, video, 0, packet.pts, bytes);
    hostQueue.push_back(packet); hostQueuedBytes += bytes;
    // Agent::enqueue also reports the queue (the send loop may be blocked in a write).
    decision(now, controller.local(now, now - hostQueue.front().producedUs, hostQueuedBytes, hostQueue.size()));
  };
  for (uint64_t now = 0; now <= s.durationUs; now += 250) {
    while (!deliveries.empty() && deliveries.front().first <= now) { applied = deliveries.front().second; deliveries.pop_front(); }
    // Encoder: CBR budget with the lag measured on the MFT (a few frames).
    if (now >= nextFrame) {
      nextFrame = uint64_t(++frameIndex) * second / s.fps;
      const double budget = std::max(double(applied), double(s.encoderFloor)) / 8 / s.fps;
      effective += 0.35 * (budget - effective);
      const auto content = s.content(now);
      size_t bytes = 0;
      if (frameIndex == 1) bytes = size_t(16 * budget);  // IDR (measured 1080p text page: 16x at 20 Mbps)
      else if (content == Content::motion) bytes = size_t(effective * (0.85 + 0.3 * unit(random)));
      else if (content == Content::pages && now - lastPageUs >= s.pageEveryUs && (pageArmed || now >= s.pageEveryUs)) { lastPageUs = now; bytes = size_t(32 * budget); refinementLeft = 90; pageArmed = true; }
      else if (refinementLeft > 0) { --refinementLeft; bytes = size_t(effective * (0.9 + 0.2 * unit(random))); }
      else if (content == Content::light) { if (frameIndex % 2 == 0) bytes = size_t(std::min(effective, 10500 * (0.5 + unit(random)))); }
      else if (frameIndex % s.fps == 0) bytes = 1500;  // 1 s refresh of an unchanged desktop
      if (bytes) produce(now, bytes, true);
    }
    if (now >= nextAudio) { nextAudio += 10 * ms; produce(now, 1920, false); }
    // Host send loop: one write at a time at the host-leg rate.
    while (!writes.empty() && writes.front().first <= now) { controller.written(now, writes.front().second); writes.pop_front(); }
    while (!hostQueue.empty() && hostBusyUs <= now) {
      auto packet = hostQueue.front();
      decision(now, controller.local(now, now - packet.producedUs, hostQueuedBytes, hostQueue.size()));
      hostQueue.pop_front(); hostQueuedBytes -= packet.bytes;
      packet.hostDoneUs = std::max(now, hostBusyUs) + uint64_t(double(packet.bytes) * 8 * 1e6 / s.hostLeg(now));
      hostBusyUs = packet.hostDoneUs; writes.push_back({packet.hostDoneUs, packet.bytes});
      // Bottleneck FIFO (relay -> client).
      const double capacity = s.capacity(packet.hostDoneUs);
      uint64_t exit = std::max(packet.hostDoneUs, bottleneckBusyUs) + uint64_t(double(packet.bytes) * 8 * 1e6 / capacity);
      if (s.stallEveryUs && exit % s.stallEveryUs < s.stallUs) exit += s.stallUs - exit % s.stallEveryUs;
      bottleneckBusyUs = exit; packet.exitUs = exit;
      uint64_t arrival = exit + s.propagationUs + (unit(random) < s.spikeProbability ? s.spikeUs : 0);
      arrival = std::max(arrival, lastArrivalUs); lastArrivalUs = arrival; packet.arrivalUs = arrival;
      bottleneck.push_back(packet); bottleneckBytes += packet.bytes;
    }
    while (!bottleneck.empty() && bottleneck.front().hostDoneUs <= now && bottleneck.front().exitUs <= now) { bottleneckBytes -= bottleneck.front().bytes; inFlight.push_back(bottleneck.front()); bottleneck.pop_front(); }
    // The relay's untransmitted backlog (its socket drains bytes progressively,
    // also inside one large message).
    uint64_t relayBacklog = 0;
    for (const auto& packet : bottleneck) {
      if (packet.hostDoneUs > now) break;
      const double remaining = double(packet.exitUs - now) * s.capacity(now) / 8e6;
      relayBacklog += uint64_t(std::min(double(packet.bytes), std::max(0.0, remaining)));
    }
    if (relayBacklog > 8 * 1024 * 1024) ++result.relayOverflow;
    // Client receive.
    while (!inFlight.empty() && inFlight.front().arrivalUs <= now) {
      const auto& packet = inFlight.front(); feedbackBytes += packet.bytes;
      if (packet.video) {
        frames.push_back({packet.pts, clientClock(packet.arrivalUs)}); result.arrivedVideo.push_back({packet.arrivalUs, packet.bytes});
        result.delays.push_back({packet.producedUs, double(packet.arrivalUs - packet.producedUs - s.propagationUs) / 1000});
      }
      inFlight.pop_front();
    }
    if (now - lastReportUs >= 100 * ms) {
      if (now < s.feedbackStopsAtUs) {
        if (frames.size() > 64) frames.erase(frames.begin(), frames.end() - 64);
        uplink.push_back({now + s.propagationUs + 2 * ms, frames, feedbackBytes, now - lastReportUs, relayBacklog});
      }
      frames.clear(); feedbackBytes = 0; lastReportUs = now;
    }
    while (!uplink.empty() && uplink.front().dueUs <= now) {
      const auto& f = uplink.front();
      decision(now, controller.feedback(now, 0, f.frames, f.bytes, f.intervalUs, f.relayQueued)); uplink.pop_front();
    }
    if (now >= nextTick) { nextTick += 100 * ms; decision(now, controller.tick(now)); result.targets.push_back({now, controller.target()});
      if (traceRequested) std::cout << s.name << " t=" << now / 1000 << " target=" << controller.target() / 1e6 << " applied=" << applied / 1e6 << " standing=" << controller.standingDelayMs(now) << " excess=" << controller.standingExcessMs() << " delivered=" << controller.deliveredKbps() / 1000 << " host=" << hostQueue.size() << " relayKB=" << relayBacklog / 1024 << "\n"; }
  }
  result.finalTarget = controller.target();
  return result;
}

void print(const Scenario& s, const Result& r, uint64_t from, uint64_t to) {
  std::cout << std::fixed << std::setprecision(2) << "{\"kind\":\"bitrate-controller-sim\",\"scenario\":\"" << s.name << "\",\"windowS\":[" << from / second << ',' << to / second
            << "],\"targetMeanMbps\":" << r.targetMean(from, to) / 1e6 << ",\"targetMinMbps\":" << r.targetMin(from, to) / 1e6 << ",\"targetMaxMbps\":" << r.targetMax(from, to) / 1e6
            << ",\"videoDeliveredMbps\":" << r.videoMbps(from, to) << ",\"delayP50Ms\":" << r.delayPercentile(from, to, .5) << ",\"delayP95Ms\":" << r.delayPercentile(from, to, .95)
            << ",\"delayMaxMs\":" << r.delayPercentile(from, to, 1) << ",\"decreases\":" << r.count(r.decreases, from, to) << ",\"increases\":" << r.count(r.increases, from, to)
            << ",\"hostOverflow\":" << r.hostOverflow << ",\"relayOverflow\":" << r.relayOverflow << ",\"finalTargetMbps\":" << r.finalTarget / 1e6 << "}\n";
}

// UDP loss patterns, 100 ms FEEDBACK as the client sends it: datagrams received and given
// up. `capacity` is a policer or a shallow queue: the excess over it, repairs included,
// is given up. `extra` is loss no rate changes (a radio, a blackout). The round trip stays
// 20 ms, so only the loss rule acts. A simulation, not a network measurement.
struct LossRun {
  std::vector<double> mbps; std::vector<double> loss; int cuts = 0, raises = 0; uint64_t firstCutMs = 0;
  double mean(int from, int to) const { double s = 0; for (int n = from; n < to; ++n) s += mbps[size_t(n)]; return s / std::max(1, to - from); }
  double lossyShare(int from, int to) const { int n = 0; for (int k = from; k < to; ++k) n += loss[size_t(k)] >= 0.02; return double(n) / std::max(1, to - from); }
  double lowest(int from, int to) const { return *std::min_element(mbps.begin() + from, mbps.begin() + to); }
  double highest(int from, int to) const { return *std::max_element(mbps.begin() + from, mbps.begin() + to); }
};
LossRun lossRun(const std::function<double(int)>& capacity, const std::function<double(int)>& extra, int reports, int start = 40000000) {
  BitrateController c(start, 100000000); c.geometry(1920, 1080, 60); c.pathChanged();
  uint64_t now = second, pts = 0; LossRun r;
  for (int n = 0; n < reports; ++n) {
    for (int f = 0; f < 6; ++f) { now += 16667; c.sent(now, true, 1, pts += 16667, size_t(double(c.target()) / 8 / 60)); }
    const double rate = c.target(), datagrams = std::round(rate * 0.1 / 8 / 1167);
    const double loss = std::min(1.0, std::max(0.0, 1 - capacity(n) / rate) + extra(n));
    const auto lost = uint64_t(std::lround(datagrams * loss));
    const int before = c.target();
    if (const auto value = c.udpFeedback(now, 20000, uint64_t(datagrams) - lost, lost)) { if (*value < before) { ++r.cuts; if (!r.firstCutMs) r.firstCutMs = uint64_t(n + 1) * 100; } else ++r.raises; }
    r.mbps.push_back(c.target() / 1e6); r.loss.push_back(loss);
    if (traceRequested) std::cout << "{\"trace\":" << n << ",\"mbps\":" << c.target() / 1e6 << ",\"loss\":" << loss << ",\"limited\":" << c.encoderLimited() << "}\n";
  }
  return r;
}
void udpLossPatterns() {
  const auto none = [](int) { return 0.0; };
  const auto print = [](const char* name, const LossRun& r, int from, int to) {
    std::cout << std::fixed << std::setprecision(2) << "{\"kind\":\"bitrate-udp-loss\",\"case\":\"" << name << "\",\"windowS\":[" << from / 10 << ',' << to / 10 << "],\"meanMbps\":" << r.mean(from, to)
              << ",\"minMbps\":" << r.lowest(from, to) << ",\"maxMbps\":" << r.highest(from, to) << ",\"reportsWith2PctLoss\":" << r.lossyShare(from, to) << ",\"cuts\":" << r.cuts << ",\"firstCutMs\":" << r.firstCutMs << ",\"simulated\":true}\n";
  };
  {  // A 30 Mbps policer under a 40 Mbps start: one cut that fits, then no sawtooth.
    const auto r = lossRun([](int) { return 30e6; }, none, 300); print("policer-30-start-40", r, 20, 300);
    require(r.firstCutMs <= 100 && r.mbps[1] <= 31, "UDP loss: an overload was not cut within 200 ms to the capacity");
    // A policer gives no delay signal, so probing finds it by loss: a shallow sawtooth, not a storm.
    require(r.lossyShare(20, 300) <= 0.1 && r.mean(20, 300) >= 0.9 * 30 && r.cuts <= 20 && r.highest(20, 300) <= 1.1 * r.lowest(20, 300), "UDP loss: it oscillated into the policer");
  }
  {  // Capacity falls from 60 to 25 Mbps under a running stream.
    const auto r = lossRun([](int n) { return n < 100 ? 60e6 : 25e6; }, none, 400); print("capacity-60-to-25", r, 110, 400);
    require(r.mbps[108] <= 25.5 && r.lossyShare(110, 400) <= 0.1 && r.mean(110, 400) >= 0.85 * 25, "UDP loss: a capacity drop was not followed within a second, or not stably");
  }
  {  // 3 % given up whatever the rate: one cut, then the rate holds.
    const auto r = lossRun([](int) { return 1e9; }, [](int) { return 0.03; }, 300); print("rate-independent-3pct", r, 0, 300);
    require(r.cuts == 1 && r.lowest(10, 300) >= 0.85 * 40, "UDP loss: loss no rate changes ratcheted the rate down");
  }
  {  // One 100 ms blackout: one cut of at most 15 %, and the rate comes back.
    const auto r = lossRun([](int) { return 1e9; }, [](int n) { return n == 100 ? 1.0 : 0.0; }, 300); print("blackout-100ms", r, 95, 300);
    require(r.lowest(95, 300) >= 0.85 * r.mbps[99] - 0.1 && r.mbps[200] >= r.mbps[99], "UDP loss: a blackout cut deeply or for long");
  }
  {  // The path falls to 20 Mbps for a second under a 100 Mbps stream: cut hard, early.
    const auto r = lossRun([](int n) { return n >= 50 && n < 60 ? 20e6 : 1e9; }, none, 120); print("drop-to-20-for-1s", r, 45, 120);
    require(r.mbps[51] <= 31 && r.mbps[57] <= 21, "UDP loss: a five-fold overload was not cut to it within 600 ms");
  }
}
}

int main(int argc, char** argv) {
  try {
    traceRequested = argc == 2 && std::string(argv[1]) == "--trace";
    constexpr double audio = 1.536e6;
    {
      // Fixed 30 Mbps path: converge near (capacity - audio) with bounded delay.
      Scenario s; s.name = "fixed-30mbps"; s.durationUs = 60 * second;
      const auto r = simulate(s); print(s, r, 15 * second, 60 * second);
      const double video = 30e6 - audio;
      require(r.hostOverflow == 0 && r.relayOverflow == 0, "fixed: queue overflow");
      require(r.targetMean(15 * second, 60 * second) >= 0.65 * video && r.targetMean(15 * second, 60 * second) <= 1.0 * video, "fixed: did not converge near capacity");
      require(r.delayPercentile(15 * second, 60 * second, .5) <= 40 && r.delayPercentile(15 * second, 60 * second, .95) <= 120, "fixed: queueing delay not bounded");
      require(r.count(r.decreases, 15 * second, 60 * second) <= 30, "fixed: oscillation storm");
    }
    {
      // Fast path: ramp from 20 Mbps to the 100 Mbps ceiling, never above it.
      Scenario s; s.name = "fast-200mbps"; s.capacity = [](uint64_t) { return 200e6; }; s.durationUs = 40 * second;
      const auto r = simulate(s); print(s, r, 0, 40 * second);
      require(r.targetMax(0, 40 * second) <= 100000000, "fast: exceeded the negotiated ceiling");
      require(r.targetAt(25 * second) >= 95000000, "fast: did not reach the ceiling within 25 s of motion");
      require(r.delayPercentile(0, 40 * second, .95) <= 60, "fast: queueing on an uncongested path");
      require(r.count(r.decreases, 0, 40 * second) == 0, "fast: decreased on an uncongested path");
    }
    {
      // Sudden drop 60 -> 12 Mbps at 20 s, recovery to 60 at 40 s.
      Scenario s; s.name = "drop-60-12-60"; s.durationUs = 60 * second;
      s.capacity = [](uint64_t at) { return at < 20 * second || at >= 40 * second ? 60e6 : 12e6; };
      const auto r = simulate(s); print(s, r, 20 * second, 40 * second); print(s, r, 40 * second, 60 * second);
      require(r.hostOverflow == 0 && r.relayOverflow == 0, "drop: queue overflow");
      require(r.targetAt(21500 * ms) <= int(12e6 - audio), "drop: no quick backoff below the new capacity");
      require(r.delayPercentile(20 * second, 40 * second, 1) <= 1500, "drop: delay after the drop not bounded");
      require(r.delayPercentile(23 * second, 40 * second, .95) <= 150, "drop: delay did not return near the base");
      require(r.targetMean(23 * second, 40 * second) >= 0.6 * (12e6 - audio), "drop: settled far below the new capacity");
      require(r.targetAt(55 * second) >= 40000000, "drop: did not recover after the capacity returned");
    }
    {
      // RTT spikes: 150 ms stalls every 4 s plus 0.2 % single 120 ms delays.
      Scenario s; s.name = "rtt-spikes-50mbps"; s.durationUs = 60 * second; s.capacity = [](uint64_t) { return 50e6; };
      s.stallEveryUs = 4 * second; s.stallUs = 150 * ms; s.spikeProbability = 0.002; s.spikeUs = 120 * ms;
      const auto r = simulate(s); print(s, r, 15 * second, 60 * second);
      require(r.hostOverflow == 0 && r.relayOverflow == 0, "spikes: queue overflow");
      require(r.targetMean(15 * second, 60 * second) >= 0.5 * (50e6 - audio), "spikes: transient delay collapsed the rate");
      require(r.count(r.decreases, 15 * second, 60 * second) <= 23, "spikes: oscillation storm");
    }
    {
      // Idle desktop with a page switch every 4 s (a ~32-frame burst, then a
      // 1.5 s refinement train at the budget) on a link that carries it: the
      // bursts are neither congestion nor probing evidence, so the rate holds.
      Scenario s; s.name = "refinement-bursts-40mbps"; s.durationUs = 40 * second; s.capacity = [](uint64_t) { return 40e6; };
      s.content = [](uint64_t) { return Content::pages; };
      const auto r = simulate(s); print(s, r, 0, 40 * second);
      require(r.decreases.empty() && r.increases.empty(), "bursts: refinement bursts changed the rate");
      require(r.hostOverflow == 0 && r.relayOverflow == 0, "bursts: queue overflow");
    }
    {
      // The same bursts on a link slower than the start rate: the refinement
      // train is real congestion there and the rate comes down.
      Scenario s; s.name = "refinement-bursts-12mbps"; s.durationUs = 40 * second; s.capacity = [](uint64_t) { return 12e6; };
      s.content = [](uint64_t) { return Content::pages; };
      const auto r = simulate(s); print(s, r, 10 * second, 40 * second);
      require(r.hostOverflow == 0 && r.relayOverflow == 0, "slow bursts: queue overflow");
      require(r.targetMax(10 * second, 40 * second) <= int(12e6), "slow bursts: rate stayed above the link");
    }
    {
      // Long idle after motion: neither growth nor decay without evidence.
      Scenario s; s.name = "long-idle"; s.durationUs = 70 * second; s.capacity = [](uint64_t) { return 50e6; };
      s.content = [](uint64_t at) { return at < 10 * second ? Content::motion : Content::idle; };
      const auto r = simulate(s); print(s, r, 12 * second, 70 * second);
      require(r.targetMin(12 * second, 70 * second) == r.targetMax(12 * second, 70 * second), "idle: rate drifted without evidence");
    }
    {
      // A fast LAN, motion for 8 s, then light typing/cursor updates through
      // 150 ms stalls every 4 s (field case: a delay spike while light
      // cut 52 Mbps to the 2.7 Mbps geometry floor, because what a mostly
      // idle desktop delivers is no capacity measure). The rate must survive.
      Scenario s; s.name = "light-stalls-lan"; s.durationUs = 40 * second; s.capacity = [](uint64_t) { return 200e6; };
      s.content = [](uint64_t at) { return at < 8 * second ? Content::motion : Content::light; };
      s.stallEveryUs = 4 * second; s.stallUs = 150 * ms;
      const auto r = simulate(s); print(s, r, 8 * second, 40 * second);
      require(r.targetMin(9 * second, 40 * second) >= int(0.6 * r.targetAt(8 * second)), "light stalls: the rate collapsed after delay spikes while application-limited");
    }
    {
      // The same, but the host's own socket write stalls for 300 ms every 4 s
      // (a busy VM): the host queue ages while almost nothing is written.
      Scenario s; s.name = "light-host-stalls"; s.durationUs = 40 * second; s.capacity = [](uint64_t) { return 200e6; };
      s.hostLeg = [](uint64_t at) { return at >= 8 * second && at % (4 * second) < 300 * ms ? 2e5 : 1e9; };
      s.content = [](uint64_t at) { return at < 8 * second ? Content::motion : Content::light; };
      const auto r = simulate(s); print(s, r, 8 * second, 40 * second);
      require(r.hostOverflow == 0, "light host stalls: host queue overflow");
      require(r.targetMin(9 * second, 40 * second) >= int(0.6 * r.targetAt(8 * second)), "light host stalls: a stalled write collapsed the rate while application-limited");
    }
    {
      // The same light updates on a link slower than they need: a real
      // bottleneck below the application-limited rate still brings it down.
      Scenario s; s.name = "light-link-3200k"; s.durationUs = 40 * second; s.capacity = [](uint64_t) { return 3.2e6; };
      s.content = [](uint64_t) { return Content::light; }; s.width = 1280; s.height = 720; s.fps = 30;
      const auto r = simulate(s); print(s, r, 20 * second, 40 * second);
      require(r.hostOverflow == 0 && r.relayOverflow == 0, "light slow link: queue overflow");
      require(r.delayPercentile(20 * second, 40 * second, .95) <= 400, "light slow link: delay not bounded");
    }
    {
      // Feedback stops (client stalled or uplink lost) while video is sent.
      Scenario s; s.name = "feedback-stops"; s.durationUs = 30 * second; s.feedbackStopsAtUs = 15 * second;
      const auto r = simulate(s); print(s, r, 15 * second, 30 * second);
      require(r.targetAt(25 * second) <= int(0.7 * r.targetAt(15 * second)), "stale: no reduction without feedback");
    }
    {
      // 720p30 (geometry floor 2 Mbps): the encoder cannot go below 6 Mbps
      // (content at its quantiser limit) on a 4 Mbps link. No rate fixes that
      // path (the queue grows regardless); cutting further is futile, so the
      // target must follow the real output instead of spiralling to the floor.
      Scenario s; s.name = "encoder-floor-6mbps-link-4"; s.durationUs = 40 * second; s.width = 1280; s.height = 720; s.fps = 30;
      s.capacity = [](uint64_t) { return 4e6; }; s.encoderFloor = 6000000;
      const auto r = simulate(s); print(s, r, 10 * second, 40 * second);
      require(r.targetMin(10 * second, 40 * second) >= 3500000, "encoder floor: target spiralled while the encoder could not follow");
    }
    {
      // The host uplink becomes the bottleneck (VM -> relay 1 Gbps, then 40 Mbps
      // at 10 s, while at the 100 Mbps ceiling): the delay builds in the host
      // send queue itself.
      Scenario s; s.name = "host-leg-drop-40mbps"; s.durationUs = 40 * second; s.capacity = [](uint64_t) { return 1e9; };
      s.hostLeg = [](uint64_t at) { return at < 10 * second ? 1e9 : 40e6; };
      const auto r = simulate(s); print(s, r, 10 * second, 40 * second);
      require(r.hostOverflow == 0, "host leg: host queue overflow");
      require(r.targetMean(13 * second, 40 * second) <= 40e6 && r.targetMean(13 * second, 40 * second) >= 0.6 * (40e6 - audio), "host leg: not near the host uplink");
      require(r.delayPercentile(13 * second, 40 * second, .95) <= 150, "host leg: delay not bounded");
    }
    {
      // 4K60: the geometry floor (MFT degenerate state) is never crossed.
      Scenario s; s.name = "4k-floor-link-10mbps"; s.width = 3840; s.height = 2160; s.durationUs = 30 * second; s.capacity = [](uint64_t) { return 10e6; };
      const auto r = simulate(s); print(s, r, 0, 30 * second);
      require(r.targetMin(0, 30 * second) >= host::adaptiveBitrateFloor(3840, 2160, 60), "4K: target below the encoder floor");
    }
    require(host::adaptiveBitrateFloor(3840, 2160, 60) >= 19000000 && host::adaptiveBitrateFloor(1920, 1080, 60) <= 5000000 && host::adaptiveBitrateFloor(1280, 720, 30) == host::adaptiveMinimumBitrate, "Floor table");
    {
      // UDP media: FEEDBACK every 100 ms on a sending stream, no queue or relay report.
      struct Udp {
        host::BitrateController controller{40000000, 100000000}; uint64_t now = 1000000, pts = 0; int cuts = 0, raises = 0;
        Udp() { controller.geometry(1920, 1080, 60); controller.pathChanged(); }
        void run(int intervals, uint64_t rttUs, uint64_t received, uint64_t lost) {
          for (int n = 0; n < intervals; ++n) {
            for (int frame = 0; frame < 6; ++frame) { now += 16667; controller.sent(now, true, 1, pts += 16667, size_t(double(controller.target()) / 8 / 60)); }
            const int before = controller.target();
            if (const auto value = controller.udpFeedback(now, rttUs, received, lost)) { if (*value < before) ++cuts; else ++raises; }
          }
        }
      };
      { Udp u; u.run(100, 20000, 400, 0); require(u.cuts == 0 && u.controller.target() > 40000000, "UDP: a flat round trip was not room to probe"); }
      { Udp u; u.run(30, 20000, 400, 0); u.cuts = 0; const int before = u.controller.target(); for (uint64_t rtt = 30000; rtt < 300000; rtt += 30000) u.run(1, rtt, 400, 0); require(u.cuts >= 1 && u.controller.target() < before, "UDP: a growing round trip was not congestion"); }
      { Udp u; u.run(30, 20000, 400, 0); u.cuts = 0; const int before = u.controller.target(); u.run(5, 200000, 400, 0); u.run(20, 20000, 400, 0); require(u.controller.target() >= before * 0.8, "UDP: one step of round trip cut like congestion"); }
      { Udp u; u.run(30, 0, 400, 0); const int before = u.controller.target(); u.run(20, 0, 300, 50); require(u.cuts >= 1 && u.controller.target() <= before * 0.86, "UDP: 14 % given up did not lower the bitrate"); }
      // 5 % given up that no cut changes: one proportional cut, (1 - 5 %) x 0.95, then the rate holds (no ratchet, no probing).
      { Udp u; u.run(30, 20000, 400, 0); const int before = u.controller.target(); u.cuts = 0; u.run(40, 20000, 400, 20);
        require(u.cuts == 1 && u.controller.target() >= before * 0.89 && u.controller.target() <= before * 0.92, "UDP: 5 % given up was not one proportional cut"); }
      // Earlier: the first report with loss in a 300 ms window acts, not a 500 ms window's end.
      // The first report cuts at most 15 % (it may be a blackout); the second cuts what both
      // say from the rate that lost it: (1 - 20 %) x 0.95.
      { Udp u; u.run(30, 0, 400, 0); u.cuts = 0; const int before = u.controller.target();
        u.run(1, 0, 320, 80); require(u.cuts == 1 && u.controller.target() >= before * 0.84 && u.controller.target() <= before * 0.86, "UDP: 20 % given up waited for a 500 ms window, or one report cut deep");
        u.run(1, 0, 320, 80); require(u.controller.target() >= before * 0.75 && u.controller.target() <= before * 0.77, "UDP: two reports of 20 % were not cut in proportion"); }
      // 1 % given up in every report: one small cut, then the rate holds without probing into the loss.
      { Udp u; u.run(30, 20000, 400, 0); const int before = u.controller.target(); u.cuts = 0; u.run(50, 20000, 396, 4);
        require(u.cuts == 1 && u.controller.target() >= before * 0.93 && u.controller.target() <= before * 0.95, "UDP: 1 % given up was not one small cut, or probed into the loss"); }
      { Udp u; u.run(30, 0, 400, 0); const auto first = u.controller.udpBacklog(u.now); require(first && *first <= 20000000, "UDP: a pacer backlog did not halve"); require(!u.controller.udpBacklog(u.now + 1000), "UDP: a second backlog cut within 150 ms"); }
      // Round trips and arrival clocks are different numbers: neither floor survives a change of path.
      { Udp u; u.run(30, 200000, 400, 0); u.controller.pathChanged(); const int before = u.controller.target(); u.run(10, 2000, 400, 0); u.cuts = 0; u.run(10, 8000, 400, 0); require(u.cuts == 0 && u.controller.target() >= before, "UDP: a new path inherited the old floor"); }
      std::cout << "{\"kind\":\"bitrate-controller-udp\",\"passed\":9}\n";
    }
    udpLossPatterns();
    std::cout << "{\"kind\":\"bitrate-controller-sim-summary\",\"passed\":true,\"simulatedLink\":true,\"realRelay\":false,\"realEncoder\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
