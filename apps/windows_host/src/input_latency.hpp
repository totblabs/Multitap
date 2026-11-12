#pragma once
#include "support.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <sstream>
#include <vector>
#include <mutex>
#include <optional>

namespace host {
// Negotiated ToTBSeamless input latency measurement (input-latency-v1; see
// docs/remote-session-protocol.md "Input latency measurement"). Only a
// session.start carrying the marker enables it, and only the broker that
// granted it in its connect response sends that marker. Every report carries
// sequence numbers, durations and a video PTS; never a key code or coordinate.
inline constexpr const char* inputLatencyProtocol = "input-latency-v1";
// A broker that sends this marker with input-latency-v1 in session.start
// accepts tags carrying this host's per-input durations (captureUs, encodeUs,
// queueUs); without it the exact five-field tag is sent.
inline constexpr const char* inputFrameTimingProtocol = "frame-timing-v1";
inline constexpr uint64_t inputLatencyMaxSample = 2147483647;
inline constexpr uint64_t inputLatencyMaxUs = 10000000;
// The first CHANGED frame captured after an injection is tagged. Without a
// change within this window the input produced no visible frame (a key that
// does nothing, a wheel step at the scroll limit, a click on nothing) or it
// reacted too late to be told apart from unrelated changes (caret blink, clock,
// spinner); no frame tag is sent for it and the host counts it as `expired`.
// It was 1 s: on a mostly still screen the next unrelated change inside that
// second named a no-effect input and the wait was reported as latency.
inline constexpr uint64_t inputLatencyFrameWindowUs = 500000;
// Slow periodic changes of the picture that no input caused are recognised
// from changed frames captured while no input had been injected for this long
// (a response comes sooner; typing at any pace never qualifies), so that the
// next beat of such a rhythm is not credited to a later input.
inline constexpr uint64_t inputLatencyQuietUs = 300000;
inline constexpr uint64_t inputLatencyAmbientMinPeriodUs = 100000;
inline constexpr uint64_t inputLatencyAmbientToleranceUs = 30000;
inline constexpr size_t inputLatencyAssignedLimit = 4;

// System-wide monotonic microseconds from QueryPerformanceCounter. QPC values
// are comparable between processes on one machine, so the Session0 service's
// receive stamp and the interactive worker's injection stamp share one clock.
inline uint64_t hostMonotonicUs() {
  static const int64_t frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart > 0 ? value.QuadPart : 1; }();
  LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
  const auto ticks = uint64_t(std::max<int64_t>(0, now.QuadPart));
  return ticks / uint64_t(frequency) * 1000000 + ticks % uint64_t(frequency) * 1000000 / uint64_t(frequency);
}

inline std::optional<uint64_t> inputLatencyInteger(const Json& value, uint64_t minimum, uint64_t maximum) {
  const auto* number = std::get_if<double>(&value.value);
  if (!number || !std::isfinite(*number) || std::floor(*number) != *number || *number < double(minimum) || *number > double(maximum)) return {};
  return uint64_t(*number);
}

// Shift, Ctrl, Alt and Windows (generic and left/right virtual keys): the keys
// the client's paste gate also treats as modifiers. Pressing one shows nothing.
inline bool inputLatencyModifierKey(const Json& input) {
  const auto vk = inputLatencyInteger(input.get("vk"), 1, 255);
  return vk && (*vk == 0x10 || *vk == 0x11 || *vk == 0x12 || (*vk >= 0xA0 && *vk <= 0xA5) || *vk == 0x5B || *vk == 0x5C);
}
// Whether an injected input can by itself change the picture, i.e. may name
// the first changed frame after it. A key release or a modifier press changes
// nothing visible in almost every application: as a candidate it named the
// next unrelated change, or the change of a later key press that was not
// sampled, so the time between the two was reported as latency. A pointer move
// of a cursor-v1 session changes no frame unless a button is held (a drag: the
// window, the selection or the map follows the pointer). relative-mouse-v1
// movement goes to an application that hid the cursor: its camera, not a cursor.
inline bool inputFrameCandidate(const Json& input, bool cursorSeparated, bool buttonHeld) {
  const auto event = input.get("event").string();
  if (event == "key") return input.get("down").boolean() && !inputLatencyModifierKey(input);
  if (event == "move") return !cursorSeparated || buttonHeld;
  return event == "button" || event == "wheel" || event == "moveRelative";
}
// An injected input that is not sampled but changes the picture as surely as
// the pending candidate: a button change (a press that showed nothing followed
// by the release that opened a link) or committed text (text-input-v1). The
// next changed frame may be its effect, so the pending candidate is dropped
// rather than credited with it. Unsampled key presses, wheel steps and drag
// moves do not do this: during a burst every response would be dropped.
inline bool inputFrameInterrupts(const Json& input) {
  const auto event = input.get("event").string();
  return event == "button" || event == "text";
}

