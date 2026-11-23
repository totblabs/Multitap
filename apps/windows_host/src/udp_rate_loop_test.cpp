// The UDP media core and the bitrate controller in one loop: the real Sender,
// Receiver and BitrateController, wired as UdpMediaPath and the capture loop
// wire them, over a simulated link with a rate, a queue that drops at its end
// and a round trip. The content and the encoder are models (sizes only).
// A simulation, not a network measurement.
//
// Field 2026-11-23, a game on a local PC at 144 fps (NVENC, round trip 8-10 ms,
// a path that carried 45-58 Mbps for 47 minutes right after): desktop content
// probed the target to 78.7 Mbps, a minute of idle kept it, the game started
// at it, and the target fell 78.7 -> 25 -> 8.3 Mbps (its floor) and stayed
// there for two minutes of freezes.
#include "bitrate_controller.hpp"
#include "../../../native/windows_video/udp_media.hpp"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <queue>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace multitap::udp;
using Bytes = std::vector<uint8_t>;
constexpr uint64_t ms = 1000, second = 1000000;
long long checks = 0;
void require(bool value, const std::string& reason) { if (!value) throw std::runtime_error(reason); ++checks; }

Bytes media(bool key, uint16_t epoch, uint64_t pts, size_t payload) {
  Bytes value(12 + payload, 0x5a); value[0] = 1; value[1] = key ? 1 : 0; value[2] = uint8_t(epoch); value[3] = uint8_t(epoch >> 8);
  for (unsigned n = 0; n < 8; ++n) value[4 + n] = uint8_t(pts >> (8 * n));
  return value;
}

enum class Content { idle, desktop, game };
struct Scenario {
  std::string name;
  int width = 1264, height = 760, fps = 144, start = 20000000, maximum = 100000000;
  uint64_t durationUs = 120 * second, rttUs = 8000, queueUs = 40000;
  std::function<double(uint64_t)> capacity = [](uint64_t) { return 60e6; };
  std::function<Content(uint64_t)> content = [](uint64_t) { return Content::game; };
  double lossPct = 0;          // random loss on the way to the client
  double gameDemand = 90e6;    // what the encoder emits for the game when the target allows it
  bool recovers = true;        // the encoder answers a LOSS with a recovery frame (else an IDR)
  uint64_t restartAtUs = 0;    // a new stream (a resize): a new epoch and a key frame
  size_t idrBytes = 40000;     // the least an IDR takes, whatever the target
  int silentRetries = 0;       // UdpMediaPath: key frames tried again on UDP before the stream leaves it
  bool arrival = true;         // the controller is told what arrived (false: counts only, as a Host before 1.1.39)
};
struct Result {
  std::vector<double> targetMbps, sentMbps, arrivedMbps;  // per second
  std::vector<int> lostPerSecond, keysPerSecond;
  int generated = 0, delivered = 0, idrs = 0, recoveries = 0, backlogKeys = 0, clientKeys = 0, stalls = 0, fallbacks = 0;
  uint64_t stallUs = 0, longestStallUs = 0, wssUs = 0, gaps = 0;
  double target(uint64_t atUs) const { return targetMbps[std::min(targetMbps.size() - 1, size_t(atUs / second))]; }
  double lowest(uint64_t from, uint64_t to) const { double v = 1e9; for (size_t n = size_t(from / second); n < size_t(to / second) && n < targetMbps.size(); ++n) v = std::min(v, targetMbps[n]); return v; }
  double mean(const std::vector<double>& values, uint64_t from, uint64_t to) const { double s = 0; int count = 0; for (size_t n = size_t(from / second); n < size_t(to / second) && n < values.size(); ++n) { s += values[n]; ++count; } return count ? s / count : 0; }
  int lost(uint64_t from, uint64_t to) const { int s = 0; for (size_t n = size_t(from / second); n < size_t(to / second) && n < lostPerSecond.size(); ++n) s += lostPerSecond[n]; return s; }
  int keys(uint64_t from, uint64_t to) const { int s = 0; for (size_t n = size_t(from / second); n < size_t(to / second) && n < keysPerSecond.size(); ++n) s += keysPerSecond[n]; return s; }
};

