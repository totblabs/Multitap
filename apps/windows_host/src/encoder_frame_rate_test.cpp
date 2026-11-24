// Real encoders at a capture rate below the one they were opened at (media.hpp,
// Encoder::setFrameRate). Synthetic pixels on a synthetic clock; no desktop is read. Each
// encoder is opened at `fps`, fed frames paced at a lower rate with their real timestamps,
// and what it sends is compared with its target: told nothing, an encoder that budgets per
// frame sends only paced/fps of the target; told the paced rate, the whole target, in
// fewer and larger frames. Run by hand: x264 and the Microsoft MFT anywhere, NVENC with
// --nvenc on a Host with that GPU.
//   MultitapHostEncoderFrameRateTest [--nvenc] [--only x264|mft|nvenc] [--width 1280 --height 720 --fps 60 --kbps 4000]
#include "media.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
uint32_t mix(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
// A detailed texture panning under blocks that change every frame: a picture that wants more
// than a low target gives it, so the rate control decides every frame's size.
struct Scene {
  int width, height, textureWidth, textureHeight; std::vector<uint8_t> texture, nv12;
  Scene(int w, int h) : width(w), height(h), textureWidth(w * 2), textureHeight(h * 2), texture(size_t(w) * 2 * h * 2), nv12(size_t(w) * h * 3 / 2, 128) {
    for (int y = 0; y < textureHeight; ++y) for (int x = 0; x < textureWidth; ++x) {
      const uint32_t coarse = mix(uint32_t(x / 24) * 7919u + uint32_t(y / 24) * 104729u), fine = mix(uint32_t(x) * 31u + uint32_t(y) * 17389u);
      texture[size_t(y) * textureWidth + x] = uint8_t(40 + coarse % 120 + fine % 40 + (x + y) % 32);
    }
  }
  // `step`: how far the scene has moved; frames paced at half the rate move twice as far each.
  const uint8_t* frame(int step) {
    const int ox = (step * 5) % width, oy = (step * 3) % height;
    for (int y = 0; y < height; ++y) std::memcpy(nv12.data() + size_t(y) * width, texture.data() + size_t(y + oy) * textureWidth + ox, size_t(width));
    uint32_t h = mix(uint32_t(step) + 99u);
    for (int block = 0; block < 40; ++block) {
      h = mix(h + uint32_t(block)); const int bx = int(h >> 8) % (width - 64), by = int(h >> 20) % (height - 64); const uint8_t shade = uint8_t(30 + (h & 0xbf));
      for (int y = by; y < by + 64; ++y) for (int x = bx; x < bx + 64; ++x) nv12[size_t(y) * width + x] = uint8_t(shade + ((x ^ y) & 15));
    }
    return nv12.data();
  }
};
struct Run { double mbps = 0, meanBytes = 0, largest = 0; int frames = 0, keys = 0; };
// Four seconds at `paced` frames a second after two to settle (the MFT's rate control takes about a second to follow); `tell`: the encoder is told the paced rate.
// `early`: told before its first frame, as an encoder that replaces a failed one is.
Run run(host::EncoderBackend backend, int width, int height, int fps, int bitrate, int paced, bool tell, bool early = false) {
  host::Encoder encoder(width, height, fps, bitrate, {}, backend);
  if (encoder.backend() != backend) throw std::runtime_error("The encoder asked for did not open");
  Scene scene(width, height); Run result; uint64_t bytes = 0; const int settle = paced * 2, total = settle + paced * 4;
  for (int n = 0; n < total; ++n) {
    const uint64_t pts = uint64_t(n) * 1000000 / uint64_t(paced);
    // Told while it streams, as the capture loop does when its pacing changes.
    if (tell && n == (early ? 0 : paced) && !encoder.setFrameRate(paced)) { Run refused; refused.keys = -1; return refused; }
    encoder.encode(scene.frame(n * encoder.captureFps() / paced), pts, [&](host::Frame frame) {
      if (frame.key && n) ++result.keys;
      if (n < settle) return;
      bytes += frame.bytes.size(); ++result.frames; result.largest = std::max(result.largest, double(frame.bytes.size()));
    });
  }
  encoder.finish([&](host::Frame frame) { bytes += frame.bytes.size(); ++result.frames; });
  result.mbps = double(bytes) * 8 / 4 / 1e6; result.meanBytes = result.frames ? double(bytes) / result.frames : 0;
  return result;
}
}

