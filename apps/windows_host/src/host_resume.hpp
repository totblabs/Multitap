#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
// host-resume-v1 (tuning backlog #32; Broker server/src/session-resume.mjs). A
// lost broker channel used to stop the stream at once, so the user's next
// connection prepared the display again. Now a stream whose channel was lost
// (not refused) keeps its worker, capture and display for holdMs. Its media is
// dropped meanwhile (the send queue is closed), and input was released when
// the channel ended. The next channel offers it to a Broker whose capabilities
// ACK names the protocol: {"type":"host.session.resume","sessions":[id]} (or
// [] with nothing kept, so the Broker ends a waiting session at once). The
// Broker answers host.session.resumed (keep streaming from an IDR) or a plain
// session.stop. A released Broker, a refusal or the end of the hold stops it.
namespace host {
inline constexpr char hostResumeProtocol[] = "host-resume-v1";
class HostResumeHold {
 public:
  // Longer than the Broker's detection (up to 30 s of silence) plus its own
  // 30 s window, so the Broker decides; this only bounds a Broker that never answers.
  static constexpr uint64_t holdMs = 60000;
  // The channel ended. True: keep the stream (already held, or a transport
  // loss of a stream a resume-capable Broker granted).
  bool lost(bool streaming, std::string_view failure, uint64_t nowMs) {
    std::lock_guard lock(mutex_);
    if (held_) return true;
    const bool transport = failure == "transport_closed" || failure == "network_unavailable" || failure == "tls_failed" || failure == "server_unavailable";
    if (!streaming || !offered_ || !transport) return false;
    held_ = true; since_ = nowMs;
    return true;
  }
  bool expired(uint64_t nowMs) const { std::lock_guard lock(mutex_); return held_ && nowMs - since_ >= holdMs; }
  bool held() const { std::lock_guard lock(mutex_); return held_; }
  // A new channel: nothing offered on it yet.
  void channel() { std::lock_guard lock(mutex_); offered_ = asked_ = false; }
  // The capabilities ACK of this channel. The message to send, once per channel,
  // when the Broker resumes; `stop` is set when a held stream must end instead.
  std::optional<std::string> acknowledged(bool resumable, const std::string& session, bool& stop) {
    std::lock_guard lock(mutex_);
    offered_ = resumable; stop = false;
    if (asked_) return std::nullopt;
    asked_ = true;
    if (!resumable) { stop = held_; held_ = false; return std::nullopt; }
    return std::string("{\"type\":\"host.session.resume\",\"sessions\":[") + (held_ && !session.empty() ? "\"" + session + "\"" : std::string()) + "]}";
  }
  // host.session.resumed for the held stream: it continues.
  bool resumed(const std::string& session, const std::string& current) {
    std::lock_guard lock(mutex_);
    if (!held_ || session.empty() || session != current) return false;
    held_ = false;
    return true;
  }
  void clear() { std::lock_guard lock(mutex_); held_ = false; }
 private:
  mutable std::mutex mutex_;
  bool held_ = false, offered_ = false, asked_ = false;
  uint64_t since_ = 0;
};
}
