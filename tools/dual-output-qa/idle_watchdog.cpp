#include "media.hpp"
#include "frame_encode_policy.hpp"
#include "remote_decoder.h"
#include <array>
#include <iomanip>
#include <iostream>
#include <optional>
#include <thread>

namespace {
constexpr int width = 640, height = 360, fps = 30;
constexpr uint64_t durationUs = 18333333;
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
struct Change {
  const char* name; uint64_t dueUs; std::optional<uint64_t> submittedPts, encodedUs, decodedUs;
};
struct Event { const char* kind; uint64_t ptsUs, wallUs; bool draining = false, key = false; };
void logEvents(const std::vector<Event>& events) {
  for (const auto& event : events) std::cout << "{\"kind\":\"" << event.kind << "\",\"ptsUs\":" << event.ptsUs << ",\"wallUs\":" << event.wallUs << ",\"key\":" << (event.key ? "true" : "false") << ",\"draining\":" << (event.draining ? "true" : "false") << "}\n";
}
}
int wmain(int argc, wchar_t**) {
  std::vector<Event> events; events.reserve(1800);
  try {
    require(argc == 1, "No desktop/VM/target arguments accepted");
    require(SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS) != FALSE, "Set QA priority failed");
    host::Runtime runtime;
    host::Encoder encoder(width, height, fps, 4000000);
    host::FrameEncodePolicy gate(size_t(width) * height * 3 / 2);
    multitap::RemoteDecoder decoder; host::check(decoder.Initialize(width, height), "Initialize actual product decoder");
    std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2, 128);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) nv12[size_t(y) * width + x] = uint8_t(190 + ((x / 16 + y / 16) % 2) * 30);
    std::array<Change, 3> changes{{{"single-byte",500000,{}, {}, {}}, {"cursor-patch",1000000,{}, {}, {}}, {"last-change-then-idle",2000000,{}, {}, {}}}};
    size_t changeIndex = 0; uint64_t submitted = 0, encoded = 0, decoded = 0;
    std::vector<uint64_t> submittedPts, encodedPts, decodedPts;
    uint64_t lastEncodeUs = 0, lastDecodeUs = 0, maxEncodeGapUs = 0, maxDecodeGapUs = 0;
    std::optional<uint64_t> firstIdrUs, firstDecodedUs;
    bool draining = false, patchVisibleBeforeDrain = false;
    const auto beginning = host::Clock::now();
    const auto nowUs = [&] { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(host::Clock::now() - beginning).count()); };
    const auto receive = [&](multitap::DecodedFrame&& frame) {
      const auto now = nowUs();
      require(frame.width == width && frame.height == height && frame.rgba.size() == size_t(width) * height * 4, "Actual decoder frame geometry changed");
      if (decoded) maxDecodeGapUs = std::max(maxDecodeGapUs, now - lastDecodeUs);
      lastDecodeUs = now; ++decoded; if (!firstDecodedUs) firstDecodedUs = now;
      decodedPts.push_back(frame.pts_us);
      events.push_back({"paced-decoded-frame",frame.pts_us,now,draining});
      for (auto& change : changes) if (change.submittedPts && !change.decodedUs && frame.pts_us >= *change.submittedPts) change.decodedUs = now;
      if (!draining && changes[1].submittedPts && frame.pts_us >= *changes[1].submittedPts) {
        const auto at = (size_t(108) * width + 108) * 4;
        if (frame.rgba[at] < 100 && frame.rgba[at + 1] < 100 && frame.rgba[at + 2] < 100) patchVisibleBeforeDrain = true;
      }
    };
    const auto emit = [&](host::Frame frame) {
      const auto now = nowUs();
      require(gate.output(frame.ptsUs, frame.key, now), "Actual MFT output PTS was rejected by policy");
      if (encoded) maxEncodeGapUs = std::max(maxEncodeGapUs, now - lastEncodeUs);
      lastEncodeUs = now; ++encoded; if (frame.key && !firstIdrUs) firstIdrUs = now;
      encodedPts.push_back(frame.ptsUs);
      for (auto& change : changes) if (change.submittedPts && !change.encodedUs && frame.ptsUs >= *change.submittedPts) change.encodedUs = now;
      events.push_back({"paced-encoded-frame",frame.ptsUs,now,draining,frame.key});
      host::check(decoder.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, receive), "Actual product decoder rejected fixture");
    };
    for (uint64_t tick = 0;; ++tick) {
      std::this_thread::sleep_until(beginning + std::chrono::microseconds(tick * 1000000 / fps));
      const auto now = nowUs(); if (now >= durationUs) break;
      if (changeIndex < changes.size() && now >= changes[changeIndex].dueUs) {
        if (changeIndex == 0) nv12[size_t(41) * width + 61] ^= 1;
        else if (changeIndex == 1) for (int y = 100; y < 116; ++y) for (int x = 100; x < 116; ++x) nv12[size_t(y) * width + x] = 24;
        else nv12[size_t(55) * width + 73] ^= 1;
        changes[changeIndex].submittedPts = now; ++changeIndex;
      }
      const auto decision = gate.prepare(nv12, now);
      if (decision.submit) {
        submittedPts.push_back(now);
        events.push_back({"paced-submitted-frame",now,nowUs()});
        encoder.encode(nv12.data(), now, emit); ++submitted;
      }
    }
    const auto beforeDrainEncoded = encoded, beforeDrainDecoded = decoded, elapsedBeforeDrainUs = nowUs();
    require(changes.back().submittedPts && elapsedBeforeDrainUs - *changes.back().submittedPts >= 15000000, "Test did not remain idle past the 15-second watchdog");
    require(firstIdrUs && firstDecodedUs && *firstIdrUs < 1000000 && *firstDecodedUs < 1000000, "Usable initial frame was not available promptly before drain");
    require(!gate.awaitingOutput() && beforeDrainEncoded && beforeDrainDecoded, "Latest changed/refresh frame was stranded in encoder until drain");
    for (const auto& change : changes) require(change.encodedUs && change.decodedUs && *change.encodedUs - *change.submittedPts < 500000 && *change.decodedUs - *change.submittedPts < 500000, "A changed frame waited for the one-second refresh instead of immediate output");
    maxEncodeGapUs = std::max(maxEncodeGapUs, elapsedBeforeDrainUs - lastEncodeUs);
    maxDecodeGapUs = std::max(maxDecodeGapUs, elapsedBeforeDrainUs - lastDecodeUs);
    require(maxEncodeGapUs <= 2000000 && maxDecodeGapUs <= 2000000, "Static output exceeded the bounded two-second refresh gap");
    require(patchVisibleBeforeDrain, "Actual decoder did not show the generated cursor-like patch");
    draining = true; encoder.finish(emit); host::check(decoder.Drain(receive), "Drain actual decoder");
    require(encoded == submitted && decoded == encoded, "Encoded/decoded frame count mismatch");
    require(submittedPts == encodedPts && encodedPts == decodedPts, "Actual product encoder/decoder did not preserve submitted ordered PTS");
    // No pipe/console/file writes occur inside the paced region: a diagnostic
    // consumer applying stdout backpressure must not become codec latency.
    logEvents(events);
    std::cout << "{\"kind\":\"paced-idle-watchdog-qa\",\"passed\":true,\"width\":" << width << ",\"height\":" << height << ",\"fpsTarget\":" << fps << ",\"wallUsBeforeDrain\":" << elapsedBeforeDrainUs << ",\"idleAfterLastChangeUs\":" << elapsedBeforeDrainUs - *changes.back().submittedPts << ",\"submittedFrames\":" << submitted << ",\"encodedFrames\":" << encoded << ",\"decodedFrames\":" << decoded << ",\"encodedBeforeDrain\":" << beforeDrainEncoded << ",\"decodedBeforeDrain\":" << beforeDrainDecoded << ",\"firstUsableIdrUs\":" << *firstIdrUs << ",\"firstDecodedUs\":" << *firstDecodedUs << ",\"maxActualEncodeOutputGapUs\":" << maxEncodeGapUs << ",\"maxActualDecodedOutputGapUs\":" << maxDecodeGapUs << ",\"patchVisibleBeforeDrain\":true,\"changes\":[";
    for (size_t index = 0; index < changes.size(); ++index) {
      const auto& change = changes[index]; if (index) std::cout << ',';
      std::cout << "{\"name\":\"" << change.name << "\",\"submittedPtsUs\":" << *change.submittedPts << ",\"actualEncodeOutputWallUs\":" << *change.encodedUs << ",\"actualDecodeOutputWallUs\":" << *change.decodedUs << ",\"encodeChangeDelayUs\":" << *change.encodedUs - *change.submittedPts << ",\"decodeChangeDelayUs\":" << *change.decodedUs - *change.submittedPts << '}';
    }
    std::cout << "],\"orderedSubmittedEncodedDecodedPtsMatch\":true,\"actualProductEncoderDecoderPolicy\":true,\"desktopCaptured\":false,\"relayOrVmUsed\":false,\"serviceDriverRegistryChanged\":false,\"liveWatchdogAcceptance\":false}\n";
    return 0;
  } catch (const std::exception& error) { logEvents(events); std::cerr << error.what() << '\n'; return 1; }
}
