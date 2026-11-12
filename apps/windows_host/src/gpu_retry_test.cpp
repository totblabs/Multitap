// encoder_gpu_retry.hpp: the bounded way back to the GPU encoder after a
// transient failure. An injected GPU backend (an actual x264 encoder behind
// the hardware interface, as in gpu_h264_fallback_test.cpp) misses its output
// deadline and is opened again by an injected factory on an injected clock;
// every output goes through the actual client decoder. No desktop, GPU device
// or driver API is opened.
#include "gpu_h264.hpp"
#include "support.hpp"
#include "stream_prepare_diagnostics.hpp"
#include "remote_decoder.h"
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <tuple>

namespace host {
std::unique_ptr<EncoderImplementation> createX264Encoder(int, int, int, int, const X264Settings&);
struct EncoderGpuTestAccess {
  static void install(Encoder& encoder, std::unique_ptr<EncoderImplementation> implementation) {
    encoder.impl_ = std::move(implementation); encoder.backend_ = EncoderBackend::nvenc; encoder.gpuFallback_ = encoderGpuNone;
  }
  static void retry(Encoder& encoder, std::function<std::unique_ptr<EncoderImplementation>(int)> reopen, std::function<uint64_t()> clock) {
    encoder.gpuReopen_ = std::move(reopen); encoder.gpuRetryClock_ = std::move(clock);
  }
};
}
namespace {
using namespace host;
int checks = 0;
void require(bool value, const char* reason) { ++checks; if (!value) throw std::runtime_error(reason); }
constexpr int width = 320, height = 180, fps = 60, bitrate = 4000000;
constexpr uint64_t second = 1000000;
// Fails its `failAt`-th picture (0-based; -1 never) with `code`.
class InjectedGpu final : public EncoderImplementation {
 public:
  explicit InjectedGpu(int rate, int failAt = -1, uint32_t code = encoderGpuOutputTimeout) : cpu_(createX264Encoder(width, height, fps, rate, {})), failAt_(failAt), code_(code) {}
  void encode(const uint8_t* pixels, uint64_t pts, std::vector<Frame>& output) override {
    if (count_++ == failAt_) throw gpu::Failure(code_, "injected GPU failure");
    cpu_->encode(pixels, pts, output); verified_ = true;
  }
  void finish(std::vector<Frame>& output) override { cpu_->finish(output); }
  bool setBitrate(int value) override { return cpu_->setBitrate(value); }
  int reportedBitrate() const override { return cpu_->reportedBitrate(); }
  EncoderTuning reportedTuning() const override { return cpu_->reportedTuning(); }
  void requestKeyFrame() override { cpu_->requestKeyFrame(); }
  EncoderHardwareInfo hardwareInfo() const override {
    EncoderHardwareInfo result; result.initialized = true; result.hardwareVerified = verified_; result.adapterLuid = 7; result.vendorId = 0x10de; result.deviceId = 1; result.adapterName = "Injected GPU fixture"; return result;
  }
 private:
  std::unique_ptr<EncoderImplementation> cpu_; int failAt_, count_ = 0; uint32_t code_; bool verified_ = false;
};
std::vector<uint8_t> pixels(int frame) {
  std::vector<uint8_t> result(size_t(width) * height * 3 / 2, 128);
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) result[size_t(y) * width + x] = uint8_t(16 + (x + y + frame * 7) % 210);
  return result;
}
using Record = std::tuple<uint32_t, uint32_t, uint32_t>;
// One stream: an Encoder on the injected GPU backend, its clock, its records
// and the actual client decoder. step() codes the next picture and checks the
// stream contract: exactly one output at the input's pts, decodable in order.
struct Stream {
  Encoder encoder{width, height, fps, bitrate, {}, EncoderBackend::x264};
  multitap::RemoteDecoder decoder;
  uint64_t now = 100 * second; int frame = 0; int opens = 0;
  std::vector<uint64_t> encoded, decoded, keys; std::vector<Record> records;
  explicit Stream(std::unique_ptr<EncoderImplementation> first, std::function<std::unique_ptr<EncoderImplementation>(int)> reopen) {
    check(decoder.Initialize(width, height), "Initialize actual client software decoder");
    EncoderGpuTestAccess::install(encoder, std::move(first));
    if (reopen) EncoderGpuTestAccess::retry(encoder, [this, reopen = std::move(reopen)](int rate) { ++opens; return reopen(rate); }, [this] { return now; });
    else EncoderGpuTestAccess::retry(encoder, {}, [this] { return now; });
    encoder.setGpuRetryObserver([this](uint32_t kind, uint32_t attempt, uint32_t code) { records.emplace_back(kind, attempt, code); });
  }
  void step() {
    const auto source = pixels(frame); const uint64_t pts = uint64_t(frame) * second / fps; ++frame;
    const auto before = encoded.size();
    encoder.encode(source.data(), pts, [&](Frame output) {
      const auto unit = gpu::inspectAnnexB(output.bytes, encoderMaximumFrameBytes);
      require(unit.valid && output.key == unit.idr, "An output broke the Annex B contract");
      if (output.key) { require(unit.sps && unit.pps, "An IDR lost SPS/PPS"); keys.push_back(output.ptsUs); }
      encoded.push_back(output.ptsUs);
      check(decoder.Decode(output.bytes.data(), output.bytes.size(), output.ptsUs, [&](multitap::DecodedFrame&& shown) { require(shown.width == width && shown.height == height, "Decoded geometry changed"); decoded.push_back(shown.pts_us); }), "Decode actual H264 across the encoder change");
    });
    require(encoded.size() == before + 1 && encoded.back() == pts, "A picture was dropped, duplicated or retimestamped");
  }
  void steps(int count) { for (int n = 0; n < count; ++n) step(); }
  // The attempt opens on another thread: code pictures until it was taken (or
  // the bound). The clock does not move.
  bool until(const std::function<bool()>& done) {
    for (int n = 0; n < 3000 && !done(); ++n) { step(); if (!done()) Sleep(1); }
    return done();
  }
  void finish() {
    encoder.finish([&](Frame output) { encoded.push_back(output.ptsUs); });
    check(decoder.Drain([&](multitap::DecodedFrame&& shown) { decoded.push_back(shown.pts_us); }), "Drain actual client decoder");
    require(decoded == encoded, "The actual client decoder did not show every picture in order");
  }
};

