// The client's audio jitter buffer (windows/runner/audio_jitter.h) against
// simulated arrival patterns: no relay, no audio device, no sound. A virtual
// device plays 48 frames per millisecond of this PC's clock; the Host sends a
// 10 ms packet per 10 ms of the VM's audio clock, delayed as each case says.
// The render thread is modelled as RemoteAudio runs it: woken by every packet
// and at least every 15 ms (Windows' default timer granularity).
#include "audio_jitter.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using multitap::AudioJitterBuffer;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
// What each case measured, printed with the result.
std::string measured;
void note(const std::string& name, double value) { char text[64]; std::snprintf(text, sizeof(text), ",\"%s\":%.1f", name.c_str(), value); measured += text; }
constexpr double kPi = 3.14159265358979323846, kToneLevel = 16000;

struct Sim {
  AudioJitterBuffer buffer;
  uint32_t capacity = 4800;          // the device buffer, 100 ms
  double device = 0;                 // frames the device still holds
  uint64_t now = 0;                  // us, this PC
  uint64_t nextWake = 0;
  bool renderStalled = false;        // the render thread does not run
  uint64_t dryMs = 0;                // device empty after the first write, while the VM was not silent
  bool everWritten = false;
  std::function<bool(uint64_t)> silentAt = [](uint64_t) { return false; };
  std::vector<uint8_t> scratch = std::vector<uint8_t>(4800 * 4);
  std::vector<double> levels;        // ms after each write while playing
  uint64_t over100Ms = 0;            // ms with more than 100 ms waiting while playing
  // A tone instead of a constant: the left channel its sine and the right its
  // cosine, so every frame the device gets shows where in the tone it was read.
  double toneHz = 0;
  std::vector<int16_t> played;       // tone only: what the device got
  struct Arrival { uint64_t at, pts; };
  std::vector<Arrival> arrivals; size_t next = 0;

  // Packet n carries VM audio [n*10, n*10+10) ms; it is sent when complete,
  // `drift` faster on the VM's clock, plus `delay(n)` ms on the way here.
  void schedule(uint64_t packets, double drift, const std::function<double(uint64_t)>& delay, const std::function<bool(uint64_t)>& skip) {
    uint64_t last = 0;
    for (uint64_t n = 0; n < packets; ++n) {
      if (skip(n)) continue;
      uint64_t at = uint64_t(std::llround((double(n + 1) * 10000.0) / (1.0 + drift) + delay(n) * 1000.0));
      at = std::max(at, last); last = at;  // one TCP/UDP stream: in order
      arrivals.push_back({at, n * 10000});
    }
  }
  void wake() {
    if (renderStalled) return;
    const uint32_t padding = uint32_t(device);
    const uint32_t written = buffer.Pull(scratch.data(), capacity - padding, padding, now);
    device += written; everWritten = everWritten || written;
    if (written && buffer.playing()) levels.push_back(double(buffer.level()) / 48.0);
    if (toneHz > 0) { const size_t at = played.size(); played.resize(at + size_t(written) * 2); std::memcpy(played.data() + at, scratch.data(), size_t(written) * 4); }
  }
  void run(uint64_t untilMs, const std::function<void(Sim&)>& tick = nullptr) {
    std::vector<uint8_t> packet(1920, 1);
    while (now < untilMs * 1000) {
      now += 1000;
      device = std::max(0.0, device - 48.0);
      if (everWritten && device == 0 && !silentAt(now)) ++dryMs;
      bool pushed = false;
      while (next < arrivals.size() && arrivals[next].at <= now) {
        const uint64_t pts = arrivals[next].pts;
        if (toneHz > 0) for (int k = 0; k < 480; ++k) {
          const double angle = 2 * kPi * toneHz * double(pts / 10000 * 480 + uint64_t(k)) / 48000.0;
          const int16_t frame[2] = {int16_t(std::lround(kToneLevel * std::sin(angle))), int16_t(std::lround(kToneLevel * std::cos(angle)))};
          std::memcpy(&packet[size_t(k) * 4], frame, 4);
        }
        buffer.Arrived(pts, now);
        require(buffer.Push(pts, packet.data(), packet.size(), now), "A valid packet was refused");
        ++next; pushed = true;
      }
      if (tick) tick(*this);
      if (pushed || now >= nextWake) { wake(); nextWake = now + 15000; }
      if (buffer.playing() && buffer.level() > AudioJitterBuffer::Ms(100)) ++over100Ms;
    }
  }
  double meanLevel(size_t last) const {
    if (levels.empty()) return 0;
    const size_t from = levels.size() > last ? levels.size() - last : 0; double sum = 0;
    for (size_t i = from; i < levels.size(); ++i) sum += levels[i];
    return sum / double(levels.size() - from);
  }
  double targetMs() const { return buffer.target() / 48.0; }
};
auto none = [](uint64_t) { return 0.0; };
auto all = [](uint64_t) { return false; };
// A stall of `ms` that starts at `atMs`: what was sent in it arrives together at its end.
double stalled(uint64_t n, double atMs, double ms) { const double sent = double(n + 1) * 10; return sent >= atMs && sent < atMs + ms ? atMs + ms - sent : 0.0; }

