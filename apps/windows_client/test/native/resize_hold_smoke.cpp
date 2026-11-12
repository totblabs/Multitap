// A viewport resize keeps the previous picture (RemoteMediaSession::Start
// `hold`, tuning backlog #1): the real RemoteMediaSession, RemoteEngine and
// Windows CPU H.264 decoder on the recorded native fixtures (argv[1]); the
// Flutter registrar is a fixture. Also times the new engine's start and its
// first picture. It does not prove a VM resize, the relay or a session
// window's lifecycle, and the times are this PC's, under its current load.
#include "remote_media_session.h"
#include "remote_track_configuration.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
using Size = std::pair<size_t, size_t>;

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
  bool Registered(int64_t id) { std::lock_guard<std::mutex> lock(mutex); return textures.count(id) != 0; }
  // What Flutter would draw for `id` now (width x height), or 0x0.
  std::pair<size_t, size_t> Shows(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = textures.find(id);
    if (it == textures.end()) return {0, 0};
    const auto* pixels = std::get<flutter::PixelBufferTexture>(*it->second).CopyPixelBuffer(0, 0);
    if (!pixels) return {0, 0};
    const std::pair<size_t, size_t> size{pixels->width, pixels->height};
    pixels->release_callback(pixels->release_context);
    return size;
  }
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
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(decoded(owner, lease) >= frames, "Incomplete actual decoding");
}

double ms(std::chrono::steady_clock::time_point from) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 1;
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
  const std::filesystem::path fixtures(argv[1]);
  int code = 0;
  try {
    Registrar registrar;
    multitap::RemoteMediaSession owner(&registrar);
    int64_t first = -1, second = -1;
    require(owner.Start(1, 800, 600, false, first) == S_OK && first > 0, "First engine did not start");
    for (unsigned frame = 0; frame < 8; ++frame) push(owner, 1, legacy(load(fixtures, 1, frame)));
    await(owner, 1, 8);
    require(registrar.Shows(first) == Size{800, 600}, "First picture missing");

    // The resize: the new size's engine starts while the old picture stays.
    auto began = std::chrono::steady_clock::now();
    require(owner.Start(2, 640, 360, false, second, {}, false, 60, 0, 0, true) == S_OK && second > 0 && second != first, "Holding start failed");
    const double startMs = ms(began);
    require(owner.HeldTexture() == first && registrar.Registered(first) && registrar.Registered(second), "The previous texture was not held");
    require(registrar.Shows(first) == Size{800, 600}, "The held texture lost its last picture");
    require(registrar.Shows(second) == Size{0, 0}, "The new texture showed a picture before decoding one");
    require(owner.Push(1, legacy(load(fixtures, 1, 0))) != S_OK, "The held engine still accepted media");
    began = std::chrono::steady_clock::now();
    for (unsigned frame = 0; frame < 8; ++frame) push(owner, 2, legacy(load(fixtures, 0, frame)));
    await(owner, 2, 1);
    const double firstPictureMs = ms(began);
    await(owner, 2, 8);
    require(registrar.Shows(second) == Size{640, 360}, "The new size's picture missing");
    require(registrar.Shows(first) == Size{800, 600} && owner.HeldTexture() == first, "The held picture went before its release");
    // The view switched: the held picture goes, once.
    require(!owner.ReleaseHeld(second) && owner.ReleaseHeld(first) && !registrar.Registered(first) && owner.HeldTexture() == -1, "Release did not drop exactly the held picture");
    require(!owner.ReleaseHeld(first) && registrar.Registered(second), "A second release changed something");

    // One picture is held at most; a later ordinary start lets it go.
    int64_t third = -1, fourth = -1, fifth = -1;
    require(owner.Start(3, 640, 360, false, third, {}, false, 60, 0, 0, true) == S_OK && owner.HeldTexture() == second, "Second hold failed");
    require(owner.Start(4, 640, 360, false, fourth) == S_OK && owner.HeldTexture() == -1 && !registrar.Registered(second) && !registrar.Registered(third), "An ordinary start kept a held picture");
    // Stop and window close let it go too.
    require(owner.Start(5, 640, 360, false, fifth, {}, false, 60, 0, 0, true) == S_OK && owner.HeldTexture() == fourth, "Third hold failed");
    require(owner.Stop(5) == S_OK && owner.HeldTexture() == -1 && !registrar.Registered(fourth) && !registrar.Registered(fifth), "Stop kept a held picture");
    int64_t sixth = -1, seventh = -1;
    require(owner.Start(6, 640, 360, false, sixth) == S_OK && owner.Start(7, 640, 360, false, seventh, {}, false, 60, 0, 0, true) == S_OK && owner.HeldTexture() == sixth, "Fourth hold failed");
    owner.Close();
    require(registrar.textures.empty(), "Window close kept a texture");
    registrar.Release();
    std::cout << "{\"kind\":\"resize-hold\",\"heldPreviousPicture\":true,\"heldReleasedOnce\":true,\"laterStartStopCloseRelease\":true"
              << ",\"newEngineStartMs\":" << startMs << ",\"firstNewPictureMs\":" << firstPictureMs
              << ",\"actualWindowsCpuDecoder\":true,\"flutterRegistrarFixture\":true,\"actualVmResize\":false}" << std::endl;
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    code = 2;
  }
  CoUninitialize();
  return code;
}
