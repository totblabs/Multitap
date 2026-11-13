// The stream's loopback sound capture (audio_capture_control.hpp): its fixed
// journal record and the service's hostAction 297 reading of it, the restart
// backoff that never gives up, the wake cadence (an even 10 ms instead of
// 1-2-1-2 packets per 15.6 ms timer tick) and the PTS of each packet from the
// capture's own position. Synthetic times only; then, where this PC has a
// render endpoint, the real Loopback for a moment (read only: nothing is
// played, no setting changes) to see it start, record and stop.
#include "support.hpp"
#include "audio_capture_control.hpp"
#include "stream_prepare_diagnostics.hpp"
#include "media.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace host;
using namespace std::chrono_literals;
using Time = AudioCaptureRestart::Clock::time_point;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
int checks = 0;

void records() {
  // started: 48 kHz stereo float, event-driven, and 44.1 kHz 6 channels s16 polled.
  auto row = audioCaptureJournal(audioCaptureStarted(48000, 2, true, AudioCaptureSample::float32));
  require(row.valid && row.ok && row.stage == 1 && row.expected == 48000 && row.items == 2 && row.active && row.format == 0, "started 48k");
  row = audioCaptureJournal(audioCaptureStarted(44100, 6, false, AudioCaptureSample::pcm16));
  require(row.valid && row.expected == 44100 && row.items == 6 && !row.active && row.format == 1, "started 44.1k");
  row = audioCaptureJournal(audioCaptureStarted(192000, 8, true, AudioCaptureSample::pcm32));
  require(row.expected == 192000 && row.items == 8 && row.format == 3, "started 192k 8ch");
  ++checks;
  // failed: the HRESULTs a loopback meets come back whole.
  for (const uint32_t hr : {0x88890004u /* AUDCLNT_E_DEVICE_INVALIDATED */, 0x80070490u /* E_NOTFOUND */, 0x8889000Au /* AUDCLNT_E_DEVICE_IN_USE */,
                            0x80004005u /* E_FAIL */, 0x8000FFFFu /* E_UNEXPECTED */, 0x88890008u /* AUDCLNT_E_UNSUPPORTED_FORMAT */, 0x8007000Eu /* E_OUTOFMEMORY */}) {
    for (const auto step : {AudioCaptureStep::endpoint, AudioCaptureStep::buffer, AudioCaptureStep::event}) {
      row = audioCaptureJournal(audioCaptureFailed(step, int32_t(hr)));
      require(row.valid && !row.ok && row.stage == 2 && row.cause == uint32_t(step) && uint32_t(row.error) == hr, "failed HRESULT round trip");
    }
  }
  row = audioCaptureJournal(audioCaptureFailed(AudioCaptureStep::formatUnsupported, 0));
  require(row.valid && !row.ok && row.cause == 5 && row.error == 0, "a failure without an HRESULT");
  require(audioCaptureJournal(audioCaptureFailed(AudioCaptureStep::start, 1)).error == 0, "a success code is no HRESULT failure");
  ++checks;
  row = audioCaptureJournal(audioCaptureRestarting(7));
  require(row.valid && row.ok && row.stage == 3 && row.expected == 7, "restarting");
  row = audioCaptureJournal(audioCaptureDeviceChanged(true));
  require(row.valid && row.ok && row.stage == 4 && row.active, "device changed while running");
  require(!audioCaptureJournal(audioCaptureDeviceChanged(false)).active, "device changed while closed");
  require(!audioCaptureJournal(0).valid && !audioCaptureJournal(5).valid, "unknown events are no record");
  ++checks;
  // The worker's packet carries it; the service calls only a failure a failure.
  const std::string session = "00000000-0000-0000-0000-000000000047";
  const uint32_t failure = audioCaptureFailed(AudioCaptureStep::buffer, int32_t(0x88890004u));
  const auto parsed = streamPreparationObservation(JsonParser::parse(streamPreparationPacket(session, audioCaptureRecordStage, failure)), session);
  require(parsed && parsed->first == 47 && parsed->second == failure, "stage 47 packet");
  require(!streamObservationOk(47, failure) && streamObservationOk(47, audioCaptureStarted(48000, 2, true, AudioCaptureSample::float32)) &&
          streamObservationOk(47, audioCaptureRestarting(1)) && streamObservationOk(47, audioCaptureDeviceChanged(true)), "stage 47 ok");
  require(!streamObservationNoHardwareCapture(47, audioCaptureStarted(48000, 2, false, AudioCaptureSample::pcm16)), "no render re-arm for sound");
  ++checks;
}