// Pure frame tagging, independent of capture/encoder objects. One instance per
// media epoch (a resize restarts it). Calls come from the capture thread except
// injected(), which the owner serializes with a mutex.
class InputFrameTagger {
 public:
  struct Tag { uint32_t sample = 0; uint64_t pts = 0; std::optional<uint64_t> captureUs, encodeUs; };
  struct Options { uint64_t windowUs = inputLatencyFrameWindowUs; bool ambient = true; };
  InputFrameTagger() = default;
  explicit InputFrameTagger(Options options) : options_(options) {}
  // Why an input got no frame tag (host clock, last 30 s in the stream.stats).
  enum class Miss : uint8_t { expired, ambient };
  struct MissEvent { Miss kind = Miss::expired; uint64_t atUs = 0; };
  // The host's share of one tagged input (host clock, diagnostics only):
  // injection -> start of the capture tick that got its changed frame (the
  // application and compositor reacting, plus capture pacing), and that
  // tick's start -> its encoder output (capture, convert, encode).
  struct Timing { uint64_t injectToCaptureUs = 0, captureToEncodedUs = 0, atUs = 0; };
  static constexpr size_t timingLimit = 64;
  // Any successfully injected input (sampled or not): the picture is changing
  // because of the user for a while, so changes that follow are no evidence of
  // an input-free rhythm.
  void activity(uint64_t atUs) { lastInputUs_ = std::max(lastInputUs_, atUs); }
  // A successful injection; a later one replaces an earlier one not yet tied
  // to a capture ("latest injected input").
  void injected(uint32_t sample, uint64_t atUs) {
    if (injected_) ++superseded_;
    injected_ = Pending{sample, atUs};
  }
  // An unsampled input that may itself change the picture (inputFrameInterrupts)
  // was injected: a pending input not yet tied to a changed frame is dropped.
  void interrupted() {
    if (injected_ || candidate_) ++superseded_;
    injected_.reset(); candidate_.reset();
  }
  // Called before capture.frame(): every injection seen here happened before
  // this frame's capture began.
  void beginCapture(uint64_t nowUs) {
    captureUs_ = nowUs;
    if (injected_) { if (candidate_) ++superseded_; candidate_ = injected_; injected_.reset(); }
    if (candidate_ && nowUs > candidate_->atUs && nowUs - candidate_->atUs > options_.windowUs) { ++unchanged_; miss(Miss::expired, nowUs); candidate_.reset(); }
  }
  // The encode decision of the frame captured after beginCapture(). Only a
  // frame whose final NV12 actually changed is tagged; an unchanged refresh,
  // refinement or skipped tick leaves the candidate waiting.
  void submitted(uint64_t pts, bool contentChanged) {
    if (!contentChanged) return;
    if (!candidate_) { noteQuietChange(); return; }
    // A change that continues a slow rhythm seen while nobody was injecting
    // (a blinking caret) is not the effect of this input: keep waiting.
    if (options_.ambient && periodic(captureUs_)) { miss(Miss::ambient, captureUs_); return; }
    assigned_.push_back({candidate_->sample, pts, candidate_->atUs, captureUs_}); candidate_.reset();
    while (assigned_.size() > inputLatencyAssignedLimit) { assigned_.pop_front(); ++superseded_; }
  }
  // An encoder output in PTS order. A dropped tagged input is represented by
  // the next real output after it, which also contains the change. `nowUs`
  // (the output time, 0 = unknown) records the tag's Timing.
  std::optional<Tag> output(uint64_t pts, uint64_t nowUs = 0) {
    std::optional<Assigned> found;
    while (!assigned_.empty() && assigned_.front().pts <= pts) { if (found) ++superseded_; found = assigned_.front(); assigned_.pop_front(); }
    if (!found) return {};
    ++tagged_;
    Tag tag{found->sample, found->pts};
    if (nowUs && found->capturedUs >= found->injectedUs && nowUs >= found->capturedUs) {
      timings_.push_back({found->capturedUs - found->injectedUs, nowUs - found->capturedUs, nowUs});
      while (timings_.size() > timingLimit) timings_.pop_front();
      tag.captureUs = timings_.back().injectToCaptureUs; tag.encodeUs = timings_.back().captureToEncodedUs;
    }
    return tag;
  }
  uint64_t tagged() const { return tagged_; }
  uint64_t superseded() const { return superseded_; }
  uint64_t unchanged() const { return unchanged_; }
  const std::deque<Timing>& timings() const { return timings_; }
  const std::deque<MissEvent>& misses() const { return misses_; }
 private:
  void miss(Miss kind, uint64_t atUs) { misses_.push_back({kind, atUs}); while (misses_.size() > timingLimit) misses_.pop_front(); }
  void noteQuietChange() {
    if (lastInputUs_ && (captureUs_ < lastInputUs_ || captureUs_ - lastInputUs_ < inputLatencyQuietUs)) return;
    quiet_.push_back(captureUs_); while (quiet_.size() > 5 || (quiet_.size() > 1 && captureUs_ - quiet_.front() > 20000000)) quiet_.pop_front();
  }
  // Whether a change at `atUs` is a beat of the rhythm of the recent input-free
  // changes: at least three, the shortest gap (the period, at least
  // inputLatencyAmbientMinPeriodUs) and every other gap a whole number of
  // periods (beats that fell within inputLatencyQuietUs of an input are
  // missing), and `atUs` a whole number (1..6) of periods after the last one.
  bool periodic(uint64_t atUs) const {
    if (quiet_.size() < 3 || atUs <= quiet_.back()) return false;
    const auto onBeat = [](uint64_t gap, uint64_t period, uint64_t& beats) {
      beats = (gap + period / 2) / period; if (beats < 1) return false;
      const uint64_t expected = beats * period, error = gap > expected ? gap - expected : expected - gap;
      return error <= inputLatencyAmbientToleranceUs;
    };
    uint64_t period = UINT64_MAX;
    for (size_t at = 1; at < quiet_.size(); ++at) period = std::min(period, quiet_[at] - quiet_[at - 1]);
    if (period < inputLatencyAmbientMinPeriodUs) return false;
    uint64_t beats = 0;
    for (size_t at = 1; at < quiet_.size(); ++at) if (!onBeat(quiet_[at] - quiet_[at - 1], period, beats)) return false;
    return onBeat(atUs - quiet_.back(), period, beats) && beats <= 6;
  }
  struct Pending { uint32_t sample = 0; uint64_t atUs = 0; };
  struct Assigned { uint32_t sample = 0; uint64_t pts = 0, injectedUs = 0, capturedUs = 0; };
  std::optional<Pending> injected_, candidate_;
  std::deque<Assigned> assigned_;
  std::deque<Timing> timings_;
  std::deque<MissEvent> misses_;
  std::deque<uint64_t> quiet_;
  Options options_;
  uint64_t captureUs_ = 0, lastInputUs_ = 0;
  uint64_t tagged_ = 0, superseded_ = 0, unchanged_ = 0;
};
// Fixed-field stream.stats member for the tagged inputs of the last 30 s
// (nearest-rank p50/p95 like the client): ",\"inputFrame\":{...}" or "".
inline std::string inputFrameTimingJson(const std::deque<InputFrameTagger::Timing>& timings, uint64_t nowUs, const std::deque<InputFrameTagger::MissEvent>& misses = {}) {
  std::vector<double> capture, encoded;
  for (const auto& timing : timings) {
    if (nowUs < timing.atUs || nowUs - timing.atUs > 30000000) continue;
    capture.push_back(double(timing.injectToCaptureUs) / 1000); encoded.push_back(double(timing.captureToEncodedUs) / 1000);
  }
  // Sampled inputs without a tagged frame (optional members; released clients
  // read only the percentile fields and ignore these): `expired` named no
  // changed frame within the window, `ambient` counts changes skipped as the
  // beat of a slow periodic change that no input caused.
  size_t expired = 0, ambient = 0;
  for (const auto& event : misses) {
    if (nowUs < event.atUs || nowUs - event.atUs > 30000000) continue;
    (event.kind == InputFrameTagger::Miss::expired ? expired : ambient)++;
  }
  if (capture.empty() && !expired && !ambient) return {};
  if (capture.empty()) { std::ostringstream none; none << ",\"inputFrame\":{\"samples\":0,\"expired\":" << expired << ",\"ambient\":" << ambient << '}'; return none.str(); }
  const auto rank = [](std::vector<double> values, double fraction) { std::sort(values.begin(), values.end()); const auto at = size_t(std::max(0.0, std::ceil(fraction * double(values.size())) - 1)); return std::min(values[std::min(at, values.size() - 1)], 10000.0); };
  std::ostringstream out; out << std::fixed << std::setprecision(2)
    << ",\"inputFrame\":{\"samples\":" << capture.size() << ",\"injectToCaptureP50Ms\":" << rank(capture, .5) << ",\"injectToCaptureP95Ms\":" << rank(capture, .95)
    << ",\"captureToEncodedP50Ms\":" << rank(encoded, .5) << ",\"captureToEncodedP95Ms\":" << rank(encoded, .95) << ",\"expired\":" << expired << ",\"ambient\":" << ambient << '}';
  return out.str();
}

