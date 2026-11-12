#pragma once
// Service (SYSTEM, session 0) side of the Role::secure bridge, as logic with the
// Windows parts injected:
//
//   * consume(): every worker.secure.* message of the user worker ends here and
//     is never forwarded to the broker. A request launches at most one helper
//     for the stream (a newer request ends the older helper first), only while
//     the stream is live and its session unlocked, and at most launchBudget
//     times a minute. The answer goes back to the worker only: ready with the
//     section handles, or failed.
//   * route(): the client's input reaches the helper from HERE, never through
//     the user worker, and only while the helper this service launched is alive
//     and the worker reported that the stream shows its frames
//     (secure::serviceRoutesInput). The helper checks its own scope again for
//     every event.
//   * close(): the helper never outlives the relay of its stream.
//
// The real launch is secure_desktop_launch.hpp; the tests pass a fake.
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "json_object.hpp"
#include "secure_desktop_ipc.hpp"
#include "secure_desktop_scope.hpp"

namespace host::secure {

// The output the worker asks the helper to capture.
struct HelperArgs {
  uint32_t adapterLow = 0;
  int32_t adapterHigh = 0;
  int32_t left = 0, top = 0;
  uint32_t width = 0, height = 0;
};

// One launched helper, as the service holds it.
struct ServiceHelper {
  virtual ~ServiceHelper() = default;
  virtual bool alive() = 0;                         // the process has not exited
  virtual bool send(const std::string& json) = 0;   // one input message to the helper's private pipe
  virtual void close() = 0;                         // end the helper (orderly first, then its job)
};
// A launch: the helper, and the two handle values the service duplicated into
// the WORKER process (read-only section, wait-only event).
struct HelperGrant {
  std::shared_ptr<ServiceHelper> helper;
  uint64_t section = 0, event = 0;
  std::function<void()> revoke;  // closes those two handles in the worker when the answer cannot be delivered
};

namespace detail {
inline bool wholeNumber(const Json& value, double low, double high, double& out) {
  const auto* number = std::get_if<double>(&value.value);
  if (!number || !std::isfinite(*number) || std::floor(*number) != *number || *number < low || *number > high) return false;
  out = *number;
  return true;
}
}  // namespace detail

// worker.secure.request -> HelperArgs. The worker supplies only the identity of
// the output it captures; session and user come from the service's own facts.
inline std::optional<HelperArgs> parseHelperRequest(const Json& value) {
  double low = 0, high = 0, left = 0, top = 0, width = 0, height = 0;
  const auto& adapter = value.get("adapter");
  const auto& rect = value.get("rect");
  if (!detail::wholeNumber(adapter.get("low"), 0, 4294967295.0, low) || !detail::wholeNumber(adapter.get("high"), -2147483648.0, 2147483647.0, high) ||
      !detail::wholeNumber(rect.get("left"), -65536, 65536, left) || !detail::wholeNumber(rect.get("top"), -65536, 65536, top) ||
      !detail::wholeNumber(rect.get("width"), ipc::minSide, ipc::maxSide, width) || !detail::wholeNumber(rect.get("height"), ipc::minSide, ipc::maxSide, height))
    return {};
  HelperArgs args;
  args.adapterLow = uint32_t(low);
  args.adapterHigh = int32_t(high);
  args.left = int32_t(left);
  args.top = int32_t(top);
  args.width = uint32_t(width);
  args.height = uint32_t(height);
  return args;
}

class ServiceSecure {
 public:
  struct Env {
    std::function<bool()> eligible;                                        // live stream, unlocked session, live worker, no resize in flight
    std::function<std::optional<HelperGrant>(const HelperArgs&)> launch;   // nullopt: no helper
    std::function<bool(const std::string&)> toWorker;                      // one command to the worker's pipe
    std::function<uint64_t()> nowMs;
    // Optional: when a helper ends, how much input the service wrote to it, kept for the worker while it was
    // held (not yet showing, or exited), and failed to write. Counts only.
    std::function<void(uint32_t routed, uint32_t heldBack, uint32_t writeFailed)> counts;
  };
  static constexpr unsigned launchBudget = 12;        // launches per budgetWindowMs for one stream
  static constexpr uint64_t budgetWindowMs = 60000;
  static constexpr size_t inputLimit = 8192;          // bytes of one routed input message

  // output: -1 for a stream of one output. For independent outputs (secure_desktop_tracks.hpp) the index of
  // the output this instance serves: its answers to the worker carry it.
  ServiceSecure(std::string sessionId, Env env, int output = -1) : session_(std::move(sessionId)), env_(std::move(env)), output_(output) {}
  ~ServiceSecure() { close(); }
  ServiceSecure(const ServiceSecure&) = delete;
  ServiceSecure& operator=(const ServiceSecure&) = delete;

