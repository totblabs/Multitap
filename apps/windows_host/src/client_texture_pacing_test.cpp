// texture-pacing-v1 on the actual legacy client RemoteEngine (H.264 decoder,
// lossless compositor, Flutter texture) fed with media-v1 packets from the
// production CPU encoder and the host LosslessTileSource. Every pacing and
// lossless case runs twice: on the CPU path (pixel-buffer texture) and on the
// GPU path (D3D11 conversion into a DXGI shared texture opened by a second
// device, the way Flutter's renderer opens it) with WARP as the "GPU". The
// hardware H.264 decoder is used where an adapter has one and replaced by the
// software decoder otherwise (also exercised with an injected failure).
// The Flutter registrar is a fixture that behaves like Flutter's: a mark requests
// one Flutter frame and this test plays the raster thread that takes it.
// Also classifies render adapters (render_environment.h) with fixed adapter
// descriptions and the real WARP device. No capture, relay, network or display.
#include "media.hpp"
#include "lossless_tile_source.hpp"
#include "remote_engine.h"
#include "render_environment.h"
#include <d3d11.h>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

namespace {
using V = flutter::EncodableValue;
using namespace std::chrono_literals;
unsigned checks = 0;
void require(bool ok, const char* reason) { ++checks; if (!ok) throw std::runtime_error(reason); }
int64_t integer(const flutter::EncodableMap& stats, const char* field) { return std::get<int64_t>(stats.at(V(field))); }
double real(const flutter::EncodableMap& stats, const char* field) { return std::get<double>(stats.at(V(field))); }

struct Flutter final : flutter::TextureRegistrar {
  std::mutex mutex; std::condition_variable changed;
  flutter::TextureVariant* texture = nullptr; int64_t serial = 0; uint64_t marks = 0;
  bool pauseNext = false, paused = false, resumed = false;
  int64_t RegisterTexture(flutter::TextureVariant* value) override { std::lock_guard lock(mutex); texture = value; return ++serial; }
  bool MarkTextureFrameAvailable(int64_t) override {
    std::unique_lock lock(mutex); ++marks; changed.notify_all();
    if (pauseNext) { pauseNext = false; paused = true; changed.notify_all(); changed.wait(lock, [this] { return resumed; }); paused = resumed = false; }
    return true;
  }
  void UnregisterTexture(int64_t, std::function<void()> callback) override { { std::lock_guard lock(mutex); texture = nullptr; } if (callback) callback(); }
  bool UnregisterTexture(int64_t id) override { UnregisterTexture(id, {}); return true; }
  // One Flutter frame: the raster thread takes the buffer, uploads (copies)
  // it and releases it. Like Flutter, no registrar lock is held meanwhile.
  // The "Flutter renderer" reading a GPU surface: another device on the same
  // adapter opens the shared handle and reads the BGRA texture back.
  Microsoft::WRL::ComPtr<ID3D11Device> reader; Microsoft::WRL::ComPtr<ID3D11DeviceContext> readerContext;
  std::vector<uint8_t> readSurface(const FlutterDesktopGpuSurfaceDescriptor& surface) {
    if (!reader) require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &reader, nullptr, &readerContext)), "Reader device");
    Microsoft::WRL::ComPtr<ID3D11Texture2D> opened;
    require(SUCCEEDED(reader->OpenSharedResource(surface.handle, IID_PPV_ARGS(&opened))), "The shared handle cannot be opened by another device");
    D3D11_TEXTURE2D_DESC desc{}; opened->GetDesc(&desc);
    require(desc.Width == surface.width && desc.Height == surface.height && desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM, "Descriptor does not describe the texture");
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    require(SUCCEEDED(reader->CreateTexture2D(&desc, nullptr, &staging)), "Staging");
    readerContext->CopyResource(staging.Get(), opened.Get());
    D3D11_MAPPED_SUBRESOURCE map{}; require(SUCCEEDED(readerContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)), "Map");
    std::vector<uint8_t> pixels(size_t(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) for (UINT x = 0; x < desc.Width; ++x) {
      const uint8_t* p = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch + x * 4; uint8_t* t = &pixels[(size_t(y) * desc.Width + x) * 4];
      t[0] = p[2]; t[1] = p[1]; t[2] = p[0]; t[3] = p[3];
    }
    readerContext->Unmap(staging.Get(), 0);
    return pixels;
  }
  std::vector<uint8_t> take() {
    flutter::TextureVariant* current = nullptr; { std::lock_guard lock(mutex); current = texture; }
    require(current != nullptr, "No texture");
    if (auto* surface = std::get_if<flutter::GpuSurfaceTexture>(current)) {
      require(surface->surface_type() == kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle, "GPU surface type");
      const auto* descriptor = surface->ObtainDescriptor(0, 0);
      require(descriptor && descriptor->handle && descriptor->struct_size == sizeof(*descriptor) && descriptor->format == kFlutterDesktopPixelFormatBGRA8888, "Flutter frame without a surface");
      auto pixels = readSurface(*descriptor);
      descriptor->release_callback(descriptor->release_context);
      return pixels;
    }
    const auto* buffer = std::get<flutter::PixelBufferTexture>(*current).CopyPixelBuffer(0, 0);
    require(buffer && buffer->buffer, "Flutter frame without pixels");
    std::vector<uint8_t> pixels(buffer->buffer, buffer->buffer + buffer->width * buffer->height * 4);
    buffer->release_callback(buffer->release_context);
    return pixels;
  }
  uint64_t markCount() { std::lock_guard lock(mutex); return marks; }
  // Generous ceilings (a loaded release machine); they return on the condition.
  bool awaitMarks(uint64_t count) { std::unique_lock lock(mutex); return changed.wait_for(lock, 60s, [&] { return marks >= count; }); }
  void armPause() { std::lock_guard lock(mutex); pauseNext = true; resumed = false; }
  bool awaitPaused() { std::unique_lock lock(mutex); return changed.wait_for(lock, 60s, [this] { return paused; }); }
  void resume() { std::lock_guard lock(mutex); resumed = true; changed.notify_all(); }
};
flutter::EncodableMap awaitDecoded(multitap::RemoteEngine& engine, int64_t count) {
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  for (;;) {
    auto stats = engine.Stats();
    require(integer(stats, "error") == S_OK, "Engine failed");
    if (integer(stats, "decodedFrames") >= count && integer(stats, "queuedPackets") == 0) return stats;
    require(std::chrono::steady_clock::now() < deadline, "Frames were not decoded in time");
    std::this_thread::sleep_for(1ms);
  }
}