class Link {
 public:
  Link(uint64_t seed, std::function<double(uint64_t)> rate, uint64_t delayUs, uint64_t queueUs, double lossPct) : random_(seed), rate_(std::move(rate)), delayUs_(delayUs), queueUs_(queueUs), lossPct_(lossPct) {}
  bool send(const uint8_t* data, size_t size, uint64_t now) {
    if (uniform() * 100 < lossPct_) return false;
    if (queueUs_ && free_ > now + queueUs_) return false;  // the queue is full: dropped at its end
    free_ = std::max(free_, now) + uint64_t(double(size + 28) * 8e6 / std::max(rate_(now), 1.0));
    queue_.push({free_ + delayUs_, order_++, Bytes(data, data + size)});
    return true;
  }
  template <class F> void arrive(uint64_t now, F handle) { while (!queue_.empty() && queue_.top().at <= now) { handle(queue_.top().bytes); queue_.pop(); } }
 private:
  struct Packet { uint64_t at, order; Bytes bytes; bool operator>(const Packet& o) const { return at != o.at ? at > o.at : order > o.order; } };
  double uniform() { return double(random_() >> 11) / double(1ull << 53); }
  std::mt19937_64 random_; std::function<double(uint64_t)> rate_; uint64_t delayUs_, queueUs_; double lossPct_; uint64_t free_ = 0, order_ = 0;
  std::priority_queue<Packet, std::vector<Packet>, std::greater<Packet>> queue_;
};

bool traceRequested = false;

