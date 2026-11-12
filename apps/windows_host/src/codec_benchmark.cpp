#include "media.hpp"
#include "../../windows_client/windows/runner/remote_decoder.h"
#include "../../../native/windows_video/color.hpp"
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>

namespace {
using host::Clock;
double millis(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}
void require(bool valid, const char* message) {
  if (!valid) throw std::runtime_error(message);
}
double percentile(std::vector<double> values, double fraction) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  return values[size_t(std::ceil(fraction * values.size())) - 1];
}
void png(const std::filesystem::path& path, int width, int height, const uint8_t* pixels, int stride, bool rgba) {
  host::Com<IWICImagingFactory> factory;
  host::check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Create benchmark image writer");
  host::Com<IWICStream> stream;
  host::check(factory->CreateStream(&stream), "Create image stream");
  host::check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open benchmark image");
  host::Com<IWICBitmapEncoder> encoder;
  host::check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG writer");
  host::check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG");
  host::Com<IWICBitmapFrameEncode> frame;
  host::check(encoder->CreateNewFrame(&frame, nullptr), "Create PNG frame");
  host::check(frame->Initialize(nullptr), "Initialize PNG frame");
  host::check(frame->SetSize(UINT(width), UINT(height)), "Set PNG geometry");
  auto format = GUID_WICPixelFormat32bppBGRA;
  host::check(frame->SetPixelFormat(&format), "Set PNG pixel format");
  require(format == GUID_WICPixelFormat32bppBGRA, "PNG pixel format changed");
  std::vector<uint8_t> bgra(size_t(width) * height * 4);
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    const auto* source = pixels + size_t(y) * stride + size_t(x) * 4;
    auto* target = bgra.data() + (size_t(y) * width + x) * 4;
    target[0] = source[rgba ? 2 : 0]; target[1] = source[1]; target[2] = source[rgba ? 0 : 2]; target[3] = 255;
  }
  host::check(frame->WritePixels(UINT(height), UINT(width * 4), UINT(bgra.size()), bgra.data()), "Write PNG pixels");
  host::check(frame->Commit(), "Finish PNG frame"); host::check(encoder->Commit(), "Finish PNG");
}
struct Scene {
  int width, height;
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ previous = nullptr;
  uint8_t* pixels = nullptr;
  std::vector<uint8_t> base, current;
  Scene(int w, int h) : width(w), height(h) {
    dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h; info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    require(dc && bitmap && pixels, "Cannot create generated benchmark scene");
    previous = SelectObject(dc, bitmap);
    RECT bounds{0, 0, w, h}; FillRect(dc, &bounds, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    int row = 24;
    for (const int size : {12, 14, 16, 20}) {
      HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");
      require(font != nullptr, "Cannot create test font");
      const auto old = SelectObject(dc, font);
      SetTextColor(dc, RGB(22, 31, 46));
      const std::wstring text = L"한글 가독성 확인  ABC abc 0123456789 · " + std::to_wstring(size) + L" px";
      TextOutW(dc, 24, row, text.c_str(), int(text.size()));
      SetTextColor(dc, RGB(32, 85, 210));
      const std::wstring colour = L"파란 글씨와 얇은 선 · coloured text 12345";
      TextOutW(dc, 24, row + size + 4, colour.c_str(), int(colour.size()));
      SelectObject(dc, old); DeleteObject(font); row += size * 2 + 17;
    }
    GdiFlush();
    base.assign(pixels, pixels + size_t(w) * h * 4); current.resize(base.size());
  }
  ~Scene() { if (dc && previous) SelectObject(dc, previous); if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); }
  const std::vector<uint8_t>& render(int index, bool motion) {
    current = base;
    // Generated motion occupies only the lower region; the same top text is
    // compared across configurations. No desktop, user file or screen is read.
    for (int y = 300; y < height; ++y) for (int x = 0; x < width; ++x) {
      auto* pixel = current.data() + (size_t(y) * width + x) * 4;
      if (motion) {
        pixel[0] = uint8_t(x + index * 7); pixel[1] = uint8_t(y + index * 3);
        pixel[2] = uint8_t(((x + index * 11) / 48 + (y + index * 3) / 48) % 2 ? 212 : 42);
      } else {
        const bool moving = x >= (index * 13) % (width - 160) && x < (index * 13) % (width - 160) + 160 && y < 460;
        pixel[0] = moving ? 220 : 241; pixel[1] = moving ? 110 : 244; pixel[2] = moving ? 46 : 248;
      }
      pixel[3] = 255;
    }
    return current;
  }
};
struct Configuration { const char* name; host::EncoderTuning tuning; };
struct WireParameters { int profile = -1, cabac = -1; };
WireParameters wireParameters(const std::vector<uint8_t>& bytes) {
  WireParameters result;
  for (size_t at = 0; at + 5 < bytes.size(); ++at) {
    size_t prefix = 0;
    if (!bytes[at] && !bytes[at+1] && bytes[at+2] == 1) prefix = 3;
    else if (!bytes[at] && !bytes[at+1] && !bytes[at+2] && bytes[at+3] == 1) prefix = 4;
    if (!prefix) continue;
    const size_t nal = at + prefix;
    const auto type = bytes[nal] & 31;
    if (type == 7 && nal + 1 < bytes.size()) result.profile = bytes[nal+1];
    if (type == 8) {
      // Only the PPS/SPS IDs and entropy flag are needed, before any later
      // H.264 syntax. Refuse truncated input instead of guessing the flag.
      size_t bit = (nal + 1) * 8;
      auto read = [&] { require(bit < bytes.size()*8, "Truncated benchmark PPS"); const auto value = (bytes[bit/8] >> (7 - bit%8)) & 1; ++bit; return value; };
      for (int id = 0; id < 2; ++id) {
        unsigned zeros = 0; while (!read()) require(++zeros < 24, "Invalid benchmark PPS identifier");
        for (unsigned i = 0; i < zeros; ++i) read();
      }
      result.cabac = read();
    }
    at = nal;
    if (result.profile >= 0 && result.cabac >= 0) break;
  }
  require(result.profile >= 0 && result.cabac >= 0, "Missing benchmark SPS/PPS");
  return result;
}
bool run(const std::filesystem::path& root, int width, int height, int frames, bool motion, const Configuration& config) {
  constexpr int fps = 60;
  const int bitrate = width == 1280 ? 6000000 : width == 1920 ? 12000000 : 20000000;
  const std::string prefix = std::to_string(width) + "-" + (motion ? "motion" : "desktop");
  std::ofstream bitstream(root / (prefix + "-" + config.name + ".h264"), std::ios::binary);
  require(bool(bitstream), "Cannot write benchmark bitstream");
  Scene scene(width, height);
  host::Encoder encoder(width, height, fps, bitrate, config.tuning, host::EncoderBackend::mft);  // MFT settings study
  const auto applied = encoder.reportedTuning();
  const auto complexity = [](int value) { return value <= 33 ? 0 : value <= 66 ? 1 : 2; };
  require(applied.profile == config.tuning.profile &&
      (config.tuning.qualityVsSpeed < 0 || (applied.qualityVsSpeed >= 0 && complexity(applied.qualityVsSpeed) == complexity(config.tuning.qualityVsSpeed))) &&
      (!config.tuning.cabac || applied.cabac), "Requested benchmark tuning was not applied");
  multitap::RemoteDecoder decoder;
  host::check(decoder.Initialize(width, height), "Initialize actual client software decoder");
  std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2), last;
  std::vector<double> convertMs, encodeMs, decodeMs, latencyMs;
  std::vector<Clock::time_point> injected(size_t(frames), Clock::time_point{});
  size_t encoded = 0, decoded = 0, bytes = 0, keyFrames = 0, maxFrameBytes = 0;
  WireParameters wire;
  int maxPending = 0;
  const auto output = [&](host::Frame frame) {
    if (!encoded) {
      wire = wireParameters(frame.bytes);
      require(wire.profile == int(config.tuning.profile) && wire.cabac == int(config.tuning.cabac), "Requested codec flags differ from the actual H264 bitstream");
    }
    ++encoded; bytes += frame.bytes.size();
    if (frame.key) ++keyFrames;
    maxFrameBytes = std::max(maxFrameBytes, frame.bytes.size());
    bitstream.write(reinterpret_cast<const char*>(frame.bytes.data()), std::streamsize(frame.bytes.size()));
    const auto began = Clock::now();
    host::check(decoder.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, [&](multitap::DecodedFrame&& shown) {
      require(shown.width == UINT(width) && shown.height == UINT(height), "Decoded geometry mismatch");
      const auto index = size_t((shown.pts_us * fps + 500000) / 1000000);
      require(index < injected.size() && injected[index] != Clock::time_point{}, "Decoded PTS did not match an injected frame");
      latencyMs.push_back(millis(Clock::now() - injected[index]));
      ++decoded; last = std::move(shown.rgba);
    }), "Decode benchmark output");
    decodeMs.push_back(millis(Clock::now() - began));
  };
  const auto start = Clock::now();
  for (int n = 0; n < frames; ++n) {
    const auto& reference = scene.render(n, motion);
    injected[size_t(n)] = Clock::now();
    host::bgraToNv12(reference.data(), nv12.data(), width, height);
    convertMs.push_back(millis(Clock::now() - injected[size_t(n)]));
    const double decodeBefore = std::accumulate(decodeMs.begin(), decodeMs.end(), 0.0);
    const auto began = Clock::now();
    encoder.encode(nv12.data(), uint64_t(n) * 1000000 / fps, output);
    const auto inclusiveMs = millis(Clock::now() - began);
    encodeMs.push_back(std::max(0.0, inclusiveMs - (std::accumulate(decodeMs.begin(), decodeMs.end(), 0.0) - decodeBefore)));
    maxPending = std::max(maxPending, n + 1 - int(encoded));
  }
  encoder.finish(output);
  host::check(decoder.Drain([&](multitap::DecodedFrame&& shown) { ++decoded; last = std::move(shown.rgba); }), "Drain benchmark decoder");
  const double wallMs = millis(Clock::now() - start);
  require(encoded == size_t(frames) && decoded == encoded && !last.empty(), "Incomplete benchmark stream");
  const auto& reference = scene.current;
  std::vector<uint8_t> roundtrip(size_t(width)*height*4);
  multitap::video::nv12ToRgba(nv12.data(),roundtrip.data(),UINT(width),UINT(height),UINT(width),UINT(height),false,false);
  double squared = 0, edgeSquared = 0, roundtripSquared = 0, codecSquared = 0, codecEdgeSquared = 0;
  uint64_t samples = 0, edgeSamples = 0;
  for (int y = 0; y < 280; ++y) for (int x = 0; x < std::min(width, 960); ++x) {
    const auto at = (size_t(y) * width + x) * 4;
    const bool edge = std::min({reference[at], reference[at + 1], reference[at + 2]}) < 220;
    for (int c = 0; c < 3; ++c) {
      const int error = int(reference[at + size_t(2 - c)]) - int(last[at + size_t(c)]);
      squared += error * error; ++samples;
      const int conversionError = int(reference[at + size_t(2-c)]) - int(roundtrip[at + size_t(c)]);
      const int codecError = int(roundtrip[at + size_t(c)]) - int(last[at + size_t(c)]);
      roundtripSquared += conversionError * conversionError;
      codecSquared += codecError * codecError;
      if (edge) { edgeSquared += error * error; ++edgeSamples; }
      if (edge) codecEdgeSquared += codecError * codecError;
    }
  }
  const auto psnr = [](double mse) { return mse < 0.000001 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse); };
  const auto textPsnr = psnr(squared / samples), inkPsnr = psnr(edgeSquared / std::max<uint64_t>(edgeSamples,1));
  // Gross corruption guard only, not a user-acceptance or competitor score.
  const bool qualityGuard = textPsnr >= 25 && inkPsnr >= 10;
  const auto full = root / (prefix + "-" + config.name + ".png");
  png(full, width, height, last.data(), width * 4, true);
  png(root / (prefix + "-" + config.name + "-text.png"), std::min(width, 960), 280, last.data(), width * 4, true);
  if (std::string(config.name) == "baseline-default") {
    png(root / (prefix + "-reference.png"), width, height, reference.data(), width * 4, false);
    png(root / (prefix + "-reference-text.png"), std::min(width, 960), 280, reference.data(), width * 4, false);
  }
  std::cout << std::fixed << std::setprecision(3)
      << "{\"width\":" << width << ",\"height\":" << height << ",\"frames\":" << frames
      << ",\"scene\":\"" << (motion ? "generated-motion" : "generated-desktop") << "\",\"setting\":\"" << config.name
      << "\",\"profileInBitstream\":" << wire.profile << ",\"qualityVsSpeedRequested\":" << config.tuning.qualityVsSpeed << ",\"qualityVsSpeedApiReported\":" << applied.qualityVsSpeed
      << ",\"cabacInBitstream\":" << (wire.cabac ? "true" : "false") << ",\"bitrateRequested\":" << bitrate
      << ",\"mediaBitrateMbps\":" << bytes * 8.0 * fps / frames / 1000000.0 << ",\"encoded\":" << encoded << ",\"decoded\":" << decoded
      << ",\"keyFrames\":" << keyFrames << ",\"maxFrameBytes\":" << maxFrameBytes
      << ",\"maxPendingEncoderFrames\":" << maxPending << ",\"conversionP95Ms\":" << percentile(convertMs,.95)
      << ",\"encodeP50Ms\":" << percentile(encodeMs,.5) << ",\"encodeP95Ms\":" << percentile(encodeMs,.95)
      << ",\"decodeP95Ms\":" << percentile(decodeMs,.95) << ",\"cpuPipelineP95Ms\":" << percentile(latencyMs,.95)
      << ",\"textRegionRgbPsnrDb\":" << textPsnr << ",\"textInkRgbPsnrDb\":" << inkPsnr << ",\"qualityGuardPassed\":" << (qualityGuard ? "true" : "false")
      << ",\"nv12RoundtripTextPsnrDb\":" << psnr(roundtripSquared/samples)
      << ",\"h264VsNv12TextPsnrDb\":" << psnr(codecSquared/samples)
      << ",\"h264VsNv12InkPsnrDb\":" << psnr(codecEdgeSquared/std::max<uint64_t>(edgeSamples,1))
      << ",\"syntheticWallFps\":" << frames * 1000.0 / wallMs
      << ",\"desktopCaptured\":false,\"relayUsed\":false,\"audioTested\":false,\"competitorCompared\":false}" << std::endl;
  return qualityGuard;
}
// Over a longer continuously changing stream, report every key frame index.
// The relay is TCP and never drops video, so only stream start needs an IDR.
bool keyCadence(int frames) {
  constexpr int width = 1280, height = 720, fps = 60;
  Scene scene(width, height);
  host::Encoder encoder(width, height, fps, 6000000, {}, host::EncoderBackend::mft);
  std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2);
  std::vector<int> keys; size_t encoded = 0, keyBytes = 0, otherMax = 0;
  const auto output = [&](host::Frame frame) {
    if (frame.key) { keys.push_back(int((frame.ptsUs * fps + 500000) / 1000000)); keyBytes = std::max(keyBytes, frame.bytes.size()); }
    else otherMax = std::max(otherMax, frame.bytes.size());
    ++encoded;
  };
  for (int n = 0; n < frames; ++n) {
    host::bgraToNv12(scene.render(n, true).data(), nv12.data(), width, height);
    encoder.encode(nv12.data(), uint64_t(n) * 1000000 / fps, output);
  }
  encoder.finish(output);
  std::cout << "{\"setting\":\"key-cadence\",\"frames\":" << frames << ",\"encoded\":" << encoded << ",\"keyFrameIndices\":[";
  for (size_t i = 0; i < keys.size(); ++i) std::cout << (i ? "," : "") << keys[i];
  std::cout << "],\"maxKeyFrameBytes\":" << keyBytes << ",\"maxOtherFrameBytes\":" << otherMax << "}" << std::endl;
  return encoded == size_t(frames) && !keys.empty() && keys.front() == 0;
}
}
int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 3, "Usage: MultitapCodecBenchmark OUTPUT_DIRECTORY FRAMES_PER_CASE");
    const std::filesystem::path root(argv[1]);
    const int frames = std::stoi(argv[2]);
    require(frames >= 6 && frames <= 120 && !std::filesystem::exists(root), "Use a new output directory and 6-120 frames per case");
    std::filesystem::create_directories(root);
    host::Runtime runtime;
    require(keyCadence(300), "Key frame cadence check produced an invalid stream");
    bool qualityGuards = true;
    for (const auto [width,height] : {std::pair{1280,720}, std::pair{1920,1080}, std::pair{2560,1440}})
      for (const bool motion : {false,true})
        for (const auto& config : {
          Configuration{"baseline-default", {}},
          Configuration{"high-fast", {eAVEncH264VProfile_High, 0, true}},
          Configuration{"high-balanced", {eAVEncH264VProfile_High, 50, true}},
          Configuration{"high-quality", {eAVEncH264VProfile_High, 100, true}},
          Configuration{"high-quality-cavlc", {eAVEncH264VProfile_High, 100, false}},
          Configuration{"baseline-quality", {eAVEncH264VProfile_Base, 100, false}},
        }) qualityGuards = run(root, width, height, frames, motion, config) && qualityGuards;
    return qualityGuards ? 0 : 2;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
