// input-latency-v1 frame path end to end inside one process: the host session
// tagger decides the first CHANGED frame captured after an injection, the
// production CPU H.264 encoder and FrameEncodePolicy produce the media-v1
// packets, and the actual legacy client RemoteEngine (CPU decoder, Flutter
// pixel-buffer publication) reports when that tagged frame was handed to the
// texture. Synthetic NV12 frames on a synthetic clock; the Flutter registrar is
// a fixture; no capture, relay, network, injection or display scan-out.
#include "media.hpp"
#include "frame_encode_policy.hpp"
#include "input_latency.hpp"
#include "remote_engine.h"
#include <iostream>
#include <map>
#include <thread>

namespace {
using V = flutter::EncodableValue;
constexpr int width = 640, height = 360, fps = 60, bitrate = 4000000;
unsigned checks = 0;
void require(bool ok, const char* reason) { ++checks; if (!ok) throw std::runtime_error(reason); }
int64_t integer(const flutter::EncodableMap& stats, const char* field) { return std::get<int64_t>(stats.at(V(field))); }
struct Registrar final : flutter::TextureRegistrar {
  int64_t RegisterTexture(flutter::TextureVariant*) override { return 1; }
  bool MarkTextureFrameAvailable(int64_t) override { return true; }
  void UnregisterTexture(int64_t, std::function<void()> callback) override { if (callback) callback(); }
  bool UnregisterTexture(int64_t id) override { UnregisterTexture(id, {}); return true; }
};
// A flat NV12 picture with one bright block whose position encodes `state`.
void picture(std::vector<uint8_t>& nv12, int state) {
  std::fill(nv12.begin(), nv12.begin() + width * height, uint8_t(60));
  std::fill(nv12.begin() + width * height, nv12.end(), uint8_t(128));
  const int left = (state * 48) % (width - 64), top = (state * 32) % (height - 64);
  for (int y = top; y < top + 64; ++y) std::fill(nv12.begin() + y * width + left, nv12.begin() + y * width + left + 64, uint8_t(220));
}
}

