// Hardware MFT texture input (gpu_h264_qsv_mft.cpp), on this PC's real GPU.
// The product opens Intel's hardware H264 MFT this way; a PC may have none, so this opens the hardware MFT of the
// capture's own adapter (NVIDIA's, for one) through the same encoder class. One encoder takes the capture's NV12
// texture (the GPU path), a second one on its own device takes the same picture's bytes (the path before), and
// the client's decoder decodes both: their pictures must match each other and the source.
// Exit 0 pass, 1 fail, 3 no hardware MFT on this adapter (nothing tested).
#include "gpu_capture.hpp"
#include "gpu_h264.hpp"
#include "remote_decoder.h"
#include <d3d11.h>
#include <dxgi.h>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace host::gpu {
std::unique_ptr<EncoderImplementation> createHardwareMftEncoderForTesting(int width, int height, int fps, int bitrate, const Adapter& adapter, ID3D11Device* shared);
uint64_t hardwareMftTextureFramesForTesting(const EncoderImplementation& encoder);
}
namespace {
using namespace host;
using Clock = std::chrono::steady_clock;
int failures = 0;
void require(bool value, const std::string& what) { if (!value) { ++failures; std::cerr << "FAIL: " << what << "\n"; } }
constexpr int W = 1280, H = 720, Fps = 60, Bitrate = 12000000, Frames = 40;

std::vector<uint8_t> scene(int n) {
  // Text-like detail (thin lines, small checks) over a slow gradient, moving every frame.
  std::vector<uint8_t> bgra(size_t(W) * H * 4);
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
    auto* p = bgra.data() + (size_t(y) * W + x) * 4;
    const bool line = ((x + n * 3) % 37 == 0) || ((y + n) % 29 == 0);
    const bool check = (((x + n * 2) / 6) + (y / 6)) % 2 == 0 && x > 200 && x < 600 && y > 150 && y < 400;
    p[0] = uint8_t(line ? 250 : check ? 30 : (x / 6 + n) & 255); p[1] = uint8_t(line ? 250 : check ? 30 : (y / 3) & 255); p[2] = uint8_t(line ? 250 : check ? 30 : 120); p[3] = 255;
  }
  return bgra;
}
// Mean absolute difference of two decoded RGBA pictures, and of one against the BGRA source.
double rgbaDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size() || a.empty()) return 1e9; uint64_t sum = 0; size_t n = 0;
  for (size_t i = 0; i < a.size(); i += 4) for (int c = 0; c < 3; ++c) { sum += uint64_t(std::abs(int(a[i + c]) - int(b[i + c]))); ++n; }
  return double(sum) / double(n);
}
double sourceDifference(const std::vector<uint8_t>& rgba, const std::vector<uint8_t>& bgra) {
  if (rgba.size() != bgra.size() || rgba.empty()) return 1e9; uint64_t sum = 0; size_t n = 0;
  for (size_t i = 0; i < rgba.size(); i += 4) { sum += uint64_t(std::abs(int(rgba[i]) - int(bgra[i + 2])) + std::abs(int(rgba[i + 1]) - int(bgra[i + 1])) + std::abs(int(rgba[i + 2]) - int(bgra[i]))); n += 3; }
  return double(sum) / double(n);
}
}

