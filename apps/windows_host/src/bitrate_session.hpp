#pragma once
#include "bitrate_controller.hpp"
#include "support.hpp"
#include "owned_output_size.hpp"
#include <algorithm>
#include <functional>
#include <iomanip>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

// Glue between the socket-owning Agent and BitrateController
// (docs/adaptive-bitrate.md). Thread-safe: media enqueue (bridge pipe thread),
// the send loop and control/feedback handling call it concurrently. It only
// stores a published target through `Publish`, never writes a pipe itself.
namespace host {
struct AdaptiveBitrateRequest {
  bool adaptive = false;
  int bitrate = 0, maximum = 0;  // fixed target, or auto start and ceiling
};
// session.start fields. Released brokers send only `bitrate` (<= 20 Mbps).
// New brokers send bitrateMode "auto" with bitrateMax only to hosts that
// advertise bitrate-adaptive-v1, and fixed values up to its maxBitrate.
inline AdaptiveBitrateRequest adaptiveBitrateRequest(const Json& control) {
  AdaptiveBitrateRequest value;
  value.adaptive = control.get("bitrateMode").string() == "auto" && control.get("mediaProtocol").string() != "tracks-v1";
  const double requested = control.get("bitrate").number(12000000);
  value.bitrate = int(std::clamp(std::isfinite(requested) ? requested : 12000000.0, value.adaptive ? double(adaptiveMinimumBitrate) : 4000000.0, double(adaptiveMaximumBitrate)));
  const double ceiling = control.get("bitrateMax").number(value.bitrate);
  value.maximum = value.adaptive ? int(std::clamp(std::isfinite(ceiling) ? ceiling : double(value.bitrate), double(value.bitrate), double(adaptiveMaximumBitrate))) : value.bitrate;
  return value;
}

class AdaptiveBitrateSession {
 public:
  using Publish = std::function<void(const std::string& session, int bitrate)>;
  void start(const std::string& session, const AdaptiveBitrateRequest& request, int fps, Publish publish) {
    std::lock_guard lock(mutex_);
    controller_.reset(); session_.clear(); publish_ = {}; udp_ = false;
    if (!request.adaptive) return;
    controller_.emplace(request.bitrate, request.maximum, fps); session_ = session; publish_ = std::move(publish); origin_ = Clock::now();
  }
  void stop() { std::lock_guard lock(mutex_); controller_.reset(); session_.clear(); publish_ = {}; udp_ = false; }
  bool active() const { std::lock_guard lock(mutex_); return controller_.has_value(); }
  // A packet entering the host send queue (before it is queued).
  void outgoing(bool text, const std::vector<uint8_t>& bytes) {
    std::lock_guard lock(mutex_);
    if (!controller_) return;
    const auto now = micros();
    if (!text) {
      if (bytes.size() < 13 || (bytes[0] != 1 && bytes[0] != 2)) return;
      uint64_t pts = 0; for (int i = 0; i < 8; ++i) pts |= uint64_t(bytes[4 + i]) << (i * 8);
      const bool tile = bytes[0] == 1 && bytes.size() >= 16 && bytes[12] == 'T' && bytes[13] == 'S' && bytes[14] == 'L' && bytes[15] == '1';
      controller_->sent(now, bytes[0] == 1, uint16_t(bytes[2] | (bytes[3] << 8)), pts, bytes.size(), tile);
      return;
    }
    static constexpr char info[] = "{\"type\":\"stream.info\"";
    if (bytes.size() > sizeof(info) && std::equal(info, info + sizeof(info) - 1, bytes.begin())) {
      try {
        const auto value = JsonParser::parse(std::string(bytes.begin(), bytes.end()));
        const auto width = value.get("width").number(), height = value.get("height").number(), fps = value.get("fpsTarget").number();
        if(std::isfinite(width)&&std::isfinite(height)&&width>=16&&width<=ownedOutputMaxSide&&height>=16&&height<=INT32_MAX&&width==std::floor(width)&&height==std::floor(height)&&encodedPictureSize(uint64_t(width),uint64_t(height))&&fps>=1&&fps<=60)apply(controller_->geometry(int(width),int(height),int(fps)));
      } catch (...) { }
    }
  }
  // Host send-queue state (oldest queued packet age includes a blocked write).
  void queue(uint64_t oldestAgeUs, size_t queuedBytes, size_t queuedPackets) {
    std::lock_guard lock(mutex_); if (controller_ && !udp_) apply(controller_->local(micros(), oldestAgeUs, queuedBytes, queuedPackets));
  }
  // udp-media-v1. While media is on UDP the broker channel's queue, its written rate
  // and the relay's stream.feedback no longer describe it: FEEDBACK and the pacer do.
  void udpPath(bool on) {
    std::lock_guard lock(mutex_); if (on == udp_) return;
    udp_ = on; if (controller_) controller_->pathChanged();
  }
  // Datagrams since the last call; the pacing target that follows, 0 with no controller.
  int udpFeedback(uint64_t rttUs, uint32_t received, uint32_t lost) {
    std::lock_guard lock(mutex_); if (!controller_) return 0;
    apply(controller_->udpFeedback(micros(), rttUs, received, lost)); return controller_->target();
  }
  void udpBacklog() { std::lock_guard lock(mutex_); if (controller_) apply(controller_->udpBacklog(micros())); }
  void written(size_t bytes) {
    std::lock_guard lock(mutex_); if (!controller_) return;
    const auto now = micros(); controller_->written(now, bytes);
    if (now - lastTickUs_ >= 100000) { lastTickUs_ = now; apply(controller_->tick(now)); }
  }
  // The relay's stream.feedback for this session; malformed reports are ignored.
  void feedback(const Json& message) {
    std::lock_guard lock(mutex_);
    if (!controller_ || udp_ || message.get("sessionId").string() != session_) return;
    const auto epoch = message.get("epoch").number(-1), bytes = message.get("bytes").number(-1), interval = message.get("intervalUs").number(-1), relay = message.get("relayQueuedBytes").number(0);
    const auto* frames = std::get_if<Json::Array>(&message.get("frames").value);
    const auto integer = [](double n, double maximum) { return std::isfinite(n) && n >= 0 && n <= maximum && std::floor(n) == n; };
    if (!frames || frames->size() > 64 || !integer(epoch, 65535) || !integer(bytes, 1e9) || !integer(interval, 10e6) || !integer(relay, 1e9)) return;
    BitrateArrival arrivals[64]; size_t count = 0;
    for (const auto& frame : *frames) {
      const auto* pair = std::get_if<Json::Array>(&frame.value);
      if (!pair || pair->size() != 2) return;
      const auto pts = (*pair)[0].number(-1), arrival = (*pair)[1].number(-1);
      if (!integer(pts, 9007199254740991.0) || !integer(arrival, 9007199254740991.0)) return;
      arrivals[count++] = {uint64_t(pts), uint64_t(arrival)};
    }
    apply(controller_->feedback(micros(), uint16_t(epoch), std::span<const BitrateArrival>(arrivals, count), uint64_t(bytes), uint64_t(interval), uint64_t(relay)));
  }
  // Appends the controller's view to a stream.stats text (observational only).
  void annotate(std::vector<uint8_t>& text) {
    static constexpr char stats[] = "{\"type\":\"stream.stats\"";
    if (text.size() <= sizeof(stats) || text.back() != '}' || !std::equal(stats, stats + sizeof(stats) - 1, text.begin())) return;
    std::lock_guard lock(mutex_); if (!controller_) return;
    std::ostringstream fields; fields << std::fixed << std::setprecision(1);
    const double delay = controller_->standingDelayMs(micros());
    if (delay >= 0) fields << ",\"networkDelayMs\":" << delay;
    if (const double delivered = controller_->deliveredKbps(); delivered > 0) fields << ",\"deliveredKbps\":" << delivered;
    const auto extra = fields.str(); text.insert(text.end() - 1, extra.begin(), extra.end());
  }
  int target() const { std::lock_guard lock(mutex_); return controller_ ? controller_->target() : 0; }
 private:
  uint64_t micros() const { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - origin_).count()) + 1; }
  void apply(std::optional<int> value) { if (value && publish_) publish_(session_, *value); }
  mutable std::mutex mutex_;
  std::optional<BitrateController> controller_;
  std::string session_;
  Publish publish_;
  Clock::time_point origin_ = Clock::now();
  uint64_t lastTickUs_ = 0;
  bool udp_ = false;
};
}
