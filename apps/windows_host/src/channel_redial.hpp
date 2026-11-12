#pragma once
// channel-resume-v1 (Broker server/src/channel-resume.mjs): a stream's side
// channels come back by themselves after the stream resumes. Before it the
// print, drive and USB channels of a session were started once and never
// again, and the clipboard channel only came back after this Host's own
// outage: when the client was away the Broker refused the redial and nobody
// told the Host that the client had returned.
//
// The Host advertises `hostChannelResumeProtocol`. A session.start that names
// `channelResumeProtocol` is a session whose Broker
//   * closes a side channel with `<channel>_stream_away` while the client or
//     this Host is away and answers a dial then with 503 (not now);
//   * says {"type":"session.channels","sessionId":id,"state":"away"|"back"}
//     when the client leaves and returns (a returning Host hears the state
//     before its host.session.resumed);
//   * lets a channel that ended for a transport reason pair again, with the
//     checks of a first pair (this Host's token, the stored permissions as
//     they are then). Anything else it refuses, and that refusal is final here.
// Nothing of an ended channel is kept on the Host: its printer, drive letters
// and USB devices go with it exactly as before, and the fresh pair sets them
// up again (the client offers its folders and devices again). Every other
// Broker gets the released behaviour: no session names the protocol, so
// nothing below is started.
//
// The rule is the clipboard's (clipboard_reconnect.hpp), now for all four:
// three fresh pairs after 1, 2 and 4 seconds, no attempt spent while the
// stream is away, one fresh set when it continues, a refusal ends it.
#include "clipboard_reconnect.hpp"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace host {
inline constexpr char channelResumeProtocol[] = "channel-resume-v1";

// A side channel of `kind` ("print", "drive", "usb") that ended with `reason`
// may be dialled again in the same session: its transport or the stream itself
// went away. A violation, a limit or a revoked permission stays ended.
inline bool channelEndTransient(const std::string& kind, const std::string& reason) {
  return reason == kind + "_transport_closed" || reason == kind + "_pair_timeout" || reason == kind + "_stream_away" || reason == kind + "_timeout";
}
// The HTTP status of a dial (0: no answer). 101 is a pair, "not now" and a
// gateway's own failures are transient, every other answer is a refusal.
inline bool channelDialRefused(unsigned long status) {
  return status != 0 && status != 101 && status != 408 && status != 502 && status != 503 && status != 504;
}

// Whether the stream itself is away: this Host's Broker channel (host-resume-v1)
// or the client (session.channels). Each setter says whether the answer changed.
class ChannelPresence {
 public:
  bool host(bool away) { return set(host_, away); }
  bool peer(bool away) { return set(peer_, away); }
  bool away() const { std::lock_guard lock(mutex_); return host_ || peer_; }
  void reset() { std::lock_guard lock(mutex_); host_ = peer_ = false; }
 private:
  bool set(bool& side, bool away) { std::lock_guard lock(mutex_); const bool before = host_ || peer_; side = away; return before != (host_ || peer_); }
  mutable std::mutex mutex_; bool host_ = false, peer_ = false;
};

// One session's print, drive or USB channel, dialled again after a transient
// end. The channel reports its own end (never the one stop() caused) from any
// thread; the dial runs on this object's thread, so the reporting thread is
// never joined by itself. `speed` divides the delays (tests).
class ChannelRedial {
 public:
  using Dial = std::function<void()>;
  using Step = ClipboardReconnect::Step;
  explicit ChannelRedial(unsigned speed = 1) : speed_(speed ? speed : 1) {}
  ~ChannelRedial() { end(); }
  ChannelRedial(const ChannelRedial&) = delete; ChannelRedial& operator=(const ChannelRedial&) = delete;
  // The session negotiated channel-resume-v1. `dial` starts a fresh pair of
  // the channel with the session's own arguments; a throw is a refusal.
  void begin(Dial dial) {
    std::lock_guard lifecycle(lifecycle_);
    end();
    { std::lock_guard lock(mutex_); dial_ = std::move(dial); reconnect_ = ClipboardReconnect{}; quitting_ = pending_ = resumed_ = final_ = dialling_ = false; redials_ = 0; }
    thread_ = std::thread([this] { run(); });
  }
  // The channel ended by itself. `transient`: the same session may pair again.
  void ended(bool transient) {
    { std::lock_guard lock(mutex_); if (quitting_ || final_) return; pending_ = true; transient_ = transient; }
    wake_.notify_all();
  }
  // The stream is away / continues (ChannelPresence). Any thread; neither blocks.
  void away() { std::lock_guard lock(mutex_); reconnect_.away(); }
  void resumed() {
    { std::lock_guard lock(mutex_); if (!reconnect_.resumed()) return; resumed_ = true; }
    wake_.notify_all();
  }
  // The session ended: no dial starts or still runs after this returns.
  void end() {
    std::lock_guard lifecycle(lifecycle_);
    { std::lock_guard lock(mutex_); quitting_ = true; }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_); dial_ = {};
  }
  // Observation (tests, diagnostics).
  unsigned redials() const { std::lock_guard lock(mutex_); return redials_; }
  bool waiting() const { std::lock_guard lock(mutex_); return reconnect_.waiting(); }
  bool stopped() const { std::lock_guard lock(mutex_); return final_; }
  bool idle() const { std::lock_guard lock(mutex_); return !pending_ && !dialling_; }
 private:
  const unsigned speed_;
  std::recursive_mutex lifecycle_; mutable std::mutex mutex_; std::condition_variable wake_; std::thread thread_;
  Dial dial_; ClipboardReconnect reconnect_;
  bool quitting_ = true, pending_ = false, transient_ = false, resumed_ = false, final_ = false, dialling_ = false; unsigned redials_ = 0;
  void run() {
    std::unique_lock lock(mutex_);
    for (;;) {
      wake_.wait(lock, [this] { return quitting_ || (pending_ && !final_); });
      if (quitting_) return;
      pending_ = false;
      std::chrono::milliseconds delay{};
      auto step = reconnect_.next(transient_, delay);
      for (;;) {
        // Lost with the stream (or out of attempts): until the stream continues.
        while (step == Step::wait) {
          wake_.wait(lock, [this] { return quitting_ || resumed_; });
          if (quitting_) return;
          resumed_ = false; step = reconnect_.next(true, delay);
        }
        if (step != Step::retry) break;
        if (wake_.wait_for(lock, delay / speed_, [this] { return quitting_; })) return;
        if (!reconnect_.isAway()) break;
        // The stream went away during the delay: a dial would say nothing.
        step = reconnect_.next(true, delay);
      }
      if (step != Step::retry) { final_ = true; continue; }
      // The fresh pair answers every end reported so far; its own end is a new report.
      resumed_ = pending_ = false; dialling_ = true; ++redials_;
      const Dial dial = dial_;
      lock.unlock();
      bool failed = false;
      try { if (dial) dial(); else failed = true; } catch (...) { failed = true; }
      lock.lock();
      dialling_ = false;
      // It could not even be started (a URL this Host refuses, a closing service): final.
      if (failed) final_ = true;
    }
  }
};
}