// ---- pacing: flat frames whose grey level names the frame -----------------
constexpr int kWidth = 640, kHeight = 360, kFps = 60;
int level(int frame) { return 30 + frame * 14; }
int identify(const std::vector<uint8_t>& rgba) {
  const size_t at = (size_t(kHeight / 2) * kWidth + kWidth / 2) * 4;
  return int(std::lround((double(rgba[at + 1]) - 30.0) / 14.0));
}
std::vector<std::vector<uint8_t>> flatPackets(int frames) {
  host::Encoder encoder(kWidth, kHeight, kFps, 4000000, host::EncoderTuning{}, host::EncoderBackend::mft);
  std::vector<std::vector<uint8_t>> packets;
  std::vector<uint8_t> bgra(size_t(kWidth) * kHeight * 4), nv12(size_t(kWidth) * kHeight * 3 / 2);
  const auto output = [&](host::Frame frame) { packets.push_back(host::mediaPacket(1, frame.ptsUs, frame.bytes, frame.key)); };
  for (int frame = 0; frame < frames; ++frame) {
    for (size_t i = 0; i < bgra.size(); i += 4) { bgra[i] = bgra[i + 1] = bgra[i + 2] = uint8_t(level(frame)); bgra[i + 3] = 255; }
    host::bgraToNv12(bgra.data(), nv12.data(), kWidth, kHeight);
    encoder.encode(nv12.data(), uint64_t(frame) * 1000000 / kFps, output);
  }
  encoder.finish(output);
  require(int(packets.size()) == frames && packets.front()[1] == 1, "Encoder did not produce one packet per frame from an IDR");
  return packets;
}
std::string pacing(const multitap::RemoteEngine::Options& options, bool gpuPath) {
  Flutter flutter; multitap::RemoteEngine engine(&flutter, options);
  require(engine.Start(kWidth, kHeight, false) > 0, "Engine start");
  const auto packets = flatPackets(12);
  int64_t published = 0;
  const auto push = [&](std::initializer_list<int> frames) {
    for (const int frame : frames) require(engine.Push(packets[size_t(frame)]), "Queue rejected a packet");
    published += int64_t(frames.size());
    auto stats = awaitDecoded(engine, published);
    // decodedFrames counts a frame before the worker publishes it; a take()
    // in between found no pixels (1 of 3 runs, idle machine). Wait for the
    // publications too.
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (integer(stats, "textureNotifyCalls") < published) {
      require(std::chrono::steady_clock::now() < deadline, "Frames were not published in time");
      std::this_thread::sleep_for(1ms);
      stats = engine.Stats();
    }
    return stats;
  };
  // One frame, taken: the normal case.
  push({0});
  require(identify(flutter.take()) == 0, "First frame");
  // Two frames that arrive within one refresh are both shown, in order.
  auto stats = push({1, 2});
  require(integer(stats, "textureQueued") == 1 && integer(stats, "skippedDecodedFrames") == 0, "A frame published behind a waiting one was not queued");
  const auto marks = flutter.markCount();
  require(identify(flutter.take()) == 1, "The older waiting frame is shown first");
  require(flutter.awaitMarks(marks + 1), "The worker did not request a Flutter frame for the next waiting frame");
  require(identify(flutter.take()) == 2, "The queued frame is shown on the next Flutter frame");
  require(identify(flutter.take()) == 2, "Without a new frame Flutter redraws the newest one");
  stats = engine.Stats();
  require(integer(stats, "textureFrames") == 3 && integer(stats, "textureRemarks") == 1, "Redraws counted as handed frames, or a missing remark");
  // A third frame while two wait: the oldest is dropped (bounded queue).
  stats = push({3, 4, 5});
  require(integer(stats, "skippedDecodedFrames") == 1, "The queue is not bounded to kPresentQueue frames");
  require(identify(flutter.take()) == 4 && identify(flutter.take()) == 5, "Bounded queue order");
  // A standing backlog (a frame still waiting from the previous take) shows the newest.
  push({6, 7});
  require(identify(flutter.take()) == 6, "Backlog start");
  stats = push({8});
  require(identify(flutter.take()) == 8, "A standing backlog was not trimmed to the newest frame");
  require(integer(engine.Stats(), "skippedDecodedFrames") == 2, "Backlog trim accounting");
  // Frames kept while Flutter drew nothing (hidden window): the newest is shown.
  push({9, 10});
  std::this_thread::sleep_for(multitap::RemoteEngine::kStaleFrame + 30ms);
  require(identify(flutter.take()) == 10, "A stale waiting frame was shown before the newest");
  push({11});
  require(identify(flutter.take()) == 11, "Final frame");
  stats = engine.Stats();
  const int64_t taken = integer(stats, "textureFrames");
  require(integer(stats, "decodedFrames") == 12 && taken + integer(stats, "skippedDecodedFrames") == 12, "Every published frame is either taken or counted as skipped");
  require(integer(stats, "textureUploads") == taken + 1 && real(stats, "textureUploadTotalMs") > 0, "Uploads (including the redraw) were not measured");
  require(real(stats, "textureWaitTotalMs") >= double(multitap::RemoteEngine::kStaleFrame.count()), "Publish -> take wait was not measured");
  require(integer(stats, "texturePath") == (gpuPath ? multitap::RemoteEngine::kTexturePathGpuSurface : multitap::RemoteEngine::kTexturePathPixelBuffer) && integer(stats, "textureNotifyCalls") == 12, "Path or publication accounting");
  if (gpuPath) require(real(stats, "gpuConvertTotalMs") > 0, "GPU conversion was not measured");
  require(integer(stats, "decodeTier") >= 0 && integer(stats, "decodeTier") <= 2 && (integer(stats, "decodeTier") != 0) == std::get<bool>(stats.at(V("hardwareDecode"))), "Decode tier and hardware flag disagree");
  engine.Stop();
  std::ostringstream out;
  out << "{\"case\":\"pacing\",\"path\":\"" << (gpuPath ? "gpu" : "cpu") << "\",\"hardwareDecode\":" << (std::get<bool>(stats.at(V("hardwareDecode"))) ? "true" : "false") << ",\"decoded\":12,\"taken\":" << taken << ",\"skipped\":" << integer(stats, "skippedDecodedFrames") << ",\"queued\":" << integer(stats, "textureQueued") << ",\"remarks\":" << integer(stats, "textureRemarks") << '}';
  return out.str();
}

