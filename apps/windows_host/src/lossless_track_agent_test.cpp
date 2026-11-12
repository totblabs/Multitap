// Agent-level lossless tiles over tracks-v1: the actual Agent worker thread,
// TrackSourcePipeline, host gate and Windows CPU encoders, and the client's
// RemoteTrackDecoder with its compositors. Synthetic owned-output pixels; no
// desktop capture, relay or network.
#include "agent.hpp"
#include "remote_track_decoder.h"
#include "../../windows_client/test/native/track_gpu_fixture.hpp"
#include <iostream>
#include <random>

namespace {
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
struct Source final : host::TrackCapture {
  int width, height; std::vector<uint8_t> bgra, nv12; std::unique_ptr<host::BufferComposedFrame> view; bool first = true;
  std::function<void(std::vector<uint8_t>&)> change;  // applied by the capture thread between ticks
  std::mutex* lock;
  Source(const host::OwnedTrackSource& output, std::mutex& mutex) : width(int(output.media.width)), height(int(output.media.height)), bgra(size_t(width) * height * 4), nv12(size_t(width) * height * 3 / 2), lock(&mutex) {
    std::mt19937 random(output.media.id + 3);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
      uint8_t* p = &bgra[(size_t(y) * width + x) * 4]; const bool ink = (x / 3 + y / 5) % 7 == 0 && (random() % 3);
      p[0] = uint8_t(ink ? 40 + random() % 60 : 250); p[1] = uint8_t(ink ? 30 + random() % 90 : 248); p[2] = uint8_t(ink ? 90 + random() % 120 : 245); p[3] = 255;
    }
    host::bgraToNv12(bgra.data(), nv12.data(), width, height);
  }
  host::TrackCapturedFrame frame() override {
    std::lock_guard guard(*lock);
    const bool changed = bool(change);
    if (changed) { change(bgra); change = nullptr; host::bgraToNv12(bgra.data(), nv12.data(), width, height); }
    view = std::make_unique<host::BufferComposedFrame>(bgra.data(), width, height, first || changed ? std::nullopt : std::optional<std::vector<host::capture::PixelRect>>(std::vector<host::capture::PixelRect>{}));
    first = false;
    return {&nv12, true, 0, 0};
  }
  host::ComposedFrame* composed() override { return view.get(); }
};
host::Json command(const std::string& id) {
  return host::JsonParser::parse("{\"type\":\"session.start\",\"sessionId\":" + host::jsonQuote(id) + ",\"mode\":\"stream\",\"mediaProtocol\":\"tracks-v1\",\"topologyEpoch\":1,\"monitor\":\"all\",\"maxWidth\":1280,\"maxHeight\":720,\"fps\":60,\"bitrate\":8000000,\"audio\":false,\"tracks\":[{\"id\":\"0\",\"width\":640,\"height\":360,\"x\":-640,\"y\":-100,\"epoch\":1},{\"id\":\"1\",\"width\":800,\"height\":600,\"x\":0,\"y\":0,\"epoch\":1}]}");
}
host::DisplayState display() {
  host::DisplayState value; value.installed = value.deviceReady = value.owned = value.active = true; value.foreign = 0; value.width = 1440; value.height = 700; value.left = -1600; value.top = 300; value.name = L"\\\\.\\DISPLAY2";
  value.monitors = {{"0", L"\\\\.\\DISPLAY2", -1600, 300, 640, 360, false}, {"1", L"\\\\.\\DISPLAY3", -960, 400, 800, 600, false}}; return value;
}
std::vector<host::CaptureMonitor> observed() { return {{L"\\\\.\\DISPLAY2", L"owned0", {-1600, 300, -960, 660}, false}, {L"\\\\.\\DISPLAY3", L"owned1", {-960, 400, -160, 1000}, false}}; }
bool exact(const std::vector<uint8_t>& bgra, const std::vector<uint8_t>& rgba) {
  if (bgra.size() != rgba.size()) return false;
  for (size_t i = 0; i < bgra.size(); i += 4) if (rgba[i] != bgra[i + 2] || rgba[i + 1] != bgra[i + 1] || rgba[i + 2] != bgra[i]) return false;
  return true;
}
}