void restart() {
  AudioCaptureRestart r; const Time t0{};
  r.opened(t0);
  // An endpoint that keeps failing: 50, 200, 500, 1000, 2000, then 2000 ms forever. Never given up.
  Time now = t0 + 1s; const std::vector<std::chrono::milliseconds> expected{50ms, 200ms, 500ms, 1000ms, 2000ms, 2000ms, 2000ms};
  for (const auto wait : expected) { const auto due = r.failed(now); require(due - now == wait, "backoff step"); now = due; }
  for (int n = 0; n < 10000; ++n) { const auto due = r.failed(now); require(due - now == 2000ms, "the last wait repeats"); now = due; }
  require(r.attempt() == expected.size() + 10000, "every attempt counted");
  ++checks;
  // A capture that worked for a while starts the backoff from its first wait; a short one does not.
  r.opened(now); now += 6s; require(r.failed(now) - now == 50ms, "healthy capture resets the backoff");
  r.opened(now); now += 1s; require(r.failed(now) - now == 200ms, "a short capture keeps counting");
  ++checks;
  // A changed default device opens again within the settle time, whatever the backoff was.
  for (int n = 0; n < 6; ++n) now = r.failed(now);
  require(r.changed(now) - now == AudioCaptureRestart::settle && r.failuresInRow() == 0, "device change reopens soon");
  require(r.failed(now) - now == 50ms, "and the backoff starts over");
  ++checks;
  // The journal: a burst, then one a minute, however long the device fails.
  AudioCaptureRestart j; Time at{}; uint32_t written = 0;
  for (int n = 0; n < 3600; ++n, at += 1s) if (j.record(at)) ++written;  // an hour, one event a second
  require(written == AudioCaptureRestart::burst + 59, "journal limiter: " + std::to_string(written));
  ++checks;
}

// Packets the engine finishes every 10 ms (with up to 0.5 ms of its own jitter); the loop looks at
// its wake times. Returns the spacing (ms) of the packets it handed on.
std::vector<double> emitted(const std::vector<double>& wakes, double jitterMs, uint32_t seed) {
  std::mt19937 random(seed); std::uniform_real_distribution<double> jitter(0, jitterMs);
  std::vector<double> ready; for (int k = 1; k <= 400; ++k) ready.push_back(k * 10.0 + jitter(random));
  std::vector<double> out; size_t next = 0;
  for (const double wake : wakes) while (next < ready.size() && ready[next] <= wake) { out.push_back(wake); ++next; }
  std::vector<double> spacing; for (size_t n = 1; n < out.size(); ++n) spacing.push_back(out[n] - out[n - 1]);
  return spacing;
}

void cadence() {
  // The capture's event: the loop wakes when the packet is there. Before it is proven, the high-resolution
  // poll; the backstop after it only keeps the silence flush and the stop on time.
  require(audioWakePeriod(false) == 2ms && audioWakePeriod(true) == 10ms && audioEventsProven > 0, "periods");
  // The old loop: sleep_for(5 ms) at the default timer resolution wakes every 15.625 ms.
  std::vector<double> coarse, fine;
  for (double t = 0; t < 4100; t += 15.625) coarse.push_back(t);
  const double poll = double(audioWakePeriod(false).count()) / 1000;
  for (double t = 0; t < 4100; t += poll + 0.05) fine.push_back(t);  // + waking overhead
  const auto before = emitted(coarse, 0.5, 1), after = emitted(fine, 0.5, 1);
  size_t together = 0; double worstBefore = 0, worstAfter = 0;
  for (const double s : before) { if (s < 1) ++together; worstBefore = std::max(worstBefore, std::abs(s - 10)); }
  for (const double s : after) worstAfter = std::max(worstAfter, std::abs(s - 10));
  require(together > 50 && worstBefore > 5, "the 15.6 ms tick handed packets on in pairs (1-2-1-2)");
  require(after.size() >= 390 && worstAfter <= poll + 0.6, "each packet leaves within one poll: worst " + std::to_string(worstAfter) + " ms");
  // Event-driven: the wake is the packet's own readiness (the spacing is the engine's own jitter).
  std::vector<double> events; { std::mt19937 random(1); std::uniform_real_distribution<double> jitter(0, 0.5); for (int k = 1; k <= 400; ++k) events.push_back(k * 10.0 + jitter(random)); }
  double worstEvent = 0; for (const double s : emitted(events, 0.5, 1)) worstEvent = std::max(worstEvent, std::abs(s - 10));
  require(worstEvent <= 0.5, "event wakes follow the engine");
  std::cout << "{\"kind\":\"audio-capture-cadence\",\"oldPairs\":" << together << ",\"oldWorstMs\":" << worstBefore << ",\"pollWorstMs\":" << worstAfter << ",\"eventWorstMs\":" << worstEvent << "}\n";
  ++checks;
}

