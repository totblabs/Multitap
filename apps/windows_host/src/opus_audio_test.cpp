// audio-opus-v1 (opus_audio.hpp) with synthetic PCM only: no audio device is
// opened. Negotiation and stream.info shape, the encoder's configuration,
// 10 ms framing with sequence numbers and PTS, one udp-media-v1 datagram per
// packet, the round trip through the real libopus decoder (SNR), the wire
// bitrate against PCM and the encoder's CPU time per second of audio.
#include "support.hpp"
#include "opus_audio.hpp"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using host::OpusAudioEncoder;
namespace udp = multitap::udp;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
constexpr int kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

// Interleaved stereo s16. A tone, a "speech-like" signal (a 140 Hz voice with
// three formant bands and a syllable envelope, the channels slightly
// different) and full-scale white noise (the largest packets).
std::vector<int16_t> tone(size_t frames, double hz, double amplitude) {
  std::vector<int16_t> out(frames * 2);
  for (size_t n = 0; n < frames; ++n) { const auto v = int16_t(std::lround(amplitude * 32767 * std::sin(2 * kPi * hz * double(n) / kRate))); out[2 * n] = v; out[2 * n + 1] = v; }
  return out;
}
std::vector<int16_t> speechLike(size_t frames) {
  std::vector<int16_t> out(frames * 2);
  for (size_t n = 0; n < frames; ++n) {
    const double t = double(n) / kRate, envelope = 0.55 + 0.45 * std::sin(2 * kPi * 4.0 * t);
    double left = 0, right = 0;
    for (int h = 1; h <= 30; ++h) {
      const double hz = 140.0 * h * (1.0 + 0.02 * std::sin(2 * kPi * 0.7 * t));
      const double gain = std::exp(-std::pow((hz - 700) / 250, 2)) + 0.6 * std::exp(-std::pow((hz - 1200) / 300, 2)) + 0.3 * std::exp(-std::pow((hz - 2600) / 400, 2));
      left += gain * std::sin(2 * kPi * hz * t + h); right += gain * std::sin(2 * kPi * hz * t + 1.3 * h);
    }
    out[2 * n] = int16_t(std::lround(std::clamp(0.25 * envelope * left, -1.0, 1.0) * 32767));
    out[2 * n + 1] = int16_t(std::lround(std::clamp(0.25 * envelope * right, -1.0, 1.0) * 32767));
  }
  return out;
}
std::vector<int16_t> noise(size_t frames) {
  std::mt19937 random(7); std::uniform_int_distribution<int> value(-32768, 32767);
  std::vector<int16_t> out(frames * 2); for (auto& s : out) s = int16_t(value(random)); return out;
}

struct Frame { uint64_t pts; std::vector<uint8_t> payload; };
// The signal in uneven loopback chunks (a 44.1 kHz mix gives 441-frame packets,
// others 480 or 512), each with the PTS of its first sample.
std::vector<Frame> encode(OpusAudioEncoder& encoder, const std::vector<int16_t>& pcm, bool flush = false) {
  std::vector<Frame> frames; const size_t total = pcm.size() / 2; const size_t chunks[] = {441, 480, 512, 7, 960, 333, 1, 479};
  for (size_t at = 0, i = 0; at < total; ++i) {
    const size_t take = std::min(chunks[i % std::size(chunks)], total - at);
    encoder.push(uint64_t(at) * 1000000 / kRate, reinterpret_cast<const uint8_t*>(pcm.data() + 2 * at), take * 4, [&](uint64_t pts, std::vector<uint8_t> payload) { frames.push_back({pts, std::move(payload)}); });
    at += take;
  }
  if (flush) encoder.flush([&](uint64_t pts, std::vector<uint8_t> payload) { frames.push_back({pts, std::move(payload)}); });
  return frames;
}
uint16_t sequence(const Frame& frame) { return uint16_t(frame.payload[0] | frame.payload[1] << 8); }
std::vector<int16_t> decode(const std::vector<Frame>& frames) {
  int error = 0; OpusDecoder* decoder = opus_decoder_create(kRate, 2, &error); require(error == OPUS_OK && decoder, "Decoder");
  std::vector<int16_t> out; int16_t pcm[OpusAudioEncoder::kFrameSamples * 2];
  for (const auto& f : frames) {
    const int got = opus_decode(decoder, f.payload.data() + 2, opus_int32(f.payload.size() - 2), pcm, OpusAudioEncoder::kFrameSamples, 0);
    require(got == OpusAudioEncoder::kFrameSamples, "A packet did not decode to exactly 10 ms");
    out.insert(out.end(), pcm, pcm + got * 2);
  }
  opus_decoder_destroy(decoder); return out;
}
// SNR (dB) of `decoded` against `original` delayed by `delay` samples, over
// the part after the first 100 ms (the codec's start).
double snr(const std::vector<int16_t>& original, const std::vector<int16_t>& decoded, int delay) {
  double signal = 0, error = 0; const size_t frames = std::min(original.size(), decoded.size()) / 2;
  for (size_t n = 4800; n + size_t(delay) < frames; ++n) for (int c = 0; c < 2; ++c) {
    const double o = original[2 * n + c], d = decoded[2 * (n + delay) + c]; signal += o * o; error += (o - d) * (o - d);
  }
  return 10 * std::log10(signal / std::max(error, 1.0));
}
double kbps(const std::vector<Frame>& frames, size_t extraPerPacket) {
  size_t bytes = 0; for (const auto& f : frames) bytes += f.payload.size() + extraPerPacket;
  return double(bytes) * 8 / (double(frames.size()) * 0.01) / 1000;
}
double threadCpuUs() { FILETIME a{}, b{}, kernel{}, user{}; GetThreadTimes(GetCurrentThread(), &a, &b, &kernel, &user);
  return (double(ULARGE_INTEGER{{kernel.dwLowDateTime, kernel.dwHighDateTime}}.QuadPart) + double(ULARGE_INTEGER{{user.dwLowDateTime, user.dwHighDateTime}}.QuadPart)) / 10; }
}