// gpu: the client decodes through the GPU presentation path (shared BGRA
// textures, tiles in a GPU overlay; WARP stands in for a GPU) instead of CPU RGBA.
int run(bool gpu) {
  try {
    host::Runtime runtime; std::mutex peer, pixels; host::Agent* agent = nullptr;
    std::unique_ptr<trackgpu::Pipelines> gpuPipelines; if (gpu) gpuPipelines = std::make_unique<trackgpu::Pipelines>();
    std::unique_ptr<multitap::RemoteTrackDecoder> decoder;
    std::array<std::shared_ptr<const multitap::DecodedFrame>, 2> shown; std::array<Source*, 2> sources{};
    std::string current; bool ready = false; uint64_t tilePackets = 0, videoPackets = 0; std::array<uint64_t, 2> lastPts{};
    host::Agent source([](const std::wstring&) {},
      [&](bool text, std::vector<uint8_t> bytes) {
        std::lock_guard lock(peer);
        if (text) {
          const auto message = host::JsonParser::parse(std::string(bytes.begin(), bytes.end()));
          if (message.get("type").string() == "stream.tracks.info") {
            current = message.get("sessionId").string(); decoder = std::make_unique<multitap::RemoteTrackDecoder>(); shown = {}; ready = false; lastPts = {};
            const auto media = host::bindOwnedTrackSources(command(current), display(), observed()).media(false);
            host::check(gpu ? decoder->ConfigurePipelines(media, gpuPipelines->pipelines) : decoder->Configure(media), "Client decoder start");
          }
          return;
        }
        multitap::tracks::Packet packet; require(multitap::tracks::parse(bytes.data(), bytes.size(), packet) && packet.kind == multitap::tracks::video, "Invalid track packet");
        require(!lastPts[packet.track] || packet.pts > lastPts[packet.track], "Per-track pts did not strictly increase (relay/gate would reject)");
        lastPts[packet.track] = packet.pts;
        if (multitap::lossless::isTilePayload(packet.payload, packet.size)) { require(!(packet.flags & 1), "Tile packet carried a key flag"); ++tilePackets; } else ++videoPackets;
        host::check(decoder->Push(bytes.data(), bytes.size(), [&](uint8_t id, uint32_t, uint32_t, multitap::DecodedFrame&& frame) { require(gpu ? (frame.gpu && frame.rgba.empty()) : !frame.gpu, "The client delivered the wrong picture kind"); shown[id] = std::make_shared<multitap::DecodedFrame>(std::move(frame)); }, {}, nullptr,
          [&](uint8_t id) { return shown[id]; }), "Client track decode");
        if (decoder->ready() && !ready) {
          ready = true;
          agent->localControl("{\"type\":\"stream.tracks.ready\",\"sessionId\":" + host::jsonQuote(current) + ",\"topologyEpoch\":1,\"tracks\":[{\"id\":\"0\",\"epoch\":1},{\"id\":\"1\",\"epoch\":1}]}");
        }
      },
      [](INPUT&) -> UINT { return 1; }, observed,
      [&](const host::OwnedTrackSource& output) -> std::unique_ptr<host::TrackCapture> { auto value = std::make_unique<Source>(output, pixels); std::lock_guard guard(pixels); sources[output.media.id] = value.get(); return value; },
      [] { return RECT{-1920, -200, 1920, 1080}; });
    agent = &source;
    // Waits on the condition it checks, with a generous ceiling: the actual
    // encoder/decoder threads run much slower on a loaded release machine (5-8 s
    // ceilings failed Host releases while other suites ran in parallel).
    constexpr int ceilingMs = 60000;
    const auto waitFor = [&](auto predicate, int ms) { const auto until = host::Clock::now() + std::chrono::milliseconds(ms); for (;;) { { std::lock_guard lock(peer); if (predicate()) return true; } if (host::Clock::now() >= until) break; std::this_thread::sleep_for(std::chrono::milliseconds(10)); } std::lock_guard lock(peer); return bool(predicate()); };
    // Only the current session's sources and frames (a slow capture thread may
    // not have replaced the previous session's destroyed sources yet).
    const auto lossless = [&] { std::lock_guard guard(pixels); return sources[0] && sources[1] && shown[0] && shown[1] && !sources[1]->change && exact(sources[0]->bgra, trackgpu::rgba(*shown[0])) && exact(sources[1]->bgra, trackgpu::rgba(*shown[1])); };
    const auto endSession = [&] { source.suspendStreaming(); { std::lock_guard guard(pixels); sources = {}; } std::lock_guard lock(peer); ready = false; };

    // 1) No opt-in: never a tile packet (released-client behaviour).
    const std::string plainId = "12345678-1234-4234-8234-123456789aa1";
    { auto start = command(plainId); source.localTrackStart(start, host::bindOwnedTrackSources(start, display(), observed())); }
    require(waitFor([&] { return ready && current == plainId; }, ceilingMs), "Plain session did not start");
    uint64_t videoAtPing = 0; { std::lock_guard lock(peer); videoAtPing = videoPackets; }
    const auto pinged = host::Clock::now();
    source.localControl("{\"type\":\"ping\",\"sessionId\":" + host::jsonQuote(plainId) + ",\"id\":\"1\"}");
    // Absence needs the pipeline to have actually run after the ping, not just
    // wall time: at least 1.2 s and two more video packets (the 1 s idle refresh
    // keeps a static source sending), whichever comes later under load.
    require(waitFor([&] { return videoPackets >= videoAtPing + 2 && host::Clock::now() - pinged >= std::chrono::milliseconds(1200); }, ceilingMs), "Plain session stopped sending video after the ping");
    { std::lock_guard lock(peer); require(tilePackets == 0 && videoPackets > 0, "A session without the opt-in received tile packets"); }
    endSession();

    // 2) Opt-in ping: both outputs become pixel-identical; a change hands back and returns.
    const std::string id = "12345678-1234-4234-8234-123456789aa2";
    { auto start = command(id); source.localTrackStart(start, host::bindOwnedTrackSources(start, display(), observed())); }
    // The plain session's `ready` must not satisfy this one's start.
    require(waitFor([&] { return ready && current == id; }, ceilingMs), "Lossless session did not start");
    const auto optedIn = host::Clock::now();
    source.localControl("{\"type\":\"ping\",\"sessionId\":" + host::jsonQuote(id) + ",\"id\":\"lt1-1\"}");
    require(waitFor([&] { return lossless(); }, ceilingMs), "Opted-in outputs did not become pixel-identical");
    const double firstMs = std::chrono::duration<double, std::milli>(host::Clock::now() - optedIn).count();
    { std::lock_guard guard(pixels); sources[1]->change = [](std::vector<uint8_t>& bgra) { for (int y = 100; y < 180; ++y) for (int x = 200; x < 330; ++x) { auto* p = &bgra[(size_t(y) * 800 + x) * 4]; p[0] = 10; p[1] = 200; p[2] = 30; } }; }
    const auto changedAt = host::Clock::now();
    require(waitFor([&] { return lossless(); }, ceilingMs), "Changed output did not return to pixel-identical");
    const double againMs = std::chrono::duration<double, std::milli>(host::Clock::now() - changedAt).count();
    // 3) 게임 마우스 holds the mouse ("lt0-"): every output drops all of its
    // tiles with its next video frame; "lt1-" brings them back on the same decoders.
    source.localControl("{\"type\":\"ping\",\"sessionId\":" + host::jsonQuote(id) + ",\"id\":\"lt0-2\"}");
    require(waitFor([&] { return decoder && decoder->losslessStats(0).presentTiles == 0 && decoder->losslessStats(1).presentTiles == 0; }, ceilingMs), "Paused tiles were not retired on every output");
    source.localControl("{\"type\":\"ping\",\"sessionId\":" + host::jsonQuote(id) + ",\"id\":\"lt1-3\"}");
    require(waitFor([&] { return lossless(); }, ceilingMs), "Tiles did not come back on every output after the pause");
    endSession();
    const auto stats0 = decoder->losslessStats(0), stats1 = decoder->losslessStats(1);
    require(!stats0.poisoned && !stats1.poisoned && stats0.addedTiles && stats1.addedTiles && stats1.droppedTiles, "Compositor did not add/drop tiles as expected");
    source.stop(); decoder.reset();
    std::cout << "{\"kind\":\"lossless-track-agent\",\"gpuPresentation\":" << (gpu ? "true" : "false") << ",\"plainSessionTilePackets\":0,\"optInToLosslessMs\":" << firstMs << ",\"changeToLosslessMs\":" << againMs
              << ",\"tilePackets\":" << tilePackets << ",\"droppedTiles\":" << stats1.droppedTiles << ",\"perTrackPtsStrictlyIncreasing\":true,\"actualAgentWorkerThread\":true,\"actualEncoders\":true,\"actualClientDecoders\":true,\"actualWindowsCapture\":false,\"actualNetwork\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "lossless track agent: " << error.what() << '\n'; return 1; }
}
int main() { const int cpu = run(false); const int gpu = run(true); return cpu == 0 && gpu == 0 ? 0 : 1; }
