// audio-opus-v1 on the client (windows/runner/opus_audio_decoder.h) with the
// real libopus and synthetic audio only: no relay, no audio device, no sound.
// Packets are made the way the Host makes them (apps/windows_host/src/
// opus_audio.hpp: RESTRICTED_LOWDELAY, 128 kbps VBR, DTX and FEC off, a u16
// sequence before each 10 ms Opus packet). Round trip quality, loss filled
// before the next packet, burst loss concealed ahead of it (bounded, fading)
// without an underrun in the real jitter buffer, late packets after that
// dropped in order, a silent VM's pause kept a pause, late and restarted
// sequences, malformed packets, one datagram per packet and the decoder's CPU
// time per second of audio.
#include "audio_jitter.h"
#include "opus_audio_decoder.h"
#include "../../../../native/windows_video/udp_media.hpp"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using multitap::AudioJitterBuffer;
using multitap::OpusAudioDecoder;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
std::string measured;
void note(const char* name, double value) { char text[96]; std::snprintf(text, sizeof(text), ",\"%s\":%.1f", name, value); measured += text; }
constexpr int kRate = 48000, kFrame = 480;
constexpr double kPi = 3.14159265358979323846;

std::vector<int16_t> tone(size_t frames, double hz) {
  std::vector<int16_t> out(frames * 2);
  for (size_t n = 0; n < frames; ++n) out[2 * n] = out[2 * n + 1] = int16_t(std::lround(0.5 * 32767 * std::sin(2 * kPi * hz * double(n) / kRate)));
  return out;
}
// A 140 Hz voice with three formant bands and a syllable envelope.
std::vector<int16_t> speechLike(size_t frames) {
  std::vector<int16_t> out(frames * 2);
  for (size_t n = 0; n < frames; ++n) {
    const double t = double(n) / kRate, envelope = 0.55 + 0.45 * std::sin(2 * kPi * 4.0 * t);
    double left = 0, right = 0;
    for (int h = 1; h <= 30; ++h) {
      const double hz = 140.0 * h, gain = std::exp(-std::pow((hz - 700) / 250, 2)) + 0.6 * std::exp(-std::pow((hz - 1200) / 300, 2)) + 0.3 * std::exp(-std::pow((hz - 2600) / 400, 2));
      left += gain * std::sin(2 * kPi * hz * t + h); right += gain * std::sin(2 * kPi * hz * t + 1.3 * h);
    }
    out[2 * n] = int16_t(std::lround(std::clamp(0.25 * envelope * left, -1.0, 1.0) * 32767));
    out[2 * n + 1] = int16_t(std::lround(std::clamp(0.25 * envelope * right, -1.0, 1.0) * 32767));
  }
  return out;
}