// ---- lossless: deferred and replaced republishes stay pixel-exact ---------
std::vector<uint8_t> page(int width, int height, uint32_t seed) {
  std::mt19937 random(seed); std::vector<uint8_t> bgra(size_t(width) * height * 4);
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    uint8_t* p = &bgra[(size_t(y) * width + x) * 4]; const bool ink = ((x / 2 + y / 7 + int(seed)) % 9 == 0) && (random() % 2);
    p[0] = uint8_t(ink ? random() % 120 : 252); p[1] = uint8_t(ink ? random() % 160 : 250); p[2] = uint8_t(ink ? 60 + random() % 190 : 247); p[3] = 255;
  }
  return bgra;
}
bool exact(const std::vector<uint8_t>& bgra, const std::vector<uint8_t>& rgba) {
  if (bgra.size() != rgba.size()) return false;
  for (size_t i = 0; i < bgra.size(); i += 4) if (rgba[i] != bgra[i + 2] || rgba[i + 1] != bgra[i + 1] || rgba[i + 2] != bgra[i]) return false;
  return true;
}
constexpr int width = 1280, height = 720, bitrate = 8000000, animated = 120, ticks = 260;
constexpr int blockX = 192, blockY = 128, blockSize = 64;
struct Packet { std::vector<uint8_t> bytes; bool tile = false; };
struct Recording { std::vector<Packet> stream; std::vector<uint8_t> frame; };
// Record a media-v1 stream of a page whose one block animates for the first
// ticks (video every tick, tiles for the settled rest) and then settles:
// per tick the tile packet (pts T-1) before its video (pts T).
Recording record() {
  std::vector<Packet> stream;
  auto frame = page(width, height, 7);
  {
    host::Encoder encoder(width, height, kFps, bitrate, host::EncoderTuning{}, host::EncoderBackend::mft);
    host::FrameEncodePolicy policy(size_t(width) * height * 3 / 2, host::FrameRefinement::forCbr(bitrate, kFps));
    auto settings = host::LosslessSettings::forBitrate(bitrate); settings.tickBudgetMs = settings.motionTickBudgetMs = 0;
    host::LosslessTileSource source(width, height, settings); host::ParallelWork helper;
    std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2);
    for (int n = 0; n < ticks; ++n) {
      const uint64_t now = uint64_t(n + 1) * 1000000 / kFps;
      std::vector<host::capture::PixelRect> dirty;
      if (n > 0 && n <= animated) {
        for (int y = blockY; y < blockY + blockSize; ++y) for (int x = blockX; x < blockX + blockSize; ++x) {
          uint8_t* p = &frame[(size_t(y) * width + x) * 4]; p[0] = uint8_t(n * 2); p[1] = uint8_t(255 - n); p[2] = uint8_t((x + y + n) & 255);
        }
        dirty.push_back({blockX, blockY, blockX + blockSize, blockY + blockSize});
      }
      host::bgraToNv12(frame.data(), nv12.data(), width, height);
      host::BufferComposedFrame composed(frame.data(), width, height, n == 0 ? std::nullopt : std::optional<std::vector<host::capture::PixelRect>>(dirty));
      std::vector<host::Frame> outputs;
      const auto tick = host::losslessTick(source, helper, composed, policy, nv12, now, [&] { encoder.encode(nv12.data(), now, [&](host::Frame output) { outputs.push_back(std::move(output)); }); });
      if (tick.payload) stream.push_back({host::mediaPacket(1, now - 1, *tick.payload, false), true});
      for (auto& output : outputs) { require(policy.output(output.ptsUs, output.key, now, output.bytes.size()), "Encoder output"); stream.push_back({host::mediaPacket(1, output.ptsUs, output.bytes, output.key), false}); }
    }
  }
  return {std::move(stream), std::move(frame)};
}
std::string lossless(const Recording& recording, const multitap::RemoteEngine::Options& options, bool gpuPath) {
  const auto& stream = recording.stream; const auto& frame = recording.frame;
  // Added tiles of a tile packet (12-byte media envelope, 24-byte TSL1 header).
  const auto adds = [](const Packet& packet) { return packet.tile ? multitap::lossless::read16(packet.bytes.data() + 12 + 22) : 0; };
  // A tile packet between two video packets: the worker is held inside the
  // earlier video's publication while the tile and next video are queued.
  size_t triple = 0;
  for (size_t i = 1; i + 1 < stream.size() && !triple; ++i) if (!stream[i - 1].tile && adds(stream[i]) && !stream[i + 1].tile) triple = i;
  require(triple > 0, "The recorded stream has no tile packet between two video packets");
  // A later video, tile, video sequence fed one at a time without Flutter
  // taking a frame: the tile packet is republished (its video is not queued
  // yet) and replaces the waiting video frame it copied; the next video frame
  // then replaces that republish. Nothing queues behind a superseded frame.
  size_t lone = 0;
  // (At least one ordinary packet after the held triple drains its waiting frame.)
  for (size_t i = triple + 4; i + 1 < stream.size() && !lone; ++i) if (!stream[i - 1].tile && adds(stream[i]) && !stream[i + 1].tile) lone = i;
  require(lone > 0, "The recorded stream has no later tile packet between two video packets");

  Flutter flutter; multitap::RemoteEngine engine(&flutter, options);
  require(engine.Start(width, height, false) > 0, "Engine start");
  int64_t decoded = 0, pushed = 0;
  const auto push = [&](const std::vector<uint8_t>& bytes) { const bool accepted = engine.Push(bytes); if (accepted) ++pushed; return accepted; };
  // Settled = every pushed packet accounted for: decodeCalls counts a video
  // packet after its publication/republish, losslessPackets a tile packet after
  // its republish or deferral. An empty queue (and a fixed 20 ms) preceded that
  // accounting on a loaded machine, so exact counts below could be read early.
  const auto settle = [&] {
    const auto until = std::chrono::steady_clock::now() + 60s;
    for (;;) {
      const auto stats = engine.Stats();
      if (integer(stats, "error") != S_OK) throw std::runtime_error("Engine failed");
      if (integer(stats, "decodeCalls") + integer(stats, "losslessPackets") == pushed) return;
      if (std::chrono::steady_clock::now() >= until) throw std::runtime_error("Engine did not finish its queued packets");
      std::this_thread::sleep_for(1ms);
    }
  };
  int64_t replacedSkips = -1;
  for (size_t i = 0; i < stream.size(); ++i) {
    if (i == triple - 1) {
      flutter.armPause();
      require(push(stream[i].bytes), "Queue rejected video");
      require(flutter.awaitPaused(), "The worker did not reach the paused publication");
      require(push(stream[i + 1].bytes) && push(stream[i + 2].bytes), "Queue rejected tile/video");
      const auto before = engine.Stats();
      flutter.resume();
      settle();
      const auto after = engine.Stats();
      require(integer(after, "losslessDeferred") == integer(before, "losslessDeferred") + 1, "A tile packet followed by its video was republished");
      require(integer(after, "losslessRepublishes") == integer(before, "losslessRepublishes"), "Deferred tiles were also republished");
      require(integer(after, "decodedFrames") == integer(before, "decodedFrames") + 1, "Tile + video published more than the video frame");
      flutter.take();
      i += 2; continue;
    }
    if (i == lone - 1) {
      const auto start = engine.Stats();
      require(push(stream[i].bytes), "Queue rejected video"); settle();
      require(push(stream[i + 1].bytes), "Queue rejected tile"); settle();
      const auto republished = engine.Stats();
      require(integer(republished, "losslessRepublishes") == integer(start, "losslessRepublishes") + 1, "A tile packet alone was not republished");
      require(integer(republished, "skippedDecodedFrames") == integer(start, "skippedDecodedFrames") + 1, "The republish did not replace the waiting frame it copied");
      require(push(stream[i + 2].bytes), "Queue rejected video"); settle();
      const auto replaced = engine.Stats();
      replacedSkips = integer(replaced, "skippedDecodedFrames") - integer(start, "skippedDecodedFrames");
      require(replacedSkips == 2 && integer(replaced, "decodedFrames") == integer(start, "decodedFrames") + 3, "The untaken republish was not replaced by the video frame");
      require(integer(replaced, "textureQueued") == integer(start, "textureQueued"), "A superseded frame was queued in front of a newer one");
      const auto taken = integer(replaced, "textureFrames");
      flutter.take();
      require(integer(engine.Stats(), "textureFrames") == taken + 1, "The video frame was not handed after its replaced frames");
      i += 2; continue;
    }
    require(push(stream[i].bytes), "Queue rejected a packet");
    settle();
    // Before the first picture exists Flutter has nothing to draw: wait for it (a
    // packet is "decoded" once the decoder call returned, not once a picture came out).
    if (i == 0) { const auto until = std::chrono::steady_clock::now() + 60s; while (integer(engine.Stats(), "decodedFrames") == 0) { require(std::chrono::steady_clock::now() < until, "No first picture"); std::this_thread::sleep_for(1ms); } }
    flutter.take();
  }
  settle();
  const auto shown = flutter.take();
  const auto stats = engine.Stats();
  decoded = integer(stats, "decodedFrames");
  require(exact(frame, shown), "The shown frame is not pixel-exact after deferred/replaced republishes");
  require(integer(stats, "losslessPresentTiles") > 0 && !std::get<bool>(stats.at(V("losslessPoisoned"))) && integer(stats, "error") == 0, "Lossless state");
  engine.Stop();
  std::ostringstream out;
  out << "{\"case\":\"lossless\",\"path\":\"" << (gpuPath ? "gpu" : "cpu") << "\",\"packets\":" << stream.size() << ",\"decoded\":" << decoded << ",\"deferred\":" << integer(stats, "losslessDeferred")
      << ",\"republishes\":" << integer(stats, "losslessRepublishes") << ",\"replacedRepublishSkips\":" << replacedSkips << ",\"exact\":true}";
  return out.str();
}

