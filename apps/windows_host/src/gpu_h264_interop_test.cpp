// Explicit actual-hardware test, run separately on the selected
// physical Host. Never part of an automatic GPU-less smoke-test run.
#include "gpu_h264.hpp"
#include "remote_decoder.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
void require(bool value, const char* reason) { if (!value) throw host::gpu::Failure(host::encoderGpuOutputContract, reason); }
uint32_t number(const char* text) {
  const std::string value(text);
  if (value.empty() || value.size() > 10 || !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })) throw std::runtime_error("Invalid numeric test argument");
  const uint64_t parsed = std::stoull(value); if (parsed > uint64_t(INT_MAX)) throw std::runtime_error("Test argument is not representable"); return uint32_t(parsed);
}
}
int main(int argc, char** argv) {
  try {
    host::gpu::Selection selection; selection.mode = host::gpu::Mode::disabled;
    int width = 1280, height = 720;
    for (int index = 1; index < argc; ++index) {
      const std::string option(argv[index]); if (++index >= argc) throw std::runtime_error("Test argument requires a value");
      if (option == "--backend") {
        const std::string backend(argv[index]);
        if (backend == "nvenc") selection.mode = host::gpu::Mode::nvenc;
        else if (backend == "amf") selection.mode = host::gpu::Mode::amf;
        else if (backend == "qsv-mft") selection.mode = host::gpu::Mode::qsv;
        else throw std::runtime_error("An explicit hardware backend is required");
      } else if (option == "--adapter") {
        selection.adapterLuid = host::gpu::parseAdapterLuid(argv[index]); if (!selection.adapterLuid) throw std::runtime_error("Invalid adapter LUID");
      } else if (option == "--width") width = int(number(argv[index]));
      else if (option == "--height") height = int(number(argv[index]));
      else throw std::runtime_error("Unknown hardware test option");
    }
    if (selection.mode == host::gpu::Mode::disabled) throw std::runtime_error("Use an explicit --backend; this test is never a default VM smoke test");
    host::Runtime runtime; host::validateFrameMemory(width, height, 0, 12);
    constexpr int fps = 60, bitrate = 8000000;
    auto encoder = host::gpu::createEncoder(width, height, fps, bitrate, selection);
    require(encoder->hardwareInfo().initialized && !encoder->hardwareInfo().hardwareVerified, "Hardware initialization was reported as successful encoding");
    multitap::RemoteDecoder decoder; host::check(decoder.Initialize(UINT(width), UINT(height)), "Initialize actual client software decoder");
    require(!decoder.hardware(), "Interop witness must use the actual software decoder");
    std::vector<uint64_t> submitted, decoded, keys;
    size_t totalBytes = 0, largest = 0;
    const auto receive = [&](multitap::DecodedFrame&& frame) { require(frame.width == UINT(width) && frame.height == UINT(height), "Actual decoded geometry differs from the requested picture"); decoded.push_back(frame.pts_us); };
    std::vector<uint8_t> pixels(size_t(width) * size_t(height) * 3 / 2, 128);
    for (int index = 0; index < 12; ++index) {
      for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) pixels[size_t(y) * size_t(width) + size_t(x)] = uint8_t(16 + (int64_t(x) + y + index * 11) % 210);
      if (index == 3) encoder->requestKeyFrame();
      if (index == 6) require(encoder->setBitrate(12000000), "Selected hardware refused its live bitrate target");
      const uint64_t pts = uint64_t(index) * 1000000 / fps;
      std::vector<host::Frame> frames; encoder->encode(pixels.data(), pts, frames);
      require(frames.size() == 1 && frames.front().ptsUs == pts, "Hardware delayed, duplicated or retimestamped the submitted picture");
      auto& frame = frames.front(); const auto unit = host::gpu::inspectAnnexB(frame.bytes, host::encoderMaximumFrameBytes);
      require(unit.valid && unit.idr == frame.key && (!unit.idr || (unit.sps && unit.pps && unit.spsProfile == 66)), "Actual hardware output broke the Baseline SPS/PPS/AnnexB contract");
      if (frame.key) keys.push_back(pts);
      submitted.push_back(pts); totalBytes += frame.bytes.size(); largest = std::max(largest, frame.bytes.size());
      host::check(decoder.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, receive), "Decode actual hardware H264 with the production client decoder");
      require(encoder->hardwareInfo().hardwareVerified, "Actual hardware output did not update its encoding witness");
    }
    std::vector<host::Frame> trailing; encoder->finish(trailing); require(trailing.empty(), "Hardware unexpectedly retained delayed pictures");
    host::check(decoder.Drain(receive), "Drain actual client decoder");
    require(submitted == decoded && decoded.size() == 12, "Actual client decoder did not preserve every hardware picture and timestamp");
    require(std::find(keys.begin(), keys.end(), 0) != keys.end() && std::find(keys.begin(), keys.end(), 50000) != keys.end(), "Requested recovery IDR was not encoded at its input timestamp");
    // udp-media-v1 LOSS on this hardware: frames 20-27 never reach one decoder, then
    // recover(frame 19). With reference invalidation (NVENC) frame 28 is a P-frame and that
    // decoder shows every frame from it like one that lost nothing; without it, an IDR.
    // Also the encode time per frame, submission to locked output, on this GPU.
    auto lossEncoder = host::gpu::createEncoder(width, height, fps, bitrate, selection);
    multitap::RemoteDecoder whole, lossy;
    host::check(whole.Initialize(UINT(width), UINT(height)), "Initialize actual client software decoder");
    host::check(lossy.Initialize(UINT(width), UINT(height)), "Initialize actual client software decoder");
    std::map<uint64_t, std::vector<uint8_t>> wholeFrames, lossyFrames; std::vector<double> encodeMs; bool recovered = false, widened = false; size_t answerBytes = 0, pBytes = 0;
    for (int index = 0; index < 40; ++index) {
      for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) pixels[size_t(y) * size_t(width) + size_t(x)] = uint8_t(16 + (int64_t(x) * 3 + y + index * 7) % 210);
      const uint64_t pts = uint64_t(index) * 1000000 / fps;
      // As host::Encoder::recover does on the first LOSS: refused, then the IDR widens the DPB.
      if (index == 6) { require(!lossEncoder->recover(uint64_t(4) * 1000000 / fps), "A narrow DPB recovered"); widened = lossEncoder->widenReferences(); lossEncoder->requestKeyFrame(); }
      if (index == 28) { recovered = lossEncoder->recover(uint64_t(19) * 1000000 / fps); require(recovered == widened, "recover() disagrees with the widened DPB"); if (!recovered) lossEncoder->requestKeyFrame(); }
      std::vector<host::Frame> frames; const auto began = std::chrono::steady_clock::now(); lossEncoder->encode(pixels.data(), pts, frames);
      encodeMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
      require(frames.size() == 1 && frames.front().ptsUs == pts, "Hardware delayed a picture in the loss run");
      const auto& frame = frames.front();
      if (index == 27) pBytes = frame.bytes.size();
      if (index == 28) { answerBytes = frame.bytes.size(); require(frame.key == !recovered, "The frame after recover() has the wrong type"); }
      host::check(whole.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, [&](multitap::DecodedFrame&& out) { wholeFrames[out.pts_us] = std::move(out.rgba); }), "Decode the loss run");
      if (index != 5 && (index < 20 || index >= 28)) host::check(lossy.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, [&](multitap::DecodedFrame&& out) { lossyFrames[out.pts_us] = std::move(out.rgba); }), "Decode the loss run without frames 20-27");
    }
    host::check(whole.Drain([&](multitap::DecodedFrame&& out) { wholeFrames[out.pts_us] = std::move(out.rgba); }), "Drain");
    host::check(lossy.Drain([&](multitap::DecodedFrame&& out) { lossyFrames[out.pts_us] = std::move(out.rgba); }), "Drain");
    size_t identical = 0;
    for (int index = 28; index < 40; ++index) { const uint64_t pts = uint64_t(index) * 1000000 / fps; const auto a = lossyFrames.find(pts), b = wholeFrames.find(pts); identical += a != lossyFrames.end() && b != wholeFrames.end() && a->second == b->second; }
    std::sort(encodeMs.begin(), encodeMs.end());
    std::cout << "{\"kind\":\"gpu-h264-reference-recovery\",\"widened\":" << (widened ? "true" : "false") << ",\"recovered\":" << (recovered ? "true" : "false") << ",\"identicalToNoLoss\":" << identical << ",\"expected\":12,\"answerBytes\":" << answerBytes
              << ",\"pFrameBytes\":" << pBytes << ",\"encodeMsP50\":" << encodeMs[encodeMs.size() / 2] << ",\"encodeMsMax\":" << encodeMs.back() << "}\n";
    require(identical == 12, "After the recovery frame the decoder that lost frames differs from one that lost nothing");
    const auto info = encoder->hardwareInfo(); const char* backend = info.vendorId == 0x10de ? "nvenc" : info.vendorId == 0x1002 ? "amf" : "qsv-mft";
    std::cout << "{\"kind\":\"gpu-h264-interop\",\"backend\":\"" << backend << "\",\"adapterLuid\":\"" << host::gpu::adapterLuidText(info.adapterLuid)
      << "\",\"vendorId\":" << info.vendorId << ",\"deviceId\":" << info.deviceId << ",\"width\":" << width << ",\"height\":" << height
      << ",\"encoded\":" << submitted.size() << ",\"decoded\":" << decoded.size() << ",\"totalBytes\":" << totalBytes << ",\"largestFrameBytes\":" << largest
      << ",\"actualGpu\":true,\"actualClientDecoder\":true,\"syntheticPixels\":true,\"capture\":false,\"relay\":false,\"displayedFrames\":false,\"performanceAcceptance\":false,\"passed\":true}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cout << "{\"kind\":\"gpu-h264-interop\",\"passed\":false,\"gpuFailureCode\":" << host::gpu::failureCode(error) << "}\n";
    std::cerr << error.what() << '\n'; return 1;
  }
}
