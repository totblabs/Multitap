#pragma once
// A bounded way back to the GPU encoder (docs/x264-encoder.md "Backends and
// fallback"). A hardware encoder that misses its output deadline while the VM
// is overloaded would otherwise leave the stream on x264 for the rest of the
// encoder's life, which loads a busy VM further (it shows as hostAction
// 254 fallback 41, 255 error 41 win32 258). After such a transient failure the
// Encoder opens the GPU encoder again later, off the encoding thread, and
// changes over at the new encoder's own first IDR (no stream.info, the epoch
// continues). A failed attempt changes nothing: the CPU encoder goes on.
// Pure policy: no clock, device or driver call here.
#include <cstdint>
#include <optional>

namespace host {
// First attempt this long after the fallback, then twice as long each time.
inline constexpr uint64_t gpuRetryFirstUs = 30'000'000;
// Attempts per Encoder (one stream epoch at one picture size).
inline constexpr uint32_t gpuRetryMaximumAttempts = 3;
// EncoderGpuFallback values (media.hpp) named here without that header.
inline constexpr uint32_t gpuRetryCodeInitializationFailed = 38, gpuRetryCodeOutputTimeout = 41;
// Whether a failure with this EncoderGpuFallback code may be tried again. A
// running encoder: only a missed output deadline. An attempt: also an open
// that did not complete (the driver was still busy). A lost device (which is
// also what NVENC reports once this process holds an encoder it could not
// close), an unsupported format or capability, a broken output contract and a
// refused reconfiguration never are.
inline constexpr bool gpuRetryTransient(uint32_t code, bool attempt) {
  return code == gpuRetryCodeOutputTimeout || (attempt && code == gpuRetryCodeInitializationFailed);
}
// Fixed record vocabulary (worker stage 45: hostAction 200 stage 245,
// error = kind | attempt << 8 | EncoderGpuFallback code << 16).
enum GpuRetryRecord : uint32_t {
  gpuRetryStarted = 1,       // an attempt began; code = why the stream is on the CPU
  gpuRetryReturned = 2,      // the stream is on the GPU encoder again; code 0
  gpuRetryOpenFailed = 3,    // the GPU encoder did not open; code = why
  gpuRetryOutputFailed = 4,  // it opened but its first picture failed; code = why
  gpuRetryEnded = 5,         // no further attempt in this encoder; code = the last failure
};
inline constexpr uint32_t gpuRetryRecordValue(uint32_t kind, uint32_t attempt, uint32_t code) {
  return (kind & 0xff) | (attempt & 0xff) << 8 | (code & 0xffff) << 16;
}
class GpuRetrySchedule {
 public:
  // The stream left the GPU encoder (or an attempt failed) at `nowUs`.
  // `transient`: gpuRetryTransient of its code, and a way to reopen exists.
  void left(bool transient, uint64_t nowUs) {
    if (!transient || attempts_ >= gpuRetryMaximumAttempts) { next_.reset(); return; }
    next_ = nowUs + (gpuRetryFirstUs << attempts_);
  }
  // An attempt is waiting for its time.
  bool pending() const { return next_.has_value(); }
  bool due(uint64_t nowUs) const { return next_ && nowUs >= *next_; }
  // The due attempt starts now; its number (1..gpuRetryMaximumAttempts).
  uint32_t begin() { next_.reset(); return ++attempts_; }
  uint32_t attempts() const { return attempts_; }
 private:
  std::optional<uint64_t> next_;
  uint32_t attempts_ = 0;
};
}