void policy() {
  require(gpuRetryTransient(encoderGpuOutputTimeout, false) && gpuRetryTransient(encoderGpuOutputTimeout, true), "An output timeout is transient");
  require(!gpuRetryTransient(encoderGpuInitializationFailed, false) && gpuRetryTransient(encoderGpuInitializationFailed, true), "An open that did not complete is transient only for an attempt");
  for (const uint32_t code : {encoderGpuNotBuilt, encoderGpuAdapterUnavailable, encoderGpuVendorUnsupported, encoderGpuDriverUnavailable, encoderGpuApiUnsupported, encoderGpuCapabilityUnsupported,
       encoderGpuAllocationFailed, encoderGpuSubmissionFailed, encoderGpuOutputContract, encoderGpuDeviceLost, encoderGpuReconfigureFailed})
    require(!gpuRetryTransient(code, false) && !gpuRetryTransient(code, true), "A lasting GPU failure was called transient");
  require(gpuRetryCodeOutputTimeout == encoderGpuOutputTimeout && gpuRetryCodeInitializationFailed == encoderGpuInitializationFailed, "The policy's codes differ from EncoderGpuFallback");
  GpuRetrySchedule schedule;
  require(!schedule.pending() && !schedule.due(UINT64_MAX), "A new schedule had an attempt");
  schedule.left(false, 1000); require(!schedule.pending(), "A lasting failure scheduled an attempt");
  uint64_t now = 5 * second; const uint64_t spacing[] = {30 * second, 60 * second, 120 * second};
  for (uint32_t attempt = 1; attempt <= gpuRetryMaximumAttempts; ++attempt) {
    schedule.left(true, now);
    require(schedule.pending() && !schedule.due(now + spacing[attempt - 1] - 1) && schedule.due(now + spacing[attempt - 1]), "The spacing is not 30 s, 60 s, 120 s");
    now += spacing[attempt - 1];
    require(schedule.begin() == attempt && !schedule.pending() && !schedule.due(UINT64_MAX), "A started attempt stayed due");
  }
  schedule.left(true, now); require(!schedule.pending() && schedule.attempts() == gpuRetryMaximumAttempts, "More attempts than the maximum");
  require(gpuRetryRecordValue(gpuRetryOutputFailed, 2, encoderGpuOutputTimeout) == (4u | 2u << 8 | 41u << 16), "The fixed record value changed");
  // The service journals the worker's stage 45 as hostAction 200 stage 245: ok for a start and a return only.
  require(streamObservationOk(45, gpuRetryRecordValue(gpuRetryStarted, 1, encoderGpuOutputTimeout)) && streamObservationOk(45, gpuRetryRecordValue(gpuRetryReturned, 3, 0)), "A start or a return was journalled as a failure");
  for (const uint32_t kind : {uint32_t(gpuRetryOpenFailed), uint32_t(gpuRetryOutputFailed), uint32_t(gpuRetryEnded)}) require(!streamObservationOk(45, gpuRetryRecordValue(kind, 2, encoderGpuDeviceLost)), "A failed attempt was journalled as ok");
}

