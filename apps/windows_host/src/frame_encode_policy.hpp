#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace host {
struct FrameEncodeDecision {
  bool submit = false, contentChanged = false, refresh = false, refine = false, forced = false;
};

// Bounded progressive refinement ("build to sharp") for the per-frame CBR
// software H.264 MFT. That encoder budgets bits per SUBMITTED frame and ignores
// timestamps, so with changed-only submission a large change after a quiet
// period is coded starved and the 1 s refresh refines it far too slowly
// (measured: page switches stay at 10-15 dB text PSNR). Refinement re-submits
// the unchanged final frame on the capture cadence once content has been quiet
// for quietUs, so CBR spends its normal per-frame budget on residual detail,
// and stops when outputs show no further gain or after maximumFrames.
// Consequence (measured): an unstarved encoder codes a later whole-page change
// as one large sharp P frame (~0.8 MB at 1080p/12 Mbps), the same state the
// original policy reaches after ~20 s idle; average bitrate stays below target.
// Disabled (budgetBytes == 0, the default) every decision is the original.
struct FrameRefinement {
  size_t budgetBytes = 0;      // CBR bytes per submitted frame (bitrate / 8 / fps)
  size_t triggerBytes = 0;     // a content-change output at least this large arms refinement
  size_t settledBytes = 0;     // a refinement output below this adds no visible detail
  uint32_t settledRun = 3;     // consecutive settled outputs (CBR debt repaid) end refinement
  uint32_t maximumFrames = 0;  // hard bound per armed change
  uint64_t quietUs = 100000;   // content must be unchanged this long before refining
  bool enabled() const { return budgetBytes && maximumFrames; }
  // Production values, from measurement. A 100 ms
  // quiet period leaves animation at >= 10 fps exactly as before (50 ms
  // refined between 15 fps map frames: 2x submissions, 4x bytes, lower PSNR).
  static FrameRefinement forCbr(int bitrate, int fps) {
    FrameRefinement value;
    if (bitrate <= 0 || fps <= 0) return value;
    value.budgetBytes = size_t(bitrate) / 8 / size_t(fps);
    value.triggerBytes = value.budgetBytes / 2;
    value.settledBytes = value.budgetBytes / 4;
    value.maximumFrames = uint32_t(fps) * 3 / 2;
    return value;
  }
};