Result simulate(const Scenario& s, uint64_t seed) {
  Result r; std::mt19937_64 random(seed);
  const auto uniform = [&] { return double(random() >> 11) / double(1ull << 53); };
  host::BitrateController controller(s.start, s.maximum, s.fps); controller.geometry(s.width, s.height, s.fps); controller.pathChanged();
  Sender sender(controller.target(), 1000, [&] { return random() | 1; });
  Receiver receiver(1, 1000); Receiver::Output out; Plain plain;
  Link forward(seed + 1, s.capacity, s.rttUs / 2, s.queueUs, s.lossPct), reverse(seed + 2, [](uint64_t) { return 1e9; }, s.rttUs / 2, 0, 0);
  const uint64_t frameUs = second / uint64_t(s.fps);
  uint16_t epoch = 1; uint64_t pts = 0, nextFrame = 1000, nextPump = 1000, nextClient = 1000, nextTick = 1000;
  // UdpMediaPath's own state.
  uint64_t keyFrameAt = 0, lossAt = 0, switchedAt = 1000, wssUntil = 0; uint32_t received = 0, lost = 0;
  uint64_t wireAt = 0, datagramsAt = 0; double meanDatagram = 1200; int retries = 0;
  bool forceIdr = true, answerLoss = false; uint64_t answerLastGood = 0;
  std::vector<uint64_t> recentPts;  // the encoder's last references (16, reference_history.hpp)
  // The client's picture.
  uint64_t lastShown = 0, restarted = false;
  // Per-second accounting.
  uint64_t sentBytes = 0, arrivedBytes = 0; int lostNow = 0, keysNow = 0; uint64_t secondAt = second;
  uint64_t burstUntil = 0, nextBurst = 2 * second, lastIdleFrame = 0;

  for (uint64_t now = 1000; now < s.durationUs; now += 250) {
    const Content content = s.content(now);
    const bool onUdp = !wssUntil;
    if (s.restartAtUs && !restarted && now >= s.restartAtUs) {
      // A resize: a new stream of a new epoch. Its first key frame switches to UDP again.
      restarted = true; ++epoch; receiver.setEpoch(epoch); forceIdr = true; switchedAt = now; controller.geometry(s.width, s.height, s.fps);
    }
    if (now >= nextFrame) {
      nextFrame += frameUs;
      const double target = controller.target(), budget = target / 8 / s.fps;
      size_t size = 0; bool tile = false;
      if (content == Content::game) size = size_t(std::max(1200.0, std::min(budget, s.gameDemand / 8 / s.fps) * (0.7 + 0.6 * uniform())));
      else if (content == Content::desktop) {
        // A page change every two seconds: 0.7 s of full frames with as many bytes of tiles, then light updates.
        if (now >= nextBurst) { burstUntil = now + 700 * ms; nextBurst = now + 2 * second; }
        if (now < burstUntil) { size = size_t(budget * (0.8 + 0.4 * uniform())); tile = true; }
        else if (uniform() < 0.15) size = 3000;
      } else if (now - lastIdleFrame >= second) { lastIdleFrame = now; size = 2000; }  // a still screen's refresh
      if (size || forceIdr) {
        pts += frameUs;
        const bool key = forceIdr;
        bool recovery = false;
        if (!key && answerLoss) {
          // The reference of the client's last good frame is still held: a recovery frame; else an IDR.
          const bool held = s.recovers && std::find(recentPts.begin(), recentPts.end(), answerLastGood) != recentPts.end();
          if (held) recovery = true;
        }
        const bool idr = key || (answerLoss && !recovery);
        if (idr) size = std::max(size, s.idrBytes); else if (recovery) size = size_t(double(std::max<size_t>(size, 1200)) * 1.5);
        answerLoss = false; forceIdr = false;
        if (idr) { ++r.idrs; ++keysNow; recentPts.clear(); } else if (recovery) ++r.recoveries;
        recentPts.push_back(pts); if (recentPts.size() > 16) recentPts.erase(recentPts.begin());
        if (content == Content::game) ++r.generated;
        controller.sent(now, true, epoch, pts, size + 12, false);
        if (onUdp) {
          const auto admitted = sender.video(media(idr, epoch, pts, size), idr, recovery);
          if (admitted.requestKey && now - keyFrameAt >= keyframeIntervalUs / 2) { keyFrameAt = now; ++r.backlogKeys; forceIdr = true; if (const auto value = controller.udpBacklog(now)) sender.setTarget(*value); }
        }
        if (tile) {  // tiles ride beside the video, outside the encoder's budget
          const size_t tiles = size; controller.sent(now, true, epoch, pts, tiles + 12, true);
          if (onUdp) (void)sender.video(media(false, epoch, pts, tiles), false, false);
        }
      }
    }
    if (now >= nextPump) {
      nextPump += 1000;
      while (sender.next(now, plain)) { sentBytes += plain.size + headerBytes + tagBytes; if (!forward.send(plain.bytes.data(), plain.size, now) && plain.bytes[0] == kindFragment) ++lostNow; }
      // UdpMediaPath: back on UDP at a key frame once the backoff allows a try; that is a switch, and
      // the key frame gets its second before silence counts again.
      if (wssUntil && now >= wssUntil) { wssUntil = 0; switchedAt = now; forceIdr = true; controller.pathChanged(); }
      // UdpMediaPath::decide: media made whole nowhere for a second sends the stream to WebSocket for a backoff.
      else if (onUdp && now - switchedAt >= feedbackSilenceUs && sender.silent(now) && sender.silenceCause(now) == 3 && retries < s.silentRetries) {
        // Datagrams arrive but no picture was made whole: once more from a key frame at the rate as it is now.
        ++retries; switchedAt = now; sender.recovering(now); forceIdr = true;
        if (traceRequested) std::printf("    %.3f s: UDP silent (cause 3), key frame again (%d)\n", double(now) / 1e6, retries);
      }
      else if (onUdp && now - switchedAt >= feedbackSilenceUs && sender.silent(now)) {
        ++r.fallbacks; retries = 0; { uint64_t wait = probeBackoffFirstUs; for (int k = 1; k < r.fallbacks; ++k) wait = std::min(wait * 2, probeBackoffMaximumUs); wssUntil = now + wait; }  // ProbeBackoff: 30 s, doubling to 5 minutes
        forceIdr = true; controller.pathChanged();
        if (traceRequested) std::printf("    %.3f s: UDP silent (cause %u), on WebSocket for %.0f s\n", double(now) / 1e6, sender.silenceCause(now), double(wssUntil - now) / 1e6);
      }
      if (!onUdp) r.wssUs += 1000;
      if (onUdp && retries && now - switchedAt >= feedbackSilenceUs && !sender.silent(now)) retries = 0;
    }
    forward.arrive(now, [&](const Bytes& d) { arrivedBytes += d.size() + headerBytes + tagBytes; receiver.datagram(d.data(), d.size(), now, out); });
    if (now >= nextClient) {
      nextClient += 2000; receiver.tick(now, out);
      for (const auto& c : out.control) reverse.send(c.data(), c.size(), now);
      out.control.clear();
    }
    for (const auto& m : out.messages) {
      if (!m.video) continue;
      if (content == Content::game) {
        ++r.delivered;
        if (lastShown && now - lastShown >= 250 * ms) { ++r.stalls; r.stallUs += now - lastShown; r.longestStallUs = std::max(r.longestStallUs, now - lastShown); }
        lastShown = now;
      } else lastShown = 0;
    }
    out.messages.clear();
    reverse.arrive(now, [&](const Bytes& d) {
      const auto c = sender.control(d.data(), d.size(), now);
      if (c.feedback) {
        const uint32_t arrived = c.received - received, given = c.lost - lost; received = c.received; lost = c.lost; r.gaps += given;
        // As UdpMediaPath: the wire bytes sent since the last report, and the same mean size for what arrived.
        const uint64_t sentWire = sender.stats().wireBytes - wireAt, sentCount = sender.stats().datagrams - datagramsAt; wireAt = sender.stats().wireBytes; datagramsAt = sender.stats().datagrams;
        if (sentCount) meanDatagram = double(sentWire) / double(sentCount);
        if (onUdp) { (void)controller.udpFeedback(now, c.rttUs, arrived, given, s.arrival ? uint64_t(double(arrived) * meanDatagram) : 0, s.arrival ? sentWire : 0); sender.setTarget(controller.target()); }
      }
      if (c.keyFrame && onUdp && c.epoch == epoch && now - keyFrameAt >= keyframeIntervalUs / 2) { keyFrameAt = now; ++r.clientKeys; forceIdr = true; }
      if (c.loss && c.lastGood && onUdp && c.epoch == epoch && now - lossAt >= 30000) { lossAt = now; sender.recovering(now); answerLoss = true; answerLastGood = c.lastGood; }
    });
    // On WebSocket the controller follows that path (not modelled): it is left as it was.
    if (now >= nextTick) { nextTick += 100 * ms; if (onUdp) if (const auto value = controller.tick(now)) sender.setTarget(*value); }
    if (now >= secondAt) {
      r.targetMbps.push_back(controller.target() / 1e6); r.sentMbps.push_back(double(sentBytes) * 8 / 1e6); r.arrivedMbps.push_back(double(arrivedBytes) * 8 / 1e6);
      r.lostPerSecond.push_back(lostNow); r.keysPerSecond.push_back(keysNow);
      if (traceRequested) std::printf("  %3llu s %s target %6.2f sent %6.2f arrived %6.2f dropped %4d keys %2d %s\n", (unsigned long long)(secondAt / second), content == Content::game ? "game   " : content == Content::desktop ? "desktop" : "idle   ",
        controller.target() / 1e6, double(sentBytes) * 8 / 1e6, double(arrivedBytes) * 8 / 1e6, lostNow, keysNow, onUdp ? "" : "WSS");
      sentBytes = arrivedBytes = 0; lostNow = keysNow = 0; secondAt += second;
    }
  }
  return r;
}