int steady() {
  Sim sim; sim.toneHz = 1000; sim.schedule(3000, 0, none, all); sim.run(30000);
  const auto& c = sim.buffer.counters();
  require(c.underruns == 0 && c.overruns == 0 && c.silences == 0 && sim.dryMs == 0, "Steady audio had an underrun, overrun or gap");
  // Nothing was late: the target is the floor from the first packet to the last.
  require(sim.buffer.target() == AudioJitterBuffer::Ms(40), "A steady stream moved the target: " + std::to_string(sim.targetMs()));
  const double level = sim.meanLevel(500);
  require(level >= 30 && level <= 44, "Steady depth is not at the target: " + std::to_string(level));
  require(c.inserted_frames == 0 && c.trimmed_frames == 0 && c.uncovered == 0, "A steady stream was corrected: " + std::to_string(c.inserted_frames) + " inserted, " + std::to_string(c.trimmed_frames) + " trimmed");
  // Untouched audio is the received audio, bit for bit.
  require(sim.played.size() > 48000 * 2 * 25, "Too little was played");
  for (size_t k = 0; k < sim.played.size() / 2; ++k) {
    const double angle = 2 * kPi * 1000 * double(k) / 48000.0;
    require(sim.played[2 * k] == int16_t(std::lround(kToneLevel * std::sin(angle))) && sim.played[2 * k + 1] == int16_t(std::lround(kToneLevel * std::cos(angle))), "Untouched audio changed at frame " + std::to_string(k));
  }
  note("steadyDepthMs", level); note("steadyTargetMs", sim.targetMs());
  return 1;
}

int stalls() {
  // A 120 ms stall every 3 s (the packets then arrive together, PTS continuous).
  Sim sim; std::vector<uint64_t> underrunAt;
  sim.schedule(3000, 0, [](uint64_t n) { const uint64_t ms = (n + 1) * 10 % 3000; return ms > 1500 && ms <= 1620 ? double(1620 - ms) : 0.0; }, all);
  uint64_t seen = 0;
  sim.run(30000, [&](Sim& s) { if (s.buffer.counters().underruns != seen) { seen = s.buffer.counters().underruns; underrunAt.push_back(s.now); } });
  const auto& c = sim.buffer.counters();
  // The first stall is measured and the next ones find the depth it needed.
  require(c.underruns == 1 && underrunAt.back() < 2000000, "A stall that repeats ran the device dry again: " + std::to_string(c.underruns) + " underruns, the last at " + std::to_string(underrunAt.empty() ? 0 : underrunAt.back() / 1000) + " ms");
  require(sim.targetMs() == 140, "The target is not the lateness (110 ms) and the margin: " + std::to_string(sim.targetMs()));
  require(c.silences == 0 && c.overruns == 0, "A late network was read as silence, or what it brought was dropped");
  note("stallUnderruns", double(c.underruns)); note("stallTargetMs", sim.targetMs()); note("stallDryMs", double(sim.dryMs));
  return 1;
}