  // Relay thread. true: the message was one of this feature's and is consumed
  // (never forwarded), whether or not it was valid.
  bool consume(const Json& value) {
    const auto type = value.get("type").string();
    if (!secureControlConsumed(type)) return false;
    if (closed_ || session_.empty() || value.get("sessionId").string() != session_) return true;
    if (type == controlRequest) {
      drop();  // an older helper never outlives a newer request
      double seq = 0;
      if (!detail::wholeNumber(value.get("seq"), 1, 2147483647.0, seq)) return true;
      std::optional<HelperGrant> grant;
      const auto args = parseHelperRequest(value);
      if (args && withinBudget() && env_.eligible && env_.eligible() && env_.launch) {
        try { grant = env_.launch(*args); } catch (...) { grant.reset(); }
      }
      const std::string head = std::string("\",\"sessionId\":") + jsonQuote(session_) + ",\"seq\":" + std::to_string(uint32_t(seq)) +
                               (output_ >= 0 ? ",\"output\":" + std::to_string(output_) : std::string());
      if (grant && grant->helper) {
        { std::lock_guard<std::mutex> lock(mutex_); helper_ = grant->helper; active_ = false; args_ = *args; }
        const std::string ready = std::string("{\"type\":\"") + controlReady + head + ",\"section\":" + std::to_string(grant->section) +
                                  ",\"event\":" + std::to_string(grant->event) + ",\"width\":" + std::to_string(args->width) +
                                  ",\"height\":" + std::to_string(args->height) + '}';
        if (!env_.toWorker || !env_.toWorker(ready)) { drop(); if (grant->revoke) try { grant->revoke(); } catch (...) {} }
      } else if (env_.toWorker) {
        (void)env_.toWorker(std::string("{\"type\":\"") + controlFailed + head + '}');
      }
      return true;
    }
    if (type == controlActive) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (helper_ && helper_->alive()) active_ = true;
      return true;
    }
    if (type == controlStop) { drop(); return true; }
    return true;
  }

  // Any thread (the broker's control path). true: the input went to the helper
  // and must not also go to the worker. input.reset goes to both, so each side
  // releases what it holds.
  bool route(const Json& message) {
    const auto type = message.get("type").string();
    if (!inputTypeAllowed(type)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!helper_) return false;
    if (!serviceRoutesInput(type, helper_->alive(), active_)) { ++heldBack_; return false; }
    const auto text = jsonObject(message);
    if (text.size() > inputLimit) return type == "input";  // never reaches a prompt; dropped
    if (!helper_->send(text)) { active_ = false; ++writeFailed_; return false; }
    ++routed_;
    return type == "input";
  }

  // Independent outputs (any thread). The rectangle of the output this instance's helper captures, while that
  // helper is alive and the worker reported that the stream shows its frames; and one already placed input for
  // it (the client's event with a desktop pixel, or one without a position). false: not written.
  std::optional<HelperArgs> routingRect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!helper_ || !active_ || !helper_->alive()) return {};
    return args_;
  }
  bool sendPlaced(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!helper_) return false;
    if (!active_ || !helper_->alive()) { ++heldBack_; return false; }
    if (text.size() > inputLimit) return false;
    if (!helper_->send(text)) { active_ = false; ++writeFailed_; return false; }
    ++routed_;
    return true;
  }

  // Relay thread, when the relay of this stream ends (and from the destructor).
  void close() { closed_ = true; drop(); }

  bool helperHeld() { std::lock_guard<std::mutex> lock(mutex_); return bool(helper_); }
  bool routing() { std::lock_guard<std::mutex> lock(mutex_); return helper_ && active_; }

 private:
  void drop() {
    std::shared_ptr<ServiceHelper> old;
    uint32_t routed = 0, heldBack = 0, writeFailed = 0;
    { std::lock_guard<std::mutex> lock(mutex_); old = std::move(helper_); helper_.reset(); active_ = false;
      routed = std::exchange(routed_, 0u); heldBack = std::exchange(heldBack_, 0u); writeFailed = std::exchange(writeFailed_, 0u); }
    if (old) { old->close(); if (env_.counts) try { env_.counts(routed, heldBack, writeFailed); } catch (...) {} }
  }
  bool withinBudget() {
    const uint64_t now = env_.nowMs ? env_.nowMs() : 0;
    while (!launches_.empty() && now - launches_.front() >= budgetWindowMs) launches_.pop_front();
    if (launches_.size() >= launchBudget) return false;
    launches_.push_back(now);
    return true;
  }

  const std::string session_;
  Env env_;
  const int output_;
  std::mutex mutex_;
  std::shared_ptr<ServiceHelper> helper_;
  HelperArgs args_;
  bool active_ = false;
  uint32_t routed_ = 0, heldBack_ = 0, writeFailed_ = 0;
  std::atomic<bool> closed_{false};
  std::deque<uint64_t> launches_;
};

}  // namespace host::secure
