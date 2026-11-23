// Explicit actual-hardware test, run separately on the selected
// physical Host. Never part of an automatic GPU-less smoke-test run.
#include "stream_refresh.hpp"
#include "remote_decoder.h"
#include "gpu_h264_nvenc_testing.hpp"
#include <dxgi.h>
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
    int width = 1280, height = 720, fps = 60, frameCount = 12, forcedSlots = 0; bool hardwareDecoder = false;
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
      else if (option == "--frames") {
        frameCount = int(number(argv[index]));
        require(frameCount >= 12 && frameCount <= 3600, "Invalid test frame count");
      } else if (option == "--fps") {
        if (std::string_view(argv[index]) == "auto") fps = 0;
        else { fps = int(number(argv[index])); require(fps >= 30 && fps <= 240, "Invalid test rate"); }
      }
      else if (option == "--loss-decoder") {
        // The decoder of the loss run: the client's software decoder, or its hardware (DXVA) decoder on the encoder's adapter.
        const std::string_view kind(argv[index]); if (kind != "software" && kind != "hardware") throw std::runtime_error("Unknown decoder"); hardwareDecoder = kind == "hardware";
      }
      else if (option == "--force-long-term-slots") {
        // More slots than the stream's level holds (gpu_h264_nvenc_testing.hpp): the encoder then reports an
        // answer it did not code from its slot, and the Host must replace it with an IDR.
        forcedSlots = int(number(argv[index])); require(forcedSlots >= 1 && forcedSlots <= 8, "Invalid slot count");
      }
      else throw std::runtime_error("Unknown hardware test option");
    }
    if (selection.mode == host::gpu::Mode::disabled) throw std::runtime_error("Use an explicit --backend; this test is never a default VM smoke test");
    host::Runtime runtime; host::validateFrameMemory(width, height, 0, 12);
    constexpr int bitrate = 8000000;
    if (!fps) {
      fps = host::StreamRefreshProbe::supportedRate({{width, height}}, selection);
      // A viewport may grow after the idle check. Exercise the same Encoder
      // that the session worker opens, then compare with the format probe.
      if (!selection.adapterLuid && fps > 60) {
        const auto backend = selection.mode == host::gpu::Mode::nvenc ? host::EncoderBackend::nvenc :
          selection.mode == host::gpu::Mode::amf ? host::EncoderBackend::amf : host::EncoderBackend::qsvMft;
        host::Encoder production(width, height, 240, bitrate, {}, backend);
        std::vector<uint8_t> sample(size_t(width) * height * 3 / 2, uint8_t{128});
        bool firstIdr = false;
        production.encode(sample.data(), 0, [&](host::Frame frame) { firstIdr = frame.key; });
        require(firstIdr && production.hardwareInfo().hardwareVerified && production.captureFps() <= fps &&
                production.captureFps() >= 60, "Session encoder did not retain the GPU within the verified rate");
        std::cout << "{\"kind\":\"session-encoder-refresh\",\"requestedFps\":240,\"probeCeiling\":" << fps
                  << ",\"captureFps\":" << production.captureFps() << ",\"actualHardwareIdr\":true}\n";
      }
    }
    auto encoder = host::gpu::createEncoder(width, height, fps, bitrate, selection);
    require(encoder->hardwareInfo().initialized && !encoder->hardwareInfo().hardwareVerified, "Hardware initialization was reported as successful encoding");
    multitap::RemoteDecoder::Options options; options.fps = UINT(fps);
    multitap::RemoteDecoder decoder; host::check(decoder.Initialize(UINT(width), UINT(height), options), "Initialize actual client software decoder");
    require(!decoder.hardware(), "Interop witness must use the actual software decoder");
    std::vector<uint64_t> submitted, decoded, keys;
    size_t totalBytes = 0, largest = 0;
    const auto receive = [&](multitap::DecodedFrame&& frame) { require(frame.width == UINT(width) && frame.height == UINT(height), "Actual decoded geometry differs from the requested picture"); decoded.push_back(frame.pts_us); };
    std::vector<uint8_t> pixels(size_t(width) * size_t(height) * 3 / 2, 128);
    const auto started = std::chrono::steady_clock::now();
    for (int index = 0; index < frameCount; ++index) {
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
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    require(submitted == decoded && decoded.size() == size_t(frameCount), "Actual client decoder did not preserve every hardware picture and timestamp");
    require(std::find(keys.begin(), keys.end(), 0) != keys.end() && std::find(keys.begin(), keys.end(), uint64_t(3) * 1000000 / fps) != keys.end(), "Requested recovery IDR was not encoded at its input timestamp");
    // udp-media-v1 LOSS on this hardware (reference_history.hpp). One decoder gets every frame, the
    // other loses the frames below and gets, as a client does, nothing more until the answer to
    // its LOSS: a P-frame coded from a long-term reference it holds (NVENC), or an IDR. From
    // that answer on it must show exactly what the decoder that lost nothing shows. The picture
    // never repeats, so a frame coded from the wrong reference cannot decode to the same pixels.
    // Lengths are times, as a LOSS is: the same run holds at any frame rate.
    host::gpu::nvenctesting::longTermSlots = forcedSlots;
    auto lossEncoder = host::gpu::createEncoder(width, height, fps, bitrate, selection);
    multitap::RemoteDecoder::Options lossOptions = options;
    if (hardwareDecoder) {
      Microsoft::WRL::ComPtr<IDXGIFactory1> factory; host::check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory"); Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      const uint64_t wanted = encoder->hardwareInfo().adapterLuid;
      for (UINT n = 0; !lossOptions.decode_device && factory->EnumAdapters1(n, &adapter) == S_OK; ++n) {
        DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
        if ((uint64_t(uint32_t(desc.AdapterLuid.HighPart)) << 32 | desc.AdapterLuid.LowPart) == wanted) lossOptions.decode_device = multitap::gpu::Device::Create(adapter.Get());
      }
      require(lossOptions.decode_device != nullptr, "No decode device on the encoder's adapter");
    }
    multitap::RemoteDecoder whole, lossy;
    host::check(whole.Initialize(UINT(width), UINT(height), lossOptions), "Initialize actual client decoder");
    host::check(lossy.Initialize(UINT(width), UINT(height), lossOptions), "Initialize actual client decoder");
    require(whole.hardware() == hardwareDecoder && lossy.hardware() == hardwareDecoder, "The loss run is not on the decoder asked for");
    const auto frames = [&](int milliseconds) { return std::max(1, milliseconds * fps / 1000); };
    // What is lost, and what the Host is told after it (the last good frame is the one before the loss).
    struct Loss { const char* name; int first, count; bool answerLostToo, recovers; int answerAt = 0; bool recovered = false, key = false; size_t bytes = 0; };
    std::vector<Loss> losses; int at = 5;
    const auto lose = [&](const char* name, int gap, int count, bool answerLostToo, bool recovers) { at += gap; losses.push_back({name, at, count, answerLostToo, recovers}); at += count + (answerLostToo ? 2 : 0); };
    lose("first LOSS: long-term references begin at its IDR", 0, 1, false, false);
    lose("inside the first marks: the client's DPB lacks a slot", 2, 1, false, false);
    lose("340 ms: beyond any DPB, three marks lost with it", frames(250), frames(340), false, true);
    lose("the answer itself is lost", frames(200), frames(60), true, true);
    lose("three frames, from a slot marked again after a loss", frames(150), 3, false, true);
    lose("700 ms: older than every slot", frames(250), frames(700), false, false);
    lose("right after an IDR and its marks, 100 ms", std::max(frames(40), 5), frames(100), false, true);
    const int lossFrames = at + frames(150);
    std::map<uint64_t, std::vector<uint8_t>> wholeFrames, lossyFrames; std::vector<double> encodeMs; bool widened = false; size_t pBytes = 0, pCount = 0, idrBytes = 0, delivered = 0, replaced = 0;
    const auto keep = [](std::map<uint64_t, std::vector<uint8_t>>& into) { return [&into](multitap::DecodedFrame&& out) { into[out.pts_us] = std::move(out.rgba); }; };
    for (int index = 0; index < lossFrames; ++index) {
      // Bars that move, and blocks whose place and shade follow a hash of the frame number.
      for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) pixels[size_t(y) * size_t(width) + size_t(x)] = uint8_t(16 + (int64_t(x) * 3 + y + index * 7) % 210);
      uint32_t hash = uint32_t(index) * 2654435761u + 12345;
      for (int block = 0; block < 24; ++block) {
        hash = hash * 1664525u + 1013904223u; const int bx = int(hash >> 8) % std::max(1, width - 96), by = int(hash >> 20) % std::max(1, height - 96); const uint8_t shade = uint8_t(16 + (hash & 0x7f) + block * 3);
        for (int y = by; y < by + 96; ++y) std::memset(pixels.data() + size_t(y) * size_t(width) + size_t(bx), shade, 96);
      }
      const uint64_t pts = uint64_t(index) * 1000000 / fps;
      bool lost = false, answer = false;
      for (auto& loss : losses) {
        const int end = loss.first + loss.count;
        lost = lost || (index >= loss.first && index < end) || (loss.answerLostToo && index >= end && index < end + 2);
        // The LOSS reaches the Host when the loss ends, and once more when its answer was lost as well.
        if (index == end || (loss.answerLostToo && index == end + 2)) {
          answer = true; loss.answerAt = index;
          // As host::Encoder::recover does: refused, then the IDR, which on the first LOSS begins the long-term references.
          loss.recovered = lossEncoder->recover(uint64_t(loss.first - 1) * 1000000 / fps);
          if (!loss.recovered) { if (!widened) widened = lossEncoder->widenReferences(); lossEncoder->requestKeyFrame(); }
        }
      }
      // The automatic rate moves the target all the time; the kept references must outlive it.
      if (index % 7 == 3) require(lossEncoder->setBitrate(index % 14 == 3 ? bitrate * 3 / 4 : bitrate * 9 / 8), "The hardware refused a live bitrate target in the loss run");
      std::vector<host::Frame> coded; const auto began = std::chrono::steady_clock::now(); lossEncoder->encode(pixels.data(), pts, coded);
      encodeMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
      require(coded.size() == 1 && coded.front().ptsUs == pts, "Hardware delayed a picture in the loss run");
      const auto& frame = coded.front();
      if (frame.key) idrBytes = std::max(idrBytes, frame.bytes.size()); else if (!answer) { pBytes += frame.bytes.size(); ++pCount; }
      for (auto& loss : losses) if (loss.answerAt == index && answer) {
        loss.bytes = frame.bytes.size(); loss.key = frame.key;
        // An answer the encoder did not code from its slot leaves as an IDR instead; only the forced run may meet one.
        if (loss.recovered && frame.key) ++replaced;
        require(forcedSlots ? frame.key || loss.recovered : frame.key == !loss.recovered, "The answer to a LOSS has the wrong type");
      }
      host::check(whole.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, keep(wholeFrames)), "Decode the loss run");
      if (!lost) { ++delivered; host::check(lossy.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, keep(lossyFrames)), "Decode the loss run without the lost frames"); }
    }
    host::check(whole.Drain(keep(wholeFrames)), "Drain"); host::check(lossy.Drain(keep(lossyFrames)), "Drain");
    require(whole.hardware() == hardwareDecoder && lossy.hardware() == hardwareDecoder && !whole.fallbacks() && !lossy.fallbacks(), "A decoder of the loss run was replaced");
    size_t identical = 0, different = 0;
    for (const auto& [pts, picture] : lossyFrames) { const auto same = wholeFrames.find(pts); if (same != wholeFrames.end() && same->second == picture && !picture.empty()) ++identical; else ++different; }
    std::sort(encodeMs.begin(), encodeMs.end());
    std::cout << "{\"kind\":\"gpu-h264-reference-recovery\",\"decoder\":\"" << (hardwareDecoder ? "hardware" : "software") << "\",\"fps\":" << fps << ",\"longTermReferences\":" << (widened ? "true" : "false") << ",\"forcedSlots\":" << forcedSlots << ",\"answersReplacedByIdr\":" << replaced << ",\"frames\":" << lossFrames
              << ",\"delivered\":" << delivered << ",\"shown\":" << lossyFrames.size() << ",\"identicalToNoLoss\":" << identical << ",\"different\":" << different
              << ",\"pFrameBytesMean\":" << (pCount ? pBytes / pCount : 0) << ",\"idrBytesMax\":" << idrBytes << ",\"encodeMsP50\":" << encodeMs[encodeMs.size() / 2] << ",\"encodeMsMax\":" << encodeMs.back() << ",\"losses\":[";
    for (size_t n = 0; n < losses.size(); ++n) std::cout << (n ? "," : "") << "{\"case\":\"" << losses[n].name << "\",\"lostFrames\":" << losses[n].count << ",\"answer\":\"" << (losses[n].key ? "IDR" : "P from a long-term reference") << "\",\"bytes\":" << losses[n].bytes << "}";
    std::cout << "]}\n";
    require(different == 0 && identical == delivered && lossyFrames.size() == delivered, "The decoder that lost frames differs from one that lost nothing");
    if (!forcedSlots) for (const auto& loss : losses) require(loss.recovered == (widened && loss.recovers), "A LOSS was answered with the wrong kind of frame");
    host::gpu::nvenctesting::longTermSlots = 0;
    const auto info = encoder->hardwareInfo(); const char* backend = info.vendorId == 0x10de ? "nvenc" : info.vendorId == 0x1002 ? "amf" : "qsv-mft";
    std::cout << "{\"kind\":\"gpu-h264-interop\",\"backend\":\"" << backend << "\",\"adapterLuid\":\"" << host::gpu::adapterLuidText(info.adapterLuid)
      << "\",\"vendorId\":" << info.vendorId << ",\"deviceId\":" << info.deviceId << ",\"width\":" << width << ",\"height\":" << height
      << ",\"fpsTarget\":" << fps << ",\"encoded\":" << submitted.size() << ",\"decoded\":" << decoded.size() << ",\"unpacedSeconds\":" << seconds << ",\"totalBytes\":" << totalBytes << ",\"largestFrameBytes\":" << largest
      << ",\"actualGpu\":true,\"actualClientDecoder\":true,\"syntheticPixels\":true,\"capture\":false,\"relay\":false,\"displayedFrames\":false,\"performanceAcceptance\":false,\"passed\":true}\n";
    return 0;
  } catch (const std::exception& error) {
    const auto detail = host::gpu::failureInfo(error);
    std::cout << "{\"kind\":\"gpu-h264-interop\",\"passed\":false,\"gpuFailureCode\":" << host::gpu::failureCode(error) << ",\"site\":" << detail.site << ",\"nativeStatus\":" << detail.nativeStatus << "}\n";
    std::cerr << error.what() << '\n'; return 1;
  }
}
