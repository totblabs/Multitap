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
#include "reference_history.hpp"
#include "path_frame_rate.hpp"
#include "../../../native/windows_video/udp_media.hpp"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
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
  // A wireless last hop that goes off the air (a scan): from each of these times, for pauseUs, nothing
  // passes either way. The access point holds up to holdBytes for the client and drops the rest; all
  // of it leaves when the pause ends. Field 2026-11-24 (a viewing PC on Wi-Fi): 0.3 s, one to five
  // minutes apart, the low-rate desktop late and whole, the 60 Mbps one short of about 120 datagrams.
  std::vector<uint64_t> pauses; uint64_t pauseUs = 300000; size_t holdBytes = 2000000;
  // Interference on that hop: this share (per cent) of what passes is lost, either way, by time.
  std::function<double(uint64_t)> lossAt;
  double gameDemand = 90e6;    // what the encoder emits for the game when the target allows it
  // How the encoder answers a LOSS (reference_history.hpp): 0 an IDR; 1 a P-frame while the client's last good
  // frame is among its last 16 (reference invalidation: NVENC on a GPU with several reference frames before
  // Host 1.1.40); 2 a P-frame from a long-term reference the client holds, begun at the first LOSS's IDR.
  int recovery = 2;
  // The frame rate follows the target (path_frame_rate.hpp); false: the stream keeps its rate and that rate's floor (a Host before 1.1.41).
  bool frameRate = true;
  // What a frame of the game costs at the encoder's worst quality, in bits per pixel: it cannot make one smaller
  // (measured on a Tesla P40 at QP 50-51: 6.5 KB for 2084x1340, 0.0185).
  double encoderLeast = 0.0185;
  uint64_t restartAtUs = 0;    // a new stream (a resize): a new epoch and a key frame
  size_t idrBytes = 40000;     // the least an IDR takes, whatever the target
  int silentRetries = 0;       // UdpMediaPath: key frames tried again on UDP before the stream leaves it
  bool arrival = true;         // the controller is told what arrived (false: counts only, as a Host before 1.1.39)
};
struct Result {
  std::vector<double> targetMbps, sentMbps, arrivedMbps;  // per second
  std::vector<int> lostPerSecond, keysPerSecond, shownPerSecond;
  int generated = 0, delivered = 0, idrs = 0, recoveries = 0, backlogKeys = 0, clientKeys = 0, stalls = 0, fallbacks = 0;
  uint64_t stallUs = 0, longestStallUs = 0, wssUs = 0, gaps = 0;
  double target(uint64_t atUs) const { return targetMbps[std::min(targetMbps.size() - 1, size_t(atUs / second))]; }
  double lowest(uint64_t from, uint64_t to) const { double v = 1e9; for (size_t n = size_t(from / second); n < size_t(to / second) && n < targetMbps.size(); ++n) v = std::min(v, targetMbps[n]); return v; }
  double mean(const std::vector<double>& values, uint64_t from, uint64_t to) const { double s = 0; int count = 0; for (size_t n = size_t(from / second); n < size_t(to / second) && n < values.size(); ++n) { s += values[n]; ++count; } return count ? s / count : 0; }
  int lost(uint64_t from, uint64_t to) const { int s = 0; for (size_t n = size_t(from / second); n < size_t(to / second) && n < lostPerSecond.size(); ++n) s += lostPerSecond[n]; return s; }
  int framesBetween(uint64_t from, uint64_t to) const { int s = 0; for (size_t n = size_t(from / second); n < size_t(to / second) && n < shownPerSecond.size(); ++n) s += shownPerSecond[n]; return s; }
  int keys(uint64_t from, uint64_t to) const { int s = 0; for (size_t n = size_t(from / second); n < size_t(to / second) && n < keysPerSecond.size(); ++n) s += keysPerSecond[n]; return s; }
};

