#pragma once
#include "bitrate_session.hpp"
#include <array>
#include <deque>
#include <map>

// bitrate-adaptive-tracks-v1 (docs/adaptive-bitrate.md, "Independent outputs").
// The one-output controller (BitrateController) for a session of independent
// outputs: Session0 owns it beside the track tunnel, sees every packet the
// worker sends (tunnel or UDP), the tunnel's queue and the client's arrival
// feedback (which the broker sends to the tunnel), and gives each output a
// share of one target. The worker applies the shares to its encoders
// (stream.tracks.bitrate). Thread-safe like AdaptiveBitrateSession; `wake`
// runs under its lock and may only flag and signal.
namespace host {
inline constexpr char bitrateTracksProtocol[] = "bitrate-adaptive-tracks-v1";

// session.start of independent outputs: automatic only with the broker's marker.
inline AdaptiveBitrateRequest adaptiveTracksBitrateRequest(const Json& control) {
  if (control.get("mediaProtocol").string() != "tracks-v1" || control.get("bitrateTracksProtocol").string() != bitrateTracksProtocol || control.get("bitrateMode").string() != "auto") return {};
  AdaptiveBitrateRequest value; value.adaptive = true;
  const double requested = control.get("bitrate").number(adaptiveStartBitrate);
  value.bitrate = int(std::clamp(std::isfinite(requested) ? requested : double(adaptiveStartBitrate), double(adaptiveMinimumBitrate), double(adaptiveMaximumBitrate)));
  const double ceiling = control.get("bitrateMax").number(value.bitrate);
  value.maximum = int(std::clamp(std::isfinite(ceiling) ? ceiling : double(value.bitrate), double(value.bitrate), double(adaptiveMaximumBitrate)));
  return value;
}

// Each output keeps its part of the target by area; what an output leaves
// unused (its part beyond 1.25 x its rate of the last 0.5 s) goes to an output
// that uses its own part (at least 80 % of it). One output moving and one
// still: the moving one gets nearly all of it (an even split kept a moving
// 1080p output at 10 of 20 Mbps, and the controller saw too little use to
// probe). Both moving, or both still: their parts. When the still one starts
// moving, the sum is at most 1.5 x the target until the next split (250 ms), a
// burst the controller's virtual queue explains; two still outputs starting
// together stay within the target.
inline std::vector<int> splitTrackBitrate(int target, const std::vector<uint64_t>& areas, const std::vector<double>& usedBitsPerSecond) {
  std::vector<int> shares;
  if (areas.empty() || areas.size() != usedBitsPerSecond.size()) return shares;
  double total = 0; for (const auto area : areas) total += double(area);
  if (total <= 0) return shares;
  std::vector<double> fair(areas.size()), spare(areas.size());
  std::vector<bool> needs(areas.size());
  for (size_t n = 0; n < areas.size(); ++n) {
    const double used = std::max(0.0, usedBitsPerSecond[n]);
    fair[n] = double(target) * double(areas[n]) / total;
    spare[n] = std::max(0.0, fair[n] - 1.25 * used);
    needs[n] = used >= 0.8 * fair[n];
  }
  for (size_t n = 0; n < areas.size(); ++n) {
    double share = fair[n];
    if (needs[n]) for (size_t other = 0; other < areas.size(); ++other) if (other != n) share += spare[other];
    shares.push_back(int(std::clamp(share, double(encoderMinimumBitrate), double(std::max(target, encoderMinimumBitrate)))));
  }
  return shares;
}

// The worker's side of stream.tracks.bitrate: topology and each output's share,
// or nothing for a malformed command (it is then ignored).
inline std::optional<std::pair<uint32_t, std::vector<std::pair<uint8_t, int>>>> trackBitrateCommand(const Json& message) {
  const auto topology = message.get("topologyEpoch").number(-1);
  const auto* rows = std::get_if<Json::Array>(&message.get("tracks").value);
  if (!rows || rows->empty() || rows->size() > 2 || !std::isfinite(topology) || topology < 1 || topology > 4294967295.0 || std::floor(topology) != topology) return std::nullopt;
  std::vector<std::pair<uint8_t, int>> rates;
  for (const auto& row : *rows) {
    const auto id = row.get("id").string(); const auto bitrate = row.get("bitrate").number(-1);
    if ((id != "0" && id != "1") || !std::isfinite(bitrate) || bitrate < encoderMinimumBitrate || bitrate > encoderMaximumBitrate || std::floor(bitrate) != bitrate) return std::nullopt;
    for (const auto& rate : rates) if (rate.first == uint8_t(id[0] - '0')) return std::nullopt;
    rates.push_back({uint8_t(id[0] - '0'), int(bitrate)});
  }
  return std::make_pair(uint32_t(topology), std::move(rates));
}

class TrackBitrateSession {
 public:
  using Wake = std::function<void()>;
  static constexpr uint64_t splitIntervalUs = 250000, usageWindowUs = 500000;
  void start(const std::string& session, const AdaptiveBitrateRequest& request, int fps, Wake wake) {
    std::lock_guard lock(mutex_); reset();
    if (!request.adaptive) return;
    controller_.emplace(request.bitrate, request.maximum, fps); session_ = session; wake_ = std::move(wake); origin_ = Clock::now(); fps_ = std::clamp(fps, 1, 240);
  }
  void stop() { std::lock_guard lock(mutex_); reset(); }
  // Tests: a simulated clock in microseconds (above 0); kept across sessions.
  void clock(std::function<uint64_t()> now) { std::lock_guard lock(mutex_); clock_ = std::move(now); }
  bool active() const { std::lock_guard lock(mutex_); return controller_.has_value(); }
  int target() const { std::lock_guard lock(mutex_); return controller_ ? controller_->target() : 0; }
  // A worker packet before it is queued (tunnel or UDP): TVT1 media, or the
  // outputs' metadata (stream.tracks.info: each output's size, the floor).
  void outgoing(bool text, const std::vector<uint8_t>& bytes) {
    std::lock_guard lock(mutex_);
    if (!controller_) return;
    const auto now = micros();
    if (text) {
      static constexpr char info[] = "{\"type\":\"stream.tracks.info\"";
      if (bytes.size() > sizeof(info) && std::equal(info, info + sizeof(info) - 1, bytes.begin())) {
        try { metadata(JsonParser::parse(std::string(bytes.begin(), bytes.end())), now); } catch (...) { }
      }
      return;
    }
    if (bytes.size() < 28 || bytes[0] != 'T' || bytes[1] != 'V' || bytes[2] != 'T' || bytes[3] != '1') return;
    const uint8_t kind = bytes[4], track = bytes[6];
    if (kind < 1 || kind > 3) return;
    const uint32_t topology = read32(bytes, 8), length = read32(bytes, 24);
    if (size_t(length) + 28 != bytes.size()) return;
    const bool video = kind == 1;
    const bool tile = video && length >= 4 && bytes[28] == 'T' && bytes[29] == 'S' && bytes[30] == 'L' && bytes[31] == '1';
    // Feedback names a packet by its pts and the topology's low 16 bits (the client does the same).
    controller_->sent(now, video, uint16_t(topology & 0xffff), read64(bytes, 16), bytes.size(), tile);
    if (video && topology == topology_) for (auto& output : outputs_) if (output.id == track) { output.usage.push_back({now, bytes.size()}); output.bytes += bytes.size(); }
    if (now - lastSplitUs_ >= splitIntervalUs) split(now, false);
  }
  // The track tunnel's queue after an enqueue, and what one write carried.
  void queue(uint64_t oldestAgeUs, size_t queuedBytes, size_t queuedPackets) {
    std::lock_guard lock(mutex_); if (controller_ && !udp_) apply(controller_->local(micros(), oldestAgeUs, queuedBytes, queuedPackets));
  }
  void written(size_t bytes) {
    std::lock_guard lock(mutex_); if (!controller_) return;
    const auto now = micros(); controller_->written(now, bytes);
    if (now - lastTickUs_ >= 100000) { lastTickUs_ = now; apply(controller_->tick(now)); }
  }
  // The client's stream.feedback (the relay adds its own queue); ignored on UDP.
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
  // udp-tracks-v1: each output's leg. The controller reads the legs' round
  // trips and losses only while every output is on UDP (the arrival feedback
  // describes the tunnel otherwise). Each returns that output's share, the
  // leg's pacing target (0: none yet, the leg keeps its own).
  int udpFeedback(const std::string& id, uint64_t rttUs, uint32_t received, uint32_t lost) {
    std::lock_guard lock(mutex_); if (!controller_) return 0;
    if (udp_) apply(controller_->udpFeedback(micros(), rttUs, received, lost));
    return share(id);
  }
  int udpPath(const std::string& id, bool on) {
    std::lock_guard lock(mutex_); if (!controller_) return 0;
    legs_[id] = on;
    bool all = !outputs_.empty();
    for (const auto& output : outputs_) { const auto leg = legs_.find(std::to_string(output.id)); all = all && leg != legs_.end() && leg->second; }
    if (all != udp_) { udp_ = all; controller_->pathChanged(); }
    return share(id);
  }
  void udpBacklog() { std::lock_guard lock(mutex_); if (controller_) apply(controller_->udpBacklog(micros())); }
  // stream.tracks.stats of this session: the target of all outputs, the
  // ceiling, what the controller sees, and each output's share.
  void annotate(Json& stats) {
    std::lock_guard lock(mutex_);
    auto* fields = std::get_if<Json::Object>(&stats.value);
    if (!controller_ || !fields || stats.get("type").string() != "stream.tracks.stats" || stats.get("sessionId").string() != session_) return;
    (*fields)["bitrateTarget"] = Json{double(std::clamp(controller_->target(), encoderMinimumBitrate, controller_->maximum()))};
    (*fields)["bitrateMax"] = Json{double(controller_->maximum())};
    const auto now = micros();
    if (const double delay = controller_->standingDelayMs(now); delay >= 0) (*fields)["networkDelayMs"] = Json{std::round(delay * 10) / 10};
    if (const double delivered = controller_->deliveredKbps(); delivered > 0) (*fields)["deliveredKbps"] = Json{std::round(std::min(delivered, 1e6) * 10) / 10};
    if (stats.get("topologyEpoch").number(-1) != double(topology_)) return;
    if (auto* rows = std::get_if<Json::Array>(&(*fields)["tracks"].value))
      for (auto& row : *rows) if (auto* track = std::get_if<Json::Object>(&row.value)) {
        const auto id = row.get("id").string();
        for (const auto& output : outputs_) if (std::to_string(output.id) == id && output.share) (*track)["bitrateTarget"] = Json{double(output.share)};
      }
  }
  // The worker's command once the shares changed (or a new topology came):
  // {"type":"stream.tracks.bitrate","sessionId",...,"topologyEpoch",...,"tracks":[{"id","bitrate"}]}.
  std::optional<std::string> takeCommand() {
    std::lock_guard lock(mutex_);
    if (!controller_ || !pending_ || outputs_.empty()) return std::nullopt;
    pending_ = false;
    std::ostringstream out; out << "{\"type\":\"stream.tracks.bitrate\",\"sessionId\":" << jsonQuote(session_) << ",\"topologyEpoch\":" << topology_ << ",\"tracks\":[";
    bool first = true;
    for (const auto& output : outputs_) { if (!first) out << ','; first = false; out << "{\"id\":\"" << unsigned(output.id) << "\",\"bitrate\":" << output.share << '}'; }
    out << "]}"; return out.str();
  }
 private:
  struct Output { uint8_t id = 0; uint64_t area = 0; int share = 0; std::deque<std::pair<uint64_t, size_t>> usage; size_t bytes = 0; };
  static uint32_t read32(const std::vector<uint8_t>& b, size_t at) { return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24; }
  static uint64_t read64(const std::vector<uint8_t>& b, size_t at) { return uint64_t(read32(b, at)) | uint64_t(read32(b, at + 4)) << 32; }
  void reset() { controller_.reset(); session_.clear(); wake_ = {}; outputs_.clear(); legs_.clear(); topology_ = 0; udp_ = pending_ = false; lastSplitUs_ = lastTickUs_ = 0; }
  uint64_t micros() const { if (clock_) return clock_(); return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - origin_).count()) + 1; }
  // A new topology: its outputs, and a floor that is the sum of theirs.
  void metadata(const Json& value, uint64_t now) {
    if (value.get("sessionId").string() != session_) return;
    const auto topology = value.get("topologyEpoch").number(-1), fps = value.get("fpsTarget").number(fps_);
    const auto* rows = std::get_if<Json::Array>(&value.get("tracks").value);
    if (!rows || rows->empty() || rows->size() > 2 || !std::isfinite(topology) || topology < 1 || topology > 4294967295.0 || std::floor(topology) != topology) return;
    std::vector<Output> next; int floor = 0;
    if (std::isfinite(fps) && fps >= 1 && fps <= 240) fps_ = int(fps);
    for (const auto& row : *rows) {
      const auto id = row.get("id").string(); const auto width = row.get("width").number(), height = row.get("height").number();
      if ((id != "0" && id != "1") || !std::isfinite(width) || !std::isfinite(height) || width < 16 || height < 16 || width > 16384 || height > 16384) return;
      Output output; output.id = uint8_t(id[0] - '0'); output.area = uint64_t(width) * uint64_t(height);
      floor += adaptiveBitrateFloor(int(width), int(height), fps_);
      next.push_back(std::move(output));
    }
    outputs_ = std::move(next); topology_ = uint32_t(topology); legs_.clear();
    if (udp_) { udp_ = false; controller_->pathChanged(); }
    controller_->geometryFloor(floor, fps_);
    split(now, true);
  }
  void apply(std::optional<int> value) { if (value) split(micros(), true); }
  void split(uint64_t now, bool force) {
    lastSplitUs_ = now;
    if (outputs_.empty()) return;
    std::vector<uint64_t> areas; std::vector<double> used;
    for (auto& output : outputs_) {
      while (!output.usage.empty() && now - output.usage.front().first > usageWindowUs) { output.bytes -= output.usage.front().second; output.usage.pop_front(); }
      areas.push_back(output.area); used.push_back(double(output.bytes) * 8 * 1e6 / double(usageWindowUs));
    }
    const auto shares = splitTrackBitrate(controller_->target(), areas, used);
    bool changed = force;
    for (size_t n = 0; n < outputs_.size(); ++n) {
      const int before = outputs_[n].share, after = shares[n];
      if (!before || std::abs(after - before) >= before / 20) changed = true;
    }
    if (!changed) return;
    for (size_t n = 0; n < outputs_.size(); ++n) outputs_[n].share = shares[n];
    pending_ = true;
    if (wake_) wake_();
  }
  int share(const std::string& id) const { for (const auto& output : outputs_) if (std::to_string(output.id) == id) return output.share; return 0; }
  mutable std::mutex mutex_;
  std::optional<BitrateController> controller_;
  std::string session_;
  Wake wake_;
  std::function<uint64_t()> clock_;
  Clock::time_point origin_ = Clock::now();
  std::vector<Output> outputs_;
  std::map<std::string, bool> legs_;
  uint32_t topology_ = 0;
  int fps_ = 60;
  uint64_t lastSplitUs_ = 0, lastTickUs_ = 0;
  bool udp_ = false, pending_ = false;
};
}