int main() {
  try {
    Runtime runtime;
    const auto shared = createCaptureDevice();
    if (!shared) { std::cout << "{\"kind\":\"mft-texture\",\"state\":\"no_hardware_device\"}\n"; return 3; }
    Com<ID3D11VideoDevice> video; const bool videoSupport = SUCCEEDED(shared->QueryInterface(IID_PPV_ARGS(&video)));
    const auto luid = captureDeviceAdapter(shared.get());
    gpu::Adapter adapter; bool found = false;
    for (const auto& candidate : gpu::enumerateAdapters()) if (luid && candidate.luid == *luid) { adapter = candidate; found = true; }
    require(found, "The capture device's adapter is in the encoder inventory");
    require(videoSupport, "The capture device was created with video support");
    std::unique_ptr<EncoderImplementation> textured, bytes;
    try { textured = gpu::createHardwareMftEncoderForTesting(W, H, Fps, Bitrate, adapter, shared.get()); bytes = gpu::createHardwareMftEncoderForTesting(W, H, Fps, Bitrate, adapter, nullptr); }
    catch (const std::exception& error) { std::cout << "{\"kind\":\"mft-texture\",\"state\":\"no_hardware_mft\",\"vendor\":" << adapter.vendorId << ",\"error\":\"" << error.what() << "\"}\n"; return 3; }
    require(textured->textureDevice() == shared.get(), "The encoder opened on the capture's device takes its textures");
    require(bytes->textureDevice() == nullptr, "The encoder on its own device takes no textures");
    Com<ID3D11DeviceContext> context; shared->GetImmediateContext(&context);
    const std::vector<capture::PixelRect> one{{0, 0, W, H}};
    gpucapture::FrameComposer composer(shared.get(), W, H, one);
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = W; desc.Height = H; desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    Com<ID3D11Texture2D> surface; check(shared->CreateTexture2D(&desc, nullptr, &surface), "Test surface");
    multitap::RemoteDecoder decodeTextured, decodeBytes; check(decodeTextured.Initialize(W, H), "Client decoder"); check(decodeBytes.Initialize(W, H), "Client decoder");
    std::vector<std::vector<uint8_t>> sources(Frames), shownTextured(Frames), shownBytes(Frames);
    std::vector<uint8_t> nv12(size_t(W) * H * 3 / 2); int reads = 0; size_t bytesTextured = 0, bytesBytes = 0;
    double textureMs = 0, byteMs = 0; int keysTextured = 0, keysBytes = 0;
    const auto store = [&](std::vector<std::vector<uint8_t>>& into) { return [&into](multitap::DecodedFrame&& frame) { const size_t at = size_t((frame.pts_us * Fps + 500000) / 1000000); if (at < into.size()) into[at] = std::move(frame.rgba); }; };
    for (int n = 0; n < Frames; ++n) {
      sources[n] = scene(n);
      context->UpdateSubresource(surface.Get(), 0, nullptr, sources[n].data(), UINT(W) * 4, 0);
      composer.surfaceFrame(0, surface.Get()); (void)composer.compose(nullptr);
      const uint64_t pts = uint64_t(n) * 1000000 / Fps;
      const EncoderPicture picture{nullptr, composer.picture(), [&]() -> const uint8_t* { ++reads; composer.read(nv12); return nv12.data(); }};
      std::vector<Frame> a, b;
      auto began = Clock::now(); textured->encodePicture(picture, pts, a); textureMs += std::chrono::duration<double, std::milli>(Clock::now() - began).count();
      // The path before: the picture read back to the CPU, then uploaded by the encoder.
      began = Clock::now(); composer.read(nv12); bytes->encode(nv12.data(), pts, b); byteMs += std::chrono::duration<double, std::milli>(Clock::now() - began).count();
      require(a.size() == 1 && b.size() == 1 && a[0].ptsUs == pts && b[0].ptsUs == pts, "Each picture gives exactly its own output");
      if (a.size() != 1 || b.size() != 1) continue;
      const auto unit = gpu::inspectAnnexB(a[0].bytes, encoderMaximumFrameBytes);
      require(unit.valid && (n || (unit.idr && unit.sps && unit.pps)), "Texture output is valid AnnexB starting with an IDR");
      keysTextured += a[0].key; keysBytes += b[0].key; bytesTextured += a[0].bytes.size(); bytesBytes += b[0].bytes.size();
      check(decodeTextured.Decode(a[0].bytes.data(), a[0].bytes.size(), pts, store(shownTextured)), "Decode texture output");
      check(decodeBytes.Decode(b[0].bytes.data(), b[0].bytes.size(), pts, store(shownBytes)), "Decode byte output");
    }
    check(decodeTextured.Drain(store(shownTextured)), "Drain"); check(decodeBytes.Drain(store(shownBytes)), "Drain");
    const uint64_t textureFrames = gpu::hardwareMftTextureFramesForTesting(*textured);
    require(textureFrames == Frames && reads == 0, "Every picture went in as a texture, none was read back");
    double worstPair = 0, worstSourceTextured = 0, worstSourceBytes = 0; int compared = 0;
    for (int n = 0; n < Frames; ++n) {
      if (shownTextured[n].empty() || shownBytes[n].empty()) continue; ++compared;
      worstPair = std::max(worstPair, rgbaDifference(shownTextured[n], shownBytes[n]));
      worstSourceTextured = std::max(worstSourceTextured, sourceDifference(shownTextured[n], sources[n]));
      worstSourceBytes = std::max(worstSourceBytes, sourceDifference(shownBytes[n], sources[n]));
    }
    require(compared >= Frames - 2, "Both decoders showed (almost) every picture");
    // Same picture, same encoder settings: the two paths must look alike, and the texture path no worse than the byte path.
    require(worstPair < 2.0, "Texture and byte paths decode to the same picture");
    // The scene (1 px lines and checks in motion at 12 Mbps) is hard for any encoder: both paths land near 20 on
    // an NVIDIA P40; a wrong picture (another frame, a blank or shifted surface) is far above 40.
    require(worstSourceTextured < worstSourceBytes + 1.0 && worstSourceTextured < 40.0, "The texture path is as close to the source as the byte path");
    std::cout << std::fixed << std::setprecision(3) << "{\"kind\":\"mft-texture\",\"adapter\":\"" << adapter.description << "\",\"vendor\":" << adapter.vendorId
              << ",\"videoSupport\":" << (videoSupport ? "true" : "false") << ",\"textureFrames\":" << textureFrames << ",\"reads\":" << reads
              << ",\"compared\":" << compared << ",\"worstPairDiff\":" << worstPair << ",\"worstSourceDiffTexture\":" << worstSourceTextured << ",\"worstSourceDiffBytes\":" << worstSourceBytes
              << ",\"keys\":[" << keysTextured << ',' << keysBytes << "],\"kbPerFrame\":[" << double(bytesTextured) / Frames / 1024 << ',' << double(bytesBytes) / Frames / 1024
              << "],\"msPerFrame\":{\"texture\":" << textureMs / Frames << ",\"readbackAndBytes\":" << byteMs / Frames << "},\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
  } catch (const std::exception& error) { std::cerr << "mft texture test failed: " << error.what() << "\n"; return 1; }
}