class Link {
 public:
  Link(uint64_t seed, std::function<double(uint64_t)> rate, uint64_t delayUs, uint64_t queueUs, double lossPct) : random_(seed), rate_(std::move(rate)), delayUs_(delayUs), queueUs_(queueUs), lossPct_(lossPct) {}
  void pauses(std::vector<uint64_t> from, uint64_t lengthUs, size_t holdBytes) { pauses_ = std::move(from); pauseUs_ = lengthUs; hold_ = holdBytes; }
  void lossAt(std::function<double(uint64_t)> pct) { lossAt_ = std::move(pct); }
  bool send(const uint8_t* data, size_t size, uint64_t now) {
    if (uniform() * 100 < lossPct_ + (lossAt_ ? lossAt_(now) : 0)) return false;
    for (const uint64_t from : pauses_) if (now >= from && now < from + pauseUs_) {
      // Off the air: held until the pause ends, in order, while the hold has room.
      if (heldFor_ != from) { heldFor_ = from; held_ = 0; }
      if (held_ + size > hold_) return false;
      held_ += size; free_ = std::max(free_, from + pauseUs_) + uint64_t(double(size + 28) * 8e6 / std::max(rate_(from + pauseUs_), 1.0));
      queue_.push({free_ + delayUs_, order_++, Bytes(data, data + size)});
      return true;
    }
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
  std::vector<uint64_t> pauses_; uint64_t pauseUs_ = 0, heldFor_ = 0; size_t hold_ = 0, held_ = 0; std::function<double(uint64_t)> lossAt_;
  std::priority_queue<Packet, std::vector<Packet>, std::greater<Packet>> queue_;
};

bool traceRequested = false;

Result simulate(const Scenario& s, uint64_t seed) {
  Result r; std::mt19937_64 random(seed);
  const auto uniform = [&] { return double(random() >> 11) / double(1ull << 53); };
  host::BitrateController controller(s.start, s.maximum, s.fps); controller.geometry(s.width, s.height, s.fps, s.frameRate ? host::pathFrameLowest(s.fps) : 0); controller.pathChanged();
  host::PathFrameRate ladder(s.fps); int paced = s.fps; const double least = s.encoderLeast * s.width * s.height / 8;
  Sender sender(controller.target(), 1000, [&] { return random() | 1; });
  Receiver receiver(1, 1000); Receiver::Output out; Plain plain;
  Link forward(seed + 1, s.capacity, s.rttUs / 2, s.queueUs, s.lossPct), reverse(seed + 2, [](uint64_t) { return 1e9; }, s.rttUs / 2, 0, 0);
  if (!s.pauses.empty()) { forward.pauses(s.pauses, s.pauseUs, s.holdBytes); reverse.pauses(s.pauses, s.pauseUs, size_t(1) << 30); }
  if (s.lossAt) { forward.lossAt(s.lossAt); reverse.lossAt(s.lossAt); }
  uint16_t epoch = 1; uint64_t pts = 0, nextFrame = 1000, nextPump = 1000, nextClient = 1000, nextTick = 1000;
  // UdpMediaPath's own state.
  uint64_t keyFrameAt = 0, lossAt = 0, switchedAt = 1000, wssUntil = 0; uint32_t received = 0, lost = 0;
  uint64_t wireAt = 0, datagramsAt = 0; double meanDatagram = 1200; int retries = 0;
  bool forceIdr = true, answerLoss = false; uint64_t answerLastGood = 0;
  std::vector<uint64_t> recentPts;  // the encoder's last references (16, reference_history.hpp)
  std::optional<host::LongTermReferences> longTerm;  // once a LOSS came
  // The client's picture.
  uint64_t lastShown = 0, restarted = false;
  // Per-second accounting.
  uint64_t sentBytes = 0, arrivedBytes = 0; int lostNow = 0, keysNow = 0, shownNow = 0; uint64_t secondAt = second;
  uint64_t burstUntil = 0, nextBurst = 2 * second, lastIdleFrame = 0;

  for (uint64_t now = 1000; now < s.durationUs; now += 250) {
    const Content content = s.content(now);
    const bool onUdp = !wssUntil;
    if (s.restartAtUs && !restarted && now >= s.restartAtUs) {
      // A resize: a new stream of a new epoch. Its first key frame switches to UDP again.
      restarted = true; ++epoch; receiver.setEpoch(epoch); forceIdr = true; longTerm.reset(); switchedAt = now; controller.geometry(s.width, s.height, s.fps, s.frameRate ? host::pathFrameLowest(s.fps) : 0); ladder = host::PathFrameRate(s.fps);
    }
    if (now >= nextFrame) {
      // The capture loop: the paced rate follows the target, and the encoder budgets each frame at it.
      if (s.frameRate) { paced = ladder.decide(now, int(controller.target()), s.width, s.height); controller.paced(paced); }
      const uint64_t frameUs = second / uint64_t(paced);
      nextFrame += frameUs;
      const double target = controller.target(), budget = target / 8 / paced;
      size_t size = 0; bool tile = false;
      if (content == Content::game) size = size_t(std::max(least, std::min(budget, s.gameDemand / 8 / s.fps) * (0.7 + 0.6 * uniform())));
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
          // The reference of the client's last good frame is still held, or a long-term one it has: a recovery frame; else an IDR.
          if (s.recovery == 1) recovery = std::find(recentPts.begin(), recentPts.end(), answerLastGood) != recentPts.end();
          else if (s.recovery == 2 && longTerm) recovery = longTerm->recover(answerLastGood).has_value();
          else if (s.recovery == 2) longTerm.emplace(4);
        }
        const bool idr = key || (answerLoss && !recovery);
        // A recovery frame: half as large again from the last good frame (a guess); from a long-term reference as large as the P-frames around it (measured on a Tesla P40, CBR with a one-frame VBV: 6.4-6.7 KB among 6.6 KB at 720p144, 15.7-16.6 among 16.3 at 1080p60, where an IDR took 1.3-2.3 times a P-frame). Twice a P-frame instead would cost more stalls than the IDRs it replaces.
        if (idr) size = std::max(size, s.idrBytes); else if (recovery) size = size_t(double(std::max<size_t>(size, 1200)) * (s.recovery == 2 ? 1.0 : 1.5));
        answerLoss = false; forceIdr = false;
        if (idr) { ++r.idrs; ++keysNow; recentPts.clear(); } else if (recovery) ++r.recoveries;
        recentPts.push_back(pts); if (recentPts.size() > 16) recentPts.erase(recentPts.begin());
        if (longTerm) { const auto mark = longTerm->due(pts, idr); longTerm->encoded(pts, idr, mark); }
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
        ++r.delivered; ++shownNow;
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
      r.lostPerSecond.push_back(lostNow); r.keysPerSecond.push_back(keysNow); r.shownPerSecond.push_back(shownNow); shownNow = 0;
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
  s.durationUs = 150 * second; s.recovery = recovers ? 2 : 0; s.restartAtUs = restartAtUs;
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
      Scenario s; s.name = "stale 78.7 Mbps target, then the game"; s.start = 78700000; s.durationUs = 130 * second; s.recovery = 0;
      s.content = [](uint64_t at) { return at < 10 * second ? Content::idle : Content::game; };
      const Result r = simulate(s, 0x5eed); print(s, r, 10 * second, 130 * second);
      require(r.stalls <= 1 && r.wssUs == 0 && r.lowest(12 * second, 130 * second) >= 30 && r.mean(r.arrivedMbps, 12 * second, 130 * second) >= 50, "a stale target was not brought to the path's rate in one episode");
    }
    // A running game through what a path does, five seeds summed. "Host 1.1.39": a LOSS is answered without an
    // IDR only while the last good frame is among the encoder's last 16 (at 144 fps it rarely is). "Host 1.1.40":
    // the answer comes from a long-term reference. "as built": the frame rate follows the target as well. Before
    // those, a Host that was not told what arrived and left UDP at the first silent second: the released 1.2.1,
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
    std::printf("%-34s %-15s %13s %6s %8s %8s %7s %6s %6s %6s\n", "running game, 120 s x 5 seeds", "", "frames", "stalls", "stall s", "longest", "WSS s", "IDRs", "P ans.", "Mbps");
    for (size_t n = 0; n < cases.size(); ++n) {
      const auto& c = cases[n]; Result sum[3]; double mbps[3] = {};
      for (int k = 0; k < 3; ++k) for (int seed = 0; seed < 5; ++seed) {
        Scenario s; s.name = c.name; s.durationUs = 120 * second; s.capacity = c.capacity; s.lossPct = c.lossPct; s.queueUs = c.queueMs * ms; s.silentRetries = 2; s.recovery = k ? 2 : 1; s.frameRate = k == 2;
        const Result r = simulate(s, 0x5eed + uint64_t(seed) * 7919);
        sum[k].delivered += r.delivered; sum[k].generated += r.generated; sum[k].stalls += r.stalls; sum[k].stallUs += r.stallUs; sum[k].longestStallUs = std::max(sum[k].longestStallUs, r.longestStallUs);
        sum[k].wssUs += r.wssUs; sum[k].idrs += r.idrs; sum[k].recoveries += r.recoveries; mbps[k] += r.mean(r.arrivedMbps, 0, 120 * second) / 5;
      }
      for (int k = 0; k < 3; ++k) {
        char frames[32]; std::snprintf(frames, sizeof(frames), "%d/%d", sum[k].delivered, sum[k].generated);
        std::printf("%-34s %-15s %13s %6d %8.1f %8.2f %7.0f %6d %6d %6.1f\n", c.name, k == 2 ? "as built" : k ? "Host 1.1.40" : "Host 1.1.39", frames, sum[k].stalls, double(sum[k].stallUs) / 1e6, double(sum[k].longestStallUs) / 1e6, double(sum[k].wssUs) / 1e6, sum[k].idrs, sum[k].recoveries, mbps[k]);
      }
      const Result& r = sum[2]; mbps[1] = mbps[2];
      // A LOSS within reach is answered from a long-term reference: far fewer IDRs than a 16-frame window gives at 144 fps.
      if (sum[0].idrs >= 40) require(sum[1].idrs * 2 <= sum[0].idrs + 20 && sum[1].recoveries > sum[0].recoveries, std::string(c.name) + ": long-term references did not replace the IDRs"); const double share = double(r.delivered) / double(std::max(r.generated, 1)), stalled = double(r.stallUs) / 1e6, longest = double(r.longestStallUs) / 1e6;
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
    // The same game at a field picture's size, 2084x1340 at 144 fps: the old floor of the rate is 16 Mbps there,
    // so a path under 16 Mbps was sent more than it carries for as long as it stayed narrow.
    std::printf("%-34s %-15s %13s %6s %8s %8s %7s %6s %6s %6s\n", "2084x1340, 120 s x 5 seeds", "", "frames", "stalls", "stall s", "longest", "WSS s", "IDRs", "fps", "Mbps");
    const std::vector<Case> narrow{
      {"link 60-12-60", [=](uint64_t at) { return between(at, 40, 80) ? 12e6 : 60e6; }, 0, 40},
      {"link 60-8-60", [=](uint64_t at) { return between(at, 40, 80) ? 8e6 : 60e6; }, 0, 40},
      {"link 60-4-60", [=](uint64_t at) { return between(at, 40, 80) ? 4e6 : 60e6; }, 0, 40},
      {"steady 60", [](uint64_t) { return 60e6; }, 0, 40},
    };
    for (size_t n = 0; n < narrow.size(); ++n) {
      const auto& c = narrow[n]; Result sum[2]; double mbps[2] = {}, narrowMbps[2] = {}, narrowFps[2] = {};
      for (int k = 0; k < 2; ++k) for (int seed = 0; seed < 5; ++seed) {
        Scenario s; s.name = c.name; s.width = 2084; s.height = 1340; s.durationUs = 120 * second; s.capacity = c.capacity; s.queueUs = c.queueMs * ms; s.silentRetries = 2; s.frameRate = k == 1;
        const Result r = simulate(s, 0x5eed + uint64_t(seed) * 7919);
        sum[k].delivered += r.delivered; sum[k].generated += r.generated; sum[k].stalls += r.stalls; sum[k].stallUs += r.stallUs; sum[k].longestStallUs = std::max(sum[k].longestStallUs, r.longestStallUs);
        sum[k].wssUs += r.wssUs; sum[k].idrs += r.idrs; mbps[k] += r.mean(r.arrivedMbps, 0, 120 * second) / 5; narrowMbps[k] += r.mean(r.arrivedMbps, 45 * second, 80 * second) / 5; narrowFps[k] += double(r.framesBetween(45 * second, 80 * second)) / 35 / 5;
      }
      for (int k = 0; k < 2; ++k) {
        char frames[32]; std::snprintf(frames, sizeof(frames), "%d/%d", sum[k].delivered, sum[k].generated);
        std::printf("%-34s %-15s %13s %6d %8.1f %8.2f %7.0f %6d %6.0f %6.1f\n", c.name, k ? "as built" : "Host 1.1.40", frames, sum[k].stalls, double(sum[k].stallUs) / 1e6, double(sum[k].longestStallUs) / 1e6, double(sum[k].wssUs) / 1e6, sum[k].idrs, narrowFps[k], narrowMbps[k]);
      }
      const Result& r = sum[1]; const std::string name = std::string("2084x1340 ") + c.name; const double stalled = double(r.stallUs) / 1e6, longest = double(r.longestStallUs) / 1e6;
      if (n != 2) require(r.wssUs == 0, name + ": the stream left UDP");
      // While the link is narrow the stream uses it: most of what it carries arrives, at a lower frame rate, without long stalls.
      if (n == 0) require(narrowMbps[1] >= 9 && stalled <= 6 && longest <= 1.5 && narrowFps[1] >= 60, name + ": 12 Mbps were not used");
      if (n == 1) require(narrowMbps[1] >= 6 && stalled <= 8 && longest <= 2 && narrowFps[1] >= 40, name + ": 8 Mbps were not used");
      // n == 2, 4 Mbps: under what 30 frames of this picture need (3.4 Mbps at 0.04 bit per pixel, with nothing to spare
      // for a key frame or the way back). Shown, not required: this rule ends at its lowest rate.
      if (n == 3) require(r.stalls == 0 && narrowFps[1] >= 140 && mbps[1] >= 52, name + ": a wide path lost frames to the frame rate rule");
    }
    // A wireless pause or a burst of interference: the target is back within seconds (as built: 1 to 8 s after
    // pauses, 2 to 21 s after two seconds of loss). The field once took two minutes (2026-11-24, 100 to 30 Mbps
    // after two stalls two seconds apart); that is not reproduced here, so nothing was changed for it.
    {
      struct Wireless { const char* name; std::vector<uint64_t> at; size_t hold; double lossPct = 0; uint64_t lossUs = 0; };
      const std::vector<Wireless> wireless{
        {"20 % lost for 2 s at 60 s", {}, 0, 20, 2 * second},
        {"40 % lost for 2 s at 60 s", {}, 0, 40, 2 * second},
        {"70 % lost for 1 s at 60 s", {}, 0, 70, second},
        {"pause, then 30 % for 2 s", {60 * second}, 2000000, 30, 2 * second},
        {"one pause at 60 s, holds 2 MB", {60 * second}, 2000000},
        {"one pause at 60 s, holds 1 MB", {60 * second}, 1000000},
        {"two pauses, 60 s and 62 s", {60 * second, 62 * second}, 2000000},
        {"three pauses, 60, 62, 64 s", {60 * second, 62 * second, 64 * second}, 2000000},
      };
      std::printf("%-32s %5s %7s %6s %6s %6s %6s %6s %6s %8s %6s %7s %5s\n", "wireless pause 0.3 s, 2084x1340", "seed", "before", "+2 s", "+5 s", "+10 s", "+20 s", "+40 s", "+80 s", "back in", "stalls", "stall s", "IDRs");
      for (const auto& c : wireless) for (int seed = 0; seed < 3; ++seed) {
        Scenario s; s.name = c.name; s.width = 2084; s.height = 1340; s.fps = 120; s.durationUs = 180 * second; s.capacity = [](uint64_t) { return 300e6; }; s.silentRetries = 2;
        s.pauses = c.at; s.holdBytes = c.hold;
        if (c.lossUs) s.lossAt = [pct = c.lossPct, length = c.lossUs](uint64_t t) { return t >= 60 * second && t < 60 * second + length ? pct : 0.0; };
        const Result r = simulate(s, 0x5eed + uint64_t(seed) * 7919);
        const uint64_t at = c.at.empty() ? 60 * second : c.at.back(); const double before = r.mean(r.targetMbps, 50 * second, 59 * second);
        int back = -1; for (uint64_t t = at + second; t < s.durationUs; t += second) if (r.target(t) >= before * 0.9) { back = int((t - at) / second); break; }
        char took[16]; if (back < 0) std::snprintf(took, sizeof(took), "never"); else std::snprintf(took, sizeof(took), "%d s", back);
        require(back >= 0 && back <= (c.lossUs ? 30 : 15), std::string(c.name) + ": the target stayed low after a wireless pause or burst");
        std::printf("%-32s %5d %7.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %8s %6d %7.2f %5d\n", c.name, seed, before, r.target(at + 2 * second), r.target(at + 5 * second), r.target(at + 10 * second), r.target(at + 20 * second), r.target(at + 40 * second), r.target(at + 80 * second), took, r.stalls, double(r.stallUs) / 1e6, r.idrs);
      }
    }
    std::printf("udp-rate-loop: %lld checks passed\n", checks);
    return 0;
  } catch (const std::exception& error) { std::fprintf(stderr, "udp rate loop check failed: %s\n", error.what()); return 1; }
}