// The live case: a missed output deadline, x264 meanwhile, NVENC again at an IDR.
void timeoutThenBack() {
  Stream stream(std::make_unique<InjectedGpu>(bitrate, 3), [](int rate) { return std::make_unique<InjectedGpu>(rate); });
  stream.steps(3);
  require(stream.encoder.backend() == EncoderBackend::nvenc && stream.keys == std::vector<uint64_t>{0}, "The injected GPU encoder did not run first");
  stream.step();  // the deadline is missed: x264 codes this picture as an IDR
  require(stream.encoder.backend() == EncoderBackend::x264 && stream.encoder.hardwareInfo().gpuFallbackCode == encoderGpuOutputTimeout && stream.keys.size() == 2 && stream.keys.back() == 3 * second / fps, "The timeout did not fall back at the failed picture");
  stream.now += 30 * second - 1; stream.steps(5);
  require(stream.opens == 0 && stream.records.empty() && stream.encoder.backend() == EncoderBackend::x264 && stream.keys.size() == 2, "An attempt began before its time");
  stream.now += 1; stream.step();
  require(stream.records == std::vector<Record>{{gpuRetryStarted, 1, encoderGpuOutputTimeout}} && stream.encoder.backend() == EncoderBackend::x264 && stream.keys.size() == 2, "The attempt did not begin at 30 s, or changed the stream when it began");
  require(stream.until([&] { return stream.encoder.backend() == EncoderBackend::nvenc; }), "The stream did not return to the GPU encoder");
  const auto state = stream.encoder.hardwareInfo();
  require(stream.opens == 1 && state.gpuFallbackCode == encoderGpuNone && state.gpuFailure == EncoderGpuFailure{} && state.hardwareVerified && state.vendorId == 0x10de, "The returned encoder kept the failure or lost its identity");
  require(stream.keys.size() == 3 && stream.keys.back() == stream.encoded.back(), "The change back was not exactly one IDR at the picture that changed");
  require(stream.records == (std::vector<Record>{{gpuRetryStarted, 1, encoderGpuOutputTimeout}, {gpuRetryReturned, 1, encoderGpuNone}}), "The return was not recorded");
  stream.now += 3600 * second; stream.steps(10);
  require(stream.opens == 1 && stream.keys.size() == 3 && stream.records.size() == 2, "A running GPU encoder was opened again");
  stream.finish();
}