void print(const Scenario& s, const Result& r, uint64_t from, uint64_t to) {
  std::printf("%-34s game %3llu-%3llu s: target mean %5.1f min %5.1f Mbps, arrived %5.1f Mbps, frames %d/%d, stalls %d (%.1f s, longest %.2f s), dropped %d, IDRs %d, recovery %d, backlog keys %d, client keys %d, WSS %.0f s\n",
    s.name.c_str(), (unsigned long long)(from / second), (unsigned long long)(to / second), r.mean(r.targetMbps, from, to), r.lowest(from, to), r.mean(r.arrivedMbps, from, to), r.delivered, r.generated,
    r.stalls, double(r.stallUs) / 1e6, double(r.longestStallUs) / 1e6, r.lost(from, to), r.keys(from, to), r.recoveries, r.backlogKeys, r.clientKeys, double(r.wssUs) / 1e6);
}

// Desktop work, a minute of nothing, then a game: the field day's order.
Scenario fieldDay(bool recovers, uint64_t restartAtUs = 0) {
  Scenario s; s.name = std::string("desktop-idle-game") + (recovers ? "" : ", IDR per loss") + (restartAtUs ? ", resize" : "");
  s.durationUs = 150 * second; s.recovers = recovers; s.restartAtUs = restartAtUs;
  s.content = [](uint64_t at) { return at < 25 * second ? Content::desktop : at < 85 * second ? Content::idle : Content::game; };
  return s;
}
}

