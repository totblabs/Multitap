#include "media.hpp"
#include "frame_encode_policy.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <latch>
#include <numeric>
#include <sstream>
#include <thread>

namespace {
constexpr int width = 1920, height = 1080, phases = 8, totalBitrate = 12000000;
using Bytes = std::vector<uint8_t>;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
uint64_t filetime(FILETIME value) { return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; }
double processCpuMs() {
  FILETIME created{}, exited{}, kernel{}, user{};
  require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE, "Process CPU measurement failed");
  return double(filetime(kernel) + filetime(user)) / 10000.0;
}
double elapsedMs(host::Clock::time_point start) { return std::chrono::duration<double, std::milli>(host::Clock::now() - start).count(); }
std::string quote(const std::string& value) {
  std::ostringstream out; out << '"';
  for (const auto c : value) { if (c == '"' || c == '\\') out << '\\'; require(static_cast<unsigned char>(c) >= 32, "Unsafe fixture identifier"); out << c; }
  return out.str() + '"';
}
Bytes frame(int phase, int output) {
  Bytes bytes(size_t(width) * height * 3 / 2);
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    uint8_t luma = 225;
    if (y < 68) luma = uint8_t(40 + output * 40);
    else if (y < 320) luma = ((x / 11 + y / 7 + output) % 9 < 3) ? uint8_t(45) : uint8_t(225);
    else {
      const int moving = x + phase * 13 + output * 97;
      luma = uint8_t(32 + ((moving / 16 + y / 12 + (moving / 96) * 7) % 190));
    }
    bytes[size_t(y) * width + x] = luma;
  }
  for (int y = 0; y < height / 2; ++y) for (int x = 0; x < width; x += 2) {
    const auto at = size_t(width) * height + size_t(y) * width + x;
    bytes[at] = y < 160 ? uint8_t(128) : uint8_t(80 + ((x / 64 + phase * 3 + output * 29) % 96));
    bytes[at + 1] = y < 160 ? uint8_t(128) : uint8_t(80 + ((y / 32 + phase * 5 + output * 17) % 96));
  }
  return bytes;
}
Bytes composite(const Bytes& left, const Bytes& right) {
  Bytes bytes(size_t(width * 2) * height * 3 / 2);
  // NV12 luma and interleaved chroma both have width bytes per source row.
  for (int row = 0; row < height * 3 / 2; ++row) {
    const auto to = bytes.data() + size_t(row) * width * 2;
    std::memcpy(to, left.data() + size_t(row) * width, width);
    std::memcpy(to + width, right.data() + size_t(row) * width, width);
  }
  return bytes;
}
struct Fixtures {
  std::array<std::array<Bytes, phases>, 2> outputs;
  std::array<std::array<Bytes, phases>, 3> combined;
  Fixtures() {
    for (int output = 0; output < 2; ++output) for (int n = 0; n < phases; ++n) outputs[output][n] = frame(n, output);
    for (int scene = 0; scene < 3; ++scene) for (int n = 0; n < phases; ++n)
      combined[scene][n] = composite(outputs[0][scene < 2 ? n : 0], outputs[1][scene == 0 ? (n * 3) % phases : 0]);
    // Assert exact equal source pixels between both architectures outside the
    // measured interval. No screen readback or RGB conversion is involved.
    for (int scene = 0; scene < 3; ++scene) for (int n = 0; n < phases; ++n) {
      const auto& a = outputs[0][scene < 2 ? n : 0]; const auto& b = outputs[1][scene == 0 ? (n * 3) % phases : 0];
      const auto& joined = combined[scene][n];
      for (int row = 0; row < height * 3 / 2; ++row) {
        require(!std::memcmp(joined.data() + size_t(row) * width * 2, a.data() + size_t(row) * width, width), "Composite left fixture mismatch");
        require(!std::memcmp(joined.data() + size_t(row) * width * 2 + width, b.data() + size_t(row) * width, width), "Composite right fixture mismatch");
      }
    }
  }
  const Bytes& get(bool separate, int scene, int output, int tick) const {
    if (!separate) return combined[scene][tick % phases];
    return outputs[output][output == 0 ? (scene < 2 ? tick % phases : 0) : (scene == 0 ? (tick * 3) % phases : 0)];
  }
};
struct Track {
  std::exception_ptr error;
  std::vector<host::Frame> frames;
  int submitted = 0; uint64_t bytes = 0; int profile = 0;
  int changeObservations = 0, refreshObservations = 0, beforeDrainFrames = 0;
  bool usableIdrBeforeDrain = false;
  uint64_t maxOutputGapUs = 0;
};
const char* sceneName(int scene) { return scene == 0 ? "both-motion" : scene == 1 ? "one-motion-one-static" : "both-static"; }
bool changed(int scene, bool separate, int output) { return separate ? (output == 0 ? scene < 2 : scene == 0) : scene < 2; }
void sample(const std::filesystem::path& directory, const Fixtures& fixtures, int fps, int seconds, int repeat, int scene, bool gated, bool separate) {
  const int tracks = separate ? 2 : 1, ticks = fps * seconds;
  const auto name = std::string(separate ? "two-tracks" : "composite") + '-' + sceneName(scene) + '-' + (gated ? "changed-gated" : "unconditional") + "-r" + std::to_string(repeat);
  std::array<Track, 2> results;
  std::latch ready(tracks), start(1), finished(tracks), teardown(1);
  std::vector<std::jthread> workers;
  const auto initWall = host::Clock::now(); const double initCpu = processCpuMs();
  for (int output = 0; output < tracks; ++output) workers.emplace_back([&, output] {
    bool announced = false, completed = false;
    try {
      host::Runtime runtime;
      host::Encoder encoder(separate ? width : width * 2, height, fps, totalBitrate / tracks);
      host::FrameEncodePolicy gate(size_t(separate ? width : width * 2) * height * 3 / 2);
      results[output].profile = int(encoder.reportedTuning().profile);
      require(results[output].profile == eAVEncH264VProfile_Base, "Actual product baseline changed");
      ready.count_down(); announced = true; start.wait();
      uint64_t nowUs = 0, lastOutputClockUs = 0; bool hadOutput = false, draining = false;
      const auto emit = [&](host::Frame value) {
        if (gated) require(gate.output(value.ptsUs, value.key, nowUs), "Actual output PTS rejected by product policy");
        if (hadOutput) results[output].maxOutputGapUs = std::max(results[output].maxOutputGapUs, nowUs - lastOutputClockUs);
        lastOutputClockUs = nowUs; hadOutput = true;
        if (!draining && value.key) results[output].usableIdrBeforeDrain = true;
        results[output].bytes += value.bytes.size(); results[output].frames.push_back(std::move(value));
      };
      for (int tick = 0; tick < ticks; ++tick) {
        nowUs = uint64_t(tick) * 1000000 / fps;
        const auto& input = fixtures.get(separate, scene, output, tick);
        if (gated) {
          const auto decision = gate.prepare(input, nowUs);
          results[output].changeObservations += int(decision.contentChanged);
          results[output].refreshObservations += int(decision.refresh);
          if (!decision.submit) continue;
        }
        encoder.encode(input.data(), nowUs, emit); ++results[output].submitted;
      }
      results[output].beforeDrainFrames = int(results[output].frames.size());
      draining = true; nowUs = uint64_t(seconds) * 1000000;
      encoder.finish(emit); finished.count_down(); completed = true; teardown.wait();
    } catch (...) {
      results[output].error = std::current_exception();
      if (!announced) ready.count_down();
      if (!completed) finished.count_down();
    }
  });
  ready.wait(); const double initializationMs = elapsedMs(initWall), initializationCpuMs = processCpuMs() - initCpu;
  const double beginningCpu = processCpuMs(); const auto beginningWall = host::Clock::now();
  start.count_down(); finished.wait();
  const double wallMs = elapsedMs(beginningWall), cpuMs = processCpuMs() - beginningCpu;
  teardown.count_down(); workers.clear();
  for (int output = 0; output < tracks; ++output) if (results[output].error) std::rethrow_exception(results[output].error);
  uint64_t totalBytes = 0; int totalFrames = 0, totalSubmitted = 0;
  std::ostringstream trackJson; trackJson << '[';
  for (int output = 0; output < tracks; ++output) {
    auto& result = results[output];
    const bool alwaysSubmits = !gated || changed(scene, separate, output);
    require(result.submitted > 0 && result.submitted <= ticks && (!alwaysSubmits || result.submitted == ticks) && int(result.frames.size()) == result.submitted && result.bytes > 0, "Encoder did not emit every submitted frame");
    require(result.beforeDrainFrames > 0 && result.usableIdrBeforeDrain, "Usable IDR was only released by final drain");
    const auto filename = name + "-t" + std::to_string(output) + ".h264"; const auto path = directory / filename;
    require(!std::filesystem::exists(path), "Refusing to replace a benchmark stream");
    std::ofstream file(path, std::ios::binary);
    for (const auto& encoded : result.frames) file.write(reinterpret_cast<const char*>(encoded.bytes.data()), std::streamsize(encoded.bytes.size()));
    require(bool(file), "Write isolated output failed");
    if (output) trackJson << ',';
    trackJson << "{\"file\":" << quote(filename) << ",\"width\":" << (separate ? width : width * 2) << ",\"height\":" << height << ",\"submittedFrames\":" << result.submitted << ",\"encodedFrames\":" << result.frames.size() << ",\"bytes\":" << result.bytes << ",\"profile\":" << result.profile << ",\"targetBitrate\":" << totalBitrate / tracks << ",\"changeObservations\":" << result.changeObservations << ",\"refreshObservations\":" << result.refreshObservations << ",\"beforeDrainFrames\":" << result.beforeDrainFrames << ",\"usableIdrBeforeDrain\":" << (result.usableIdrBeforeDrain ? "true" : "false") << ",\"maxSyntheticOutputGapUs\":" << result.maxOutputGapUs << '}';
    totalBytes += result.bytes; totalFrames += int(result.frames.size()); totalSubmitted += result.submitted;
  }
  trackJson << ']';
  std::cout << std::fixed << std::setprecision(6) << "{\"kind\":\"dual-output-encode-sample\",\"name\":" << quote(name) << ",\"architecture\":" << quote(separate ? "two-tracks" : "composite") << ",\"scene\":" << quote(sceneName(scene)) << ",\"policy\":" << quote(gated ? "changed-gated" : "unconditional") << ",\"repeat\":" << repeat << ",\"fpsTarget\":" << fps << ",\"mediaSeconds\":" << seconds << ",\"tickCount\":" << ticks << ",\"sourcePixelsPerTick\":" << width * 2 * height << ",\"totalTargetBitrate\":" << totalBitrate << ",\"encodeCpuMs\":" << cpuMs << ",\"encodeWallMs\":" << wallMs << ",\"initializationCpuMs\":" << initializationCpuMs << ",\"initializationWallMs\":" << initializationMs << ",\"oneCoreEquivalentPercent\":" << cpuMs / (seconds * 10) << ",\"unpacedMediaSecondsPerWallSecond\":" << seconds * 1000 / wallMs << ",\"submittedFrames\":" << totalSubmitted << ",\"encodedFrames\":" << totalFrames << ",\"encodedBytes\":" << totalBytes << ",\"actualMediaMbps\":" << double(totalBytes) * 8 / (seconds * 1000000.0) << ",\"tracks\":" << trackJson.str() << "}\n" << std::flush;
}
}
int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 5 && std::filesystem::is_directory(argv[1]), "Usage: QA_EXE EXISTING_NEW_OUTPUT_DIR FPS REPEATS MEDIA_SECONDS");
    const int fps = std::stoi(argv[2]), repeats = std::stoi(argv[3]), seconds = std::stoi(argv[4]);
    require((fps == 30 || fps == 60) && repeats >= 2 && repeats <= 3 && seconds >= 2 && seconds <= 3, "Benchmark bounds exceeded");
    require(SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS) != FALSE && GetPriorityClass(GetCurrentProcess()) == BELOW_NORMAL_PRIORITY_CLASS, "Could not verify below-normal QA priority");
    DWORD_PTR available = 0, system = 0, selected = 0; unsigned selectedCount = 0;
    require(GetProcessAffinityMask(GetCurrentProcess(), &available, &system) != FALSE, "Could not inspect QA affinity");
    for (unsigned bit = 0; bit < sizeof(available) * 8 && selectedCount < 4; ++bit) if (available & (DWORD_PTR(1) << bit)) { selected |= DWORD_PTR(1) << bit; ++selectedCount; }
    require(selectedCount > 0 && SetProcessAffinityMask(GetCurrentProcess(), selected) != FALSE, "Could not restrict QA affinity");
    DWORD_PTR verified = 0; require(GetProcessAffinityMask(GetCurrentProcess(), &verified, &system) != FALSE && verified == selected, "Could not verify QA affinity");
    host::Runtime runtime;
    { Bytes warmup(640 * 360 * 3 / 2, 128); host::Encoder encoder(640, 360, fps, 4000000); const auto discard = [](host::Frame) {}; for (unsigned n = 0; n < 2; ++n) encoder.encode(warmup.data(), uint64_t(n) * 1000000 / fps, discard); encoder.finish(discard); }
    const Fixtures fixtures;
    std::cout << "{\"kind\":\"dual-output-encode-config\",\"fpsTarget\":" << fps << ",\"repeats\":" << repeats << ",\"mediaSeconds\":" << seconds << ",\"affinityLogicalCpusVerified\":" << selectedCount << ",\"belowNormalPriorityVerified\":true,\"unpaced\":true,\"concurrentIndependentEncoders\":true,\"fixturesPrecomputedAndEqual\":true,\"policyImplementation\":\"product-frame-encode-policy-v1\",\"actualDirtyComparisonMeasured\":true,\"policyClock\":\"synthetic media microseconds, including actual MFT output callbacks\",\"captureNetworkPresentationMeasured\":false,\"realDualSessionTested\":false,\"warmupFramesExcluded\":2}\n" << std::flush;
    for (int repeat = 0; repeat < repeats; ++repeat) for (int index = 0; index < 3; ++index) {
      const int scene = repeat % 2 ? 2 - index : index;
      for (int policy = 0; policy < 2; ++policy) for (int order = 0; order < 2; ++order)
        sample(argv[1], fixtures, fps, seconds, repeat, scene, bool(policy ^ (repeat % 2)), bool(order ^ (repeat % 2)));
    }
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
