// The client's audio jitter buffer (windows/runner/audio_jitter.h) against
// simulated arrival patterns: no relay, no audio device, no sound. A virtual
// device plays 48 frames per millisecond of this PC's clock; the Host sends a
// 10 ms packet per 10 ms of the VM's audio clock, delayed as each case says.
// The render thread is modelled as RemoteAudio runs it: woken by every packet
// and at least every 15 ms (Windows' default timer granularity).
#include "audio_jitter.h"
#include <cmath>
#include <cstdio>
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
  }
  void run(uint64_t untilMs, const std::function<void(Sim&)>& tick = nullptr) {
    const std::vector<uint8_t> packet(1920, 1);
    while (now < untilMs * 1000) {
      now += 1000;
      device = std::max(0.0, device - 48.0);
      if (everWritten && device == 0 && !silentAt(now)) ++dryMs;
      bool pushed = false;
      while (next < arrivals.size() && arrivals[next].at <= now) {
        require(buffer.Push(arrivals[next].pts, packet.data(), packet.size(), now), "A valid packet was refused");
        ++next; pushed = true;
      }
      if (tick) tick(*this);
      if (pushed || now >= nextWake) { wake(); nextWake = now + 15000; }
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

int steady() {
  Sim sim; sim.schedule(3000, 0, none, all); sim.run(30000);
  const auto& c = sim.buffer.counters();
  require(c.underruns == 0 && c.overruns == 0 && c.silences == 0 && sim.dryMs == 0, "Steady audio had an underrun, overrun or gap");
  // Two calm periods (10 s each) take the 40 ms start to the 30 ms floor; it stays there.
  require(sim.buffer.target() == AudioJitterBuffer::Ms(30), "A calm stream did not shrink to the floor: " + std::to_string(sim.targetMs()));
  const double level = sim.meanLevel(500);
  require(level >= 20 && level <= 34, "Steady depth is not at the target: " + std::to_string(level));
  // Only the two shrinks' 10 ms leave by trimming; nothing is ever repeated.
  require(c.inserted_frames == 0 && c.trimmed_frames <= AudioJitterBuffer::Ms(12), "A steady stream was corrected: " + std::to_string(c.inserted_frames) + " inserted, " + std::to_string(c.trimmed_frames) + " trimmed");
  note("steadyDepthMs", level); note("steadyTargetMs", sim.targetMs()); note("steadyTrimmed", double(c.trimmed_frames));
  return 1;
}

int stalls() {
  // A 120 ms stall every 3 s (the packets then arrive together, PTS continuous).
  Sim sim; std::vector<uint64_t> underrunAt;
  sim.schedule(3000, 0, [](uint64_t n) { const uint64_t ms = (n + 1) * 10 % 3000; return ms > 1500 && ms <= 1620 ? double(1620 - ms) : 0.0; }, all);
  uint64_t seen = 0;
  sim.run(30000, [&](Sim& s) { if (s.buffer.counters().underruns != seen) { seen = s.buffer.counters().underruns; underrunAt.push_back(s.now); } });
  const auto& c = sim.buffer.counters();
  require(c.underruns >= 1 && c.underruns <= 7, "Stalls did not cause a bounded number of underruns: " + std::to_string(c.underruns));
  require(!underrunAt.empty() && underrunAt.back() < 21000000, "The target never grew past the stall: last underrun at " + std::to_string(underrunAt.back() / 1000) + " ms");
  require(sim.targetMs() >= 120 && sim.targetMs() <= 200, "The target did not settle above the stall: " + std::to_string(sim.targetMs()));
  require(c.silences == 0, "A late network was read as silence");
  note("stallUnderruns", double(c.underruns)); note("stallTargetMs", sim.targetMs()); note("stallOverrunMs", c.overrun_frames / 48.0); note("stallDryMs", double(sim.dryMs));
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
  require(sim.buffer.target() == AudioJitterBuffer::Ms(40), "Silence changed the target");
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
  require(c.underruns == 1, "The dry device after the stall was not one underrun: " + std::to_string(c.underruns));
  note("renderStallDroppedMs", c.overrun_frames / 48.0); note("renderStallDepthAfterMs", after);
  return 1;
}

int drift(double rate) {
  Sim sim; sim.schedule(6000, rate, none, all); sim.run(60000);
  const auto& c = sim.buffer.counters();
  const double expected = std::abs(rate) * 48000 * 60;
  const double corrected = double(rate > 0 ? c.trimmed_frames : c.inserted_frames);
  require(c.underruns == 0 && c.overruns == 0, "Clock drift " + std::to_string(rate) + " was not absorbed: underruns " + std::to_string(c.underruns) + ", overruns " + std::to_string(c.overruns));
  require(corrected >= expected * 0.6 && corrected <= expected * 1.4, "Drift correction " + std::to_string(corrected) + " frames, expected about " + std::to_string(expected));
  const double level = sim.meanLevel(300);
  require(level >= sim.targetMs() - 12 && level <= sim.targetMs() + 6, "Drift moved the depth: " + std::to_string(level) + " ms for target " + std::to_string(sim.targetMs()));
  note(rate > 0 ? "fastDriftTrimmed" : "slowDriftInserted", corrected); note(rate > 0 ? "fastDriftDepthMs" : "slowDriftDepthMs", level);
  return 1;
}

int jitter() {
  std::mt19937 random(7); std::uniform_real_distribution<double> spread(0, 25);
  std::vector<double> delays(3000); for (auto& d : delays) d = spread(random);
  Sim sim; sim.schedule(3000, 0, [&](uint64_t n) { return delays[n]; }, all); sim.run(30000);
  const auto& c = sim.buffer.counters();
  require(c.underruns <= 1 && sim.targetMs() <= 60, "25 ms of jitter grew the buffer: " + std::to_string(c.underruns) + " underruns, target " + std::to_string(sim.targetMs()));
  require(sim.dryMs <= 40, "25 ms of jitter left audible gaps: " + std::to_string(sim.dryMs) + " ms");
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
    cases += steady(); cases += stalls(); cases += silence(); cases += renderStall();
    cases += drift(0.003); cases += drift(-0.003); cases += jitter(); cases += playingPts(); cases += malformed();
    std::cout << "{\"kind\":\"audio-jitter\",\"cases\":" << cases << measured << ",\"audioDevice\":false,\"network\":false}" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << std::endl;
    return 1;
  }
}