int main() {
  try {
    host::Runtime runtime;
    Registrar registrar; multitap::RemoteEngine engine(&registrar);
    require(engine.Start(width, height, false) > 0, "Engine start");
    host::Encoder encoder(width, height, fps, bitrate);
    host::FrameEncodePolicy policy(size_t(width) * height * 3 / 2, host::FrameRefinement::forCbr(bitrate, fps));
    host::InputLatencySession latency;
    latency.configure(host::JsonParser::parse(R"({"type":"session.start","mode":"stream","sessionId":"00000000-0000-4000-8000-000000000009","inputLatencyProtocol":"input-latency-v1"})"));
    require(latency.enabled(), "Negotiated session start");
    std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2);
    // Ticks at which an input is injected (after that tick) and the tick at
    // which the remote picture first reacts. 300 never reacts (a modifier key).
    const std::map<int, int> reactions = {{20, 22}, {60, 61}, {100, 104}, {140, 141}, {300, -1}};
    std::map<int, uint32_t> sampleAt; std::map<uint32_t, uint64_t> tagged; std::map<uint64_t, int> outputTick;
    std::vector<uint64_t> unwatchedOutputs;
    int state = 0, tick = 0; uint32_t nextSample = 1;
    int64_t pushed = 0;
    const auto push = [&](const std::vector<uint8_t>& packet, bool watch) {
      for (int n = 0; n < 30000 && integer(engine.Stats(), "queuedPackets") >= 6; ++n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      require(engine.Push(packet, watch), "Engine queue rejected a packet"); ++pushed;
    };
    for (tick = 0; tick < 420; ++tick) {
      const uint64_t now = uint64_t(tick + 1) * 1000000 / fps;
      latency.beginCapture(now);
      for (const auto& [injectedAfter, reactsAt] : reactions) if (reactsAt == tick) ++state;
      picture(nv12, state);
      const auto decision = policy.prepare(nv12, now);
      latency.submitted(now, decision.contentChanged);
      if (decision.submit) encoder.encode(nv12.data(), now, [&](host::Frame output) {
        require(policy.output(output.ptsUs, output.key, now, output.bytes.size()), "Encoder output was not a submitted PTS");
        outputTick[output.ptsUs] = tick;
        const auto tag = latency.output(output.ptsUs, 0);
        if (tag) { const auto value = host::JsonParser::parse(*tag); require(uint64_t(value.get("pts").number()) == output.ptsUs, "The tag does not name its video packet"); tagged[uint32_t(value.get("sample").number())] = output.ptsUs; }
        else unwatchedOutputs.push_back(output.ptsUs);
        push(host::mediaPacket(1, output.ptsUs, output.bytes, output.key), tag.has_value());
      });
      if (reactions.contains(tick)) {
        // The service stamp and the worker report around a successful SendInput.
        const auto input = host::JsonParser::parse("{\"type\":\"input\",\"event\":\"button\",\"button\":\"left\",\"down\":true,\"x\":0.5,\"y\":0.5,\"sample\":" + std::to_string(nextSample) + '}');
        const auto stamped = latency.stamp(input, now + 100);
        require(stamped && latency.injected(*stamped, true, now + 300).has_value(), "Injection report");
        sampleAt[tick] = nextSample++;
      }
    }
    // Wait for the engine's accounting of every pushed packet (an empty queue
    // precedes the last decode/publication; a fixed 50 ms was a load race).
    // decodeCalls is counted after the packet's publication and watch report.
    for (const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);;) {
      const auto current = engine.Stats(); require(integer(current, "error") == 0, "Engine failed");
      if (integer(current, "decodeCalls") == pushed) break;
      require(integer(current, "decodeCalls") < pushed && std::chrono::steady_clock::now() < until, "Engine did not finish its queued packets");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto stats = engine.Stats();
    std::map<uint64_t, int64_t> delivered;
    for (const auto& item : std::get<flutter::EncodableList>(stats.at(V("inputFrames")))) {
      const auto& pair = std::get<flutter::EncodableList>(item);
      require(pair.size() == 2, "inputFrames entry shape");
      delivered[uint64_t(std::get<int64_t>(pair[0]))] = std::get<int64_t>(pair[1]);
    }
    std::ostringstream cases;
    for (const auto& [injectedAfter, reactsAt] : reactions) {
      const auto sample = sampleAt.at(injectedAfter);
      if (reactsAt < 0) { require(!tagged.contains(sample), "An input without a visible change was tagged"); continue; }
      require(tagged.contains(sample), "A visible reaction was not tagged");
      const auto pts = tagged.at(sample);
      // The first changed frame after the injection: the reaction tick itself.
      require(outputTick.contains(pts) && pts == uint64_t(reactsAt + 1) * 1000000 / fps, "The tag is not the first changed frame after the injection");
      require(delivered.contains(pts) && delivered.at(pts) >= 0 && delivered.at(pts) < 5000000, "The client engine did not report the tagged frame's publication");
      cases << (cases.tellp() ? "," : "") << "{\"sample\":" << sample << ",\"framesAfterInjection\":" << reactsAt - injectedAfter << ",\"queuedToTextureUs\":" << delivered.at(pts) << '}';
    }
    for (const auto pts : unwatchedOutputs) require(!delivered.contains(pts), "An unwatched frame was reported");
    require(integer(stats, "error") == 0, "Engine failed");
    engine.Stop();
    std::cout << "{\"kind\":\"client-input-frame\",\"checks\":" << checks << ",\"cases\":[" << cases.str() << "],\"actualEncoder\":true,\"actualRemoteEngine\":true,\"flutterRegistrarFixture\":true,\"capture\":false,\"actualNetwork\":false,\"displayScanOut\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "client input frame: " << error.what() << '\n'; return 1; }
}
