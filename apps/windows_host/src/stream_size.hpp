#pragma once
#include "stream_encode_size.hpp"
#include "owned_output_size.hpp"
#include <chrono>
#include <optional>

// Negotiated stream sizes (legacy-v1 or capability-based-v2).
// docs/remote-session-protocol.md
// "Stream sizes"). A viewport stream is encoded at its source's own size. It is
// reduced only for what cannot keep up: the client decoder's declared limits
// (sides, pixels per frame, pixels per second), the client's measured decode
// load (stream.decoder reports), this Host's measured encoder overload, or an
// optional administrator cap (settings.json "videoPixelBudget"; unset = none).
// Sessions without the grant keep stream_encode_size.hpp's released budget.
namespace host {
inline constexpr char streamSizeProtocolName[] = "stream-size-v1";
inline constexpr char streamSizeProtocolV2[] = "stream-size-v2";
inline bool streamSizeV2(const Json& command){return command.get("streamSizeProtocol").string()==streamSizeProtocolV2;}

// What the client's decoder takes. maxPixelRate 0: not declared (a software
// decoder); the stream then starts within the released safe budget and goes
// above it only while the client's decode reports show headroom.
struct DecoderLimits {
  int maxWidth = 3840, maxHeight = 2160;
  uint64_t maxPixels = 8294400, maxPixelRate = 0;
};
inline DecoderLimits streamDecoderLimits(const Json& value,bool v2){
  const auto* fields=std::get_if<Json::Object>(&value.value);
  if(!fields||fields->size()!=4)throw std::runtime_error("Invalid stream size request");
  const auto integer=[&](const char* key,double minimum,double maximum,bool zero){
    const auto n=value.get(key).number(-1);
    if(!(zero&&n==0)&&(!std::isfinite(n)||n<minimum||n>maximum||n!=std::floor(n)))throw std::runtime_error("Invalid stream size request");
    return n;
  };
  return {int(integer("maxWidth",v2?16:320,v2?double(INT32_MAX):3840,false)),
    int(integer("maxHeight",v2?16:180,v2?double(INT32_MAX):3840,false)),
    uint64_t(integer("maxPixels",v2?256:320.0*180,v2?9007199254740991.0:8294400.0,false)),
    uint64_t(integer("maxPixelRate",v2?256:320.0*180*30,v2?9007199254740991.0:8294400.0*60,true))};
}

// session.start of a granted session: the marker on a separate-v1 viewport
// stream and exactly the four limits. Absent: nullopt; malformed: throws.
inline std::optional<DecoderLimits> streamSizeRequest(const Json& command) {
  const auto* fields = std::get_if<Json::Object>(&command.value);
  if (!fields || (!fields->contains("streamSizeProtocol") && !fields->contains("decoderLimits"))) return {};
  const auto fail = [] { throw std::runtime_error("Invalid stream size request"); };
  const bool v2=streamSizeV2(command),tracks=v2&&command.get("mediaProtocol").string()=="tracks-v1";
  if ((!v2&&command.get("streamSizeProtocol").string()!=streamSizeProtocolName)||(!tracks&&command.get("viewportEncoderLimitsProtocol").string()!="separate-v1")||
      command.get("type").string()!="session.start"||command.get("mode").string()!="stream"||(!tracks&&(fields->contains("mediaProtocol")||fields->contains("tracks"))))fail();
  return streamDecoderLimits(command.get("decoderLimits"),v2);
}

// settings.json "videoPixelBudget" in a stream-size-v1 session: an optional
// administrator cap in pixels per frame at 60 fps (twice at 30 fps, subject to
// buffer ABI representation). Unset or invalid: no fixed cap.
inline std::optional<uint64_t> streamPixelCap(const Json& settings) {
  const auto value = settings.get("videoPixelBudget").number(-1);
  if (!std::isfinite(value) || value != std::floor(value) || value < double(streamPixelBudgetMinimum) || value > double(streamPixelBudgetMaximum)) return {};
  return uint64_t(value);
}

// Capability `modes` of an owned output whose size the Host provisions. A
// released broker refuses the whole capability set for a mode taller than
// 2160 rows (and for dynamicResolution without any mode), so a portrait
// output (left by a stream-size-v1 session) advertises the 1920x1080 mode it
// can be provisioned to instead of its own size. Sizes outside the owned rule
// advertise nothing, as before.
inline std::string advertisedOwnedModes(uint64_t width, uint64_t height) {
  if (!ownedOutputSize(width, height)) return "";
  if (width > 3840 || height > 2160) width = 1920, height = 1080;
  return ",\"modes\":[{\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) + ",\"refreshRate\":60}]";
}

// Why the picture is smaller than its source (fixed diagnostic codes).
enum class EncodeLimit : uint8_t { none = 0, decoderSize = 1, decoderRate = 2, decoderLoad = 3, hostCpu = 4, adminCap = 5 };
inline const char* encodeLimitName(EncodeLimit limit) {
  switch (limit) {
    case EncodeLimit::decoderSize: return "decoder_size";
    case EncodeLimit::decoderRate: return "decoder_rate";
    case EncodeLimit::decoderLoad: return "decoder_load";
    case EncodeLimit::hostCpu: return "host_cpu";
    case EncodeLimit::adminCap: return "admin_cap";
    default: return "none";
  }
}

// Everything every picture of one source must satisfy.
struct StreamSizeBounds {
  int sourceWidth = 0, sourceHeight = 0, fps = 60, boxWidth = int(ownedOutputMaxSide), boxHeight = int(ownedOutputMaxSide);
  DecoderLimits decoder;
  std::optional<uint64_t> adminCap;
  int maxWidth() const { return std::min({boxWidth, decoder.maxWidth, int(ownedOutputMaxSide)}); }
  int maxHeight() const { return std::min({boxHeight, decoder.maxHeight, int(ownedOutputMaxSide)}); }
  // Whole macroblocks per frame: decoders count frame size and rate in them.
  uint64_t macroblocks() const {
    const uint64_t rate = uint64_t(std::clamp(fps, 1, 60));
    uint64_t limit = std::min<uint64_t>(decoder.maxPixels, streamPixelBudgetMaximum) / 256;
    if (decoder.maxPixelRate) limit = std::min(limit, decoder.maxPixelRate / 256 / rate);
    if (adminCap) limit = std::min(limit, std::min<uint64_t>(*adminCap * 60 / rate, streamPixelBudgetMaximum) / 256);
    return limit;
  }
};

// The picture at `scale` of the source, within the bounds (even sides, at
// least 16, aspect kept); padding to whole macroblocks can cost a row.
inline std::pair<int, int> streamPictureAt(const StreamSizeBounds& b, double scale) {
  const int sw = b.sourceWidth, sh = b.sourceHeight;
  if (sw < 16 || sh < 16) return {std::max(16, sw & ~1), std::max(16, sh & ~1)};
  const uint64_t blocks = std::max<uint64_t>(1, b.macroblocks());
  scale = std::min({scale, 1.0, double(b.maxWidth()) / sw, double(b.maxHeight()) / sh, std::sqrt(double(blocks) * 256 / (double(sw) * sh))});
  scale = std::max(scale, 0.0);
  const auto at = [&](double value) { return std::pair{std::max(16, int(sw * value) & ~1), std::max(16, int(sh * value) & ~1)}; };
  auto size = at(scale);
  for (int step = 0; step < 400 && (streamMacroblocks(size.first, size.second) > blocks || size.first > b.maxWidth() || size.second > b.maxHeight()); ++step) { scale *= 0.995; size = at(scale); }
  return size;
}

// The largest scale the declared limits allow, and which limit binds.
struct EncodeCeiling { double scale = 1; EncodeLimit reason = EncodeLimit::none; };
inline EncodeCeiling streamEncodeCeiling(const StreamSizeBounds& b) {
  EncodeCeiling ceiling;
  const double pixels = double(std::max(1, b.sourceWidth)) * std::max(1, b.sourceHeight), fps = std::clamp(b.fps, 1, 60);
  const auto take = [&](double scale, EncodeLimit reason) { if (scale < ceiling.scale - 1e-9) { ceiling.scale = scale; ceiling.reason = reason; } };
  take(std::min(double(b.maxWidth()) / std::max(1, b.sourceWidth), double(b.maxHeight()) / std::max(1, b.sourceHeight)), EncodeLimit::decoderSize);
  take(std::sqrt(double(std::min<uint64_t>(b.decoder.maxPixels, streamPixelBudgetMaximum)) / pixels), EncodeLimit::decoderSize);
  if (b.decoder.maxPixelRate) take(std::sqrt(double(b.decoder.maxPixelRate) / fps / pixels), EncodeLimit::decoderRate);
  if (b.adminCap) take(std::sqrt(double(std::min<uint64_t>(*b.adminCap * 60 / uint64_t(fps), streamPixelBudgetMaximum)) / pixels), EncodeLimit::adminCap);
  ceiling.scale = std::clamp(ceiling.scale, 0.0, 1.0);
  return ceiling;
}

// The released safe budget (4,194,304 pixels at 60 fps, H.264 level 5.1) as
// a scale of this source: where a decoder without a declared rate starts.
inline double streamSafeScale(const StreamSizeBounds& b) {
  const uint64_t fps = uint64_t(std::clamp(b.fps, 1, 60));
  const double budget = double(std::min<uint64_t>(streamPixelBudgetDefault * 60 / fps, streamPixelBudgetMaximum));
  return std::min(1.0, std::sqrt(budget / (double(std::max(1, b.sourceWidth)) * std::max(1, b.sourceHeight))));
}

// One ~1 s window of the capture loop. busyMs: capture + convert + encode of
// every tick; frames: ticks that submitted a picture to the encoder, with the
// capture share (source-sized, not reduced by scaling) and the convert +
// encode share (proportional to the picture's pixels) of those ticks.
struct EncodeHostWindow { double seconds = 0, busyMs = 0, fixedMs = 0, scalableMs = 0; uint32_t frames = 0; };
// The client's latest decoder report (within the last few seconds).
struct ClientDecodeReport { int busy = -1, backlogMs = 0; };
// stream.decoder (client -> relay -> service -> worker): exactly type,
// sessionId, busy (-1..1000 per mille, -1 = not measured) and backlogMs
// (0..60000). Packed into one non-zero value for an atomic hand-over.
inline std::optional<uint64_t> streamDecoderReport(const Json& value) {
  const auto* fields = std::get_if<Json::Object>(&value.value);
  if (!fields || fields->size() != 4 || value.get("type").string() != "stream.decoder" || value.get("sessionId").string().empty()) return {};
  const auto busy = value.get("busy").number(-2), backlog = value.get("backlogMs").number(-1);
  if (!std::isfinite(busy) || std::floor(busy) != busy || busy < -1 || busy > 1000 || !std::isfinite(backlog) || std::floor(backlog) != backlog || backlog < 0 || backlog > 60000) return {};
  return (uint64_t(1) << 40) | (uint64_t(busy + 1) << 16) | uint64_t(backlog);
}
inline ClientDecodeReport unpackDecoderReport(uint64_t packed) { return {int((packed >> 16) & 0x7ff) - 1, int(packed & 0xffff)}; }
inline std::string streamDecoderMessage(const std::string& session, uint64_t packed) {
  const auto report = unpackDecoderReport(packed);
  return "{\"type\":\"stream.decoder\",\"sessionId\":" + jsonQuote(session) + ",\"busy\":" + std::to_string(report.busy) + ",\"backlogMs\":" + std::to_string(report.backlogMs) + '}';
}
// What a session learned about load: the pixels per frame the slower side
// sustained after its last step down (0 = nothing), kept across resizes.
// fps: the capture rate a software encoder's overload settled at (0 = the requested rate).
struct EncodeScaleMemory { uint64_t pixels = 0; EncodeLimit reason = EncodeLimit::none; int fps = 0; };

// Steps the picture scale down on measured overload and back up with
// hysteresis. Each change restarts the encoder (a new IDR), so changes are
// rate limited: a step down needs two overloaded windows and three seconds
// since the last change; a step up needs two calm windows, two seconds since
// the last change and ten seconds since the last step down.
//
// Any overload (the Host's encoder or capture, or the client's decoder; for every encoder, not
// only x264 and the MFT, whose CPU time grows with pixels
// and frames alike) lowers the capture rate first, 60 -> 45 -> 30, because for
// desktop work a sharp picture matters more than its rate (size first took a
// field stream to a quarter of 5120x1440 under x264). At the lowest rate the
// picture stays at 0.75 of the source or more; below that only while that rate
// still cannot be held, never below 0.5. Calm restores the size first, then the
// rate. The rate change needs no new encoder: x264 and the MFT budget per
// submitted frame, and the stream announces the requested rate as before.
class EncodeScaleController {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr double minimumScale = 0.25;
  static constexpr double overloadBusy = 0.90, overloadWork = 0.90, targetWork = 0.75, upWork = 0.70;
  static constexpr int overloadClientBusy = 900, targetClientBusy = 750, upClientBusy = 700;
  static constexpr uint32_t measuredFrames = 5;
  static constexpr auto downInterval = std::chrono::seconds(3), upInterval = std::chrono::seconds(2), upAfterDown = std::chrono::seconds(10);
  static constexpr int softwareRates[] = {45, 30};
  static constexpr double softwareKeepScale = 0.75, softwareMinimumScale = 0.5;