// Session0's share after the encoder for one tagged video packet (host clock,
// diagnostics only): the worker's encoder output -> the start of its WebSocket
// write (worker pipe, Session0 receive and send queue), and that write's
// duration (WinHttpWebSocketSend until the bytes were accepted by the
// connection; a full send buffer shows here).
struct InputSendTiming { uint64_t queueUs = 0, writeUs = 0, atUs = 0; };
// Fixed-field stream.stats member for the tagged packets of the last 30 s
// (nearest-rank p50/p95): ",\"inputSend\":{...}" or "".
inline std::string inputSendTimingJson(const std::deque<InputSendTiming>& timings, uint64_t nowUs) {
  std::vector<double> queue, write;
  for (const auto& timing : timings) {
    if (nowUs < timing.atUs || nowUs - timing.atUs > 30000000) continue;
    queue.push_back(double(timing.queueUs) / 1000); write.push_back(double(timing.writeUs) / 1000);
  }
  if (queue.empty()) return {};
  const auto rank = [](std::vector<double> values, double fraction) { std::sort(values.begin(), values.end()); const auto at = size_t(std::max(0.0, std::ceil(fraction * double(values.size())) - 1)); return std::min(values[std::min(at, values.size() - 1)], 10000.0); };
  std::ostringstream out; out << std::fixed << std::setprecision(2)
    << ",\"inputSend\":{\"samples\":" << queue.size() << ",\"queueP50Ms\":" << rank(queue, .5) << ",\"queueP95Ms\":" << rank(queue, .95)
    << ",\"writeP50Ms\":" << rank(write, .5) << ",\"writeP95Ms\":" << rank(write, .95) << '}';
  return out.str();
}