// ---- render environment classification ------------------------------------
std::string environment() {
  require(multitap::SoftwareAdapter(0x1414, 0x8c, 0), "The Basic Render Driver without the software flag is a software adapter");
  require(multitap::SoftwareAdapter(0x10de, 0x2684, DXGI_ADAPTER_FLAG_SOFTWARE), "A software-flagged adapter is a software adapter");
  require(!multitap::SoftwareAdapter(0x10de, 0x2684, 0) && !multitap::SoftwareAdapter(0x8086, 0x46a6, 0) && !multitap::SoftwareAdapter(0x1414, 0x5353, 0), "Hardware adapters are not software");
  require(!multitap::InspectRenderAdapter(nullptr).known, "No adapter is unknown, not software");
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)), "WARP device");
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi; Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  require(SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)), "WARP adapter");
  const auto warp = multitap::InspectRenderAdapter(adapter.Get());
  require(warp.known && !warp.hardware, "WARP is reported as hardware");
  const auto hz = multitap::CompositionRefreshHz();
  require(hz >= 0 && hz <= 1000, "Composition rate bounds");
  std::ostringstream out;
  out << "{\"case\":\"environment\",\"warpHardware\":false,\"compositionHz\":" << hz << ",\"remoteSession\":" << (multitap::RemoteDesktopSession() ? "true" : "false") << '}';
  return out.str();
}
// ---- hardware decoder failure: the software decoder takes over mid-stream --
// The decoder pretends to be the hardware one and fails a Decode call; the
// fallback replays the packets since the last IDR, so the pictures are the same
// as those of an undisturbed software decoder and nothing is lost.
std::string fallback() {
  const auto packets = flatPackets(24);
  struct Output { uint64_t fallbacks = 0; HRESULT reason = S_OK; bool hardware = true; std::vector<std::pair<uint64_t, std::vector<uint8_t>>> frames; };
  const auto run = [&](int failAt) {
    Output out;
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) , "COM");
    require(SUCCEEDED(MFStartup(MF_VERSION)), "Media Foundation");
    {
      multitap::RemoteDecoder decoder; multitap::RemoteDecoder::Options options; options.test_fail_hardware_after = failAt;
      require(SUCCEEDED(decoder.Initialize(kWidth, kHeight, options)) && decoder.hardware() == (failAt >= 0), "Decoder start");
      for (const auto& packet : packets) {
        uint64_t pts = 0; std::memcpy(&pts, packet.data() + 4, 8);
        require(SUCCEEDED(decoder.Decode(packet.data() + 12, packet.size() - 12, pts, [&](multitap::DecodedFrame&& frame) { out.frames.push_back({frame.pts_us, std::move(frame.rgba)}); })), "Decode after a hardware failure");
      }
      out.fallbacks = decoder.fallbacks(); out.reason = decoder.fallback_reason(); out.hardware = decoder.hardware();
    }
    MFShutdown(); CoUninitialize();
    return out;
  };
  const auto reference = run(-1);
  require(reference.frames.size() == packets.size() && reference.fallbacks == 0, "Reference decode");
  for (const int failAt : {0, 1, 7, 23}) {
    const auto result = run(failAt);
    require(result.fallbacks == 1 && result.reason == E_FAIL && !result.hardware, "A hardware failure was not turned into a software fallback");
    require(result.frames.size() == reference.frames.size(), "The fallback lost or repeated pictures");
    for (size_t i = 0; i < reference.frames.size(); ++i) require(result.frames[i] == reference.frames[i], "A picture differs after the fallback");
  }
  return "{\"case\":\"fallback\",\"failedAt\":[0,1,7,23],\"pictures\":24,\"identicalToSoftware\":true}";
}

