// Streaming backpressure outcomes without a VM, relay or audio device: the
// audio jitter buffer drops its oldest audio instead of refusing a packet (a
// refusal used to end the whole stream as media_backpressure), and the media
// engines report a full queue apart from a stopped decoder or a malformed
// packet. It does not prove decode throughput, audio playback or relay pacing.
// No audio endpoint is opened (audio_jitter_test.cpp covers playback timing).
#include "media_push.h"
#include "audio_jitter.h"
#include "remote_audio.h"
#include "remote_engine.h"
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

std::vector<uint8_t> chunk(uint8_t id, size_t size = 1920) { return std::vector<uint8_t>(size, id); }

struct Registrar final : flutter::TextureRegistrar {
  std::map<int64_t, flutter::TextureVariant*> textures;
  int64_t next = 0;
  int64_t RegisterTexture(flutter::TextureVariant* texture) override { textures.emplace(++next, texture); return next; }
  bool MarkTextureFrameAvailable(int64_t id) override { return textures.count(id) != 0; }
  void UnregisterTexture(int64_t id, std::function<void()> callback) override { textures.erase(id); if (callback) callback(); }
  bool UnregisterTexture(int64_t id) override { UnregisterTexture(id, {}); return true; }
};

std::vector<uint8_t> media(uint8_t kind, uint8_t flags, size_t payload) {
  std::vector<uint8_t> packet(12 + payload, 0); packet[0] = kind; packet[1] = flags; return packet;
}

void pcmQueue() {
  using multitap::AudioJitterBuffer;
  AudioJitterBuffer queue;
  for (const size_t bad : {size_t(0), size_t(3), size_t(6), AudioJitterBuffer::kMaxPacketBytes + 4}) {
    const auto bytes = chunk(9, bad ? bad : 4);
    require(!queue.Push(0, bytes.data(), bad, 0), "Malformed PCM accepted");
  }
  require(queue.queued() == 0, "Malformed PCM was queued");
  // Up to target + headroom (80 ms at the start) waits without a drop.
  for (uint8_t id = 0; id < 8; ++id) { const auto bytes = chunk(id); require(queue.Push(id * 10000, bytes.data(), bytes.size(), 0), "Bounded PCM refused"); }
  require(queue.queued() == AudioJitterBuffer::Ms(80) && queue.counters().overrun_frames == 0, "Bound accounting differs");
  // A burst beyond the bound drops the oldest audio down to the target instead of refusing.
  for (uint8_t id = 8; id < 13; ++id) { const auto bytes = chunk(id); require(queue.Push(id * 10000, bytes.data(), bytes.size(), 0), "Full PCM queue refused a packet"); }
  require(queue.queued() == AudioJitterBuffer::Ms(80) && queue.counters().overruns == 1 && queue.counters().overrun_frames == AudioJitterBuffer::Ms(50), "Overflow did not drop the oldest packets");
  std::vector<uint8_t> out(1924, 0xee);
  require(queue.Pull(out.data(), 481, 0, 0) == 481 && out[0] == 5 && out[1919] == 5 && out[1920] == 6, "Playback order after overflow differs");
  queue.Clear(); require(queue.queued() == 0 && queue.Pull(out.data(), 1, 0, 0) == 0, "Cleared queue kept audio");
}

void remoteAudio() {
  // Without a started endpoint, valid audio never refuses (video continues).
  multitap::RemoteAudio audio;
  const auto bytes = chunk(1);
  require(audio.Push(0, bytes.data(), bytes.size()), "Idle audio refused valid PCM");
  require(!audio.Push(0, bytes.data(), 3), "Idle audio accepted malformed PCM");
  require(audio.received_frames() == 480, "Received audio accounting differs");
  // PCM keeps the condition-variable wait it always had; only Opus waits on an
  // event and a high-resolution timer. UsesTimerWait decides it for Start
  // (the event), Accept (which wake) and the render thread (which wait).
  require(!multitap::RemoteAudio::UsesTimerWait(false), "A PCM session would leave its condition-variable wait");
  require(multitap::RemoteAudio::UsesTimerWait(true), "An Opus session would not wait on the high-resolution timer");
}

void outcomes() {
  using multitap::MediaPushResult;
  require(multitap::MediaPushHresult(MediaPushResult::accepted) == S_OK, "Accepted mapping");
  require(multitap::MediaPushHresult(MediaPushResult::full) == multitap::kMediaQueueFull, "Full mapping");
  require(multitap::MediaPushHresult(MediaPushResult::stopped) == multitap::kMediaDecoderStopped, "Stopped mapping");
  require(multitap::MediaPushHresult(MediaPushResult::invalid) == multitap::kMediaPacketInvalid, "Invalid mapping");
  require(FAILED(multitap::kMediaQueueFull) && multitap::kMediaQueueFull != multitap::kMediaDecoderStopped &&
          multitap::kMediaDecoderStopped != multitap::kMediaPacketInvalid && multitap::kMediaQueueFull != E_FAIL, "Outcomes are not distinct failures");
  Registrar registrar;
  multitap::RemoteEngine engine(&registrar);
  require(engine.Offer(media(1, 1, 8)) == MediaPushResult::stopped, "An engine before Start took video");
  require(engine.Start(64, 64, false) >= 0, "Engine did not start");
  require(engine.Offer(media(3, 0, 8)) == MediaPushResult::invalid, "Unknown media kind accepted");
  require(engine.Offer(media(1, 2, 8)) == MediaPushResult::invalid, "Unknown video flags accepted");
  require(engine.Offer(media(2, 0, 8)) == MediaPushResult::invalid && !engine.Push(media(2, 0, 8)), "Audio-off engine accepted audio");
  const auto stats = engine.Stats();
  const auto dropped = stats.find(flutter::EncodableValue("audioDroppedFrames"));
  require(dropped != stats.end() && std::get<int64_t>(dropped->second) == 0, "Audio drop count is not reported");
  engine.Stop();
  require(engine.Offer(media(1, 1, 8)) == MediaPushResult::stopped, "A stopped engine took video");
}
}

int main() {
  try {
    pcmQueue(); remoteAudio(); outcomes();
    std::cout << "{\"kind\":\"media-backpressure-smoke\",\"pcmOverflowDropsOldest\":true,\"distinctPushOutcomes\":true,\"pcmConditionWait\":true,\"audioPlayback\":false,\"decodeThroughput\":false}" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << std::endl;
    return 1;
  }
}