int main() {
  try {
    int checks = 0;
    // Negotiation: only an ordinary stream whose broker added audioCodec "opus".
    const auto start = [](const std::string& extra) { return host::JsonParser::parse("{\"type\":\"session.start\",\"sessionId\":\"s\",\"mode\":\"stream\",\"audio\":true" + extra + "}"); };
    require(host::opusAudioRequested(start(",\"audioCodec\":\"opus\"")), "A granted session.start was not taken"); ++checks;
    for (const char* extra : {"", ",\"audioCodec\":\"pcm\"", ",\"audioCodec\":\"OPUS\"", ",\"audioCodec\":true", ",\"audioCodec\":\"opus\",\"mediaProtocol\":\"tracks-v1\""}) {
      require(!host::opusAudioRequested(start(extra)), std::string("Opus was taken from ") + extra); ++checks;
    }
    require(!host::opusAudioRequested(host::JsonParser::parse("{\"type\":\"session.resize\",\"mode\":\"stream\",\"audioCodec\":\"opus\"}")), "Another message was taken"); ++checks;
    // stream.info: PCM keeps its released bytes; Opus says so.
    require(host::streamAudioInfoJson(true, false) == "{\"sampleRate\":48000,\"channels\":2,\"format\":\"s16le\",\"available\":true}", "The PCM audio object changed");
    require(host::streamAudioInfoJson(false, false) == "{\"sampleRate\":48000,\"channels\":2,\"format\":\"s16le\",\"available\":false}", "The PCM audio object changed");
    const auto info = host::JsonParser::parse(host::streamAudioInfoJson(true, true));
    require(info.get("format").string() == "opus" && info.get("sampleRate").number() == 48000 && info.get("channels").number() == 2 && info.get("available").boolean(), "The Opus audio object"); checks += 3;

    // Encoder configuration.
    auto encoder = OpusAudioEncoder::create(); require(encoder.has_value(), "libopus refused the configuration");
    opus_int32 application = 0, bitrate = 0, vbr = 0, dtx = 1, fec = 1;
    opus_encoder_ctl(encoder->native(), OPUS_GET_APPLICATION(&application)); opus_encoder_ctl(encoder->native(), OPUS_GET_BITRATE(&bitrate));
    opus_encoder_ctl(encoder->native(), OPUS_GET_VBR(&vbr)); opus_encoder_ctl(encoder->native(), OPUS_GET_DTX(&dtx)); opus_encoder_ctl(encoder->native(), OPUS_GET_INBAND_FEC(&fec));
    require(application == OPUS_APPLICATION_RESTRICTED_LOWDELAY && bitrate == 128000 && vbr == 1 && dtx == 0 && fec == 0, "Encoder settings");
    require(encoder->lookahead() == 120, "Restricted low delay looks ahead 2.5 ms (120 samples), found " + std::to_string(encoder->lookahead())); checks += 2;

    // Framing: 2 s + 123 samples in uneven chunks -> 200 frames of 10 ms, numbered
    // 0..199, the PTS of each first sample; the rest waits, flush sends it padded.
    const auto speech = speechLike(2 * kRate + 123);
    auto frames = encode(*encoder, speech);
    require(frames.size() == 200 && encoder->held() == 123, "Frames of exactly 480 samples: " + std::to_string(frames.size()) + " held " + std::to_string(encoder->held()));
    for (size_t k = 0; k < frames.size(); ++k) {
      require(sequence(frames[k]) == k, "Sequence numbers are not consecutive");
      require(frames[k].pts + 2 >= k * 10000 && frames[k].pts <= k * 10000 + 2, "Frame PTS is not its first sample's: " + std::to_string(frames[k].pts));
    }
    encoder->flush([&](uint64_t pts, std::vector<uint8_t> payload) { frames.push_back({pts, std::move(payload)}); });
    require(frames.size() == 201 && sequence(frames.back()) == 200 && frames.back().pts + 2 >= 2000000 && frames.back().pts <= 2000002 && encoder->held() == 0, "Flush of the held part");
    encoder->flush([&](uint64_t, std::vector<uint8_t>) { require(false, "An empty flush sent a frame"); });
    checks += 4;

    // Size: every packet, the media envelope included, is one udp-media-v1
    // datagram; even full-scale noise. PCM's 10 ms packet takes two.
    size_t largest = 0;
    auto loud = OpusAudioEncoder::create(); const auto noisy = encode(*loud, noise(kRate));
    for (const std::vector<Frame>* set : std::initializer_list<const std::vector<Frame>*>{&frames, &noisy}) for (const auto& f : *set) {
      const auto packet = host::mediaPacket(2, f.pts, f.payload);
      largest = std::max(largest, packet.size());
      require(udp::fragmentCount(packet.size()) == 1 && f.payload.size() <= 2 + OpusAudioEncoder::kMaxOpusBytes, "An Opus packet needs more than one datagram");
    }
    require(udp::fragmentCount(12 + 1920) == 2, "PCM fixture: a 10 ms PCM packet takes two datagrams"); checks += 2;

    // Round trip through the real decoder (the client's library), aligned by
    // the encoder's lookahead.
    auto sineEncoder = OpusAudioEncoder::create(); const auto sine = tone(2 * kRate, 997, 0.5);
    const double sineSnr = snr(sine, decode(encode(*sineEncoder, sine)), 120);
    const double speechSnr = snr(speech, decode(std::vector<Frame>(frames.begin(), frames.begin() + 200)), 120);
    const double misaligned = snr(speech, decode(std::vector<Frame>(frames.begin(), frames.begin() + 200)), 0);
    require(sineSnr >= 25, "Sine round trip SNR " + std::to_string(sineSnr));
    require(speechSnr >= 15, "Speech-like round trip SNR " + std::to_string(speechSnr));
    require(misaligned < speechSnr - 3, "The SNR check does not notice a 2.5 ms shift"); checks += 3;

    // Wire bitrate (WebSocket: envelope + sequence + Opus; UDP: every datagram
    // with its header, tag and fragment header) against PCM's.
    const double opusWs = kbps(frames, 12), opusNoiseWs = kbps(noisy, 12);
    const size_t udpOverhead = udp::headerBytes + udp::tagBytes + udp::fragmentHeaderBytes;
    const double opusUdp = kbps(frames, 12 + udpOverhead), pcmWs = (12 + 1920) * 8 / 0.01 / 1000, pcmUdp = (12 + 1920 + 2 * udpOverhead) * 8 / 0.01 / 1000;

    // Encoder CPU (this thread's CPU time, 15.6 ms ticks) per second of audio: 30 s of speech-like sound.
    constexpr int timedSeconds = 30;
    auto timed = OpusAudioEncoder::create(); const auto long_ = speechLike(timedSeconds * kRate); size_t packets = 0;
    const double cpuBefore = threadCpuUs(); const auto wallBefore = std::chrono::steady_clock::now();
    timed->push(0, reinterpret_cast<const uint8_t*>(long_.data()), long_.size() * 2, [&](uint64_t, std::vector<uint8_t>) { ++packets; });
    const double cpuUs = threadCpuUs() - cpuBefore, wallUs = double(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - wallBefore).count());
    require(packets == timedSeconds * 100, "Timed encode"); ++checks;

    std::cout << std::fixed << std::setprecision(1)
              << "{\"opusAudioCases\":" << checks << ",\"opusVersion\":\"" << opus_get_version_string() << "\",\"lookaheadSamples\":" << encoder->lookahead()
              << ",\"measured\":{\"sineSnrDb\":" << sineSnr << ",\"speechSnrDb\":" << speechSnr << ",\"speechSnrUnalignedDb\":" << misaligned
              << ",\"speechKbpsWebSocket\":" << opusWs << ",\"speechKbpsUdp\":" << opusUdp << ",\"noiseKbpsWebSocket\":" << opusNoiseWs
              << ",\"largestPacketBytes\":" << largest << ",\"pcmKbpsWebSocket\":" << pcmWs << ",\"pcmKbpsUdp\":" << pcmUdp
              << ",\"encodeCpuUsPerAudioSecond\":" << cpuUs / timedSeconds << ",\"encodeWallUsPerAudioSecond\":" << wallUs / timedSeconds << "},\"network\":false,\"audioDevice\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