int episode() {
  // The field picture (2026-11-24, a game over Wi-Fi): a calm minute, three
  // seconds of radio pauses of 150 ms, two calm minutes.
  Sim sim; uint64_t backAt = 0;
  sim.schedule(18300, 0, [](uint64_t n) { double late = 0; for (int k = 0; k < 5; ++k) late = std::max(late, stalled(n, 60000 + k * 600, 150)); return late; }, all);
  sim.run(183000, [&](Sim& s) { if (s.now > 63000000 && !backAt && s.buffer.target() == AudioJitterBuffer::Ms(40) && s.buffer.level() < AudioJitterBuffer::Ms(60)) backAt = s.now; });
  const auto& c = sim.buffer.counters();
  // The first pause runs the device dry; the others of the episode do not.
  require(c.underruns == 1 && c.overruns == 0, "An episode of pauses: underruns " + std::to_string(c.underruns) + ", overruns " + std::to_string(c.overruns));
  // Fifteen seconds remembered, then down as fast as playing 1 % fast allows:
  // the stepped rule before this one took 5.7 minutes from 200 ms.
  require(backAt && backAt < 63000000 + 40000000, "The sound did not come back to the picture within 40 s of the episode: " + std::to_string(backAt / 1000) + " ms");
  require(sim.over100Ms < 35000, "More than 100 ms behind for " + std::to_string(sim.over100Ms) + " ms");
  require(c.trimmed_frames > AudioJitterBuffer::Ms(100) && sim.dryMs < 200, "The level did not come down by playing fast, or the device ran dry for " + std::to_string(sim.dryMs) + " ms");
  note("episodeBackAfterMs", double(backAt / 1000 - 63000)); note("episodeOver100Ms", double(sim.over100Ms)); note("episodeDryMs", double(sim.dryMs));
  return 1;
}

int uncovered() {
  // One 400 ms stall: no depth allowed covers it, so nothing is kept for it,
  // not for the packets held up behind it either (they come 390, 380 ... ms
  // late). What it brought is dropped down to the target at once.
  Sim sim; sim.schedule(6000, 0, [](uint64_t n) { return stalled(n, 20000, 400); }, all);
  uint32_t most = 0; sim.run(60000, [&](Sim& s) { most = std::max(most, s.buffer.target()); });
  const auto& c = sim.buffer.counters();
  require(most == AudioJitterBuffer::Ms(40), "A stall no depth covers raised the target to " + std::to_string(most / 48));
  require(c.uncovered >= 1 && c.overruns >= 1 && c.underruns == 1, "A 400 ms stall: uncovered " + std::to_string(c.uncovered) + ", overruns " + std::to_string(c.overruns) + ", underruns " + std::to_string(c.underruns));
  require(sim.over100Ms == 0, "Sound stayed more than 100 ms behind after a stall no depth covers");
  return 1;
}

int remembered() {
  // A 40 ms stall every 30 s: a depth the picture does not show (70 ms) is
  // kept for two minutes, so only the first one is heard. Then nothing for
  // three minutes: back to the floor.
  Sim sim; sim.schedule(36000, 0, [](uint64_t n) { double late = 0; for (int k = 0; k < 6; ++k) late = std::max(late, stalled(n, 20000 + k * 30000, 40)); return late; }, all);
  uint64_t underrunsAt180 = 0; uint32_t targetAt165 = 0;
  sim.run(360000, [&](Sim& s) { if (s.now == 165000000) targetAt165 = s.buffer.target(); if (s.now == 180000000) underrunsAt180 = s.buffer.counters().underruns; });
  require(underrunsAt180 <= 1 && sim.buffer.counters().underruns == underrunsAt180, "A small stall that repeats each half minute kept running the device dry: " + std::to_string(sim.buffer.counters().underruns));
  require(targetAt165 == AudioJitterBuffer::Ms(70), "The depth of a small stall was not kept: " + std::to_string(targetAt165 / 48));
  require(sim.buffer.target() == AudioJitterBuffer::Ms(40), "The target did not come back after three calm minutes: " + std::to_string(sim.targetMs()));
  require(sim.over100Ms == 0, "A small stall put the sound more than 100 ms behind");
  return 1;
}

