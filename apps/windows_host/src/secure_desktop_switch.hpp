#pragma once
// Worker (user-session) side of the Role::secure source switch: one object the
// capture loop ticks while the secure desktop shows. It decides when to ask the
// service for the SYSTEM helper, reads the helper's frames from the shared
// section the service handed over, watches the helper's heartbeat, and says for
// every tick whether the picture is the helper's real frame or the notice, which
// stage-244 records to emit and which control messages to send. It never ends
// the stream: every failure is the notice.
//
// Pure (no Windows calls). The capture loop maps the section and passes the view
// with a keep-alive; the tests pass an ordinary buffer and play the helper with
// the same ipc writer the helper uses.
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "secure_desktop_ipc.hpp"
#include "secure_desktop_scope.hpp"

namespace host::secure {

class WorkerSwitch {
 public:
  using Clock = HelperSession::Clock;
  // A helper that was streaming and whose heartbeat stopped this long is lost.
  static constexpr auto heartbeatLimit = std::chrono::seconds(2);

  struct Tick {
    bool request = false;    // send worker.secure.request with `seq`
    bool activated = false;  // send worker.secure.active (the picture is the helper's from now on)
    bool stop = false;       // send worker.secure.stop (no helper is wanted any more)
    bool hold = false;       // the helper was asked for and has not answered yet: encode nothing this tick,
                             // so the client keeps its last picture instead of flashing the notice
    bool notice = true;      // otherwise the picture of this tick: the notice (true) or frame() (false)
    bool fresh = false;      // frame() changed in this tick
    bool sourceChanged = false;  // the picture became the helper's, or the notice after it: code an IDR
    uint32_t seq = 0;
    uint32_t records[4] = {};  // stage-244 records of this tick, in order
    unsigned recordCount = 0;
  };

  // lastSeq: the last request number an earlier switch of the same worker used
  // (a resize starts a new capture loop), so an answer to that one is never
  // taken for an answer to this one.
  explicit WorkerSwitch(uint32_t lastSeq = 0) : seq_(lastSeq) {}

  // One tick while the secure desktop shows. canRequest: this is the service's
  // user worker and the stream owns exactly one output.
  Tick tick(Clock::time_point now, bool canRequest) {
    Tick out;
    bool again = false;
    const auto record = [&](uint32_t value) { if (value && out.recordCount < 4) out.records[out.recordCount++] = value; };
    if (session_.phase() == HelperPhase::idle) {
      view_.reset(); reader_ = {};
      if (canRequest && session_.enter(now)) { ++seq_; wanted_ = true; out.request = true; out.seq = seq_; }
      // No helper for this stream: the notice, with the record it always had (1). A helper that was asked
      // for answers with its own record instead (5, or 6/7/8 which also say that the notice shows).
      else if (session_.phase() == HelperPhase::idle) { session_.unavailable(); record(record244NoticeShown); }
    }
    if (session_.wantHelper()) {
      if (denied_ == seq_) session_.lost();
      else if (view_) {
        counts_ = reader_.inputCounts(); haveCounts_ = true;
        if (reader_.read(frame_, scratch_)) { out.fresh = true; session_.frame(); }
        const uint32_t alive = reader_.alive();
        if (out.fresh || alive != alive_ || !heard_) { alive_ = alive; aliveAt_ = now; heard_ = true; }
        const uint32_t state = reader_.state();
        if (state == ipc::stateEnded) { session_.helperEnded(); again = session_.phase() == HelperPhase::idle; }
        else if (state != ipc::stateRunning) session_.lost();
        else if (session_.phase() == HelperPhase::active && now - aliveAt_ >= heartbeatLimit) session_.lost();
      }
      session_.checkDeadline(now);
    }
    if (const auto taken = session_.takeRecord()) { record(taken); if (taken == record244HelperActive) out.activated = true; }
    if (!session_.wantHelper() && wanted_) { wanted_ = false; out.stop = true; view_.reset(); reader_ = {}; heard_ = false; }
    // A helper that ended by itself is asked for again on the next tick: hold over that gap too.
    out.hold = session_.phase() == HelperPhase::starting || again;
    out.notice = session_.showNotice() || frame_.empty();
    if (!out.hold) {
      const int source = out.notice ? 1 : 2;
      out.sourceChanged = (source == 2 && source_ != 2) || (source == 1 && source_ == 2);
      source_ = source;
    }
    return out;
  }

  // The service's answer to request `seq`: the mapped, read-only view of the
  // section (kept alive by `view`). A late answer to an older request is dropped.
  bool attach(std::shared_ptr<const uint8_t> view, uint32_t width, uint32_t height, uint32_t seq) {
    if (seq != seq_ || session_.phase() != HelperPhase::starting || !view || !ipc::sizeValid(width, height)) return false;
    view_ = std::move(view);
    reader_ = {view_.get(), width, height, 0};
    heard_ = false;
    return true;
  }
  // The service could not launch a helper for request `seq`.
  void denied(uint32_t seq) { if (seq && seq == seq_) denied_ = seq; }

  // The desktop returned, or the capture loop is ending. true: a helper was
  // wanted, so worker.secure.stop must be sent.
  bool reset() {
    const bool stop = wanted_;
    wanted_ = false; heard_ = false; source_ = 0;
    view_.reset(); reader_ = {}; frame_.clear();
    session_.reset();
    return stop;
  }

  // What the helper that just ended (or is still attached) counted of its input, once per helper: for the
  // stage-246 records. Empty when no helper was attached since the last call.
  std::optional<ipc::InputCounts> takeInputCounts() {
    if (view_) { counts_ = reader_.inputCounts(); haveCounts_ = true; }
    if (!haveCounts_) return {};
    haveCounts_ = false;
    return counts_;
  }

  HelperPhase phase() const { return session_.phase(); }
  uint32_t seq() const { return seq_; }
  const std::vector<uint8_t>& frame() const { return frame_; }
  uint32_t frameWidth() const { return reader_.width; }
  uint32_t frameHeight() const { return reader_.height; }

 private:
  HelperSession session_;
  std::shared_ptr<const uint8_t> view_;
  ipc::Reader reader_;
  std::vector<uint8_t> frame_, scratch_;
  Clock::time_point aliveAt_{};
  uint32_t seq_ = 0, denied_ = 0, alive_ = 0;
  int source_ = 0;
  ipc::InputCounts counts_;
  bool wanted_ = false, heard_ = false, haveCounts_ = false;
};

}  // namespace host::secure