  EncodeScaleController(const StreamSizeBounds& bounds, EncodeScaleMemory memory = {}, Clock::time_point now = Clock::now())
      : bounds_(bounds), ceiling_(streamEncodeCeiling(bounds)), safe_(bounds.decoder.maxPixelRate ? 1.0 : streamSafeScale(bounds)), lastChange_(now), fps_(std::clamp(bounds.fps, 1, 60)) {
    target_ = std::min(ceiling_.scale, safe_);
    reason_ = safe_ < ceiling_.scale ? EncodeLimit::decoderRate : ceiling_.reason;
    const double pixels = double(std::max(1, bounds.sourceWidth)) * std::max(1, bounds.sourceHeight);
    if (memory.pixels) {
      const double carried = std::max(minimumScale, std::min(1.0, std::sqrt(double(memory.pixels) / pixels)));
      if (carried < target_) { target_ = carried; reason_ = downReason_ = memory.reason; memory_ = memory; lastDown_ = now; }
    }
    if (memory.fps > 0 && memory.fps < fps_) { fps_ = std::max(memory.fps, lowestRate()); downReason_ = memory.reason != EncodeLimit::none ? memory.reason : EncodeLimit::hostCpu; memory_ = memory; lastDown_ = now; }
    size_ = streamPictureAt(bounds_, target_);
    settleReason();
  }
  std::pair<int, int> size() const { return size_; }
  EncodeLimit reason() const { return reason_; }
  // Picture width per source width, per mille.
  int scalePermille() const { return int(std::lround(1000.0 * size_.first / std::max(1, bounds_.sourceWidth))); }
  EncodeScaleMemory memory() const { return memory_; }
  // The capture rate now (at most the requested rate; lower only for a software encoder).
  int fps() const { return fps_; }
  const EncodeCeiling& ceiling() const { return ceiling_; }