int silence() {
  // 2 s sound, 1 s nothing (the VM is silent), 2 s, 1 s nothing, 2 s.
  const auto quiet = [](uint64_t n) { return (n >= 200 && n < 300) || (n >= 500 && n < 600); };
  Sim sim; sim.schedule(800, 0, none, quiet);
  sim.silentAt = [](uint64_t us) { const uint64_t ms = us / 1000; return (ms >= 2000 && ms < 3100) || (ms >= 5000 && ms < 6100); };
  sim.run(8000);
  const auto& c = sim.buffer.counters();
  require(c.silences == 2 && c.underruns == 0, "A silent VM was counted as underruns: " + std::to_string(c.underruns) + "/" + std::to_string(c.silences));
  require(sim.buffer.target() == AudioJitterBuffer::Ms(40) && c.uncovered == 0, "Silence changed the target");
  return 1;
}

int renderStall() {
  // This PC's render thread stops for 400 ms; packets keep coming.
  Sim sim; sim.schedule(600, 0, none, all); double after = 0; bool checked = false;
  sim.run(6000, [&](Sim& s) {
    s.renderStalled = s.now >= 3000000 && s.now < 3400000;
    if (s.now == 3500000) { after = s.buffer.level() / 48.0; checked = true; }
  });
  const auto& c = sim.buffer.counters();
  require(c.overruns >= 1 && c.overrun_frames > 0, "A 400 ms backlog was kept instead of dropped");
  require(checked && after <= sim.targetMs() + 40 + 1, "Latency stayed high after the backlog: " + std::to_string(after) + " ms");
  require(c.underruns == 1 && c.uncovered == 1 && sim.targetMs() == 40, "The dry device after the stall: underruns " + std::to_string(c.underruns) + ", target " + std::to_string(sim.targetMs()));
  note("renderStallDroppedMs", c.overrun_frames / 48.0); note("renderStallDepthAfterMs", after);
  // A stall a depth can cover (70 ms), twice: the second finds that depth.
  Sim small; small.schedule(1200, 0, none, all);
  small.run(12000, [&](Sim& s) { s.renderStalled = (s.now >= 3000000 && s.now < 3070000) || (s.now >= 8000000 && s.now < 8070000); });
  require(small.buffer.counters().underruns == 1 && small.targetMs() >= 70 && small.targetMs() <= 90, "A render thread that stops 70 ms: underruns " + std::to_string(small.buffer.counters().underruns) + ", target " + std::to_string(small.targetMs()));
  note("renderStall70TargetMs", small.targetMs());
  return 1;
}