void ptsClock() {
  // 10 ms packets (480 output samples) read up to 2 ms late (the read-time fallback): their PTS stay
  // 10 ms apart within the follow step, and never run away from the stream clock.
  AudioPtsClock clock; std::mt19937 random(7); std::uniform_real_distribution<double> late(0, 2000);
  int64_t previous = -1; int64_t worstSpacing = 0, worstOffset = 0;
  for (int k = 0; k < 6000; ++k) {
    const int64_t truth = 100000 + int64_t(k) * 10000;
    const int64_t pts = clock.stamp(truth + int64_t(late(random)), false); clock.advance(480);
    if (previous >= 0) worstSpacing = std::max<int64_t>(worstSpacing, std::llabs(pts - previous - 10000));
    worstOffset = std::max<int64_t>(worstOffset, std::llabs(pts - truth - 1000));
    previous = pts;
  }
  require(clock.restarts() == 1 && worstSpacing <= 40 && worstOffset <= 2100, "jittered reads: spacing " + std::to_string(worstSpacing) + " offset " + std::to_string(worstOffset));
  ++checks;
  // The QPC position WASAPI gives: exactly 10 ms apart.
  AudioPtsClock exact; previous = -1;
  for (int k = 0; k < 1000; ++k) { const int64_t pts = exact.stamp(5000 + int64_t(k) * 10000, false); exact.advance(480); if (previous >= 0) require(pts - previous == 10000, "exact spacing"); previous = pts; }
  // 44.1 kHz: 441 source frames make 480 or 481 output samples; the stamps follow the samples sent.
  AudioPtsClock resampled; int64_t sent = 0; double next = 0; uint64_t source = 0;
  for (int k = 0; k < 1000; ++k) {
    uint64_t out = 0; for (int i = 0; i < 441; ++i, ++source) while (next <= double(source)) { ++out; next += 44100.0 / 48000.0; }
    const int64_t pts = resampled.stamp(int64_t(k) * 10000, false);
    require(std::llabs(pts - sent * 1000000 / 48000) <= 1, "resampled stamps follow the samples");
    resampled.advance(out); sent += int64_t(out);
  }
  ++checks;
  // A device clock 200 ppm fast: the stamps follow the stream clock, without a jump.
  AudioPtsClock drift; previous = -1; int64_t worstDrift = 0;
  for (int k = 0; k < 30000; ++k) {
    const int64_t measured = int64_t(std::llround(k * 10000 * (1 - 200e-6)));
    const int64_t pts = drift.stamp(measured, false); drift.advance(480);
    worstDrift = std::max<int64_t>(worstDrift, std::llabs(pts - measured));
    if (previous >= 0) require(std::llabs(pts - previous - 10000) <= 10, "drift without a jump");
    previous = pts;
  }
  require(drift.restarts() == 1 && worstDrift <= 200, "drift followed: " + std::to_string(worstDrift));
  ++checks;
  // A silence (WASAPI sent nothing for 400 ms) or a reopened capture starts the stamps over at the
  // measured time; so does a measurement far behind.
  AudioPtsClock gap; gap.stamp(0, false); gap.advance(480); gap.stamp(10000, false); gap.advance(480);
  require(gap.stamp(420000, false) == 420000 && gap.restarts() == 2, "silence restarts the stamps");
  gap.advance(480);
  require(gap.stamp(430000, true) == 430000 && gap.restarts() == 3, "a discontinuity restarts the stamps");
  gap.advance(480);
  require(gap.stamp(100000, false) == 100000 && gap.restarts() == 4, "a measurement far behind restarts the stamps");
  gap.reset(); require(gap.stamp(777, false) == 777, "reset");
  ++checks;
}

// The real loopback of this PC's default render endpoint for a moment, where there is one.
void live() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  struct Uninit { HRESULT com; ~Uninit() { if (SUCCEEDED(com)) CoUninitialize(); } } uninit{com};
  Loopback loopback;
  if (!loopback.available()) {
    const auto row = audioCaptureJournal(loopback.failureRecord());
    std::cout << "{\"kind\":\"audio-capture-live\",\"available\":false,\"step\":" << row.cause << ",\"error\":" << row.error << "}\n";
    return;
  }
  std::atomic<bool> stop{false}; std::mutex mutex; std::vector<uint32_t> seen; size_t packets = 0;
  std::thread runner([&] {
    loopback.run(stop, Clock::now(), [&](std::vector<uint8_t> bytes) { std::lock_guard lock(mutex); require(bytes.size() > 12 && bytes[0] == 2, "a kind-2 packet"); ++packets; },
                 nullptr, nullptr, [&](uint32_t value) { std::lock_guard lock(mutex); seen.push_back(value); });
  });
  std::this_thread::sleep_for(400ms);
  const auto stopped = Clock::now(); stop = true; runner.join();
  const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stopped).count();
  require(!seen.empty() && audioCaptureJournal(seen.front()).stage == 1, "the capture recorded its start");
  require(stopMs < 200, "the capture stops promptly: " + std::to_string(stopMs) + " ms");
  const auto started = audioCaptureJournal(seen.front());
  std::cout << "{\"kind\":\"audio-capture-live\",\"available\":true,\"rate\":" << started.expected << ",\"channels\":" << started.items << ",\"eventDriven\":" << (started.active ? "true" : "false")
            << ",\"records\":" << seen.size() << ",\"packets\":" << packets << ",\"stopMs\":" << stopMs << "}\n";
  ++checks;
}
}

int main() {
  try {
    records(); restart(); cadence(); ptsClock(); live();
    std::cout << "{\"kind\":\"audio-capture-control\",\"passed\":" << checks << "}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "audio capture control check failed: " << error.what() << "\n"; return 1; }
}
