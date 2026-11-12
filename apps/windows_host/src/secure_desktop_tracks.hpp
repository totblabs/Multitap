#pragma once
// The secure desktop on independent outputs ("모든 모니터").
//
// The secure desktop is one desktop across every output, and a stream of independent outputs codes each output on
// its own encoder. So each output gets its own Role::secure helper, unchanged: the same SYSTEM process, the same
// frame section and scope rules as for one output (secure_desktop_helper.hpp), bound to exactly that output's
// rectangle. Every worker.secure.* message of such a stream carries "output" (0 or 1); the service keeps one
// ServiceSecure per output and answers with the same number.
//
//   worker  (TrackSwitches): one WorkerSwitch per output. Each output shows its helper's frames or the notice on
//           its own; a helper that fails leaves the notice on that output only and never ends the stream.
//   service (ServiceSecureTracks): consume() by output; send() writes one already placed input to a helper.
//
// Input. The client names an output and a position on its picture. As for the SYSTEM injector (system_input.hpp)
// the worker's track plan places it (a desktop pixel, nothing else) and the service writes the client's own event:
// a positional event to the helper whose output the pixel is on, which accepts a pixel only inside its own
// output; keys and other events without a position to one helper, the same one for as long as it shows the prompt
// (a key goes down and up in the same process). An output whose helper does not show the prompt takes no input.
// Nothing here widens what a helper may do: it still injects only on the Winlogon desktop of the stream's own
// unlocked session, only while that desktop takes input, and only what the service wrote to its private pipe.
//
// Pure (no Windows calls): the tests play the helpers with the ipc writer and fakes.
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "json_object.hpp"
#include "secure_desktop_helper.hpp"  // OwnedOutput
#include "secure_desktop_ipc.hpp"
#include "secure_desktop_scale.hpp"
#include "secure_desktop_scope.hpp"
#include "secure_desktop_service.hpp"
#include "secure_desktop_switch.hpp"

namespace host::secure {

inline constexpr size_t trackOutputs = 2;
inline constexpr char controlOutput[] = "output";

// The output a worker.secure.* message of independent outputs names: 0 or 1.
inline std::optional<size_t> trackOutput(const Json& message) {
  const auto* number = std::get_if<double>(&message.get(controlOutput).value);
  if (!number || (*number != 0 && *number != 1)) return {};
  return size_t(*number);
}
inline std::string trackRequestJson(const std::string& sessionId, size_t output, uint32_t seq, const OwnedOutput& owned) {
  return std::string("{\"type\":\"") + controlRequest + "\",\"sessionId\":" + jsonQuote(sessionId) + ",\"" + controlOutput + "\":" + std::to_string(output) +
         ",\"seq\":" + std::to_string(seq) + ",\"adapter\":{\"low\":" + std::to_string(owned.adapterLow) + ",\"high\":" + std::to_string(owned.adapterHigh) +
         "},\"rect\":{\"left\":" + std::to_string(owned.left) + ",\"top\":" + std::to_string(owned.top) +
         ",\"width\":" + std::to_string(owned.width) + ",\"height\":" + std::to_string(owned.height) + "}}";
}
inline std::string trackControlJson(const char* type, const std::string& sessionId, size_t output) {
  return std::string("{\"type\":\"") + type + "\",\"sessionId\":" + jsonQuote(sessionId) + ",\"" + controlOutput + "\":" + std::to_string(output) + '}';
}

// ---- worker side ----------------------------------------------------------------

class TrackSwitches {
 public:
  using Clock = WorkerSwitch::Clock;
  struct Env {
    std::function<void(const std::string&)> send;                        // one control message to the service
    std::function<std::optional<OwnedOutput>(size_t)> owned;             // the DXGI identity of output n as it is now
    std::function<void(uint32_t)> record;                                // hostAction 200 stage 244
    std::function<void(const ipc::InputCounts&)> counts;                 // what a helper that ended counted (stage 246)
    std::function<void(const uint8_t*, uint8_t*, int, int)> toNv12;      // BGRA -> NV12 at the coded size
  };
  struct Still { const std::vector<uint8_t>* nv12 = nullptr; bool hold = false, real = false, key = false; };