// ---- stats and the user's "CPU only" choice --------------------------------
std::string selection(const multitap::RemoteEngine::Options& gpuOptions) {
  Flutter flutter;
  // Hardware decoding requested but switched off by the user: CPU decode, tier 0.
  auto options = gpuOptions; options.hardware_decode = false;
  multitap::RemoteEngine off(&flutter, options);
  require(off.Start(kWidth, kHeight, false) > 0, "Engine start");
  auto stats = off.Stats();
  require(!std::get<bool>(stats.at(V("hardwareDecode"))) && integer(stats, "decodeTier") == 0 && integer(stats, "decodeVendor") == 0, "Hardware decode reported while switched off");
  require(std::get<std::string>(stats.at(V("decoderPath"))) == "software-mft", "Decoder path while switched off");
  off.Stop();
  // GPU presentation not wanted (or no render adapter): the pixel buffer path.
  options = gpuOptions; options.gpu_present = false;
  multitap::RemoteEngine cpu(&flutter, options);
  require(cpu.Start(kWidth, kHeight, false) > 0, "Engine start");
  require(integer(cpu.Stats(), "texturePath") == multitap::RemoteEngine::kTexturePathPixelBuffer, "CPU presentation was not selected");
  cpu.Stop();
  // A software render adapter (WARP, Basic Render Driver) without the test
  // override never presents on the GPU: the CPU path is faster there.
  options = gpuOptions; options.allow_software_render = false;
  multitap::RemoteEngine software(&flutter, options);
  require(software.Start(kWidth, kHeight, false) > 0, "Engine start");
  require(integer(software.Stats(), "texturePath") == multitap::RemoteEngine::kTexturePathPixelBuffer, "A software adapter presented on the GPU");
  software.Stop();
  return "{\"case\":\"selection\",\"cpuDecodeSetting\":true,\"softwareAdapterUsesCpuPath\":true}";
}
// stream-stall-v1: a publication gap of 250 ms or more leaves one kind 1 record in
// Stats()["stalls"] ([kind, serial, gapMs, agoMs, ...]); the session turns it into a row.
std::string stalls(const multitap::RemoteEngine::Options& options) {
  Flutter flutter; multitap::RemoteEngine engine(&flutter, options);
  require(engine.Start(kWidth, kHeight, false) > 0, "Engine start");
  const auto packets = flatPackets(2);
  const auto records = [&] { const auto stats = engine.Stats(); return std::get<flutter::EncodableList>(stats.at(V("stalls"))); };
  const auto publish = [&](size_t frame) { const auto marks = flutter.markCount(); require(engine.Push(packets[frame]), "Queue rejected a packet"); require(flutter.awaitMarks(marks + 1), "The frame was not published"); flutter.take(); };
  publish(0);
  const size_t before = records().size();
  std::this_thread::sleep_for(300ms);
  publish(1);
  const auto list = records();
  require(list.size() == before + 1, "A 300 ms publication gap did not leave one stall record");
  const auto& stall = std::get<flutter::EncodableList>(list.back());
  require(stall.size() == 14 && std::get<int64_t>(stall[0]) == 1 && std::get<int64_t>(stall[2]) >= 300, "The stall record is not kind 1 with the gap");
  engine.Stop();
  return "{\"case\":\"stall\",\"gapMs\":" + std::to_string(std::get<int64_t>(stall[2])) + "}";
}
}

int main() {
  try {
    host::Runtime runtime;
    multitap::RemoteEngine::Options cpu, gpu;
    gpu.hardware_decode = true; gpu.gpu_present = true; gpu.allow_software_render = true; gpu.warp_render = true;
    // First: the pacing cases below are timing-sensitive on a loaded machine.
    const std::string s = stalls(cpu);
    const std::string a = pacing(cpu, false), b = pacing(gpu, true);
    const auto recording = record();
    const std::string c = lossless(recording, cpu, false), d = lossless(recording, gpu, true);
    const std::string e = fallback(), f = selection(gpu), g = environment();
    std::cout << "{\"kind\":\"client-texture-pacing\",\"checks\":" << checks << ",\"cases\":[" << s << ',' << a << ',' << b << ',' << c << ',' << d << ',' << e << ',' << f << ',' << g << "],\"actualRemoteEngine\":true,\"flutterRegistrarFixture\":true,\"gpuAdapter\":\"warp\",\"actualFlutterRaster\":false,\"actualNetwork\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "client texture pacing: " << error.what() << '\n'; return 1; }
}