int main(int argc, char** argv) {
  try {
    traceRequested = argc == 2 && std::string(argv[1]) == "--trace";
    std::printf("SIMULATION: the real Sender, Receiver and BitrateController over a modelled link (60 Mbps, a 40 ms queue, an 8 ms round trip unless named), a 144 fps game. Not a network measurement.\n");
    {  // The field day's order: desktop work, a minute of nothing, then the game.
      const Scenario s = fieldDay(false); const Result r = simulate(s, 0x5eed); print(s, r, 85 * second, 150 * second);
      require(r.stalls == 0 && r.wssUs == 0 && r.lowest(87 * second, 150 * second) >= 25, "desktop, idle, game: the game did not simply run on a path that carries it");
    }
    {  // The target already at 78.7 Mbps when the game starts (what the field had): one episode, then the path's rate.
      Scenario s; s.name = "stale 78.7 Mbps target, then the game"; s.start = 78700000; s.durationUs = 130 * second; s.recovers = false;
      s.content = [](uint64_t at) { return at < 10 * second ? Content::idle : Content::game; };
      const Result r = simulate(s, 0x5eed); print(s, r, 10 * second, 130 * second);
      require(r.stalls <= 1 && r.wssUs == 0 && r.lowest(12 * second, 130 * second) >= 30 && r.mean(r.arrivedMbps, 12 * second, 130 * second) >= 50, "a stale target was not brought to the path's rate in one episode");
    }
    // A running game through what a path does, five seeds summed. "counts only": the controller is not told
    // what arrived and UdpMediaPath leaves UDP at the first silent second (a Host before 1.1.39 in those two
    // respects; the remembered limit and the way back are this version's in both rows). The released 1.2.1,
    // measured with this file against its headers (2026-11-23): link 60-25-60 73307/86405 frames, 35.6 s
    // stalled, 78 s on WebSocket; link 60-8-60 42088 frames, 159 s stalled, 293 s on WebSocket; outage
    // 63016 frames, 155 s stalled, 150 s on WebSocket; dips 11.2 Mbps arrived (the rate stayed at its floor).
    struct Case { const char* name; std::function<double(uint64_t)> capacity; double lossPct; uint64_t queueMs; };
    const auto between = [](uint64_t at, uint64_t from, uint64_t to) { return at >= from * second && at < to * second; };
    const std::vector<Case> cases{
      {"link 60-25-60", [=](uint64_t at) { return between(at, 40, 80) ? 25e6 : 60e6; }, 0, 40},
      {"link 60-8-60", [=](uint64_t at) { return between(at, 40, 80) ? 8e6 : 60e6; }, 0, 40},
      {"link 60-25-60, 8 ms queue", [=](uint64_t at) { return between(at, 40, 80) ? 25e6 : 60e6; }, 0, 8},
      {"outage 1.5 s", [](uint64_t at) { return at >= 60 * second && at < 61500 * ms ? 1e5 : 60e6; }, 0, 40},
      {"dips to 5 Mbps, 0.3 s every 6 s", [](uint64_t at) { return at % (6 * second) < 300 * ms && at > 10 * second ? 5e6 : 60e6; }, 0, 40},
      {"2 % random loss", [](uint64_t) { return 60e6; }, 2, 40},
      {"steady 60", [](uint64_t) { return 60e6; }, 0, 40},
    };
    std::printf("%-34s %-11s %13s %6s %8s %8s %7s %6s %6s\n", "running game, 120 s x 5 seeds", "", "frames", "stalls", "stall s", "longest", "WSS s", "IDRs", "Mbps");
    for (size_t n = 0; n < cases.size(); ++n) {
      const auto& c = cases[n]; Result sum[2]; double mbps[2] = {};
      for (int k = 0; k < 2; ++k) for (int seed = 0; seed < 5; ++seed) {
        Scenario s; s.name = c.name; s.durationUs = 120 * second; s.capacity = c.capacity; s.lossPct = c.lossPct; s.queueUs = c.queueMs * ms; s.arrival = k == 1; s.silentRetries = k == 1 ? 2 : 0;
        const Result r = simulate(s, 0x5eed + uint64_t(seed) * 7919);
        sum[k].delivered += r.delivered; sum[k].generated += r.generated; sum[k].stalls += r.stalls; sum[k].stallUs += r.stallUs; sum[k].longestStallUs = std::max(sum[k].longestStallUs, r.longestStallUs);
        sum[k].wssUs += r.wssUs; sum[k].idrs += r.idrs; mbps[k] += r.mean(r.arrivedMbps, 0, 120 * second) / 5;
      }
      for (int k = 0; k < 2; ++k) {
        char frames[32]; std::snprintf(frames, sizeof(frames), "%d/%d", sum[k].delivered, sum[k].generated);
        std::printf("%-34s %-11s %13s %6d %8.1f %8.2f %7.0f %6d %6.1f\n", c.name, k ? "as built" : "counts only", frames, sum[k].stalls, double(sum[k].stallUs) / 1e6, double(sum[k].longestStallUs) / 1e6, double(sum[k].wssUs) / 1e6, sum[k].idrs, mbps[k]);
      }
      const Result& r = sum[1]; const double share = double(r.delivered) / double(std::max(r.generated, 1)), stalled = double(r.stallUs) / 1e6, longest = double(r.longestStallUs) / 1e6;
      const std::string name = c.name;
      // No path here ends the binding, so nothing may send the stream to WebSocket.
      require(r.wssUs == 0, name + ": the stream left UDP");
      if (n == 0) require(share >= 0.96 && stalled <= 5 && longest <= 1, name + ": a link that halved was not followed");
      if (n == 1) require(share >= 0.93 && longest <= 2.5, name + ": a link at an eighth was not followed");
      if (n == 2) require(share >= 0.96 && r.idrs <= 60, name + ": a shallow queue was probed into again and again");
      if (n == 3) require(share >= 0.95 && longest <= 2.5, name + ": an outage cost more than itself");
      if (n == 4) require(mbps[1] >= 30 && longest <= 1, name + ": the rate did not come back between dips");
      if (n == 5) require(share >= 0.96 && mbps[1] >= 50, name + ": loss no rate changes lowered the rate");
      if (n == 6) require(r.stalls == 0 && mbps[1] >= 52 && r.idrs <= 80, name + ": a steady path was not simply used");
    }
    std::printf("udp-rate-loop: %lld checks passed\n", checks);
    return 0;
  } catch (const std::exception& error) { std::fprintf(stderr, "udp rate loop check failed: %s\n", error.what()); return 1; }
}
