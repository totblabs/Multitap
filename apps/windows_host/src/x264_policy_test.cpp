// Pure protected-policy fixtures and real H.264 encode/decode on synthetic pixels.
// No machine settings, desktop, input, GPU or installed service is changed.
#include "x264_policy.hpp"
#include "x264_encoder_testing.hpp"
#include "gpu_h264.hpp"
#include "remote_decoder.h"
#include <array>
#include <iostream>

namespace host {
struct EncoderGpuTestAccess {
  static void install(Encoder& encoder, std::unique_ptr<EncoderImplementation> implementation) {
    encoder.impl_ = std::move(implementation); encoder.backend_ = EncoderBackend::nvenc;
  }
};
}
namespace {
int checks = 0, settingsReads = 0;
struct Open { int requested = 0, applied = 0; };
std::array<Open, 16> opens{};
size_t openCount = 0;
host::X264Settings selected;
void require(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
host::EncoderBackend cpuBackend() { return host::EncoderBackend::x264; }
host::X264Settings cpuSettings() { ++settingsReads; return selected; }
void observeOpen(int requested, int applied) noexcept { if (openCount < opens.size()) opens[openCount++] = {requested, applied}; }
struct Fixture {
  Fixture() {
    require(!host::x264testing::configurationProvider.load() && !host::x264testing::settingsProvider.load() && !host::x264testing::openObserver.load(), "Policy fixture already active");
    host::x264testing::configurationProvider.store(cpuBackend);
    host::x264testing::settingsProvider.store(cpuSettings);
    host::x264testing::openObserver.store(observeOpen);
  }
  ~Fixture() {
    host::x264testing::openObserver.store(nullptr);
    host::x264testing::settingsProvider.store(nullptr);
    host::x264testing::configurationProvider.store(nullptr);
  }
};
void policy() {
  const std::array<const char*, 13> absentOrInvalid{{"", "{}", "[]", "null", "{", R"({"x264Threads":null})", R"({"x264Threads":true})", R"({"x264Threads":"2"})", R"({"x264Threads":0})", R"({"x264Threads":-1})", R"({"x264Threads":9})", R"({"x264Threads":1.5})", R"({"x264Threads":1e100})"}};
  for (const auto* json : absentOrInvalid) require(host::x264policy::fromConfig(json).threads == 0, "Invalid policy changed legacy automatic threads");
  for (int threads = 1; threads <= 8; ++threads) {
    const auto settings = host::x264policy::fromConfig("{\"videoEncoder\":\"auto\",\"x264Threads\":" + std::to_string(threads) + '}');
    require(settings.threads == threads && settings.preset == "ultrafast" && settings.profile == "baseline", "Bounded policy changed encoder tuning");
  }
  const auto valid = R"({"x264Threads":2})";
  require(host::x264policy::fromConfig(std::string(host::x264policy::maximumConfigBytes, ' ') + valid).threads == 0, "Oversize policy accepted");
  int reads = 0;
  require(host::x264policy::readProtected(false, [&] { ++reads; return std::string(valid); }).threads == 0 && reads == 0, "Untrusted policy was read or applied");
  require(host::x264policy::readProtected(true, [&] { ++reads; return std::string(valid); }).threads == 2 && reads == 1, "Trusted bounded policy was ignored");
  require(host::x264policy::readProtected(true, []() -> std::string { throw std::runtime_error("read failed"); }).threads == 0, "Read failure changed legacy automatic threads");
}
class FailedGpu final : public host::EncoderImplementation {
 public:
  void encode(const uint8_t*, uint64_t, std::vector<host::Frame>&) override { throw host::gpu::Failure(host::encoderGpuDeviceLost, "injected GPU failure"); }
  void finish(std::vector<host::Frame>&) override {}
  bool setBitrate(int) override { return false; }
  int reportedBitrate() const override { return 4000000; }
  host::EncoderTuning reportedTuning() const override { return {}; }
  void requestKeyFrame() override {}
};
void codec(int limit, bool failGpu) {
  selected = host::x264policy::fromConfig("{\"x264Threads\":" + std::to_string(limit) + '}');
  const auto before = openCount; const int readsBefore = settingsReads;
  constexpr int width = 320, height = 180;
  host::Encoder encoder(width, height, 30, 4000000);
  if (failGpu) host::EncoderGpuTestAccess::install(encoder, std::make_unique<FailedGpu>());
  multitap::RemoteDecoder decoder; host::check(decoder.Initialize(width, height), "Initialize actual client decoder");
  std::vector<uint8_t> pixels(size_t(width) * height * 3 / 2, 128);
  std::vector<uint64_t> submitted, decoded;
  const auto shown = [&](multitap::DecodedFrame&& frame) { require(frame.width == width && frame.height == height, "Policy changed decoded geometry"); decoded.push_back(frame.pts_us); };
  const auto output = [&](host::Frame frame) {
    require(frame.key == submitted.empty(), "Policy changed first-frame IDR contract");
    submitted.push_back(frame.ptsUs);
    host::check(decoder.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, shown), "Decode actual bounded-thread H264");
  };
  for (int n = 0; n < 4; ++n) {
    std::fill_n(pixels.begin(), size_t(width) * height, uint8_t(32 + n * 20));
    encoder.encode(pixels.data(), uint64_t(n) * 1000000 / 30, output);
  }
  encoder.finish(output); host::check(decoder.Drain(shown), "Drain actual client decoder");
  require(submitted == decoded && decoded.size() == 4, "Bounded threads or GPU fallback dropped pictures");
  const size_t expected = failGpu ? 2 : 1;
  require(openCount == before + expected && settingsReads == readsBefore + int(expected), "Normal/fallback production path did not load policy consistently");
  for (size_t i = before; i < openCount; ++i)
    require(opens[i].requested == limit && opens[i].applied >= 1 && opens[i].applied <= limit, "Actual x264 threads exceeded the protected cap");
  require(encoder.backend() == host::EncoderBackend::x264, "Policy unexpectedly selected a different encoder");
  if (failGpu) require(encoder.hardwareInfo().gpuFallbackCode == host::encoderGpuDeviceLost, "Policy lost GPU fallback reason");
}
}
int main() {
  try {
    policy(); host::Runtime runtime; Fixture fixture;
    codec(1, false); codec(2, false); codec(8, false); codec(2, true);
    std::cout << "{\"policyCases\":25,\"realCodecCases\":4,\"checks\":" << checks << ",\"actualX264Opens\":" << openCount << ",\"settingsProviderCalls\":" << settingsReads << ",\"actualClientDecoder\":true,\"syntheticPixels\":true,\"settingsRead\":false,\"actualGpu\":false,\"desktopCaptured\":false,\"pass\":true}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
