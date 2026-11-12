// Real RemoteMediaSession, RemoteEngine/RemoteTrackEngine and Windows CPU H.264
// decoder on the recorded native fixtures (argv[1]); the Flutter registrar is a
// fixture. Checks that every stream end hands its last decoded frame to the
// thumbnail writer and that the periodic read copies nothing. It does not
// prove VM capture, relay behaviour or a session window's lifecycle.
#include "remote_media_session.h"
#include "remote_track_configuration.h"
#include "thumbnail_writer.h"
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct Registrar final : flutter::TextureRegistrar {
  std::mutex mutex;
  std::map<int64_t, flutter::TextureVariant*> textures;
  std::vector<const FlutterDesktopPixelBuffer*> held;
  int64_t next = 0;
  ~Registrar() { Release(); }
  int64_t RegisterTexture(flutter::TextureVariant* texture) override {
    std::lock_guard<std::mutex> lock(mutex);
    textures.emplace(++next, texture);
    return next;
  }
  // Like Flutter, keep the most recent pixel buffer until the next one.
  bool MarkTextureFrameAvailable(int64_t id) override {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = textures.find(id);
    if (it == textures.end()) return false;
    const auto pixels = std::get<flutter::PixelBufferTexture>(*it->second).CopyPixelBuffer(0, 0);
    if (!pixels) return false;
    held.push_back(pixels);
    if (held.size() > 2) { held.front()->release_callback(held.front()->release_context); held.erase(held.begin()); }
    return true;
  }
  void UnregisterTexture(int64_t id, std::function<void()> callback) override {
    { std::lock_guard<std::mutex> lock(mutex); textures.erase(id); }
    if (callback) callback();
  }
  bool UnregisterTexture(int64_t id) override { UnregisterTexture(id, {}); return true; }
  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto* pixels : held) pixels->release_callback(pixels->release_context);
    held.clear();
  }
};

std::vector<uint8_t> load(const std::filesystem::path& directory, unsigned track, unsigned frame) {
  const auto name = "topology-1-track-" + std::to_string(track) + "-frame-" + std::to_string(frame) + ".tvt1";
  std::ifstream file(directory / name, std::ios::binary);
  require(bool(file), "Native fixture is unavailable");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// The legacy single-output envelope carrying the same H.264 access unit.
std::vector<uint8_t> legacy(const std::vector<uint8_t>& bytes) {
  multitap::tracks::Packet parsed;
  require(multitap::tracks::parse(bytes.data(), bytes.size(), parsed) && parsed.kind == 1, "Invalid fixture envelope");
  std::vector<uint8_t> media(12 + parsed.size);
  media[0] = 1; media[1] = parsed.flags;
  multitap::tracks::write64(media.data() + 4, parsed.pts);
  std::copy(parsed.payload, parsed.payload + parsed.size, media.begin() + 12);
  return media;
}

// The engine queue is bounded (backpressure); retry like the relay reader.
void push(multitap::RemoteMediaSession& owner, int64_t lease, const std::vector<uint8_t>& bytes) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (owner.Push(lease, bytes) != S_OK) {
    require(std::chrono::steady_clock::now() < end, "Packet rejected");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

int64_t decoded(multitap::RemoteMediaSession& owner, int64_t lease) {
  flutter::EncodableMap stats;
  require(owner.Stats(lease, stats) == S_OK, "Stats rejected");
  int64_t value = -1;
  require(multitap::MediaInteger(multitap::MediaField(stats, "decodedFrames"), value), "No decode count");
  return value;
}

void await(multitap::RemoteMediaSession& owner, int64_t lease, int64_t frames) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (decoded(owner, lease) < frames && std::chrono::steady_clock::now() < end)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  require(decoded(owner, lease) == frames, "Incomplete actual decoding");
}

void idle(multitap::ThumbnailWriter& writer) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!writer.Idle() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  require(writer.Idle(), "Writer did not finish");
}

