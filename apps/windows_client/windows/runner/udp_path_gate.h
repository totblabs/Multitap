#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>
#include "media_push.h"
#include "../../../../native/windows_video/lossless_tiles.hpp"
#include "../../../../native/windows_video/udp_media.hpp"

namespace multitap {
// The contract's path rule, the epoch filter and the key gate for both media paths
// (udp-media-v1, Phase 3). Pure: nothing here touches a decoder or a clock.
class PathRule {
 public:
  bool engaged() const { return engaged_; }
  uint16_t epoch() const { return epoch_; }
  udp::Path active() const { return state_.active; }
  uint32_t fallbacks() const { return fallbacks_; }
  bool awaitingKey() const { return !state_.delivered; }

  // A stream run announced on WSS: PTS restarts with it, so "newer" starts over.
  void stream(uint16_t epoch) { epoch_ = epoch; state_.lastPts = 0; }

  // A new decoder takes nothing before a key frame.
  void decoderStarted() { state_.delivered = false; state_.lastPts = 0; }
  // From here on both paths follow the rule. `keyed`: WSS already fed this decoder
  // a key frame, so its P-frames go on.
  void engage(bool keyed) { engaged_ = true; state_ = {udp::Path::wss, epoch_, 0, keyed}; }
  void disengage() { engaged_ = false; state_ = {}; fallbacks_ = 0; }

  // The state this video message would leave, for commit once the decoder took it:
  // a full queue must leave the rule as it was, because WSS offers the same packet again.
  bool admit(udp::Path from, uint16_t epoch, bool key, uint64_t pts, bool tile, udp::PathState& next) const {
    if (epoch != epoch_) return false;
    next = state_;
    // Tiles are stamped from the tick, ahead of the encoder's PTS, and are no frame:
    // they follow the active path and leave "newer" to the video.
    if (tile) return state_.delivered && epoch == state_.epoch && from == state_.active;
    return udp::acceptVideo(next, from, epoch, key, pts);
  }
  void commit(const udp::PathState& next) {
    if (state_.active == udp::Path::udp && next.active == udp::Path::wss) ++fallbacks_;
    state_ = next;
  }
  // Audio has no path: it needs only a decoder that took this epoch's key frame.
  bool admitAudio(uint16_t epoch) const { return epoch == epoch_ && state_.delivered; }

 private:
  udp::PathState state_;
  uint16_t epoch_ = 0;
  bool engaged_ = false;
  uint32_t fallbacks_ = 0;
};

// Where both paths meet the decoder. The lock covers the rule and the engine's offer
// together, so frames cannot pass each other, and Attach cannot return while an offer
// is inside the engine: a detached engine can stop safely. Until the Host's key arrives
// this is a straight pass, and WSS bytes reach the engine as they always did.
class UdpPathGate {
 public:
  using Offer = std::function<MediaPushResult(const std::vector<uint8_t>&, bool)>;
  enum class Delivery { accepted, dropped, full };
  struct View { uint8_t active = 0; uint32_t fallbacks = 0; uint64_t keyedStream = 0; };

  // UI thread. Null detaches.
  void Attach(Offer offer) {
    std::lock_guard<std::mutex> lock(mutex_);
    offer_.swap(offer);
    if (offer_) rule_.decoderStarted();
  }
  bool attached() { std::lock_guard<std::mutex> lock(mutex_); return bool(offer_); }
  void Stream(uint16_t epoch, uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    rule_.stream(epoch); generation_ = generation; epoch_ = epoch;
  }
  void Engage(bool keyed) { std::lock_guard<std::mutex> lock(mutex_); rule_.engage(keyed); }
  void Disengage() { std::lock_guard<std::mutex> lock(mutex_); rule_.disengage(); }

  // UI thread, WSS: a dropped packet reads as accepted, a dropped frame is no failure.
  // The epoch is the announced one: Dart has already refused any other.
  MediaPushResult OfferWss(const std::vector<uint8_t>& packet, bool watch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!offer_) return MediaPushResult::accepted;
    udp::Envelope envelope;
    if (!rule_.engaged() || !udp::parseEnvelope(packet.data(), packet.size(), udp::classVideo, envelope)) return offer_(packet, watch);
    udp::PathState next;
    if (!rule_.admit(udp::Path::wss, rule_.epoch(), envelope.key, envelope.pts, tile(packet), next)) return MediaPushResult::accepted;
    const auto result = offer_(packet, watch);
    if (result == MediaPushResult::accepted) { rule_.commit(next); if (envelope.key) wss_keys_.fetch_add(1); }
    return result;
  }

  // Receive thread, a whole UDP message. The decoder's stable input has reserved zero
  // where the epoch travels. Every video message is watched: a frame's input tag comes
  // on WSS, possibly after the frame.
  Delivery OfferUdp(std::vector<uint8_t>& message, bool video, bool key, uint16_t epoch, uint64_t pts) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!offer_) return Delivery::dropped;
    udp::PathState next;
    if (video ? !rule_.admit(udp::Path::udp, epoch, key, pts, tile(message), next) : !rule_.admitAudio(epoch)) return Delivery::dropped;
    message[2] = message[3] = 0;
    const auto result = offer_(message, video);
    if (result == MediaPushResult::full) return Delivery::full;
    if (result != MediaPushResult::accepted) return Delivery::dropped;
    if (video) { rule_.commit(next); if (key) keyed_stream_ = generation_; }
    return Delivery::accepted;
  }

  uint16_t epoch() const { return epoch_.load(); }
  // Moves with every WSS key frame the rule accepted: the leg's cue that UDP lost a switch.
  uint64_t wssKeys() const { return wss_keys_.load(); }
  View view() {
    std::lock_guard<std::mutex> lock(mutex_);
    return {uint8_t(rule_.active() == udp::Path::udp ? 1 : 0), rule_.fallbacks(), keyed_stream_};
  }

 private:
  static bool tile(const std::vector<uint8_t>& message) { return message.size() > 12 && lossless::isTilePayload(message.data() + 12, message.size() - 12); }

  std::mutex mutex_;
  Offer offer_;
  PathRule rule_;
  uint64_t generation_ = 0, keyed_stream_ = 0;
  std::atomic<uint16_t> epoch_{0};
  std::atomic<uint64_t> wss_keys_{0};
};
}