int main(int argc, char** argv) {
  try {
    int width = 1280, height = 720, fps = 60, kbps = 4000; bool nvenc = false; std::string only;
    for (int n = 1; n < argc; ++n) {
      const std::string option(argv[n]);
      if (option == "--nvenc") { nvenc = true; continue; }
      if (++n >= argc) throw std::runtime_error("Test argument requires a value");
      if (option == "--only") { only = argv[n]; continue; }
      const int value = std::stoi(argv[n]);
      if (option == "--width") width = value; else if (option == "--height") height = value; else if (option == "--fps") fps = value; else if (option == "--kbps") kbps = value;
      else throw std::runtime_error("Unknown option");
    }
    host::Runtime runtime; const int bitrate = kbps * 1000; bool passed = true;
    struct Backend { host::EncoderBackend backend; const char* name; };
    std::vector<Backend> backends{{host::EncoderBackend::x264, "x264"}, {host::EncoderBackend::mft, "mft"}};
    if (nvenc) backends.push_back({host::EncoderBackend::nvenc, "nvenc"});
    for (const auto& b : backends) {
      if (!only.empty() && only != b.name) continue;
      // A software encoder captures at 60 a second at most (Encoder::captureFps): that is its own rate here.
      const int own = host::hardwareEncoderBackend(b.backend) ? fps : std::min(fps, 60);
      const Run full = run(b.backend, width, height, fps, bitrate, own, false);
      for (const int paced : {own * 3 / 4, own / 2}) {
        const Run untold = run(b.backend, width, height, fps, bitrate, paced, false), told = run(b.backend, width, height, fps, bitrate, paced, true), early = run(b.backend, width, height, fps, bitrate, paced, true, true);
        // Told the paced rate, the encoder sends what it sends at its own rate (within a fifth), in frames as much larger as there are fewer.
        const bool rate = told.mbps >= 0.8 * full.mbps && told.mbps <= 1.2 * full.mbps, size = told.meanBytes >= 0.8 * full.meanBytes * own / paced;
        const bool atStart = early.mbps >= 0.8 * full.mbps && early.mbps <= 1.2 * full.mbps && !early.keys;
        passed = passed && rate && size && !told.keys && atStart;
        std::cout << "{\"kind\":\"encoder-frame-rate\",\"backend\":\"" << b.name << "\",\"width\":" << width << ",\"height\":" << height << ",\"fps\":" << fps << ",\"targetMbps\":" << bitrate / 1e6
                  << ",\"paced\":" << paced << ",\"atOwnRateMbps\":" << full.mbps << ",\"atOwnRateFrameBytes\":" << full.meanBytes << ",\"untoldMbps\":" << untold.mbps << ",\"untoldFrameBytes\":" << untold.meanBytes
                  << ",\"toldMbps\":" << told.mbps << ",\"toldFrameBytes\":" << told.meanBytes << ",\"toldLargestBytes\":" << told.largest << ",\"toldBeforeFirstFrameMbps\":" << early.mbps << ",\"keyFramesAdded\":" << told.keys
                  << ",\"followsTarget\":" << (rate ? "true" : "false") << ",\"largerFrames\":" << (size ? "true" : "false") << "}\n";
      }
    }
    std::cout << "{\"kind\":\"encoder-frame-rate-summary\",\"passed\":" << (passed ? "true" : "false") << ",\"actualEncoders\":true,\"syntheticPixels\":true}\n";
    return passed ? 0 : 1;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