struct Packet { uint64_t pts; std::vector<uint8_t> payload; };
// The Host's encoder configuration and packet layout.
std::vector<Packet> encode(const std::vector<int16_t>& pcm, uint16_t first = 0, int frameSamples = kFrame) {
  int error = 0; OpusEncoder* encoder = opus_encoder_create(kRate, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error); require(error == OPUS_OK, "Encoder");
  opus_encoder_ctl(encoder, OPUS_SET_BITRATE(128000)); opus_encoder_ctl(encoder, OPUS_SET_VBR(1)); opus_encoder_ctl(encoder, OPUS_SET_DTX(0)); opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(0));
  std::vector<Packet> out; uint16_t sequence = first;
  for (size_t at = 0; at + size_t(frameSamples) <= pcm.size() / 2; at += size_t(frameSamples)) {
    std::vector<uint8_t> payload(2 + 1143);
    const int length = opus_encode(encoder, pcm.data() + 2 * at, frameSamples, payload.data() + 2, 1143); require(length > 0, "Encode");
    payload[0] = uint8_t(sequence); payload[1] = uint8_t(sequence >> 8); ++sequence; payload.resize(2 + size_t(length));
    out.push_back({uint64_t(at) * 1000000 / kRate, std::move(payload)});
  }
  opus_encoder_destroy(encoder); return out;
}
struct Output { std::vector<uint64_t> pts; std::vector<int16_t> pcm; };
Output decodeAll(OpusAudioDecoder& decoder, const std::vector<Packet>& packets, const std::set<size_t>& lost = {}) {
  Output out;
  for (size_t k = 0; k < packets.size(); ++k) {
    if (lost.count(k)) continue;
    require(decoder.Decode(packets[k].pts, packets[k].payload.data(), packets[k].payload.size(), [&](uint64_t pts, const uint8_t* bytes, size_t size) {
      require(size == OpusAudioDecoder::kFrameBytes, "Not 10 ms"); out.pts.push_back(pts);
      const auto* samples = reinterpret_cast<const int16_t*>(bytes); out.pcm.insert(out.pcm.end(), samples, samples + size / 2);
    }), "A packet was refused");
  }
  return out;
}
// SNR (dB) against the original delayed by the encoder's 120-sample lookahead, after 100 ms.
double snr(const std::vector<int16_t>& original, const std::vector<int16_t>& decoded, size_t fromFrame = 4800, size_t toFrame = SIZE_MAX) {
  double signal = 0, error = 0; const size_t frames = std::min({original.size() / 2, decoded.size() / 2 - 120, toFrame});
  for (size_t n = fromFrame; n < frames; ++n) for (int c = 0; c < 2; ++c) { const double o = original[2 * n + c], d = decoded[2 * (n + 120) + c]; signal += o * o; error += (o - d) * (o - d); }
  return 10 * std::log10(signal / std::max(error, 1.0));
}
double threadCpuUs() { FILETIME a{}, b{}, kernel{}, user{}; GetThreadTimes(GetCurrentThread(), &a, &b, &kernel, &user);
  return (double(ULARGE_INTEGER{{kernel.dwLowDateTime, kernel.dwHighDateTime}}.QuadPart) + double(ULARGE_INTEGER{{user.dwLowDateTime, user.dwHighDateTime}}.QuadPart)) / 10; }

