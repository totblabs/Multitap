#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <span>

// Adaptive ToTBSeamless bitrate (bitrate-adaptive-v1, docs/adaptive-bitrate.md).
// Pure, clock-injected logic: the socket-owning Agent feeds it what entered the
// host send queue, the queue state and the client's arrival feedback; it returns
// a new video target when one should be applied. No I/O, no threads, no clock.
namespace host {
inline constexpr const char* bitrateAdaptiveProtocol = "bitrate-adaptive-v1";
constexpr int legacyMaximumBitrate = 20000000;     // released fixed ceiling
constexpr int adaptiveMaximumBitrate = 100000000;  // = encoderMaximumBitrate
constexpr int adaptiveStartBitrate = 20000000;
constexpr int adaptiveMinimumBitrate = 2000000;
// Host send-queue packet limit of an adaptive session (fixed sessions: 32).
// 100 PCM packets/s made 32 only ~0.2 s of queue, which a 100 Mbps stream
// overflows before any rate change can take effect; bytes stay 8 MiB.
constexpr size_t adaptiveHostQueuePackets = 128;

// Lowest target the controller may give the Microsoft software H.264 MFT at a
// geometry. Measured (encoder_bitrate_test.cpp, Windows 26100): a 4K60 encoder
// started at 8 Mbps, or lowered to 12-14 Mbps under detailed motion, entered a
// state of ~130-155 Mbps output at ~14 dB that a later 100 Mbps target did not
// repair; 0.04 bit per pixel-frame (20 Mbps at 4K60) recovered in every case.
inline int adaptiveBitrateFloor(int width, int height, int fps) {
  if (width <= 0 || height <= 0 || fps <= 0) return adaptiveMinimumBitrate;
  const double value = 0.04 * double(width) * double(height) * double(fps);
  return int(std::clamp(value, double(adaptiveMinimumBitrate), double(legacyMaximumBitrate)));
}

// One client-reported video packet: host PTS and the client's monotonic arrival.
struct BitrateArrival { uint64_t pts = 0; uint64_t arrivalUs = 0; };

class BitrateController {
 public:
  // Tuning (simulated in bitrate_controller_test.cpp).
  static constexpr int64_t overuseExcessUs = 30000;     // standing excess that is congestion
  static constexpr int64_t burstExcessUs = 400000;      // reacts on a single interval
  static constexpr int64_t probeExcessUs = 10000;       // below this the path is clear
  static constexpr uint64_t stableUs = 500000;          // no probing this soon after congestion
  static constexpr uint64_t staleFeedbackUs = 2000000;  // feedback silence while sending video
  static constexpr double probeStep = 1.03, nearProbeStep = 1.006;  // per 100 ms
  static constexpr size_t localPacketLimit = 48, localByteLimit = 4 * 1024 * 1024;  // host queue: 128 / 8 MiB
  static constexpr size_t relayByteLimit = 4 * 1024 * 1024;                          // relay: 8 MiB
  static constexpr double virtualHeadroom = 1.25;
  // UDP loss. Only what NACK could not repair counts: random loss up to 10 % leaves almost
  // none (udp_media_test.cpp simulation), and each datagram given up costs a frame and a
  // recovery or an IDR. So 1 % given up, or loss in every report of the window, is a full
  // queue or a policer. Each report decides over the last 300 ms, with at least this many
  // datagrams. The cut follows the loss: at (1 - loss) the excess is gone, 0.95 leaves
  // room. A spike in one report cuts at most 15 % (a blackout is not capacity).
  static constexpr uint64_t lossWindowUs = 300000;
  static constexpr uint64_t lossMinimumDatagrams = 30;
  static constexpr double lossCut = 0.01, lossDrain = 0.95;
  // The rate that lost is remembered (a policer or a shallow queue gives no delay signal, so
  // probing finds it only by loss, and each datagram given up costs a frame and a recovery).
  // Field 2026-11-20 (a game on a local PC over UDP, a path that carried 35-45 Mbps): probing
  // touched the limit about every 2.5 s (254 recoveries in ten minutes), and once the 30 s
  // "gentle" memory ran out it climbed at 34 %/s to 69-79 Mbps, then 0.6 s freezes came in
  // bursts. Now the rate stays below 97 % of it; a try above it comes after a wait that
  // doubles while the loss keeps coming at the same rate (2 s up to 32 s), climbs gently,
  // and a try that passes it by 5 % without loss moves it up (the path carries more now).
  // Five minutes without loss forget it.
  // What arrived. A datagram given up is reported 60-600 ms after it was lost, and all at once, so
  // the loss share of a report says when to act but little about how far (closed loop,
  // udp_rate_loop_test.cpp: a link that halved under a game was followed in over a second, long
  // enough for the stream to count as silent and leave UDP for its 30 s backoff). The share of what
  // was sent that arrived, over this report and the one before it, says how far: a cut goes to
  // arrivalDrain of it. Two reports in a row under arrivalShort are a path that carries less, not a
  // blip, and are decided at once. Under arrivalBlackout almost nothing arrives: an outage is no
  // capacity, the rate is held for when the path is back. A report needs arrivalMinimumBytes sent
  // to mean anything.
  static constexpr double arrivalShort = 0.8, arrivalDrain = 0.95, arrivalBlackout = 0.1;
  static constexpr uint64_t arrivalMinimumBytes = 30000;
  static constexpr double ceilingMargin = 0.97, ceilingRepeat = 1.1, ceilingPassed = 1.05;
  // Only a loss within ceilingRepeat of the limit, either way, is the same limit again (a longer
  // wait): one at a rate far below it is another limit, with the first wait. Closed loop
  // (udp_rate_loop_test.cpp) and field 2026-11-23: after one 300 ms dip the later cuts of the same
  // episode set the limit to the reduced rate, each counted as a repeat and doubled the wait, so
  // a path that carried 58 Mbps stayed at its floor for minutes.
  // A cut to under restoreDeep of the rate before it is as likely a dip as a lasting change, and the
  // only way to know is to try: once the path is clear again the rate goes back to half of that
  // rate at once and on at restoreStep per 100 ms up to restoreShare of it. A cut on the way ends
  // it, and none starts for restoreAgainUs after one that failed (the change lasts: plain probing).
  static constexpr double restoreDeep = 0.7, restoreShare = 0.9, restoreStep = 1.072;
  static constexpr uint64_t restoreAgainUs = 10000000, restoreMemoryUs = 30000000;
  static constexpr uint64_t ceilingWaitFirstUs = 2000000, ceilingWaitLongestUs = 32000000, ceilingMemoryUs = 300000000;