std::pair<UINT, UINT> pictureSize(IWICImagingFactory* factory, const std::wstring& path) {
  ComPtr<IWICBitmapDecoder> decoder;
  require(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)),
          "Picture is not decodable");
  ComPtr<IWICBitmapFrameDecode> picture;
  require(SUCCEEDED(decoder->GetFrame(0, &picture)), "No picture frame");
  UINT w = 0, h = 0;
  require(SUCCEEDED(picture->GetSize(&w, &h)), "No picture size");
  return {w, h};
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 1;
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
  const std::filesystem::path fixtures(argv[1]);
  const auto root = std::filesystem::temp_directory_path() / (L"multitap-thumbnail-engine-" + std::to_wstring(GetCurrentProcessId()));
  int code = 0;
  try {
    ComPtr<IWICImagingFactory> factory;
    require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC unavailable");
    const auto tracksTarget = multitap::ThumbnailWriter::Resolve("0123456789abcdef", "pve-149", root.wstring());
    const auto legacyTarget = multitap::ThumbnailWriter::Resolve("0123456789abcdef", "pve-150", root.wstring());
    require(tracksTarget && legacyTarget, "Targets");
    multitap::ThumbnailWriter writer;
    std::vector<std::pair<uint32_t, uint32_t>> retired;
    double latestMicros = 0;

    {
      // Independent outputs: the primary output's frame is the picture.
      Registrar registrar;
      multitap::RemoteMediaSession owner(&registrar);
      const multitap::ThumbnailWriter::Target* target = &*tracksTarget;
      owner.OnRetiredFrame([&](std::shared_ptr<const multitap::DecodedFrame> frame) {
        retired.emplace_back(frame->width, frame->height);
        writer.Submit(*target, std::move(frame));
      });
      using Track = multitap::tracks::Track;
      const multitap::tracks::Configuration configuration{
          1, false, {Track{1, 1, 800, 600, 800, 600, 0, 0, false}, Track{0, 1, 640, 360, 640, 360, -640, -100, true}}};
      std::map<uint8_t, int64_t> textures;
      require(owner.StartTracks(1, configuration, textures) == S_OK, "Track engine did not start");
      int64_t lease = -1;
      uint64_t serial = 0;
      require(!owner.Latest(lease, serial), "A frame existed before decoding");
      for (unsigned frame = 0; frame < 8; ++frame)
        for (unsigned track : {1u, 0u}) push(owner, 1, load(fixtures, track, frame));
      await(owner, 1, 16);
      const auto latest = owner.Latest(lease, serial);
      require(latest && lease == 1 && serial == 8 && latest->width == 640 && latest->height == 360, "Primary output not chosen");
      // The periodic path: a shared reference only.
      const auto begin = std::chrono::steady_clock::now();
      for (int i = 0; i < 10000; ++i) { int64_t l = 0; uint64_t s = 0; require(owner.Latest(l, s) == latest, "Latest changed"); }
      latestMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / 10000;
      require(retired.empty(), "A running stream retired a frame");
      require(owner.Stop(1) == S_OK, "Stop failed");
      require(retired.size() == 1 && retired.back() == std::make_pair(640u, 360u), "Stop did not hand over the last primary frame");
      require(!owner.Latest(lease, serial), "A stopped stream still reported a frame");
      registrar.Release();
    }
    {
      // Legacy single output, ended by closing the window (owner destroyed).
      Registrar registrar;
      auto owner = std::make_unique<multitap::RemoteMediaSession>(&registrar);
      owner->OnRetiredFrame([&](std::shared_ptr<const multitap::DecodedFrame> frame) {
        retired.emplace_back(frame->width, frame->height);
        writer.Submit(*legacyTarget, std::move(frame));
      });
      int64_t texture = -1;
      // A stream that ends before any frame leaves no picture.
      require(owner->Start(2, 800, 600, false, texture) == S_OK && texture > 0, "Legacy engine did not start");
      require(owner->Stop(2) == S_OK && retired.size() == 1, "An empty stream produced a picture");
      require(owner->Start(3, 800, 600, false, texture) == S_OK, "Legacy restart failed");
      for (unsigned frame = 0; frame < 8; ++frame) push(*owner, 3, legacy(load(fixtures, 1, frame)));
      await(*owner, 3, 8);
      owner.reset();
      require(retired.size() == 2 && retired.back() == std::make_pair(800u, 600u), "Window close did not hand over the last frame");
      registrar.Release();
    }
    idle(writer);
    const auto done = writer.TakeCompleted();
    require(done.size() == 2, "Both pictures were not reported");
    require(pictureSize(factory.Get(), tracksTarget->path) == std::make_pair(480u, 270u), "Track picture size");
    require(pictureSize(factory.Get(), legacyTarget->path) == std::make_pair(360u, 270u), "Legacy picture size");
    std::cout << "{\"actualWindowsCpuDecodedFrames\":24,\"retiredFramesHandedOver\":" << retired.size()
              << ",\"primaryOutputChosen\":true,\"emptyStreamWroteNothing\":true,\"latestReadMicroseconds\":" << latestMicros
              << ",\"flutterRegistrarFixture\":true,\"actualVmStream\":false}" << std::endl;
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    code = 2;
  }
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  CoUninitialize();
  return code;
}
