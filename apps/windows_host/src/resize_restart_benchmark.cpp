// What a resize restart costs after the display change, on this PC (tuning
// backlog #1): the Host's encoder creation (host::Encoder, automatic backend,
// as the capture thread creates it) and its first IDR, then the client's
// decoder start (multitap::RemoteDecoder, CPU) and its first picture. Generated
// frames at the field's sizes; no desktop is captured and no display, audio
// device or driver is touched. Measurement only, not a test, and not a real
// resize: the display change itself, capture creation and the network are not
// in these numbers (the resizeStages fields measure those in the field).
// Run it on an otherwise idle PC: under load every number grows.
//
//   MultitapHostResizeRestartBenchmark [repetitions=6]
#include "media.hpp"
#include "remote_decoder.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
double median(std::vector<double> v) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
// A desktop-like picture: a gradient, window blocks and dense "text" rows, so
// the first IDR costs what a real one would rather than a flat colour's.
std::vector<uint8_t> nv12Picture(int width, int height, uint32_t seed) {
  std::vector<uint8_t> bgra(size_t(width) * height * 4);
  uint32_t state = seed * 2654435761u + 1;
  const auto next = [&] { state = state * 1664525u + 1013904223u; return state >> 8; };
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    uint8_t* p = &bgra[(size_t(y) * width + x) * 4];
    p[0] = uint8_t(60 + x * 120 / width); p[1] = uint8_t(70 + y * 100 / height); p[2] = uint8_t(90 + (x + y) % 40); p[3] = 255;
  }
  for (int window = 0; window < 6; ++window) {
    const int x0 = int(next() % uint32_t(std::max(1, width - 400))), y0 = int(next() % uint32_t(std::max(1, height - 300)));
    const int x1 = std::min(width, x0 + 300 + int(next() % 500)), y1 = std::min(height, y0 + 200 + int(next() % 400));
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
      uint8_t* p = &bgra[(size_t(y) * width + x) * 4];
      const bool glyph = (y - y0) % 18 < 11 && (x - x0) % 9 < 6 && ((next() & 3) != 0);
      p[0] = p[1] = p[2] = glyph ? uint8_t(30) : uint8_t(245);
    }
  }
  std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2);
  host::bgraToNv12(bgra.data(), nv12.data(), width, height);
  return nv12;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const int repetitions = argc > 1 ? std::clamp(std::atoi(argv[1]), 2, 20) : 6;
    host::Runtime runtime;
    const std::vector<std::pair<int, int>> sizes{{1264, 760}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {5120, 1440}};
    std::cout << std::fixed << std::setprecision(1);
    for (const auto [width, height] : sizes) {
      const std::vector<std::vector<uint8_t>> pictures{nv12Picture(width, height, 1), nv12Picture(width, height, 2)};
      std::vector<double> encoderMs, idrMs, probeMs, decoderMs, pictureMs;
      std::string backend;
      for (int run = 0; run < repetitions; ++run) {
        auto began = Clock::now();
        host::Encoder encoder(width, height, 60, 12000000);
        encoderMs.push_back(ms(Clock::now() - began));
        backend = encoder.backendName();
        // The first output of a fresh encoder must be an IDR; feed frames at
        // 60 Hz timestamps (no pacing) until it leaves.
        std::vector<host::Frame> out;
        began = Clock::now();
        for (int fed = 0; out.empty() && fed < 8; ++fed) encoder.encode(pictures[size_t(fed) % 2].data(), uint64_t(fed) * 16666, [&](host::Frame frame) { out.push_back(std::move(frame)); });
        if (out.empty()) encoder.finish([&](host::Frame frame) { out.push_back(std::move(frame)); });
        idrMs.push_back(ms(Clock::now() - began));
        require(!out.empty() && out.front().key, "A fresh encoder did not begin with an IDR");
        // The client's capacity check before it asks (stream-size-v2): a
        // decoder created and configured for the size, then dropped.
        began = Clock::now();
        require(multitap::RemoteDecoder::Accepts(UINT(width), UINT(height)), "The client decoder refused the size");
        probeMs.push_back(ms(Clock::now() - began));
        began = Clock::now();
        multitap::RemoteDecoder decoder;
        host::check(decoder.Initialize(UINT(width), UINT(height)), "Client decoder start");
        decoderMs.push_back(ms(Clock::now() - began));
        int shownCount = 0;
        began = Clock::now();
        const auto shown = [&](multitap::DecodedFrame&& frame) { require(frame.width == uint32_t(width) && frame.height == uint32_t(height), "Decoded picture has another size"); ++shownCount; };
        for (const auto& frame : out) { host::check(decoder.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, shown), "Client decode"); if (shownCount) break; }
        if (!shownCount) host::check(decoder.Drain(shown), "Client decoder drain");
        pictureMs.push_back(ms(Clock::now() - began));
        require(shownCount > 0, "The client decoder showed no picture from the first IDR");
      }
      // The first run also loads codec libraries; a resize restarts warm.
      const auto warm = [](const std::vector<double>& v) { return median(std::vector<double>(v.begin() + 1, v.end())); };
      std::cout << "{\"kind\":\"resize-restart\",\"width\":" << width << ",\"height\":" << height << ",\"encoder\":\"" << backend << "\",\"runs\":" << repetitions
                << ",\"encoderCreateMs\":{\"first\":" << encoderMs.front() << ",\"warmP50\":" << warm(encoderMs) << '}'
                << ",\"firstIdrMs\":{\"first\":" << idrMs.front() << ",\"warmP50\":" << warm(idrMs) << '}'
                << ",\"clientDecoderProbeMs\":{\"first\":" << probeMs.front() << ",\"warmP50\":" << warm(probeMs) << '}'
                << ",\"clientDecoderStartMs\":{\"first\":" << decoderMs.front() << ",\"warmP50\":" << warm(decoderMs) << '}'
                << ",\"clientFirstPictureMs\":{\"first\":" << pictureMs.front() << ",\"warmP50\":" << warm(pictureMs) << '}'
                << ",\"displayChanged\":false,\"desktopCaptured\":false}\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Resize restart benchmark failed: " << error.what() << "\n";
    return 1;
  }
}