  // One window; true when the picture size changed (restart the encoder).
  // software: the encoder is x264 or the MFT, so a frame rate step relieves it.
  bool observe(const EncodeHostWindow& host, std::optional<ClientDecodeReport> client, Clock::time_point now, bool software = false) {
    const double period = 1000.0 / fps_;
    const bool measured = host.frames >= measuredFrames && host.seconds > 0;
    const double fixed = measured ? host.fixedMs / host.frames : 0, scalable = measured ? host.scalableMs / host.frames : 0;
    const double busy = host.seconds > 0 ? host.busyMs / (host.seconds * 1000) : 0;
    const double encodedFps = host.seconds > 0 ? host.frames / host.seconds : 0;
    // Saturated capture thread that still misses the frame rate it is asked for.
    const bool hostOverloaded = measured && busy >= overloadBusy && encodedFps < 0.92 * fps_ && fixed + scalable >= overloadWork * period;
    const bool clientKnown = client && client->busy >= 0;
    const bool clientOverloaded = client && (client->busy >= overloadClientBusy || client->backlogMs >= std::max(100.0, 4 * period));
    if (hostOverloaded || clientOverloaded) { ++overloaded_; calm_ = 0; } else { overloaded_ = 0; ++calm_; }

    // Any overload: the rate first (60 -> 45 -> 30), the size only after it (a
    // desktop is read, so sharp text matters more than its rate, for every encoder and for the client's
    // decoder too, whose work also follows the rate). No new encoder: the capture pacing follows fps().
    // Host: the highest rung the measured work fits at 75 %. Client: the rung its busy share fits at 750 per
    // mille (one rung when it reports only a backlog).
    (void)software;
    if (overloaded_ >= 2 && now - lastChange_ >= downInterval && (hostOverloaded || clientOverloaded) && fps_ > lowestRate()) {
      int next = lowestRate();
      for (const int rate : softwareRates) {
        if (rate >= fps_ || rate < next) continue;
        const bool hostFits = !hostOverloaded || fixed + scalable <= targetWork * 1000.0 / rate;
        const bool clientFits = !clientOverloaded || (clientKnown && client->busy > 0 ? double(client->busy) * rate / fps_ <= targetClientBusy : true);
        if (hostFits && clientFits) { next = rate; break; }
      }
      const EncodeLimit why = hostOverloaded ? EncodeLimit::hostCpu : EncodeLimit::decoderLoad;
      fps_ = next; lastChange_ = now; lastDown_ = now; downReason_ = reason_ = why; overloaded_ = 0;
      memory_.fps = fps_; memory_.reason = why;
      return false;
    }
    // At the lowest rate the Host's own overload keeps 0.75 of the source (0.5 while that rate still cannot be
    // held); only the client's decoder may need less, down to a quarter.
    const double floorScale = hostOverloaded && !clientOverloaded ? (target_ > softwareKeepScale + 1e-9 ? softwareKeepScale : softwareMinimumScale) : minimumScale;
    // Capture alone over the budget (it reads the source at its own size, whatever the picture's): a smaller
    // picture cannot relieve it and only blurs text, so the size stays (field case: a VM composed on
    // Microsoft Basic Render captured in 20-33 ms, the picture went to a quarter and stopped at 0.76).
    const bool captureBound = hostOverloaded && !clientOverloaded && fixed >= targetWork * period;
    if (overloaded_ >= 2 && now - lastChange_ >= downInterval && target_ > floorScale + 1e-9 && !captureBound) {
      double factor = 1; EncodeLimit why = EncodeLimit::none;
      const auto consider = [&](double value, EncodeLimit reason) { if (value < factor) { factor = value; why = reason; } };
      if (hostOverloaded) { const double room = targetWork * period - fixed; consider(room > 0 && scalable > 0 ? std::sqrt(room / scalable) : 0.5, EncodeLimit::hostCpu); }
      if (clientOverloaded) consider(clientKnown && client->busy > 0 ? std::sqrt(double(targetClientBusy) / client->busy) : 0.8, EncodeLimit::decoderLoad);
      factor = std::clamp(factor, 0.5, 0.9);
      const double next = std::max(floorScale, target_ * factor);
      if (apply(next, now)) {
        lastDown_ = now; downReason_ = why; reason_ = why;
        memory_ = {uint64_t(size_.first) * uint64_t(size_.second), why, fps_ < bounds_.fps ? fps_ : 0};
        overloaded_ = 0;
        return true;
      }
      return false;
    }
    // Without a declared decoder rate the safe budget is exceeded only while
    // the client measures its own decoding.
    const double cap = clientKnown ? ceiling_.scale : std::min(ceiling_.scale, safe_);
    if (calm_ >= 2 && target_ < cap - 1e-9 && now - lastChange_ >= upInterval && (!lastDown_ || now - *lastDown_ >= upAfterDown)) {
      double candidate = cap;
      if (measured && scalable > 0) {
        const double room = upWork * period - fixed;
        double factor = room > 0 ? std::sqrt(room / scalable) : 1;
        // Below the requested rate (the picture changes less often, or capture paces it) no frame has to fit a
        // period: the thread's actual busy share decides, with the same 70 % a step up keeps. Without it a slow
        // capture kept the picture small long after the overload (the per-frame projection never had room).
        if (encodedFps < 0.92 * fps_ && encodedFps > 0) {
          const double share = 1 + (upWork - busy) * 1000.0 / (encodedFps * scalable);
          if (share > 1) factor = std::max(factor, std::sqrt(share));
        }
        candidate = std::min(candidate, target_ * factor);
      }
      if (clientKnown && client->busy > 0) candidate = std::min(candidate, target_ * std::sqrt(double(upClientBusy) / client->busy));
      if (candidate >= cap * 0.98) candidate = cap;
      if (candidate > target_ * 1.05 || (candidate == cap && cap > target_)) {
        const auto before = size_;
        if (apply(candidate, now)) {
          if (target_ >= cap - 1e-9) memory_ = {0, EncodeLimit::none, fps_ < bounds_.fps ? fps_ : 0};
          settleReason();
          return size_ != before;
        }
      }
    }
    // The size is back at its cap: then the rate, one rung, when the measured work
    // fits the shorter period with the same headroom a size step up asks for.
    if (calm_ >= 2 && fps_ < bounds_.fps && target_ >= cap - 1e-9 && now - lastChange_ >= upInterval && (!lastDown_ || now - *lastDown_ >= upAfterDown)) {
      int next = std::clamp(bounds_.fps, 1, 60);
      for (const int rate : softwareRates) if (rate > fps_ && rate < next) next = rate;
      if (!measured || fixed + scalable <= upWork * 1000.0 / next) {
        fps_ = next; lastChange_ = now; memory_.fps = fps_ < bounds_.fps ? fps_ : 0;
        if (!memory_.fps && !memory_.pixels) memory_ = {};
        settleReason();
      }
    }
    return false;
  }