// Failed attempts change nothing in the stream, grow their spacing and end.
void failedAttemptsKeepCpu() {
  int call = 0;
  Stream stream(std::make_unique<InjectedGpu>(bitrate, 2), [&call](int rate) -> std::unique_ptr<EncoderImplementation> {
    switch (++call) {
      case 1: throw gpu::Failure(encoderGpuInitializationFailed, "injected busy open");
      case 2: return std::make_unique<InjectedGpu>(rate, 0);  // opens, misses its first deadline
      default: throw gpu::Failure(encoderGpuDeviceLost, "injected admission fence");
    }
  });
  stream.steps(4);
  require(stream.encoder.backend() == EncoderBackend::x264 && stream.keys.size() == 2, "Fixture: not on the CPU after the timeout");
  const auto keys = stream.keys;
  stream.now += 30 * second;
  require(stream.until([&] { return stream.records.size() >= 2; }), "The first attempt did not finish");
  require(stream.records == (std::vector<Record>{{gpuRetryStarted, 1, encoderGpuOutputTimeout}, {gpuRetryOpenFailed, 1, encoderGpuInitializationFailed}}), "A busy open was not recorded as a failed attempt that may be repeated");
  stream.now += 60 * second - 1; stream.steps(5);
  require(stream.opens == 1, "The second attempt did not wait twice as long");
  stream.now += 1;
  require(stream.until([&] { return stream.records.size() >= 4; }), "The second attempt did not finish");
  require(stream.records[2] == Record{gpuRetryStarted, 2, encoderGpuOutputTimeout} && stream.records[3] == Record{gpuRetryOutputFailed, 2, encoderGpuOutputTimeout}, "A missed first deadline of the attempt was not recorded");
  stream.now += 120 * second - 1; stream.steps(5);
  require(stream.opens == 2, "The third attempt did not wait four times as long");
  stream.now += 1;
  require(stream.until([&] { return stream.records.size() >= 7; }), "The third attempt did not finish");
  require(stream.records[4] == Record{gpuRetryStarted, 3, encoderGpuOutputTimeout} && stream.records[5] == Record{gpuRetryOpenFailed, 3, encoderGpuDeviceLost} && stream.records[6] == Record{gpuRetryEnded, 3, encoderGpuDeviceLost}, "A lost device did not end the attempts");
  stream.now += 100000 * second; stream.steps(20);
  require(stream.opens == 3 && stream.records.size() == 7, "An attempt followed the end");
  const auto state = stream.encoder.hardwareInfo();
  require(stream.encoder.backend() == EncoderBackend::x264 && state.gpuFallbackCode == encoderGpuOutputTimeout && stream.keys == keys, "A failed attempt changed the encoder, its recorded cause or added an IDR");
  stream.finish();
}

// Three attempts at most, also when each one succeeds and then times out again.
void attemptsAreBounded() {
  Stream stream(std::make_unique<InjectedGpu>(bitrate, 1), [](int rate) { return std::make_unique<InjectedGpu>(rate, 2); });
  stream.steps(3);
  const uint64_t spacing[] = {30 * second, 60 * second, 120 * second};
  for (int attempt = 1; attempt <= 3; ++attempt) {
    require(stream.encoder.backend() == EncoderBackend::x264, "Fixture: not on the CPU before the attempt");
    stream.now += spacing[attempt - 1] - 1; stream.steps(3);
    require(stream.opens == attempt - 1, "An attempt came before its growing spacing");
    stream.now += 1;
    require(stream.until([&] { return stream.encoder.backend() == EncoderBackend::nvenc; }) && stream.opens == attempt, "An attempt did not return to the GPU encoder");
    stream.steps(3);  // its third picture misses the deadline again
  }
  require(stream.encoder.backend() == EncoderBackend::x264, "Fixture: the last GPU encoder did not time out");
  stream.now += 100000 * second; stream.steps(20);
  require(stream.opens == 3 && stream.encoder.backend() == EncoderBackend::x264, "A fourth attempt was made");
  stream.finish();
}