// The negotiated state of one stream session, shared by the control, capture
// and send threads of one Agent (service or worker).
class InputLatencySession {
 public:
  // session.start of the service or worker. A missing/other marker disables it.
  void configure(const Json& start) {
    std::lock_guard lock(mutex_);
    session_ = start.get("sessionId").string();
    enabled_ = !session_.empty() && start.get("type").string() == "session.start" && start.get("mode").string() != "rdp" &&
               start.get("inputLatencyProtocol").string() == inputLatencyProtocol && start.get("mediaProtocol").string().empty();
    frameTiming_ = enabled_ && start.get("inputFrameTimingProtocol").string() == inputFrameTimingProtocol;
    tagger_ = {}; sendPending_.reset(); sendTimings_.clear();
  }
  void stop() { std::lock_guard lock(mutex_); enabled_ = frameTiming_ = false; session_.clear(); tagger_ = {}; sendPending_.reset(); sendTimings_.clear(); }
  bool enabled() const { std::lock_guard lock(mutex_); return enabled_; }

  // Service: a sampled input gains the private receive stamp for its worker.
  // Anything else is forwarded unchanged by the caller.
  std::optional<Json> stamp(const Json& input, uint64_t receivedUs) const {
    std::lock_guard lock(mutex_);
    if (!enabled_ || !inputLatencyInteger(input.get("sample"), 1, inputLatencyMaxSample) || !std::holds_alternative<Json::Object>(input.value)) return {};
    auto stamped = input; Json field; field.value = double(receivedUs);
    std::get<Json::Object>(stamped.value)["latencyReceivedUs"] = std::move(field);
    return stamped;
  }
  // Worker: after the actual injection call returned, for every input. Returns
  // the private report for the service when the input was sampled; a
  // successful sampled injection also becomes the frame candidate unless it
  // cannot change a frame by itself (`frameCandidate` false, see
  // inputFrameCandidate). A successful unsampled injection that interrupts
  // (inputFrameInterrupts) drops a pending candidate. Acknowledgements are
  // unchanged.
  std::optional<std::string> injected(const Json& input, bool ok, uint64_t injectedUs, bool frameCandidate = true) {
    std::lock_guard lock(mutex_);
    const auto sample = inputLatencyInteger(input.get("sample"), 1, inputLatencyMaxSample);
    const auto received = inputLatencyInteger(input.get("latencyReceivedUs"), 1, uint64_t(1) << 53);
    if (enabled_ && ok) tagger_.activity(injectedUs);
    if (enabled_ && !sample && ok && inputFrameInterrupts(input)) tagger_.interrupted();
    if (!enabled_ || !sample || !received || *received > injectedUs) return {};
    if (ok && frameCandidate) tagger_.injected(uint32_t(*sample), injectedUs);
    return "{\"type\":\"worker.input.latency\",\"sessionId\":" + jsonQuote(session_) + ",\"sample\":" + std::to_string(*sample) +
           ",\"ok\":" + (ok ? "true" : "false") + ",\"receivedUs\":" + std::to_string(*received) + ",\"injectedUs\":" + std::to_string(injectedUs) + '}';
  }
  // Service send thread, immediately before the WebSocket write: the public
  // acknowledgement. holdUs covers receive -> this write (worker pipe, injection
  // and this send queue), so the controller can remove it from its round trip.
  std::optional<std::string> acknowledge(const Json& report, uint64_t sendUs) const {
    std::lock_guard lock(mutex_);
    const auto sample = inputLatencyInteger(report.get("sample"), 1, inputLatencyMaxSample);
    const auto received = inputLatencyInteger(report.get("receivedUs"), 1, uint64_t(1) << 53);
    const auto injected = inputLatencyInteger(report.get("injectedUs"), 1, uint64_t(1) << 53);
    if (!enabled_ || report.get("type").string() != "worker.input.latency" || report.get("sessionId").string() != session_ || !sample || !received || !injected ||
        !std::holds_alternative<bool>(report.get("ok").value) || *received > *injected || *injected > sendUs || sendUs - *received > inputLatencyMaxUs) return {};
    return "{\"type\":\"input.ack\",\"sessionId\":" + jsonQuote(session_) + ",\"sample\":" + std::to_string(*sample) + ",\"ok\":" +
           (report.get("ok").boolean() ? "true" : "false") + ",\"injectUs\":" + std::to_string(*injected - *received) + ",\"holdUs\":" + std::to_string(sendUs - *received) + '}';
  }
  // Service: a worker frame tag of this negotiated session, rebuilt exactly.
  // With frame-timing-v1 granted (and `nowUs` given at the write) it carries
  // the worker's private durations and encoder output -> this write.
  std::optional<std::string> frame(const Json& tag, uint64_t nowUs = 0) const {
    std::lock_guard lock(mutex_);
    const auto sample = inputLatencyInteger(tag.get("sample"), 1, inputLatencyMaxSample);
    const auto epoch = inputLatencyInteger(tag.get("epoch"), 0, 65535);
    const auto pts = inputLatencyInteger(tag.get("pts"), 0, uint64_t(1) << 53);
    if (!enabled_ || tag.get("type").string() != "input.frame" || tag.get("sessionId").string() != session_ || !sample || !epoch || !pts) return {};
    auto message = frameMessage(session_, *epoch, uint32_t(*sample), *pts);
    const auto capture = inputLatencyInteger(tag.get("captureUs"), 0, inputLatencyMaxUs), encode = inputLatencyInteger(tag.get("encodeUs"), 0, inputLatencyMaxUs);
    const auto output = inputLatencyInteger(tag.get("outputUs"), 1, uint64_t(1) << 53);
    if (frameTiming_ && nowUs && capture && encode && output && *output <= nowUs && nowUs - *output <= inputLatencyMaxUs) {
      message.pop_back();
      message += ",\"captureUs\":" + std::to_string(*capture) + ",\"encodeUs\":" + std::to_string(*encode) + ",\"queueUs\":" + std::to_string(nowUs - *output) + '}';
    }
    return message;
  }
  // Service, at the publication of a worker tag frame() accepted: its private
  // `outputUs` (the worker's encoder output, host clock) waits for the write of
  // the video packet with the tag's PTS. A tag without it (or out of range)
  // measures nothing.
  void expectVideo(const Json& tag, uint64_t nowUs) {
    std::lock_guard lock(mutex_);
    const auto pts = inputLatencyInteger(tag.get("pts"), 0, uint64_t(1) << 53);
    const auto output = inputLatencyInteger(tag.get("outputUs"), 1, uint64_t(1) << 53);
    if (!enabled_ || tag.get("sessionId").string() != session_ || !pts || !output || *output > nowUs || nowUs - *output > inputLatencyMaxUs) { sendPending_.reset(); return; }
    sendPending_ = std::pair{*pts, *output};
  }
  // Service send thread: the WebSocket write of a video packet `pts` ran from
  // startUs until doneUs. Only the packet the last tag named is recorded; a
  // later packet means that one was not written (dropped) and ends the wait.
  void videoWritten(uint64_t pts, uint64_t startUs, uint64_t doneUs) {
    std::lock_guard lock(mutex_);
    if (!enabled_ || !sendPending_ || pts < sendPending_->first) return;
    const auto [expected, output] = *sendPending_; sendPending_.reset();
    if (pts != expected || startUs < output || doneUs < startUs) return;
    sendTimings_.push_back({startUs - output, doneUs - startUs, doneUs});
    while (sendTimings_.size() > InputFrameTagger::timingLimit) sendTimings_.pop_front();
  }
  // Service, in the stream.stats it writes: inputSendTimingJson or "".
  std::string sendTimingJson(uint64_t nowUs) const {
    std::lock_guard lock(mutex_);
    return enabled_ ? inputSendTimingJson(sendTimings_, nowUs) : std::string();
  }
  // Worker capture thread.
  void beginCapture(uint64_t nowUs) { std::lock_guard lock(mutex_); if (enabled_) tagger_.beginCapture(nowUs); }
  void submitted(uint64_t pts, bool contentChanged) { std::lock_guard lock(mutex_); if (enabled_) tagger_.submitted(pts, contentChanged); }
  // The tag to send immediately before this video output, if any. A worker
  // (`privateOutputTime`) adds the encoder output time for its service, which
  // rebuilds the public tag without it (frame()).
  std::optional<std::string> output(uint64_t pts, uint16_t epoch, bool privateOutputTime = false) {
    std::lock_guard lock(mutex_);
    if (!enabled_) return {};
    const auto now = hostMonotonicUs();
    const auto tag = tagger_.output(pts, now);
    if (!tag) return {};
    auto message = frameMessage(session_, epoch, tag->sample, pts);
    if (privateOutputTime) {
      message.pop_back(); message += ",\"outputUs\":" + std::to_string(now);
      if (tag->captureUs && tag->encodeUs) message += ",\"captureUs\":" + std::to_string(*tag->captureUs) + ",\"encodeUs\":" + std::to_string(*tag->encodeUs);
      message += '}';
    }
    return message;
  }
  // Worker capture thread, in its stream.stats: the host's share of recently
  // tagged inputs (inputFrameTimingJson), or "" when none / not negotiated.
  std::string frameTimingJson(uint64_t nowUs) const {
    std::lock_guard lock(mutex_);
    return enabled_ ? inputFrameTimingJson(tagger_.timings(), nowUs, tagger_.misses()) : std::string();
  }
 private:
  static std::string frameMessage(const std::string& session, uint64_t epoch, uint32_t sample, uint64_t pts) {
    return "{\"type\":\"input.frame\",\"sessionId\":" + jsonQuote(session) + ",\"epoch\":" + std::to_string(epoch) + ",\"sample\":" + std::to_string(sample) + ",\"pts\":" + std::to_string(pts) + '}';
  }
  mutable std::mutex mutex_;
  bool enabled_ = false, frameTiming_ = false;
  std::string session_;
  InputFrameTagger tagger_;
  std::optional<std::pair<uint64_t, uint64_t>> sendPending_;  // pts, worker encoder output (host clock)
  std::deque<InputSendTiming> sendTimings_;
};
}