 private:
  bool apply(double scale, Clock::time_point now) {
    const auto size = streamPictureAt(bounds_, scale);
    target_ = scale;
    if (size == size_) return false;
    size_ = size; lastChange_ = now; return true;
  }
  int lowestRate() const { return std::min(softwareRates[1], std::clamp(bounds_.fps, 1, 60)); }
  void settleReason() {
    if (size_.first >= (bounds_.sourceWidth & ~1) && size_.second >= (bounds_.sourceHeight & ~1)) { reason_ = fps_ < bounds_.fps ? (downReason_ != EncodeLimit::none ? downReason_ : EncodeLimit::hostCpu) : EncodeLimit::none; return; }
    if (target_ >= ceiling_.scale - 1e-9) reason_ = ceiling_.reason == EncodeLimit::none ? EncodeLimit::decoderSize : ceiling_.reason;
    else if (target_ >= safe_ - 1e-9 && safe_ < ceiling_.scale) reason_ = EncodeLimit::decoderRate;
    else if (downReason_ != EncodeLimit::none) reason_ = downReason_;
  }
  StreamSizeBounds bounds_;
  EncodeCeiling ceiling_;
  double safe_ = 1, target_ = 1;
  std::pair<int, int> size_{};
  EncodeLimit reason_ = EncodeLimit::none, downReason_ = EncodeLimit::none;
  EncodeScaleMemory memory_;
  Clock::time_point lastChange_;
  std::optional<Clock::time_point> lastDown_;
  uint32_t overloaded_ = 0, calm_ = 0;
  int fps_ = 60;
};
}