// Lasting failures, and encoders that cannot reopen, never try again.
void lastingFailuresStay() {
  for (const uint32_t code : {uint32_t(encoderGpuDeviceLost), uint32_t(encoderGpuOutputContract), uint32_t(encoderGpuSubmissionFailed), uint32_t(encoderGpuCapabilityUnsupported)}) {
    Stream stream(std::make_unique<InjectedGpu>(bitrate, 1, code), [](int rate) { return std::make_unique<InjectedGpu>(rate); });
    stream.steps(3); stream.now += 100000 * second; stream.steps(10);
    require(stream.opens == 0 && stream.records.empty() && stream.encoder.backend() == EncoderBackend::x264 && stream.encoder.hardwareInfo().gpuFallbackCode == code, "A lasting GPU failure was tried again");
    stream.finish();
  }
  {
    Stream stream(std::make_unique<InjectedGpu>(bitrate, 1), {});  // no way to reopen
    stream.steps(3); stream.now += 100000 * second; stream.steps(10);
    require(stream.records.empty() && stream.encoder.backend() == EncoderBackend::x264, "An encoder without a reopen path tried again");
    stream.finish();
  }
  {
    // A refused live bitrate is a reconfiguration failure, not a timeout.
    class Refusing final : public EncoderImplementation {
     public:
      void encode(const uint8_t* p, uint64_t pts, std::vector<Frame>& out) override { inner_.encode(p, pts, out); }
      void finish(std::vector<Frame>& out) override { inner_.finish(out); }
      bool setBitrate(int) override { return false; }
      int reportedBitrate() const override { return inner_.reportedBitrate(); }
      EncoderTuning reportedTuning() const override { return inner_.reportedTuning(); }
      void requestKeyFrame() override { inner_.requestKeyFrame(); }
      EncoderHardwareInfo hardwareInfo() const override { return inner_.hardwareInfo(); }
     private:
      InjectedGpu inner_{bitrate};
    };
    Stream stream(std::make_unique<Refusing>(), [](int rate) { return std::make_unique<InjectedGpu>(rate); });
    stream.steps(2); require(stream.encoder.setBitrate(8000000), "The CPU encoder did not take the refused target");
    stream.steps(2); stream.now += 100000 * second; stream.steps(10);
    require(stream.opens == 0 && stream.encoder.backend() == EncoderBackend::x264 && stream.encoder.hardwareInfo().gpuFallbackCode == encoderGpuReconfigureFailed, "A refused reconfiguration was tried again");
    stream.finish();
  }
}

// The target moved while the attempt was opening: the GPU encoder takes the current one.
void bitrateMovesWhileOpening() {
  std::promise<void> gate; auto opened = gate.get_future().share();
  Stream stream(std::make_unique<InjectedGpu>(bitrate, 1), [opened](int rate) { opened.wait(); return std::make_unique<InjectedGpu>(rate); });
  stream.steps(3); stream.now += 30 * second; stream.step();
  require(stream.records.size() == 1 && stream.encoder.backend() == EncoderBackend::x264, "Fixture: the attempt did not begin");
  require(stream.encoder.setBitrate(8000000), "The CPU encoder refused the new target");
  stream.steps(5);
  require(stream.encoder.backend() == EncoderBackend::x264 && stream.records.size() == 1, "An attempt that had not opened yet was taken");
  gate.set_value();
  require(stream.until([&] { return stream.encoder.backend() == EncoderBackend::nvenc; }), "The stream did not return to the GPU encoder");
  require(stream.encoder.bitrate() == 8000000 && stream.encoder.reportedBitrate() == 8000000, "The returned GPU encoder runs at the old target");
  stream.finish();
}

// An Encoder that ends while its attempt is still opening joins it.
void endsWhileOpening() {
  std::atomic<bool> returned{false};
  {
    const auto gate = std::make_shared<std::promise<void>>(); auto opened = gate->get_future().share();
    Stream stream(std::make_unique<InjectedGpu>(bitrate, 1), [opened, &returned](int rate) { opened.wait(); auto value = std::make_unique<InjectedGpu>(rate); returned = true; return value; });
    stream.steps(3); stream.now += 30 * second; stream.step();
    require(stream.records.size() == 1, "Fixture: the attempt did not begin");
    std::thread release([gate] { Sleep(50); gate->set_value(); });
    release.detach();
  }
  require(returned.load(), "The Encoder ended before its opening attempt did");
}
}
int main() {
  const struct { const char* name; void (*run)(); } cases[] = {{"policy", policy}, {"timeout-then-back", timeoutThenBack}, {"failed-attempts-keep-cpu", failedAttemptsKeepCpu},
    {"attempts-are-bounded", attemptsAreBounded}, {"lasting-failures-stay", lastingFailuresStay}, {"bitrate-moves-while-opening", bitrateMovesWhileOpening}, {"ends-while-opening", endsWhileOpening}};
  unsigned passed = 0, failed = 0;
  try {
    host::Runtime runtime;
    for (const auto& value : cases) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << "FAILED " << value.name << ": " << error.what() << '\n'; }
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  std::cout << "{\"kind\":\"gpu-encoder-retry\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << ",\"actualGpu\":false,\"gpuFaultInjected\":true,\"actualCpuEncoder\":true,\"actualClientDecoder\":true,\"injectedClock\":true}\n";
  return failed ? 1 : 0;
}