  BitrateController(int start, int maximum, int fps = 60)
      : maximum_(std::clamp(maximum, adaptiveMinimumBitrate, adaptiveMaximumBitrate)), floor_(adaptiveMinimumBitrate), fps_(std::clamp(fps, 1, 240)) {
    target_ = std::clamp(double(start), double(floor_), double(maximum_)); published_ = int(target_);
  }
  int target() const { return published_; }
  int maximum() const { return maximum_; }
  int floor() const { return floor_; }
  bool encoderLimited() const { return encoderLimited_; }
  // Standing one-way queueing delay after the host send queue (ms), or -1.
  double standingDelayMs(uint64_t nowUs) const { return delayAtUs_ && nowUs - delayAtUs_ <= 2000000 ? double(standingDelayUs_) / 1000 : -1; }
  double deliveredKbps() const { return delivered() / 1000; }
  double standingExcessMs() const { return double(standingExcessUs_) / 1000; }

  // The encoder geometry of a (new) media epoch; the floor follows it. `lowest`: the capture
  // paces this stream's frames between that rate and `fps` as the target allows
  // (path_frame_rate.hpp), so the floor is the lowest rate's and the target can follow a path
  // that carries less than the stream's own rate needs. 0: the stream keeps its rate.
  std::optional<int> geometry(int width, int height, int fps, int lowest = 0) {
    fps_ = std::clamp(fps, 1, 240);
    floor_ = std::min(adaptiveBitrateFloor(width, height, lowest > 0 ? std::min(lowest, fps_) : fps_), maximum_);
    target_ = std::clamp(target_, double(floor_), double(maximum_));
    idleRestore_ = std::min(idleRestore_, double(maximum_));
    return publish(true);
  }
  // The rate the capture paces frames at now: a frame's usual size is the target over it.
  void paced(int fps) { fps_ = std::clamp(fps, 1, 240); }
  // Independent outputs (bitrate-adaptive-tracks-v1): the floor is the sum of
  // the outputs' own floors (each encoder's limit holds at its own geometry).
  std::optional<int> geometryFloor(int floor, int fps) {
    fps_ = std::clamp(fps, 1, 240);
    floor_ = std::clamp(floor, adaptiveMinimumBitrate, maximum_);
    target_ = std::clamp(target_, double(floor_), double(maximum_));
    idleRestore_ = std::min(idleRestore_, double(maximum_));
    return publish(true);
  }
  // A media packet entered the host send queue. `tile` = lossless tile payload
  // (kind 1, not encoder output). Video packets are remembered for feedback.
  void sent(uint64_t nowUs, bool video, uint16_t epoch, uint64_t pts, size_t bytes, bool tile = false) {
    drain(nowUs);
    // The virtual link has 25 % headroom over the steady send rate: at exactly
    // the target a burst's backlog would never drain while CBR continues.
    const double rate = virtualHeadroom * target_ + audioRate(nowUs);
    vqUs_ = std::min(3e6, vqUs_ + double(bytes) * 8 * 1e6 / std::max(rate, 1.0));
    if (!video) { push(audio_, nowUs, bytes); return; }
    if (!firstVideoUs_) firstVideoUs_ = nowUs;
    push(video_, nowUs, bytes);
    const bool burst = !tile && double(bytes) > 4 * target_ / 8 / fps_;
    // Bursts (IDR, page change) are content, not encoder overshoot.
    if (!tile && !burst) push(encoded_, nowUs, bytes);
    if (burst) burstUntilUs_ = std::max(burstUntilUs_, nowUs + uint64_t(vqUs_) + 1600000);
    sent_.push_back({nowUs, pts, uint32_t(std::min<size_t>(bytes, UINT32_MAX)), uint32_t(std::min(vqUs_, 4e9)), epoch});
    while (sent_.size() > 1024 || (!sent_.empty() && nowUs - sent_.front().atUs > 4000000)) sent_.pop_front();
  }
  // Host send queue state: age of the oldest queued packet (including a write
  // still in progress), queued bytes and packets. Guards the overflow that ends
  // the session (adaptiveHostQueuePackets / 8 MiB), and a queue our own bursts
  // do not explain.
  std::optional<int> local(uint64_t nowUs, uint64_t oldestAgeUs, size_t queuedBytes, size_t queuedPackets) {
    drain(nowUs);
    const bool fullBytes = queuedBytes >= localByteLimit;
    const bool full = queuedPackets >= localPacketLimit || fullBytes;
    const bool aged = double(oldestAgeUs) > vqUs_ + 150000;
    // While application-limited, an aged or packet-full queue is a stalled
    // write (100 audio packets/s reach the packet limit in ~0.4 s), not ours
    // to fix: a lower target does not change what a mostly idle desktop
    // sends, and what was written during the stall is no capacity measure
    // (in the field such cuts took 52 Mbps to the 2.7 Mbps floor). A
    // queue filling with bytes still is.
    if ((fullBytes || ((full || aged) && !limited(nowUs))) && nowUs - lastEmergencyUs_ >= 150000) emergency(nowUs, rate(written_, nowUs, 150000));
    return publish();
  }
  // A WebSocket write of `bytes` completed: while the queue is backed up this
  // is the host uplink's real rate.
  void written(uint64_t nowUs, size_t bytes) { drain(nowUs); push(written_, nowUs, bytes); }
  // Client feedback for one interval: video arrivals of `epoch`, all media
  // bytes received, the interval and the relay's own queue toward the client.
  std::optional<int> feedback(uint64_t nowUs, uint16_t epoch, std::span<const BitrateArrival> frames, uint64_t receivedBytes, uint64_t intervalUs, uint64_t relayQueuedBytes) {
    drain(nowUs);
    lastFeedbackUs_ = nowUs;
    if (intervalUs >= 10000 && intervalUs <= 5000000) { delivered_.push_back({intervalUs, receivedBytes}); while (delivered_.size() > 5) delivered_.pop_front(); }
    struct Match { int64_t raw; uint32_t vqUs; };
    Match matches[64]; size_t count = 0;
    for (const auto& frame : frames) {
      if (count == 64) break;
      for (auto at = sent_.rbegin(); at != sent_.rend(); ++at)
        if (at->pts == frame.pts && at->epoch == epoch) { matches[count++] = {int64_t(frame.arrivalUs) - int64_t(at->atUs), at->vqUs}; break; }
    }
    for (size_t n = 0; n < count; ++n) baseline(nowUs, matches[n].raw);
    bool informative = false;
    if (count) {
      int64_t delay = std::numeric_limits<int64_t>::max(), excess = delay;
      for (size_t n = 0; n < count; ++n) {
        const int64_t d = std::max<int64_t>(0, matches[n].raw - baseline_);
        delay = std::min(delay, d); excess = std::min(excess, d - int64_t(matches[n].vqUs));
      }
      standingDelayUs_ = delay; standingExcessUs_ = std::max<int64_t>(0, excess); delayAtUs_ = nowUs; informative = true;
    }
    // The relay's queue toward the client predicts the delay of packets that
    // have not arrived yet (feedback about a growing queue is itself delayed).
    // It drains at least at the virtual link rate (an idle desktop delivers
    // little, which is no capacity measure), consistent with the virtual queue.
    const double drainRate = std::max({recentDelivered(), virtualHeadroom * target_ + audioRate(nowUs), 1e6});
    relayExcessUs_ = std::max<int64_t>(0, int64_t(double(relayQueuedBytes) * 8e6 / drainRate - vqUs_));
    if (relayQueuedBytes >= relayByteLimit && nowUs - lastEmergencyUs_ >= 300000) { emergency(nowUs); return publish(); }
    return decide(nowUs, informative || relayExcessUs_ > overuseExcessUs);
  }
  // udp-media-v1: media changed path, and neither path's delay or delivery says
  // anything about the other (a round trip here, an arrival clock there).
  void pathChanged() {
    buckets_.clear(); delivered_.clear(); written_.clear(); baselineValid_ = false; delayAtUs_ = 0; overuseRuns_ = 0;
    standingDelayUs_ = standingExcessUs_ = relayExcessUs_ = previousExcessUs_ = cutDelayUs_ = 0;
    losses_.clear(); lossSeenUs_ = lossCutUs_ = lossHoldUs_ = 0; lossCutFraction_ = spikeBase_ = 0; lossStuck_ = false;
    lossCeiling_ = 0; lossCeilingAtUs_ = ceilingTryAtUs_ = 0; ceilingWaitUs_ = ceilingWaitFirstUs;
    udpAtUs_ = 0; arrival_[0] = arrival_[1] = {}; restoreRate_ = 0; restoreAtUs_ = restoreFailedUs_ = 0; restoreJumped_ = false;
  }
  // One UDP FEEDBACK: the round trip (0 when it echoed no TAIL) and the datagrams
  // received and given up since the last. TAIL skips the pacer, so its round trip
  // above its own floor is network queue alone, with no virtual queue to subtract.
  // A repaired datagram is no loss here: only what the deadline gave up on counts.
  // arrivedBytes / sentBytes: the wire bytes of the datagrams the client counted, and what the
  // sender put on the wire since the last report (0, 0 from a caller that does not know: then
  // only the counts decide, as before).
  std::optional<int> udpFeedback(uint64_t nowUs, uint64_t rttUs, uint64_t received, uint64_t lost, uint64_t arrivedBytes = 0, uint64_t sentBytes = 0) {
    drain(nowUs);
    lastFeedbackUs_ = nowUs;
    // This report and the one before it: what was sent and what of it arrived.
    arrival_[0] = arrival_[1]; arrival_[1] = {};
    if (const uint64_t interval = udpAtUs_ ? nowUs - udpAtUs_ : 0; (arrivedBytes || sentBytes) && interval >= 10000 && interval <= 5000000) {
      delivered_.push_back({interval, arrivedBytes}); while (delivered_.size() > 5) delivered_.pop_front();
      if (sentBytes >= arrivalMinimumBytes) arrival_[1] = {sentBytes, std::min(arrivedBytes, sentBytes)};
    }
    udpAtUs_ = nowUs;
    const auto shareOf = [](const Arrival& a) { return a.sent ? double(a.arrived) / double(a.sent) : -1.0; };
    const bool told = arrival_[1].sent != 0;
    const double share = told ? double(arrival_[0].arrived + arrival_[1].arrived) / double(arrival_[0].sent + arrival_[1].sent) : -1;
    const bool shortfall = told && arrival_[0].sent && shareOf(arrival_[0]) < arrivalShort && shareOf(arrival_[1]) < arrivalShort;
    const bool blackout = told && share < arrivalBlackout;
    if (rttUs) { baseline(nowUs, int64_t(rttUs)); standingDelayUs_ = standingExcessUs_ = std::max<int64_t>(0, int64_t(rttUs) - baseline_); delayAtUs_ = nowUs; }
    if (lost) lossSeenUs_ = nowUs;
    losses_.push_back({nowUs, received, lost});
    while (nowUs - losses_.front().atUs > lossWindowUs) losses_.pop_front();
    // From the first report with loss on, so clean reports before it do not dilute it, unless
    // that leaves too few datagrams to mean anything.
    uint64_t total = 0, given = 0, all = 0; size_t lossy = 0;
    for (const auto& report : losses_) { all += report.received + report.lost; if (report.lost || given) { total += report.received + report.lost; given += report.lost; lossy += report.lost != 0; } }
    if (total < lossMinimumDatagrams) total = all;
    const double fraction = total >= lossMinimumDatagrams ? double(given) / double(total) : 0;
    // 2 s without loss, or far more of it than the cut was made for: a new episode.
    if (lossStuck_ && (nowUs - lossSeenUs_ >= 2000000 || fraction > 1.25 * lossCutFraction_)) lossStuck_ = false;
    if (!lossy) spikeBase_ = 0;  // the spike left the window
    if (given && (fraction >= lossCut || lossy >= 3) && nowUs >= lossHoldUs_ && !encoderLimited_ && !lossStuck_) {
      const auto cut = [&](double base, double lowest) {
        cutting(true);
        // How far: the share that arrives now where the caller says it, else the share reported lost.
        const double factor = std::clamp(told ? share * arrivalDrain : (1 - fraction) * lossDrain, lowest, 0.95);
        target_ = std::clamp(std::min(target_, base * factor), double(floor_), double(maximum_));
        remember(nowUs, base);
        restoring(nowUs, base);
        lastOveruseUs_ = nowUs; congestionVideo_ = target_; congestionAtUs_ = nowUs;  // probing comes back gently around it
        return publish();
      };
      // One report with loss: a blackout as likely as an overload, so at most 15 % now. A
      // second one decides from the rate before this cut, which is what it still reports.
      // (Two short reports in a row are no blip: that is decided now, like a second report.)
      // Almost nothing arrives: an outage, whatever its reports say. At most that 15 %, once.
      if (blackout || (lossy < 2 && !shortfall)) { if (!spikeBase_) { spikeBase_ = target_; return cut(target_, 0.85); } }
      // Loss at the level a cut did not lower is the path's own (a radio, an outage), not our
      // rate: more cuts would only ratchet down to the floor. It holds the rate (no probing
      // either) until the loss stops. Much more loss than that is a new event.
      else if (!(lossStuck_ = lossCutUs_ && nowUs - lossCutUs_ < 3000000 && fraction >= 0.8 * lossCutFraction_ && fraction <= 1.25 * lossCutFraction_)) {
        const double base = spikeBase_ ? spikeBase_ : target_;
        // The cut shows in what the client reports a window and a round trip later: decide
        // again only on reports after it.
        lossCutUs_ = nowUs; lossCutFraction_ = fraction; losses_.clear(); spikeBase_ = 0;
        lossHoldUs_ = holdUntilUs_ = nowUs + std::max<uint64_t>(lossWindowUs + 2 * (rttUs ? rttUs : 50000), 400000);
        return cut(base, 0.3);
      }
    }
    return decide(nowUs, rttUs != 0);
  }
  // The UDP pacer refused video: the overflow guard of local(), with no written
  // rate to bound it (the broker channel carries only control now).
  std::optional<int> udpBacklog(uint64_t nowUs) {
    drain(nowUs);
    if (nowUs - lastEmergencyUs_ >= 150000) emergency(nowUs);
    return publish();
  }
  // Periodic check without feedback (call at least every second while sending).
  std::optional<int> tick(uint64_t nowUs) {
    drain(nowUs);
    const bool sending = !video_.empty() && nowUs - video_.back().atUs <= staleFeedbackUs;
    const uint64_t silentSince = lastFeedbackUs_ ? lastFeedbackUs_ : firstVideoUs_ ? firstVideoUs_ + 1000000 : 0;
    if (sending && silentSince && nowUs - silentSince > staleFeedbackUs && nowUs - lastStaleUs_ >= 1000000) {
      // Silence measures nothing either way: the rate from before it is kept for a clear report.
      lastStaleUs_ = nowUs; cutting(false); target_ = std::max(double(floor_), target_ * 0.85);
    }
    overshoot(nowUs);
    return publish();
  }