  // lastSeq: the last request number an earlier TrackSwitches of this worker used for each output (a resize
  // starts a new capture loop), so an answer to that one is never taken for an answer to this one.
  TrackSwitches(std::string session, std::array<uint32_t, trackOutputs> lastSeq, Env env)
      : session_(std::move(session)), env_(std::move(env)), outputs_{Output(lastSeq[0]), Output(lastSeq[1])} {}

  // The service's answer for output n: the mapped, read-only view of the helper's section, or a refusal.
  // false: a late answer to an older request, dropped.
  bool attach(size_t n, std::shared_ptr<const uint8_t> view, uint32_t width, uint32_t height, uint32_t seq) {
    return n < trackOutputs && outputs_[n].sw.attach(std::move(view), width, height, seq);
  }
  void denied(size_t n, uint32_t seq) { if (n < trackOutputs) outputs_[n].sw.denied(seq); }

  // One tick of output n while the secure desktop shows. width/height: the output's coded size.
  // canRequest: this is the service's own user worker. Never throws: a failure is the notice on this output.
  Still tick(size_t n, uint32_t width, uint32_t height, Clock::time_point now, bool canRequest) noexcept {
    Still out;
    if (n >= trackOutputs) return out;
    auto& o = outputs_[n];
    if (o.off) return out;
    try {
      const auto tick = o.sw.tick(now, canRequest);
      for (unsigned i = 0; i < tick.recordCount; ++i) record(tick.records[i]);
      if (tick.request) {
        const auto owned = env_.owned ? env_.owned(n) : std::nullopt;
        if (owned) send(trackRequestJson(session_, n, tick.seq, *owned)); else o.sw.denied(tick.seq);
      }
      if (tick.activated) send(trackControlJson(controlActive, session_, n));
      if (tick.stop) { counts(o); send(trackControlJson(controlStop, session_, n)); }
      out.key = tick.sourceChanged;
      if (tick.hold) { out.hold = true; return out; }
      if (!tick.notice) {
        const auto& bgra = o.sw.frame();
        const uint32_t ow = o.sw.frameWidth(), oh = o.sw.frameHeight();
        const size_t bytes = size_t(width) * size_t(height) * 3 / 2;
        if (!width || !height || bgra.size() != size_t(ow) * oh * 4 || !env_.toNv12) throw std::runtime_error("secure_frame_size");
        if (tick.fresh || o.nv12.size() != bytes) {
          const uint8_t* picture = bgra.data();
          if (ow != width || oh != height) { o.scaled.resize(size_t(width) * size_t(height) * 4); scaleBgra(bgra.data(), ow, oh, o.scaled.data(), width, height); picture = o.scaled.data(); }
          if (o.nv12.size() != bytes) o.nv12.assign(bytes, 0);
          env_.toNv12(picture, o.nv12.data(), int(width), int(height));
        }
        out.nv12 = &o.nv12; out.real = true;
      }
    } catch (...) {
      // The notice on this output for the rest of this secure desktop.
      const bool streamed = o.sw.phase() == HelperPhase::active;
      o.off = true; o.nv12.clear();
      out = {}; out.key = true;
      try { counts(o); if (o.sw.reset()) send(trackControlJson(controlStop, session_, n)); record(streamed ? record244HelperLost : record244HelperLaunchFail); } catch (...) {}
    }
    return out;
  }

  // The desktop returned, or the capture loop ends: no helper is wanted any more.
  void ended() noexcept {
    for (size_t n = 0; n < trackOutputs; ++n) {
      auto& o = outputs_[n];
      try { counts(o); o.off = false; o.nv12.clear(); if (o.sw.reset()) send(trackControlJson(controlStop, session_, n)); } catch (...) {}
    }
  }
  uint32_t seq(size_t n) const { return n < trackOutputs ? outputs_[n].sw.seq() : 0; }
  HelperPhase phase(size_t n) const { return n < trackOutputs ? outputs_[n].sw.phase() : HelperPhase::idle; }

