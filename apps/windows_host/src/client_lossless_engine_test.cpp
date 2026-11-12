// The actual legacy client RemoteEngine (bounded queue, worker thread, CPU
// decoder, lossless compositor, republish and Flutter pixel-buffer texture)
// fed with media-v1 packets from the production CPU encoder and the host
// LosslessTileSource, unscaled and with a downscaled video (encoder cap). The
// Flutter registrar is a fixture; synthetic pixels; no capture, relay or network.
#include "media.hpp"
#include "lossless_tile_source.hpp"
#include "remote_engine.h"
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>

namespace {
using V = flutter::EncodableValue;
constexpr int fps = 60, bitrate = 8000000;
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
int64_t integer(const flutter::EncodableMap& stats, const char* field) { return std::get<int64_t>(stats.at(V(field))); }
struct Registrar final : flutter::TextureRegistrar {
  std::mutex mutex; flutter::TextureVariant* texture = nullptr; int64_t serial = 0;
  int64_t RegisterTexture(flutter::TextureVariant* value) override { std::lock_guard lock(mutex); texture = value; return ++serial; }
  bool MarkTextureFrameAvailable(int64_t) override { return true; }
  void UnregisterTexture(int64_t, std::function<void()> callback) override { { std::lock_guard lock(mutex); texture = nullptr; } if (callback) callback(); }
  bool UnregisterTexture(int64_t id) override { UnregisterTexture(id, {}); return true; }
  // The pixels Flutter would draw now, at whatever size the engine published.
  Microsoft::WRL::ComPtr<ID3D11Device> reader; Microsoft::WRL::ComPtr<ID3D11DeviceContext> readerContext;
  std::vector<uint8_t> shownSurface(const FlutterDesktopGpuSurfaceDescriptor& surface) {
    if (!reader) require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &reader, nullptr, &readerContext)), "Reader device");
    Microsoft::WRL::ComPtr<ID3D11Texture2D> opened;
    require(SUCCEEDED(reader->OpenSharedResource(surface.handle, IID_PPV_ARGS(&opened))), "The shared handle cannot be opened");
    D3D11_TEXTURE2D_DESC desc{}; opened->GetDesc(&desc);
    require(desc.Width == surface.width && desc.Height == surface.height, "Descriptor size");
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging; require(SUCCEEDED(reader->CreateTexture2D(&desc, nullptr, &staging)), "Staging");
    readerContext->CopyResource(staging.Get(), opened.Get());
    D3D11_MAPPED_SUBRESOURCE map{}; require(SUCCEEDED(readerContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)), "Map");
    std::vector<uint8_t> value(size_t(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) for (UINT x = 0; x < desc.Width; ++x) {
      const uint8_t* p = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch + x * 4; uint8_t* o = &value[(size_t(y) * desc.Width + x) * 4];
      o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3];
    }
    readerContext->Unmap(staging.Get(), 0); return value;
  }
  std::vector<uint8_t> shown() {
    std::lock_guard lock(mutex); require(texture != nullptr, "No texture");
    if (auto* surface = std::get_if<flutter::GpuSurfaceTexture>(texture)) {
      const auto* descriptor = surface->ObtainDescriptor(0, 0);
      if (!descriptor) return {};
      auto value = shownSurface(*descriptor); descriptor->release_callback(descriptor->release_context); return value;
    }
    const auto* buffer = std::get<flutter::PixelBufferTexture>(*texture).CopyPixelBuffer(0, 0);
    if (!buffer) return {};
    std::vector<uint8_t> value(buffer->buffer, buffer->buffer + buffer->width * buffer->height * 4);
    buffer->release_callback(buffer->release_context); return value;
  }
};
bool exact(const std::vector<uint8_t>& bgra, const std::vector<uint8_t>& rgba) {
  if (bgra.size() != rgba.size()) return false;
  for (size_t i = 0; i < bgra.size(); i += 4) if (rgba[i] != bgra[i + 2] || rgba[i + 1] != bgra[i + 1] || rgba[i + 2] != bgra[i]) return false;
  return true;
}
std::vector<uint8_t> page(int width, int height, uint32_t seed) {
  std::mt19937 random(seed); std::vector<uint8_t> bgra(size_t(width) * height * 4);
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    uint8_t* p = &bgra[(size_t(y) * width + x) * 4]; const bool ink = ((x / 2 + y / 7 + int(seed)) % 9 == 0) && (random() % 2);
    p[0] = uint8_t(ink ? random() % 120 : 252); p[1] = uint8_t(ink ? random() % 160 : 250); p[2] = uint8_t(ink ? 60 + random() % 190 : 247); p[3] = 255;
  }
  return bgra;
}
// Capture's scaled path: StretchDIBits HALFTONE into the encoded size.
std::vector<uint8_t> downscale(const std::vector<uint8_t>& source, int sw, int sh, int w, int h) {
  if (sw == w && sh == h) return source;
  HDC dc = CreateCompatibleDC(nullptr); BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
  uint8_t* pixels = nullptr; HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0); require(dc && bitmap && pixels, "Downscale canvas");
  const auto previous = SelectObject(dc, bitmap); SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, nullptr);
  info.bmiHeader.biWidth = sw; info.bmiHeader.biHeight = -sh;
  require(StretchDIBits(dc, 0, 0, w, h, 0, 0, sw, sh, source.data(), &info, DIB_RGB_COLORS, SRCCOPY) != 0, "Downscale"); GdiFlush();
  std::vector<uint8_t> value(pixels, pixels + size_t(w) * h * 4); SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc); return value;
}
std::string run(int videoWidth, int videoHeight, int sourceWidth, int sourceHeight, const multitap::RemoteEngine::Options& options, bool gpuPath) {
  Registrar registrar; multitap::RemoteEngine engine(&registrar, options);
  require(engine.Start(uint32_t(videoWidth), uint32_t(videoHeight), false) > 0, "Engine start");
  host::Encoder encoder(videoWidth, videoHeight, fps, bitrate); host::FrameEncodePolicy policy(size_t(videoWidth) * videoHeight * 3 / 2, host::FrameRefinement::forCbr(bitrate, fps));
  host::LosslessTileSource source(uint32_t(sourceWidth), uint32_t(sourceHeight), host::LosslessSettings::forBitrate(bitrate), uint32_t(videoWidth), uint32_t(videoHeight)); host::ParallelWork helper;
  // Waits on the engine's own accounting with generous ceilings: an empty
  // queue precedes the popped packet's decode/publish, and a loaded release
  // machine needed more than the former fixed 20 ms (and 2-5 s ceilings).
  // Every pushed packet ends in exactly one decodeCalls (video, after its
  // publication) or losslessPackets (tiles, after any republish) increment.
  int64_t pushed = 0;
  const auto push = [&](std::vector<uint8_t> packet) {
    for (int n = 0; n < 30000 && integer(engine.Stats(), "queuedPackets") >= 6; ++n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(engine.Push(packet), "Engine queue rejected a packet"); ++pushed;
  };
  const auto drain = [&] {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
      const auto stats = engine.Stats(); require(integer(stats, "error") == 0, "Engine failed");
      const auto processed = integer(stats, "decodeCalls") + integer(stats, "losslessPackets");
      if (processed == pushed) return;
      require(processed < pushed && std::chrono::steady_clock::now() < until, "Engine did not finish its queued packets");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  auto frame = page(sourceWidth, sourceHeight, 1); std::vector<uint8_t> nv12(size_t(videoWidth) * videoHeight * 3 / 2);
  int exactAt = -1, changeExactAt = -1; uint64_t tilePackets = 0;
  for (int n = 0; n < 240; ++n) {
    if (n == 120) frame = page(sourceWidth, sourceHeight, 2);  // a whole-page change
    const auto encoded = downscale(frame, sourceWidth, sourceHeight, videoWidth, videoHeight);
    host::bgraToNv12(encoded.data(), nv12.data(), videoWidth, videoHeight);
    const uint64_t now = uint64_t(n + 1) * 1000000 / fps;
    host::BufferComposedFrame composed(frame.data(), sourceWidth, sourceHeight, n == 0 || n == 120 ? std::nullopt : std::optional<std::vector<host::capture::PixelRect>>(std::vector<host::capture::PixelRect>{}));
    std::vector<host::Frame> outputs;
    const auto tick = host::losslessTick(source, helper, composed, policy, nv12, now, [&] { encoder.encode(nv12.data(), now, [&](host::Frame output) { outputs.push_back(std::move(output)); }); });
    if (tick.payload) { push(host::mediaPacket(1, now - 1, *tick.payload, false)); ++tilePackets; }
    for (auto& output : outputs) { require(policy.output(output.ptsUs, output.key, now, output.bytes.size()), "Encoder output"); push(host::mediaPacket(1, output.ptsUs, output.bytes, output.key)); }
    drain();
    const auto shown = registrar.shown(); const bool same = exact(frame, shown);
    if (same && n < 120 && exactAt < 0) exactAt = n;
    if (same && n >= 120 && changeExactAt < 0) changeExactAt = n - 120;
  }
  // drain() above already waited for every packet's accounting.
  const auto stats = engine.Stats();
  std::ostringstream out;
  out << "{\"path\":\"" << (gpuPath ? "gpu" : "cpu") << "\",\"video\":\"" << videoWidth << 'x' << videoHeight << "\",\"source\":\"" << sourceWidth << 'x' << sourceHeight << "\",\"ticksToExact\":" << exactAt << ",\"ticksToExactAfterChange\":" << changeExactAt
      << ",\"tilePackets\":" << tilePackets << ",\"republishes\":" << integer(stats, "losslessRepublishes") << ",\"presentTiles\":" << integer(stats, "losslessPresentTiles")
      << ",\"scaledSource\":" << (std::get<bool>(stats.at(V("losslessScaledSource"))) ? "true" : "false") << ",\"composeMs\":" << std::get<double>(stats.at(V("losslessComposeMs")))
      << ",\"packetMs\":" << std::get<double>(stats.at(V("losslessPacketMs"))) << ",\"decodedFrames\":" << integer(stats, "decodedFrames") << '}';
  require(exactAt >= 0 && changeExactAt >= 0, "The engine's texture never became pixel-identical at source size");
  // The values go into the failure so a rare release-build failure explains itself.
  const std::string accounting = "Engine lossless accounting: losslessPackets=" + std::to_string(integer(stats, "losslessPackets")) +
      " tilePackets=" + std::to_string(tilePackets) + " decodeCalls=" + std::to_string(integer(stats, "decodeCalls")) +
      " pushed=" + std::to_string(pushed) + " republishes=" + std::to_string(integer(stats, "losslessRepublishes")) +
      " deferred=" + std::to_string(integer(stats, "losslessDeferred")) +
      " poisoned=" + (std::get<bool>(stats.at(V("losslessPoisoned"))) ? "true" : "false");
  // Tiles republish at once only when no video packet is already queued
  // behind them (remote_engine.cpp: republish = !video_next); otherwise the
  // next video frame carries them. Which one happens is engine-thread timing
  // (a loaded release machine deferred all 9 of 9), so either counts; the
  // pixel-exact checks above prove the tiles reached the texture.
  require(integer(stats, "losslessPackets") == int64_t(tilePackets) && integer(stats, "losslessRepublishes") + integer(stats, "losslessDeferred") > 0 && !std::get<bool>(stats.at(V("losslessPoisoned"))), accounting.c_str());
  require(std::get<bool>(stats.at(V("losslessScaledSource"))) == (videoWidth != sourceWidth), "Engine scaled-source state");
  require(integer(stats, "error") == 0, "Engine failed");
  require(integer(stats, "texturePath") == (gpuPath ? multitap::RemoteEngine::kTexturePathGpuSurface : multitap::RemoteEngine::kTexturePathPixelBuffer), "Texture path");
  engine.Stop();
  return out.str();
}
}

int main() {
  try {
    host::Runtime runtime;
    multitap::RemoteEngine::Options cpu, gpu;
    gpu.hardware_decode = true; gpu.gpu_present = true; gpu.allow_software_render = true; gpu.warp_render = true;
    const std::string unscaled = run(1280, 720, 1280, 720, cpu, false), scaled = run(960, 540, 1440, 810, cpu, false);
    const std::string gpuUnscaled = run(1280, 720, 1280, 720, gpu, true), gpuScaled = run(960, 540, 1440, 810, gpu, true);
    std::cout << "{\"kind\":\"client-lossless-engine\",\"cases\":[" << unscaled << ',' << scaled << ',' << gpuUnscaled << ',' << gpuScaled << "],\"actualRemoteEngine\":true,\"flutterRegistrarFixture\":true,\"actualNetwork\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "client lossless engine: " << error.what() << '\n'; return 1; }
}