// The decoded stream into the real jitter buffer as RemoteAudio runs them (no
// device): each packet is decoded when it arrives (one for a concealed number
// tells the buffer: NoteLate or NotePause); the render thread wakes on every
// packet and every `wakeMs` (its high-resolution timer), conceals while the
// buffer starves when `conceal` is on (off: the behaviour before it) and
// pulls. A virtual device plays 48 frames a millisecond.
struct Arrival { uint64_t at; size_t packet; };
// Packet k arrives 25 ms after its 10 ms of audio ends plus `late(k)` ms, in
// order (one stream); lost ones never.
std::vector<Arrival> arrivals(const std::vector<Packet>& packets, const std::set<size_t>& lost = {}, const std::function<uint64_t(size_t)>& late = nullptr) {
  std::vector<Arrival> out; uint64_t last = 0;
  for (size_t k = 0; k < packets.size(); ++k) {
    if (lost.count(k)) continue;
    last = std::max(last, packets[k].pts + 35000 + (late ? late(k) * 1000 : 0)); out.push_back({last, k});
  }
  return out;
}
struct Played {
  AudioJitterBuffer::Counters buffer; OpusAudioDecoder::Counters decoder; double targetMs = 0;
  std::vector<uint64_t> timeline;           // PTS handed to the jitter buffer, in order
  std::vector<uint64_t> ahead;              // of those, concealed ahead by the render thread
  std::vector<uint64_t> aheadAt;            // when (ms)
  std::vector<double> aheadRms;             // each one's RMS
  std::vector<int> aheadLastSample;         // each one's last sample (magnitude)
  std::vector<uint64_t> dryAt;              // ms with the device empty after the first write, up to the last arrival
  bool headBack = false;                    // the play head (A/V offset) went back more than 1 ms
};
Played play(const std::vector<Packet>& packets, const std::vector<Arrival>& in, bool conceal, uint64_t wakeMs = 5) {
  OpusAudioDecoder decoder; require(decoder.Open(), "Decoder");
  AudioJitterBuffer buffer; std::vector<uint8_t> scratch(4800 * 4); double device = 0; bool written = false;
  Played out; size_t next = 0; uint64_t woke = 0; int64_t head = INT64_MIN;
  const auto concealed = [&](uint64_t pts, const uint8_t* bytes, size_t size, uint64_t now) {
    const auto* s = reinterpret_cast<const int16_t*>(bytes); double sum = 0;
    for (size_t n = 0; n < size / 2; ++n) sum += double(s[n]) * s[n];
    out.timeline.push_back(pts); out.ahead.push_back(pts); out.aheadAt.push_back(now / 1000);
    out.aheadRms.push_back(std::sqrt(sum / double(size / 2))); out.aheadLastSample.push_back(std::max(std::abs(int(s[size / 2 - 2])), std::abs(int(s[size / 2 - 1]))));
    require(buffer.PushConcealed(pts, bytes, size), "Concealed audio refused");
  };
  for (uint64_t now = 1000; now < in.back().at + 300000; now += 1000) {
    device = std::max(0.0, device - 48);
    if (written && device == 0 && now <= in.back().at) out.dryAt.push_back(now / 1000);
    bool pushed = false;
    for (; next < in.size() && in[next].at <= now; ++next, pushed = true) {
      const Packet& p = packets[in[next].packet]; const auto before = decoder.counters();
      require(decoder.Decode(p.pts, p.payload.data(), p.payload.size(), [&](uint64_t pts, const uint8_t* bytes, size_t size) { out.timeline.push_back(pts); buffer.Push(pts, bytes, size, now); }), "A packet was refused");
      if (decoder.counters().late_concealed != before.late_concealed) buffer.NoteLate(now);
      if (decoder.counters().pauses != before.pauses) buffer.NotePause();
    }
    if (!pushed && now - woke < wakeMs * 1000) continue;
    woke = now;
    const uint32_t padding = uint32_t(device);
    // After the last packet the fixture ends (its tail is a pause like the ones tested below).
    if (conceal && now <= in.back().at) while (buffer.Starving(padding) && decoder.Conceal([&](uint64_t pts, const uint8_t* bytes, size_t size) { concealed(pts, bytes, size, now); })) {}
    const uint32_t frames = buffer.Pull(scratch.data(), 4800 - padding, padding, now);
    device += frames; written = written || frames;
    if (const auto at = buffer.PlayingPtsUs(now)) { if (*at + 1000 < head) out.headBack = true; head = *at; }
  }
  out.buffer = buffer.counters(); out.decoder = decoder.counters(); out.targetMs = buffer.target() / 48.0;
  return out;
}
// The PTS of `packets` from `from` to `to` (inclusive), in order.
std::vector<uint64_t> span(const std::vector<Packet>& packets, size_t from, size_t to) {
  std::vector<uint64_t> out; for (size_t k = from; k <= to; ++k) out.push_back(packets[k].pts); return out;
}
std::vector<uint64_t> joined(std::vector<uint64_t> a, const std::vector<uint64_t>& b) { a.insert(a.end(), b.begin(), b.end()); return a; }
// Where a timeline first differs from the expected one (for failure messages).
std::string differs(const std::vector<uint64_t>& got, const std::vector<uint64_t>& want) {
  size_t k = 0; while (k < got.size() && k < want.size() && got[k] == want[k]) ++k;
  return " (got " + std::to_string(got.size()) + " PTS, expected " + std::to_string(want.size()) + "; first difference at " + std::to_string(k) + ": " +
         (k < got.size() ? std::to_string(got[k]) : "none") + " for " + (k < want.size() ? std::to_string(want[k]) : "none") + ")";
}
double rms(const std::vector<int16_t>& pcm, size_t fromFrame, size_t frames) {
  double sum = 0; for (size_t n = 2 * fromFrame; n < 2 * (fromFrame + frames); ++n) sum += double(pcm[n]) * pcm[n];
  return std::sqrt(sum / double(2 * frames));
}
}