// One capture/encoder owner thread, one instance per output or new media epoch.
// Input is the FINAL NV12 image, including any cursor composition. WGC's fresh
// surface flag alone does not describe cursor-only changes or GDI duplicates.
// Both clocks are microseconds from the same monotonic stream origin; tests and
// benchmarks may inject a synthetic clock without sleeping or capturing a PC.
class FrameEncodePolicy {
 public:
  static constexpr uint64_t refreshIntervalUs = 1000000;
  static constexpr size_t maximumPendingInputs = 1024;
  static constexpr uint32_t maximumRefinementFrames = 600;
  explicit FrameEncodePolicy(size_t frameBytes, FrameRefinement refinement = {}) : frameBytes_(frameBytes), refinement_(refinement) {
    // NV12 buffers use a DWORD length in the MFT API. Resource availability
    // is checked by the capture/encoder owner before allocation.
    if (!frameBytes || frameBytes > std::numeric_limits<uint32_t>::max())
      throw std::runtime_error("Invalid encoding policy frame size");
    validate(refinement_);
  }
  // Adaptive bitrate: the refinement budget follows a live encoder target
  // change. The running episode (armed, settled run, CBR debt) is kept; only the
  // per-frame budget and its thresholds change. Enabled state cannot change.
  void retarget(FrameRefinement refinement) {
    if (refinement.enabled() != refinement_.enabled()) throw std::runtime_error("Invalid encoding refinement policy");
    validate(refinement); refinement_ = refinement;
  }
  const FrameRefinement& refinement() const { return refinement_; }
  // `force` (lossless tiles only) submits this tick like a refresh once an IDR
  // exists: a tile drop must accompany a decoded frame even when the NV12 did
  // not change. Without it every decision is exactly the original policy's.
  FrameEncodeDecision prepare(std::span<const uint8_t> frame, uint64_t nowUs, bool force = false) {
    if (frame.size() != frameBytes_ || (haveClock_ && nowUs < clockUs_))
      throw std::runtime_error("Invalid encoding policy frame or clock");
    // Exact byte comparison. Remember where the first differing block starts:
    // the snapshot before it already equals the frame, so only the rest is
    // copied below (same resulting snapshot, less memory traffic).
    size_t firstDifferent = 0;
    if (!snapshot_.empty()) firstDifferent = firstDifferentBlock(snapshot_.data(), frame.data(), frameBytes_);
    const bool changed = snapshot_.empty() || firstDifferent < frameBytes_;
    return decide(changed, nowUs, force, [&] {
      // A retained capture.frame() reference aliases its next frame; own bytes.
      if (snapshot_.empty()) snapshot_.assign(frame.begin(), frame.end());
      else std::memcpy(snapshot_.data() + firstDifferent, frame.data() + firstDifferent, frameBytes_ - firstDifferent);
    });
  }
  // gpu-capture-v1: a picture without CPU bytes (the capture's NV12 texture).
  // `changed` is the capture's own evidence (Capture::gpuPictureChanged: a
  // surface or the composed cursor was redrawn); the rules are prepare()'s.
  // The byte snapshot no longer describes what the encoder last received, so
  // it is dropped and the next prepare() counts its frame as changed.
  FrameEncodeDecision prepareChanged(bool changed, uint64_t nowUs, bool force = false) {
    if (haveClock_ && nowUs < clockUs_) throw std::runtime_error("Invalid encoding policy frame or clock");
    if (dropSnapshot) snapshot_.clear();
    return decide(changed || !haveSubmission_, nowUs, force, [] {});
  }
  // Tests only: false keeps the snapshot across GPU pictures (fail-without-fix).
  static inline bool dropSnapshot = true;
  // Call from the real encoder output callback, not after ProcessInput or from
  // a text heartbeat. Only an actually submitted, ordered PTS is evidence.
  // `bytes` is that output's encoded size; refinement needs it (0 = unknown,
  // which never arms refinement).
  bool output(uint64_t ptsUs, bool keyFrame, uint64_t nowUs, size_t bytes = 0) {
    if (!haveSubmission_ || (haveClock_ && nowUs < clockUs_) || ptsUs > nowUs ||
        (haveOutput_ && ptsUs <= lastOutputPts_)) return false;
    const auto found = std::find_if(pending_.begin(), pending_.end(), [&](const Pending& value) { return value.pts == ptsUs; });
    if (found == pending_.end()) return false;
    const bool refinementOutput = found->refine; lastOutputRefinement_ = refinementOutput;
    // Encoders may drop older inputs; bounded retirement also accepts their
    // next real output, without treating an invented/intermediate PTS as one.
    pending_.erase(pending_.begin(), std::next(found));
    clockUs_ = nowUs; haveClock_ = true;
    lastOutputUs_ = nowUs; lastOutputPts_ = ptsUs; haveOutput_ = true;
    if (keyFrame) haveIdr_ = true;
    if (requiredOutputPts_ && ptsUs >= *requiredOutputPts_) requiredOutputPts_.reset();
    if (refinement_.enabled()) observe(ptsUs, refinementOutput, bytes);
    return true;
  }
  bool hasUsableIdr() const { return haveIdr_; }
  bool awaitingOutput() const { return !haveIdr_ || requiredOutputPts_.has_value(); }
  size_t pendingInputs() const { return pending_.size(); }
  bool refinementArmed() const { return armed_; }
  // The output last accepted by output() answered a refinement submission
  // (an unchanged picture re-sent for detail), not a change or a refresh.
  bool lastOutputRefinement() const { return lastOutputRefinement_; }
 private:
  template<class Keep>
  FrameEncodeDecision decide(bool changed, uint64_t nowUs, bool force, Keep&& keep) {
    const bool scheduled = haveIdr_ && !requiredOutputPts_ && haveOutput_ && nowUs - lastOutputUs_ >= refreshIntervalUs;
    const bool forced = force && haveIdr_ && !changed && !scheduled && !requiredOutputPts_;
    const bool refresh = scheduled || forced;
    // Refinement never replaces or delays a changed frame: it only fills ticks
    // that would otherwise submit nothing.
    const bool refine = !changed && !refresh && refining(nowUs);
    const bool submit = changed || refresh || refine || !haveIdr_ || requiredOutputPts_.has_value();
    if (submit && (pending_.size() >= maximumPendingInputs || (haveSubmission_ && nowUs <= lastSubmissionUs_)))
      throw std::runtime_error("Encoder output timestamps did not progress");
    clockUs_ = nowUs; haveClock_ = true;
    if (!submit) return {};
    // Duplicate priming submissions MUST NOT move the required watermark.
    if (changed) {
      keep();
      lastChangeUs_ = nowUs; changePts_ = nowUs; changeOutputPending_ = true;
    }
    if (changed || refresh) requiredOutputPts_ = nowUs;
    if (refine) ++refineFrames_;
    pending_.push_back({nowUs, refine}); lastSubmissionUs_ = nowUs; haveSubmission_ = true;
    return {true, changed, refresh, refine, forced};
  }
  struct Pending { uint64_t pts; bool refine; };
  static void validate(const FrameRefinement& value) {
    if (value.enabled() && (value.maximumFrames > maximumRefinementFrames || !value.settledRun ||
        value.quietUs > refreshIntervalUs || value.settledBytes > value.budgetBytes))
      throw std::runtime_error("Invalid encoding refinement policy");
  }
  bool refining(uint64_t nowUs) {
    if (!refinement_.enabled() || !armed_ || !haveIdr_ || requiredOutputPts_) return false;
    if (refineFrames_ >= refinement_.maximumFrames) { armed_ = false; return false; }
    return nowUs - lastChangeUs_ >= refinement_.quietUs;
  }
  void observe(uint64_t ptsUs, bool refinementOutput, size_t bytes) {
    // Leaky-bucket model of the per-frame CBR budget: while the encoder still
    // repays a large frame its tiny outputs are rate control, not convergence.
    debt_ = debt_ + bytes > refinement_.budgetBytes ? debt_ + bytes - refinement_.budgetBytes : 0;
    if (changeOutputPending_ && ptsUs >= changePts_) {
      changeOutputPending_ = false;
      // Only a change the encoder could not code within about one budget arms
      // a new bounded episode. Cursor/caret-sized changes never do, and they
      // do not extend an episode that is already running.
      if (bytes >= refinement_.triggerBytes && bytes) { armed_ = true; refineFrames_ = 0; settled_ = 0; }
      return;
    }
    if (!armed_ || !refinementOutput) return;
    if (bytes < refinement_.settledBytes && debt_ <= refinement_.budgetBytes) {
      if (++settled_ >= refinement_.settledRun) armed_ = false;
    } else settled_ = 0;
  }
  // Offset of the first differing 4 KiB block, or `bytes` when equal.
  static size_t firstDifferentBlock(const uint8_t* a, const uint8_t* b, size_t bytes) {
    constexpr size_t block = 4096;
    for (size_t at = 0; at < bytes; at += block)
      if (std::memcmp(a + at, b + at, std::min(block, bytes - at)) != 0) return at;
    return bytes;
  }
  const size_t frameBytes_;
  FrameRefinement refinement_;
  std::vector<uint8_t> snapshot_;
  std::deque<Pending> pending_;
  std::optional<uint64_t> requiredOutputPts_;
  uint64_t clockUs_ = 0, lastSubmissionUs_ = 0, lastOutputUs_ = 0, lastOutputPts_ = 0;
  uint64_t lastChangeUs_ = 0, changePts_ = 0, debt_ = 0;
  uint32_t refineFrames_ = 0, settled_ = 0;
  bool haveClock_ = false, haveSubmission_ = false, haveOutput_ = false, haveIdr_ = false;
  bool armed_ = false, changeOutputPending_ = false, lastOutputRefinement_ = false;
};
}