int drift(double rate) {
  Sim sim; sim.schedule(6000, rate, none, all); sim.run(60000);
  const auto& c = sim.buffer.counters();
  const double expected = std::abs(rate) * 48000 * 60;
  const double corrected = double(rate > 0 ? c.trimmed_frames : c.inserted_frames) - double(rate > 0 ? c.inserted_frames : c.trimmed_frames);
  require(c.underruns == 0 && c.overruns == 0, "Clock drift " + std::to_string(rate) + " was not absorbed: underruns " + std::to_string(c.underruns) + ", overruns " + std::to_string(c.overruns));
  require(corrected >= expected * 0.6 && corrected <= expected * 1.4, "Drift correction " + std::to_string(corrected) + " frames, expected about " + std::to_string(expected));
  const double level = sim.meanLevel(300);
  require(level >= sim.targetMs() - 12 && level <= sim.targetMs() + 6, "Drift moved the depth: " + std::to_string(level) + " ms for target " + std::to_string(sim.targetMs()));
  // A slow VM clock reads as packets coming later and later against the floor
  // of ten seconds ago: 3 ms a second here, thirty times a real clock.
  require(sim.targetMs() <= (rate > 0 ? 40 : 75), "Drift grew the target: " + std::to_string(sim.targetMs()));
  note(rate > 0 ? "fastDriftTrimmed" : "slowDriftInserted", corrected); note(rate > 0 ? "fastDriftDepthMs" : "slowDriftDepthMs", level);
  return 1;
}

int resampled(double hz, double rate) {
  // A tone while the level is corrected the whole time (a clock 0.4 % off).
  // Each frame played shows where in the tone it was read (sine left, cosine
  // right): the read position moves 0.99 to 1.01 frames for each frame played,
  // never a whole frame at once (that was the 240 Hz buzz), and the tone keeps
  // its level.
  Sim sim; sim.toneHz = hz; sim.schedule(4000, rate, none, all); sim.run(40000);
  const auto& c = sim.buffer.counters();
  require(c.underruns == 0 && c.overruns == 0, "The tone run had an underrun or an overrun");
  require((rate > 0 ? c.trimmed_frames : c.inserted_frames) > 5000, "Too few corrections to judge: " + std::to_string(c.trimmed_frames) + " / " + std::to_string(c.inserted_frames));
  const double step = 2 * kPi * hz / 48000.0; double worstStep = 0, worstLevel = 0; size_t passed = 0;
  // Not the first second (priming) nor the last half (the fixture ends there: a slip is given up).
  for (size_t k = 48000; k + 24000 < sim.played.size() / 2; ++k) {
    const double a = std::atan2(double(sim.played[2 * k]), double(sim.played[2 * k + 1])), b = std::atan2(double(sim.played[2 * k + 2]), double(sim.played[2 * k + 3]));
    double moved = b - a; while (moved < -kPi) moved += 2 * kPi; while (moved > kPi) moved -= 2 * kPi;
    worstStep = std::max(worstStep, std::abs(moved / step - 1));
    worstLevel = std::max(worstLevel, std::abs(std::hypot(double(sim.played[2 * k]), double(sim.played[2 * k + 1])) / kToneLevel - 1));
    if (std::abs(moved / step - 1) < 0.0005) ++passed;
  }
  require(worstStep <= 0.012, "The read position jumped: a frame played moved it " + std::to_string(1 + worstStep) + " frames at " + std::to_string(hz) + " Hz");
  require(worstLevel <= 0.003, "A corrected tone changed its level by " + std::to_string(worstLevel * 100) + " % at " + std::to_string(hz) + " Hz");
  char name[48]; std::snprintf(name, sizeof(name), "tone%.0fHz%sWorstStepPermille", hz, rate > 0 ? "Fast" : "Slow"); note(name, worstStep * 1000);
  std::snprintf(name, sizeof(name), "tone%.0fHz%sWorstLevelPermille", hz, rate > 0 ? "Fast" : "Slow"); note(name, worstLevel * 1000);
  return 1;
}

int jitter() {
  std::mt19937 random(7); std::uniform_real_distribution<double> spread(0, 25);
  std::vector<double> delays(3000); for (auto& d : delays) d = spread(random);
  Sim sim; sim.schedule(3000, 0, [&](uint64_t n) { return delays[n]; }, all); sim.run(30000);
  const auto& c = sim.buffer.counters();
  require(c.underruns == 0 && sim.targetMs() == 55, "25 ms of jitter: " + std::to_string(c.underruns) + " underruns, target " + std::to_string(sim.targetMs()));
  require(sim.dryMs == 0, "25 ms of jitter left audible gaps: " + std::to_string(sim.dryMs) + " ms");
  note("jitterUnderruns", double(c.underruns)); note("jitterDryMs", double(sim.dryMs)); note("jitterTargetMs", sim.targetMs()); note("jitterDepthMs", sim.meanLevel(500));
  return 1;
}