int main() {
  try {
    int checks = 0;
    // Round trip.
    const auto sine = tone(2 * kRate, 997), speech = speechLike(2 * kRate);
    const auto sinePackets = encode(sine), speechPackets = encode(speech);
    OpusAudioDecoder decoder; require(decoder.Open(), "libopus refused the decoder");
    const auto sineOut = decodeAll(decoder, sinePackets); require(decoder.Open(), "Reopen");
    const auto speechOut = decodeAll(decoder, speechPackets);
    for (size_t k = 0; k < speechOut.pts.size(); ++k) require(speechOut.pts[k] == speechPackets[k].pts, "Decoded audio lost its packet's PTS");
    const double sineSnr = snr(sine, sineOut.pcm), speechSnr = snr(speech, speechOut.pcm);
    require(sineSnr >= 25 && speechSnr >= 15, "Round trip SNR " + std::to_string(sineSnr) + " / " + std::to_string(speechSnr));
    note("sineSnrDb", sineSnr); note("speechSnrDb", speechSnr); checks += 2;

    // Loss: single, double and a 4-packet run are filled before the packet that
    // shows them, with the PTS right before it; a 10-packet run is not filled.
    const std::set<size_t> lost = {50, 80, 81, 120, 121, 122, 123, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169};
    require(decoder.Open(), "Reopen");
    const auto lossy = decodeAll(decoder, speechPackets, lost);
    require(decoder.counters().concealed == 7 && decoder.counters().unfilled == 10 && decoder.counters().packets == speechPackets.size() - lost.size(), "Concealment counts");
    require(lossy.pts.size() == speechPackets.size() - 10, "Frames out: every packet and every filled loss");
    for (size_t k = 1; k < lossy.pts.size(); ++k) {
      const uint64_t step = lossy.pts[k] - lossy.pts[k - 1];
      require(step == 10000 || (lossy.pts[k - 1] == speechPackets[159].pts && lossy.pts[k] == speechPackets[170].pts), "A gap in the PTS at " + std::to_string(k));
    }
    // The filled frames carry sound like the original, not silence: compare the
    // stretch around the single, double and 4-packet losses (before the long gap).
    std::vector<int16_t> aligned(lossy.pcm.begin(), lossy.pcm.begin() + 160 * kFrame * 2);
    const double lossySnr = snr(speech, aligned, 4800, 160 * kFrame - 120);
    double concealedEnergy = 0, originalEnergy = 0;
    for (size_t n = 50 * kFrame; n < 51 * kFrame; ++n) { concealedEnergy += double(lossy.pcm[2 * (n + 120)]) * lossy.pcm[2 * (n + 120)]; originalEnergy += double(speech[2 * n]) * speech[2 * n]; }
    require(concealedEnergy > 0.1 * originalEnergy, "A concealed frame is close to silence");
    note("speechSnrWith7Of160LostDb", lossySnr); note("concealedEnergyRatio", concealedEnergy / originalEnergy); checks += 4;

    // Into the real jitter buffer, arriving as the network brings them. Single
    // and paired losses (6 of 200) are filled at the next packet: every 10 ms
    // of the timeline, no underrun, overrun or silence, nothing concealed ahead.
    const auto all = span(speechPackets, 0, speechPackets.size() - 1);
    {
      const auto singles = play(speechPackets, arrivals(speechPackets, {20, 45, 46, 90, 133, 170}), true);
      require(singles.timeline == all, "Single and paired losses left a hole in the audio handed to the jitter buffer" + differs(singles.timeline, all) + " ahead " + std::to_string(singles.ahead.size()));
      require(singles.buffer.underruns == 0 && singles.buffer.overruns == 0 && singles.buffer.silences == 0 && singles.dryAt.empty() && singles.ahead.empty(),
              "Single and paired losses: underruns " + std::to_string(singles.buffer.underruns) + ", dry " + std::to_string(singles.dryAt.size()) + " ms, concealed ahead " + std::to_string(singles.ahead.size()));
      note("underrunsWithSinglesAndPairs", double(singles.buffer.underruns)); checks += 2;
    }
    // Burst losses of 30, 60 and 120 ms outlast the buffer (30-40 ms) before
    // the next packet can show them. Without concealment ahead (as before) the
    // device runs dry. With it: Opus PLC keeps the device fed, every 10 ms in
    // order (concealed ahead, then filled at the next packet), no underrun;
    // past 80 ms the concealment has faded to silence and stops, the rest of
    // the loss is left unfilled and the device runs dry only after that.
    const double speechRms = rms(speech, 150 * kFrame, 3 * kFrame);
    for (const size_t burst : {3, 6, 12}) {
      std::set<size_t> burstLost; for (size_t k = 0; k < burst; ++k) burstLost.insert(150 + k);
      const auto before = play(speechPackets, arrivals(speechPackets, burstLost), false);
      require(before.buffer.underruns + before.buffer.silences >= 1 && !before.dryAt.empty(), "Without concealment ahead a " + std::to_string(burst * 10) + " ms burst did not run the device dry: the fixture shows nothing");
      const auto played = play(speechPackets, arrivals(speechPackets, burstLost), true);
      const auto& d = played.decoder; const auto& b = played.buffer;
      require(!played.headBack, "The play head went back across a concealed burst");
      require(b.underruns == 0 && b.overruns == 0, std::to_string(burst * 10) + " ms burst: underruns " + std::to_string(b.underruns) + ", overruns " + std::to_string(b.overruns));
      require(d.ahead + d.concealed + d.unfilled == burst && d.ahead >= 1 && d.ahead <= OpusAudioDecoder::kMaxRun && d.late == 0, std::to_string(burst * 10) + " ms burst: concealed ahead " + std::to_string(d.ahead) + ", at the packet " + std::to_string(d.concealed) + ", unfilled " + std::to_string(d.unfilled));
      require(b.concealed_frames == d.ahead * kFrame && b.late_concealments == 0 && played.targetMs == 40, "A lost burst counted as late or moved the target");
      for (size_t k = 0; k < std::min<size_t>(3, played.aheadRms.size()); ++k) require(played.aheadRms[k] > 0.1 * speechRms, "A frame concealed ahead is close to silence");
      if (burst * 10 <= 60) {
        require(played.timeline == all, std::to_string(burst * 10) + " ms burst: the jitter buffer did not get every 10 ms in order" + differs(played.timeline, all));
        require(played.dryAt.empty() && b.silences == 0, std::to_string(burst * 10) + " ms burst: the device ran dry for " + std::to_string(played.dryAt.size()) + " ms");
      } else {
        // 80 ms concealed ahead (the bound), fading to silence; 40 ms left unfilled.
        require(d.ahead == OpusAudioDecoder::kMaxRun && d.unfilled == burst - OpusAudioDecoder::kMaxRun, "120 ms burst: not 80 ms concealed then the rest unfilled");
        require(played.timeline == joined(span(speechPackets, 0, 149 + OpusAudioDecoder::kMaxRun), span(speechPackets, 150 + burst, speechPackets.size() - 1)), "120 ms burst: concealed frames out of order or misplaced");
        require(played.aheadLastSample.back() == 0 && played.aheadRms.back() < 0.5 * played.aheadRms[2], "120 ms burst: the concealment did not fade to silence");
        require(!played.dryAt.empty() && played.dryAt.front() > played.aheadAt.back(), "120 ms burst: the device ran dry while concealment went on");
        require(played.dryAt.size() <= 100, "120 ms burst: the device stayed dry " + std::to_string(played.dryAt.size()) + " ms");
        note("dryMsAfter120msBurstConcealed", double(played.dryAt.size()));
      }
      note(("dryMsWithoutConcealment" + std::to_string(burst * 10) + "msBurst").c_str(), double(before.dryAt.size()));
      note(("concealedAheadWith" + std::to_string(burst * 10) + "msBurst").c_str(), double(d.ahead));
      checks += 4;
    }

    // Late, not lost: 150-155 come together 50 ms late, then 150-169 come
    // together 190 ms late. What was concealed ahead is dropped when it comes
    // (its time was played), in order; the rest is decoded in order; the
    // target grows once, as for an underrun.
    for (const size_t lastLate : {size_t(155), size_t(169)}) {
      const auto played = play(speechPackets, arrivals(speechPackets, {}, [&](size_t k) { return k >= 150 && k <= lastLate ? uint64_t(10 * (lastLate - k)) : 0; }), true);
      const auto& d = played.decoder; const auto& b = played.buffer;
      require(d.ahead >= 1 && d.late_concealed == d.ahead && d.late == d.ahead && d.unfilled == 0 && d.concealed == 0 && d.pauses == 0,
              "Late packets: concealed ahead " + std::to_string(d.ahead) + ", dropped late " + std::to_string(d.late) + " (" + std::to_string(d.late_concealed) + " concealed)");
      require(played.timeline == all, "Late packets: the jitter buffer did not get every 10 ms exactly once, in order" + differs(played.timeline, all));
      require(b.late_concealments == 1 && played.targetMs == 60 && b.underruns == 0, "Late packets did not grow the target once: " + std::to_string(b.late_concealments) + ", target " + std::to_string(played.targetMs));
      require(!played.headBack, "The play head went back across late packets");
      if (lastLate == 155) require(played.dryAt.empty(), "A 50 ms stall ran the device dry");
      else require(d.ahead == OpusAudioDecoder::kMaxRun && !played.dryAt.empty() && played.dryAt.front() > played.aheadAt.back(), "A 190 ms stall: not 80 ms concealed before the device ran dry");
      note(lastLate == 155 ? "lateDroppedAfter50msStall" : "lateDroppedAfter190msStall", double(d.late_concealed)); checks += 3;
    }

    // The VM goes silent for 500 ms, then 40 ms: the Host sends nothing and
    // its numbers go on without a gap. The end of the sound before it is
    // concealed (bounded, fading) like a loss, but the sound after it is new,
    // not late: every packet is played, one silence, no target change, no
    // underrun (the device ran dry in the long one; the short one is primed
    // again after the concealment).
    for (const uint64_t pauseMs : {uint64_t(500), uint64_t(40)}) {
      auto paused = speechPackets; for (size_t k = 100; k < paused.size(); ++k) paused[k].pts += pauseMs * 1000;
      const auto played = play(paused, arrivals(paused), true);
      const auto& d = played.decoder; const auto& b = played.buffer;
      std::vector<uint64_t> received; for (const uint64_t pts : played.timeline) if (std::find(played.ahead.begin(), played.ahead.end(), pts) == played.ahead.end()) received.push_back(pts);
      require(received == span(paused, 0, paused.size() - 1), std::to_string(pauseMs) + " ms pause: a packet after it was dropped or reordered");
      require(d.pauses == 1 && d.late == 0 && d.unfilled == 0 && d.ahead >= 1 && d.ahead <= OpusAudioDecoder::kMaxRun, std::to_string(pauseMs) + " ms pause: pauses " + std::to_string(d.pauses) + ", late " + std::to_string(d.late) + ", concealed ahead " + std::to_string(d.ahead));
      require(b.underruns == 0 && b.late_concealments == 0 && played.targetMs == 40 && b.silences == 1,std::to_string(pauseMs) + " ms pause was read as loss: underruns " + std::to_string(b.underruns) + ", silences " + std::to_string(b.silences) + ", target " + std::to_string(played.targetMs));
      note(pauseMs == 500 ? "concealedTailBefore500msPause" : "concealedTailBefore40msPause", double(d.ahead)); checks += 3;
    }

    // Measured, not required: 0-25 ms of uniform arrival jitter with
    // concealment ahead (how often it stands in for packets that were only
    // late), and the 30 ms burst with the render thread waking every 15 ms
    // (Windows' default timer, without the high-resolution one).
    {
      std::vector<uint64_t> jitter(speechPackets.size()); uint32_t seed = 7;
      for (auto& j : jitter) { seed = seed * 1664525u + 1013904223u; j = (seed >> 16) % 26; }
      const auto jittered = play(speechPackets, arrivals(speechPackets, {}, [&](size_t k) { return jitter[k]; }), true);
      require(jittered.buffer.underruns == 0 && jittered.targetMs <= 100, "25 ms of jitter: underruns " + std::to_string(jittered.buffer.underruns) + ", target " + std::to_string(jittered.targetMs));
      note("jitter25msConcealedAhead", double(jittered.decoder.ahead)); note("jitter25msTargetMs", jittered.targetMs);
      const auto coarse = play(speechPackets, arrivals(speechPackets, {150, 151, 152}), true, 15);
      note("dryMs30msBurstWaking15ms", double(coarse.dryAt.size())); ++checks;
    }

    // Late and repeated packets are dropped; a sequence that starts over (the
    // Host's encoder restarted) is decoded from scratch, not dropped.
    require(decoder.Open(), "Reopen"); size_t frames = 0;
    const auto count = [&](uint64_t, const uint8_t*, size_t) { ++frames; };
    for (size_t k = 0; k < 10; ++k) require(decoder.Decode(speechPackets[k].pts, speechPackets[k].payload.data(), speechPackets[k].payload.size(), count), "In order");
    require(decoder.Decode(speechPackets[9].pts, speechPackets[9].payload.data(), speechPackets[9].payload.size(), count) && decoder.Decode(speechPackets[4].pts, speechPackets[4].payload.data(), speechPackets[4].payload.size(), count), "Late");
    require(frames == 10 && decoder.counters().late == 2, "A late packet was played");
    const auto restarted = encode(speech, 0); auto jumped = restarted[0]; jumped.payload[0] = 0x00; jumped.payload[1] = 0x90;  // 0x9000 is far behind 10
    require(decoder.Decode(5000000, jumped.payload.data(), jumped.payload.size(), count) && frames == 11 && decoder.counters().restarts == 1, "A restarted sequence was dropped");
    checks += 3;

    // Malformed: too short, not 10 ms (a 20 ms packet), not Opus.
    require(decoder.Open(), "Reopen");
    const uint8_t tiny[2] = {0, 0}; require(!decoder.Decode(0, tiny, sizeof(tiny), count), "A 2-byte payload was taken");
    const auto twenty = encode(speech, 0, 960); require(!decoder.Decode(0, twenty[0].payload.data(), twenty[0].payload.size(), count), "A 20 ms packet was taken");
    std::vector<uint8_t> junk = {1, 0, 0xff, 0xff, 0xff}; require(!decoder.Decode(0, junk.data(), junk.size(), count), "A malformed packet was taken");
    checks += 3;

    // One udp-media-v1 datagram per packet (the PCM packet took two).
    size_t largest = 0;
    for (const auto* set : {&sinePackets, &speechPackets}) for (const auto& p : *set) { largest = std::max(largest, 12 + p.payload.size()); require(multitap::udp::fragmentCount(12 + p.payload.size()) == 1, "Two datagrams"); }
    require(multitap::udp::fragmentCount(12 + 1920) == 2, "PCM fixture"); note("largestPacketBytes", double(largest)); ++checks;

    // Decoder CPU time (15.6 ms ticks) per second of audio: 30 s of speech-like sound.
    constexpr int timedSeconds = 30;
    const auto longPackets = encode(speechLike(timedSeconds * kRate)); require(decoder.Open(), "Reopen"); size_t decoded = 0;
    const double cpuBefore = threadCpuUs(); const auto wallBefore = std::chrono::steady_clock::now();
    for (const auto& p : longPackets) decoder.Decode(p.pts, p.payload.data(), p.payload.size(), [&](uint64_t, const uint8_t*, size_t) { ++decoded; });
    const double cpuUs = threadCpuUs() - cpuBefore, wallUs = double(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - wallBefore).count());
    require(decoded == timedSeconds * 100, "Timed decode"); note("decodeCpuUsPerAudioSecond", cpuUs / timedSeconds); note("decodeWallUsPerAudioSecond", wallUs / timedSeconds); ++checks;

    std::cout << "{\"opusClientCases\":" << checks << ",\"opusVersion\":\"" << opus_get_version_string() << "\"" << measured << ",\"network\":false,\"audioDevice\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