 private:
  struct Sent { uint64_t atUs, pts; uint32_t bytes, vqUs; uint16_t epoch; };
  struct Bytes { uint64_t atUs; size_t bytes; };
  struct Bucket { uint64_t index; int64_t minimum; };
  static void push(std::deque<Bytes>& window, uint64_t nowUs, size_t bytes) { window.push_back({nowUs, bytes}); }
  static double rate(const std::deque<Bytes>& window, uint64_t nowUs, uint64_t spanUs) {
    size_t total = 0; for (auto at = window.rbegin(); at != window.rend() && nowUs - at->atUs < spanUs; ++at) total += at->bytes;
    return double(total) * 8 * 1e6 / double(spanUs);
  }
  double audioRate(uint64_t nowUs) const { return audio_.empty() ? 0 : rate(audio_, nowUs, 2000000); }
  double delivered() const {
    uint64_t interval = 0, bytes = 0; for (const auto& [span, received] : delivered_) { interval += span; bytes += received; }
    return interval >= 200000 ? double(bytes) * 8 * 1e6 / double(interval) : 0;
  }
  // The last ~200 ms: during congestion this is the bottleneck rate now.
  double recentDelivered() const {
    uint64_t interval = 0, bytes = 0;
    for (auto at = delivered_.rbegin(); at != delivered_.rend() && interval < 150000; ++at) { interval += at->first; bytes += at->second; }
    return interval >= 150000 ? double(bytes) * 8 * 1e6 / double(interval) : delivered();
  }
  // Virtual queue: the delay our own sending would see on a link 25 % faster
  // than the video target (plus audio). It drains in real time.
  void drain(uint64_t nowUs) {
    if (vqAtUs_ && nowUs > vqAtUs_) vqUs_ = std::max(0.0, vqUs_ - double(nowUs - vqAtUs_));
    vqAtUs_ = std::max(vqAtUs_, nowUs);
    for (auto* window : {&video_, &audio_, &encoded_, &written_}) while (!window->empty() && nowUs - window->front().atUs > 2000000) window->pop_front();
  }
  // Clock offset + base path delay: 10 s windowed minimum of arrival - enqueue,
  // falling at once, rising at most 2 ms per second (clock drift, route change)
  // so a standing queue is not learned as the baseline.
  void baseline(uint64_t nowUs, int64_t raw) {
    const uint64_t index = nowUs / 1000000;
    if (buckets_.empty() || buckets_.back().index != index) buckets_.push_back({index, raw}); else buckets_.back().minimum = std::min(buckets_.back().minimum, raw);
    while (buckets_.front().index + 10 < index) buckets_.pop_front();
    int64_t window = std::numeric_limits<int64_t>::max(); for (const auto& bucket : buckets_) window = std::min(window, bucket.minimum);
    if (!baselineValid_ || window <= baseline_) baseline_ = window;
    else baseline_ = std::min(window, baseline_ + int64_t(double(nowUs - baselineAtUs_) * 0.002));
    baselineValid_ = true; baselineAtUs_ = nowUs;
  }
  // Application-limited: a mostly idle desktop sends far below the target.
  bool limited(uint64_t nowUs) const { return rate(video_, nowUs, 500000) < 0.7 * target_; }
  // Called before every cut. One made while application-limited changes nothing
  // that is sent and measures no capacity, so the rate from before the first
  // such cut is kept: a clear path gives it back (decide), also to a desktop
  // that stays idle and therefore never probes. In the field an idle desktop
  // that sat through three minutes of stalls came back at 2.4 of 17.5 Mbps.
  // A cut with evidence (loss, a queue our own sending filled) ends that.
  void cutting(bool evidence) { idleRestore_ = evidence ? 0 : std::max(idleRestore_, target_); }
  // A loss cut at `lost` (the rate that lost): the same limit again waits twice as long
  // for its next try, a new one starts over.
  void remember(uint64_t nowUs, double lost) {
    const bool again = lossCeiling_ > 0 && nowUs - lossCeilingAtUs_ < ceilingMemoryUs && lost <= ceilingRepeat * lossCeiling_ && lost * ceilingRepeat >= lossCeiling_;
    ceilingWaitUs_ = again ? std::min(2 * ceilingWaitUs_, ceilingWaitLongestUs) : ceilingWaitFirstUs;
    lossCeiling_ = lost; lossCeilingAtUs_ = nowUs; ceilingTryAtUs_ = nowUs + ceilingWaitUs_;
  }
  // After every cut: one below restoreDeep of the rate before it arms the way back (decide); one
  // made while that way back was being taken ends it.
  void restoring(uint64_t nowUs, double before) {
    if (restoreRate_ > 0 && restoreJumped_) { restoreRate_ = 0; restoreFailedUs_ = nowUs; return; }
    if (target_ >= restoreDeep * before || (restoreFailedUs_ && nowUs - restoreFailedUs_ < restoreAgainUs)) return;
    restoreRate_ = std::max(restoreRate_, before); restoreAtUs_ = nowUs; restoreJumped_ = false;
  }
  // Overflow guard: at most half, and below what the backed-up leg carries.
  void emergency(uint64_t nowUs, double carried = 0) {
    lastEmergencyUs_ = lastOveruseUs_ = nowUs; holdUntilUs_ = nowUs + 500000;
    // An encoder already emitting far above its target would not follow.
    if (encoderLimited_) return;
    cutting(!limited(nowUs));
    double next = target_ * 0.5;
    if (carried > 0) next = std::min(next, 0.8 * carried - audioRate(nowUs));
    target_ = std::clamp(next, double(floor_), double(maximum_));
  }
  // The encoder emitting far above its target cannot be slowed by cutting it
  // further (content at the MFT's quantiser limit); cuts would only push it
  // toward the measured degenerate state. Hold and follow its real output.
  void overshoot(uint64_t nowUs) {
    if (!firstVideoUs_ || nowUs - firstVideoUs_ < 2000000) return;
    const double output = rate(encoded_, nowUs, 2000000);
    if (output > 1.5 * target_ && target_ < maximum_) {
      if (!overshootSinceUs_) overshootSinceUs_ = nowUs;
      if (nowUs - overshootSinceUs_ >= 2000000) { encoderLimited_ = true; limitedUntilUs_ = nowUs + 5000000; target_ = std::min(double(maximum_), std::max(target_, output / 1.3)); overshootSinceUs_ = nowUs; }
    } else {
      overshootSinceUs_ = 0;
      if (encoderLimited_ && nowUs >= limitedUntilUs_ && output < 1.2 * target_) encoderLimited_ = false;
    }
  }
  std::optional<int> decide(uint64_t nowUs, bool informative) {
    overshoot(nowUs);
    if (!informative) return publish();  // no delay evidence this interval: hold
    const int64_t excess = std::max(standingExcessUs_, relayExcessUs_);
    // Congestion grows the queue interval over interval; a stall (Wi-Fi scan,
    // retransmission) leaves one that already shrinks and drains on its own.
    const bool growing = excess * 10 >= previousExcessUs_ * 9;
    previousExcessUs_ = excess;
    const bool over = excess > overuseExcessUs && growing;
    overuseRuns_ = over ? overuseRuns_ + 1 : 0;
    if (excess <= overuseExcessUs) { cutDelayUs_ = 0; limitedPending_ = false; }
    // Spurious-cut undo: a link stall looks like a capacity drop for its first
    // ~150 ms. If, right after the cut, the path delivers MORE than the previous
    // send rate while the queue shrinks, the cut was not needed: restore it.
    if (undoUntilUs_ && nowUs >= undoUntilUs_) undoUntilUs_ = 0;
    // A cut made while application-limited never changed what we send, so a
    // queue that drains anyway was not ours (a stall, another flow): undo it.
    const bool undo = limitedCut_ ? excess <= overuseExcessUs : recentDelivered() >= 1.1 * (undoTarget_ + audioRate(nowUs));
    if (undoUntilUs_ && !growing && undo) {
      target_ = undoTarget_; congestionVideo_ = undoCongestion_; congestionAtUs_ = undoCongestionAtUs_;
      if (target_ >= idleRestore_) idleRestore_ = 0;
      undoUntilUs_ = 0; cutDelayUs_ = 0; overuseRuns_ = 0; holdUntilUs_ = nowUs + 200000;
      return publish();
    }
    // Two consecutive growing intervals; an overflow-sized queue on one.
    if ((overuseRuns_ >= 2 || (over && excess > burstExcessUs)) && nowUs >= holdUntilUs_) {
      lastOveruseUs_ = nowUs; overuseRuns_ = 0;
      if (restoreRate_ > 0 && restoreJumped_) { restoreRate_ = 0; restoreFailedUs_ = nowUs; }
      // After a cut the queue drains for a while; cut again only if it is not
      // shrinking (one reaction per congestion event, not per interval).
      if (cutDelayUs_ && excess < cutDelayUs_ * 85 / 100) { cutDelayUs_ = excess; holdUntilUs_ = nowUs + 200000; return publish(); }
      if (!encoderLimited_) {
        // Application-limited (an idle desktop sends far below the target):
        // the path delivered only what we sent, which is no capacity measure;
        // cutting to it dropped a stream to the geometry floor (2.7 Mbps) after
        // one delay spike. Step down instead: a real bottleneck
        // below even this rate keeps cutting until we are no longer limited.
        // A queue that drains anyway restores the rate before the series.
        limitedCut_ = limited(nowUs);
        cutting(!limitedCut_);
        if (limitedCut_ && !limitedPending_) limitedRestore_ = target_;
        limitedPending_ = limitedCut_;
        undoTarget_ = limitedCut_ ? limitedRestore_ : target_; undoCongestion_ = congestionVideo_; undoCongestionAtUs_ = congestionAtUs_; undoUntilUs_ = nowUs + (limitedCut_ ? 3000000 : 1000000);
        // A growing queue: the last ~200 ms delivered is the bottleneck rate,
        // unless a stall emptied it (then at most half the ~500 ms rate).
        const double audio = audioRate(nowUs), recent = recentDelivered(), capacity = std::max(recent, 0.5 * delivered());
        // Below the bottleneck rate by enough to drain the unexplained queue in ~1 s.
        const double drainFactor = std::clamp(1 - double(excess) / 1000000, 0.4, 0.95);
        double next = target_ * 0.85;
        if (capacity > 0 && !limitedCut_) next = std::min(next, capacity * drainFactor - audio);
        target_ = std::clamp(next, double(floor_), double(maximum_));
        if (capacity > 0 && !limitedCut_ && recent >= 0.5 * delivered()) { congestionVideo_ = std::max(double(floor_), capacity - audio); congestionAtUs_ = nowUs; }
      }
      cutDelayUs_ = std::max<int64_t>(excess, 1);
      holdUntilUs_ = nowUs + uint64_t(std::clamp(double(excess), 300000.0, 1000000.0));
      return publish();
    }
    const uint64_t elapsed = lastProbeUs_ ? std::min<uint64_t>(200000, nowUs - lastProbeUs_) : 100000;
    lastProbeUs_ = nowUs;
    // UDP: any datagram given up in the last second is no room to probe into.
    const bool lossy = lossSeenUs_ && nowUs - lossSeenUs_ < 1000000;
    if (excess > overuseExcessUs || nowUs < holdUntilUs_ || nowUs - lastOveruseUs_ < stableUs || nowUs < burstUntilUs_ || encoderLimited_ || excess >= probeExcessUs || lossy) return publish();
    // The path is clear here. Cuts made while application-limited took a rate there was no
    // evidence against (cutting): it is back at once, before the next picture needs it.
    if (idleRestore_ > target_) { target_ = idleRestore_; idleRestore_ = 0; return publish(); }
    const double used = rate(video_, nowUs, 500000);
    if (used < 0.7 * target_) return publish();  // application-limited: no evidence above it
    // The way back after a deep cut (restoring): the path is clear here, so it was a dip unless the try says otherwise.
    if (restoreRate_ > 0 && (nowUs - restoreAtUs_ > restoreMemoryUs || target_ >= restoreShare * restoreRate_)) restoreRate_ = 0;
    if (restoreRate_ > 0) {
      const double step = target_ * std::pow(restoreStep, double(elapsed) / 100000);
      target_ = std::min({double(maximum_), restoreShare * restoreRate_, std::max(step, restoreJumped_ ? 0.0 : 0.5 * restoreRate_)});
      restoreJumped_ = true;
      if (lossCeiling_ > 0 && target_ >= ceilingPassed * lossCeiling_) { lossCeiling_ = 0; ceilingWaitUs_ = ceilingWaitFirstUs; }  // a limit the episode's later cuts left under it
      if (target_ >= idleRestore_) idleRestore_ = 0;
      return publish();
    }
    // Gentle only around the last congestion rate; past it the path changed.
    bool gentle = congestionVideo_ > 0 && nowUs - congestionAtUs_ < 30000000 && target_ >= 0.9 * congestionVideo_ && target_ <= 1.15 * congestionVideo_;
    // The rate that lost (remember): below it until the next try, then gently past it.
    if (lossCeiling_ > 0 && nowUs - lossCeilingAtUs_ >= ceilingMemoryUs) { lossCeiling_ = 0; ceilingWaitUs_ = ceilingWaitFirstUs; }
    double cap = 0;
    if (lossCeiling_ > 0) {
      if (nowUs < ceilingTryAtUs_) cap = ceilingMargin * lossCeiling_;
      else if (target_ >= ceilingMargin * lossCeiling_) gentle = true;
    }
    double next = target_ * std::pow(gentle ? nearProbeStep : probeStep, double(elapsed) / 100000);
    if (cap > 0) next = std::min(next, std::max(cap, target_));  // holds, never lowers
    target_ = std::min({double(maximum_), next, std::max(target_, 1.5 * used)});
    // A try that passed it by 5 % without loss: the path carries more now; the next limit is new.
    if (lossCeiling_ > 0 && target_ >= ceilingPassed * lossCeiling_) { lossCeiling_ = target_; ceilingWaitUs_ = ceilingWaitFirstUs; ceilingTryAtUs_ = nowUs; }
    if (target_ >= idleRestore_) idleRestore_ = 0;
    return publish();
  }  std::optional<int> publish(bool force = false) {
    const int value = int(std::lround(target_));
    const bool boundary = (value == floor_ || value == maximum_) && value != published_;
    if (value != published_ && (force || boundary || std::abs(value - published_) >= published_ / 50)) { published_ = value; return value; }
    return std::nullopt;
  }
  int maximum_, floor_, fps_, published_ = 0;
  double target_ = 0, vqUs_ = 0, congestionVideo_ = 0;
  uint64_t vqAtUs_ = 0, firstVideoUs_ = 0, lastFeedbackUs_ = 0, delayAtUs_ = 0, baselineAtUs_ = 0;
  uint64_t holdUntilUs_ = 0, lastOveruseUs_ = 0, lastProbeUs_ = 0, lastEmergencyUs_ = 0, lastStaleUs_ = 0, burstUntilUs_ = 0;
  uint64_t congestionAtUs_ = 0, overshootSinceUs_ = 0, limitedUntilUs_ = 0, undoUntilUs_ = 0, undoCongestionAtUs_ = 0;
  struct Loss { uint64_t atUs, received, lost; };
  std::deque<Loss> losses_;  // UDP FEEDBACK of the last lossWindowUs
  uint64_t lossSeenUs_ = 0, lossCutUs_ = 0, lossHoldUs_ = 0; double lossCutFraction_ = 0, spikeBase_ = 0; bool lossStuck_ = false;
  double lossCeiling_ = 0; uint64_t lossCeilingAtUs_ = 0, ceilingTryAtUs_ = 0, ceilingWaitUs_ = ceilingWaitFirstUs;
  double restoreRate_ = 0; uint64_t restoreAtUs_ = 0, restoreFailedUs_ = 0; bool restoreJumped_ = false;  // restoring
  struct Arrival { uint64_t sent = 0, arrived = 0; };
  uint64_t udpAtUs_ = 0; Arrival arrival_[2];  // udpFeedback: the report before this one, and this one
  double undoTarget_ = 0, undoCongestion_ = 0, limitedRestore_ = 0, idleRestore_ = 0;
  int64_t standingDelayUs_ = 0, standingExcessUs_ = 0, relayExcessUs_ = 0, previousExcessUs_ = 0, cutDelayUs_ = 0, baseline_ = 0;
  int overuseRuns_ = 0;
  bool baselineValid_ = false, encoderLimited_ = false, limitedCut_ = false, limitedPending_ = false;
  std::deque<Sent> sent_;
  std::deque<Bytes> video_, audio_, encoded_, written_;
  std::deque<std::pair<uint64_t, uint64_t>> delivered_;
  std::deque<Bucket> buckets_;
};
}