int playingPts() {
  Sim sim; sim.schedule(1000, 0, none, all); int samples = 0; int64_t previous = 0;
  sim.run(9000, [&](Sim& s) {
    if (s.now < 2000000 || s.now % 97000) return;
    const auto pts = s.buffer.PlayingPtsUs(s.now);
    require(pts.has_value(), "No play head while playing");
    // The packet that ends at local time t holds VM audio up to t: the play
    // head is that far back by the device's and the buffer's depth.
    const int64_t expected = int64_t(s.now) - int64_t(s.device * 1000000 / 48000) - int64_t(s.buffer.queued()) * 1000000 / 48000;
    require(std::llabs(*pts - expected) <= 16000, "Play head " + std::to_string(*pts) + " differs from " + std::to_string(expected));
    require(*pts >= previous, "The play head went back"); previous = *pts; ++samples;
  });
  require(samples > 50, "Too few play-head samples");
  AudioJitterBuffer idle; require(!idle.PlayingPtsUs(0), "A play head before any audio");
  return 1;
}

int clocks() {
  // The Host's capture starts over under the same stream: its PTS begin again
  // at 0 while this PC's clock goes on. That is another clock, not five
  // seconds of lateness: nothing is measured from it.
  AudioJitterBuffer buffer; const std::vector<uint8_t> packet(1920, 1);
  for (uint64_t n = 0; n < 500; ++n) buffer.Arrived(n * 10000, 1000000 + n * 10000);
  for (uint64_t n = 0; n < 500; ++n) buffer.Arrived(n * 10000, 6000000 + n * 10000);
  require(buffer.target() == AudioJitterBuffer::Ms(40) && buffer.counters().uncovered == 0, "A PTS that started over was read as lateness: target " + std::to_string(buffer.target() / 48));
  // And one packet 20 ms late on the new clock is 20 ms late.
  buffer.Arrived(5000000, 11000000 + 20000);
  require(buffer.target() == AudioJitterBuffer::Ms(50), "20 ms late after the new clock: target " + std::to_string(buffer.target() / 48));
  return 1;
}

int malformed() {
  AudioJitterBuffer buffer; const std::vector<uint8_t> bytes(AudioJitterBuffer::kMaxPacketBytes + 4, 0);
  for (const size_t bad : {size_t(0), size_t(3), size_t(6), AudioJitterBuffer::kMaxPacketBytes + 4}) require(!buffer.Push(0, bytes.data(), bad, 0), "Malformed PCM accepted");
  require(buffer.queued() == 0 && buffer.Push(0, bytes.data(), AudioJitterBuffer::kMaxPacketBytes, 0), "The largest valid packet");
  // One 200 ms packet is beyond target + headroom: kept down to the target, newest last.
  require(buffer.counters().overruns == 1 && buffer.queued() == AudioJitterBuffer::Ms(40), "An oversized backlog was not cut to the target");
  return 1;
}
}

int main() {
  try {
    int cases = 0;
    cases += steady(); cases += stalls(); cases += episode(); cases += uncovered(); cases += remembered(); cases += silence(); cases += renderStall();
    cases += drift(0.003); cases += drift(-0.003);
    for (const double hz : {1000.0, 7000.0}) { cases += resampled(hz, 0.004); cases += resampled(hz, -0.004); }
    cases += jitter(); cases += playingPts(); cases += clocks(); cases += malformed();
    std::cout << "{\"kind\":\"audio-jitter\",\"cases\":" << cases << measured << ",\"audioDevice\":false,\"network\":false}" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << std::endl;
    return 1;
  }
}