 private:
  struct Output {
    explicit Output(uint32_t lastSeq) : sw(lastSeq) {}
    WorkerSwitch sw;
    std::vector<uint8_t> scaled, nv12;
    bool off = false;
  };
  void send(const std::string& message) { if (env_.send) try { env_.send(message); } catch (...) {} }
  void record(uint32_t error) { if (env_.record) try { env_.record(error); } catch (...) {} }
  void counts(Output& o) { if (const auto seen = o.sw.takeInputCounts(); seen && env_.counts) try { env_.counts(*seen); } catch (...) {} }

  const std::string session_;
  Env env_;
  std::array<Output, trackOutputs> outputs_;
};

// ---- service side ---------------------------------------------------------------

class ServiceSecureTracks {
 public:
  ServiceSecureTracks(const std::string& session, const ServiceSecure::Env& env) {
    for (size_t n = 0; n < trackOutputs; ++n) outputs_[n] = std::make_unique<ServiceSecure>(session, env, int(n));
  }
  ~ServiceSecureTracks() { close(); }
  ServiceSecureTracks(const ServiceSecureTracks&) = delete;
  ServiceSecureTracks& operator=(const ServiceSecureTracks&) = delete;

  // Relay thread. true: one of this feature's messages; consumed (never forwarded), valid or not. A message
  // that names no output is consumed and dropped.
  bool consume(const Json& value) {
    if (!secureControlConsumed(value.get("type").string())) return false;
    if (const auto output = trackOutput(value)) (void)outputs_[*output]->consume(value);
    return true;
  }

  // Any thread: a helper of this stream shows the prompt now.
  bool routing() {
    for (auto& output : outputs_) if (output->routingRect()) return true;
    return false;
  }
  // Any thread: one placed input (system_input.hpp). positional: (x, y) is the desktop pixel the worker placed it
  // on, and it goes to the helper whose output that pixel is on. Otherwise it goes to the helper that took the
  // last such input while that one still shows the prompt, else to the first that does. false: not written.
  bool send(const std::string& message, int32_t x, int32_t y, bool positional) {
    std::array<std::optional<HelperArgs>, trackOutputs> rects;
    for (size_t n = 0; n < trackOutputs; ++n) rects[n] = outputs_[n]->routingRect();
    if (positional) {
      for (size_t n = 0; n < trackOutputs; ++n) {
        if (!rects[n]) continue;
        const int64_t left = rects[n]->left, top = rects[n]->top;
        if (x >= left && x < left + int64_t(rects[n]->width) && y >= top && y < top + int64_t(rects[n]->height)) return outputs_[n]->sendPlaced(message);
      }
      return false;
    }
    size_t target = trackOutputs;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (keys_ < trackOutputs && rects[keys_]) target = keys_;
      else for (size_t n = 0; n < trackOutputs && target == trackOutputs; ++n) if (rects[n]) target = n;
      keys_ = target;
    }
    return target < trackOutputs && outputs_[target]->sendPlaced(message);
  }
  // An input.reset: every helper that shows the prompt lets go of what it holds.
  bool reset(const std::string& message) {
    bool written = false;
    for (auto& output : outputs_) if (output->routingRect()) written |= output->sendPlaced(message);
    return written;
  }

  // Relay thread, at the end of the relay (and from the destructor): no helper outlives its stream.
  void close() { for (auto& output : outputs_) if (output) output->close(); }
  bool helperHeld(size_t n) { return n < trackOutputs && outputs_[n]->helperHeld(); }

 private:
  std::array<std::unique_ptr<ServiceSecure>, trackOutputs> outputs_;
  std::mutex mutex_;
  size_t keys_ = trackOutputs;
};

}  // namespace host::secure
