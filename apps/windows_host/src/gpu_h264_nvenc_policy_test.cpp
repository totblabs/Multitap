#include "gpu_h264_nvenc_policy.hpp"
#include "gpu_h264_nvenc_config.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {
unsigned checks = 0;
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
// The same regression source can exercise the preserved 103 header and the
// fixed header. Only the old signature consumes the separate raw status field.
template<class Metadata>
auto metadataAfterSuccessfulApi(Metadata metadata, uint32_t rawStatus,
    bool hasBuffer, uint64_t bytes, uint64_t maximumBytes,
    uint64_t outputPts, uint64_t inputPts, uint32_t pictureType) {
  if constexpr (std::is_invocable_v<Metadata, uint32_t, bool, uint64_t, uint64_t, uint64_t, uint64_t, uint32_t>) {
    return metadata(rawStatus, hasBuffer, bytes, maximumBytes, outputPts, inputPts, pictureType);
  } else {
    (void)rawStatus;
    return metadata(hasBuffer, bytes, maximumBytes, outputPts, inputPts, pictureType);
  }
}
struct FakeOutput {
  uint64_t now = 0;
  std::vector<uint32_t> statuses;
  size_t calls = 0;
  uint32_t paused = 0;
  bool frozen = false;
  host::gpu::NvencOutputLockResult run(host::gpu::NvencOutputDeadline deadline = {}) {
    return host::gpu::nvencLockOutputBounded(deadline, 17, 89,
      [&] { return now; }, [&] { const size_t at = calls++; return statuses.at(std::min(at, statuses.size() - 1)); },
      [&](uint32_t delay) { paused += delay; if (!frozen) now += delay; });
  }
};
}
// The streaming configuration over a preset the driver could return (here one that would
// add B-frames, lookahead, AQ, a short GOP and a big VBV). No GPU: what the encoder is
// asked for, not what an RTX card then does.
void streamingConfig() {
  using namespace host::gpu;
  NV_ENC_CONFIG preset{}; preset.gopLength = 250; preset.frameIntervalP = 3; preset.rcParams.enableLookahead = 1; preset.rcParams.lookaheadDepth = 8;
  preset.rcParams.enableAQ = 1; preset.rcParams.enableTemporalAQ = 1; preset.rcParams.zeroReorderDelay = 0; preset.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
  preset.rcParams.vbvBufferSize = 50000000; preset.encodeCodecConfig.h264Config.idrPeriod = 250; preset.encodeCodecConfig.h264Config.maxNumRefFrames = 2;
  for (const bool recovers : {false, true}) for (const bool wide : {false, true}) {
    NV_ENC_CONFIG c = preset; nvencStreamingConfig(c, 1920, 1080, 60, 30000000, {recovers, true}, wide);
    const auto& h = c.encodeCodecConfig.h264Config;
    require(c.gopLength == NVENC_INFINITE_GOPLENGTH && h.idrPeriod == NVENC_INFINITE_GOPLENGTH, "A periodic IDR remains: key frames must come only on request");
    require(c.frameIntervalP == 1 && c.rcParams.zeroReorderDelay == 1 && !c.rcParams.enableLookahead && !c.rcParams.lookaheadDepth, "B-frames, reordering or lookahead delay a frame");
    require(c.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CBR && c.rcParams.averageBitRate == 30000000 && c.rcParams.maxBitRate == 30000000, "Not CBR at the target");
    require(c.rcParams.vbvBufferSize == 500000 && c.rcParams.vbvInitialDelay == 500000, "The VBV is not one frame (30 Mbps / 60 fps)");
    require(!c.rcParams.enableAQ && !c.rcParams.enableTemporalAQ, "AQ left on");
    require(IsEqualGUID(c.profileGUID, NV_ENC_H264_PROFILE_BASELINE_GUID) && h.entropyCodingMode == NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC && h.repeatSPSPPS == 1, "Not the Baseline/CAVLC stream with headers before every IDR the client decodes");
    // The preset's DPB until a client sent LOSS: other decoders keep the stream they had.
    require(recovers && wide ? h.maxNumRefFrames == 16 && h.numRefL0 == NV_ENC_NUM_REF_FRAMES_1 : h.maxNumRefFrames == 2, "The DPB for reference invalidation is wrong, set without the capability, or set before any LOSS");
    require(nvencRecovers({recovers, true}) == recovers && !nvencRecovers({true, false}), "Recovery offered without both capabilities");
  }
  NV_ENC_CONFIG k4 = preset; nvencStreamingConfig(k4, 3840, 2160, 60, 80000000, {true, true}, true);
  require(k4.encodeCodecConfig.h264Config.maxNumRefFrames == 5, "A 4K DPB beyond what level 5.2 allows");
  NV_ENC_CONFIG low = preset; nvencRate(low, 2000000, 30); require(low.rcParams.vbvBufferSize == 66666, "Live bitrate changes lost the one-frame VBV");
}
// Which frames to invalidate, and when only an IDR helps.
void referenceHistory() {
  host::ReferenceHistory h(4);
  require(!h.after(10), "An unknown frame was recoverable");
  h.encoded(10, true); h.encoded(20, false); h.encoded(30, false);
  require(h.after(30) == std::vector<uint64_t>{} && h.after(10) == std::vector<uint64_t>({20, 30}), "Wrong frames after the last good one");
  h.encoded(40, false); h.encoded(50, false);
  require(!h.after(10) && h.after(20) == std::vector<uint64_t>({30, 40, 50}), "A frame that slid out of the DPB was recoverable");
  h.encoded(60, true);
  require(!h.after(50) && h.after(60) == std::vector<uint64_t>{}, "A frame before the last IDR was recoverable (the client lost that IDR)");
  require(host::recoveryReferenceFrames(1920, 1080) == 16 && host::recoveryReferenceFrames(2560, 1440) == 12 && host::recoveryReferenceFrames(3840, 2160) == 5 && host::recoveryReferenceFrames(7680, 4320) == 1, "DPB sizes outside the level limit");
}
int main() {
  try {
    using namespace host::gpu;
    streamingConfig(); referenceHistory();
    FakeOutput busyThenReady{0, {89, 89, 17}};
    const auto ready = busyThenReady.run();
    require(ready.ready && !ready.expired && ready.nativeStatus == 17 && ready.attempts == 3, "LOCK_BUSY must retry the same output until success");
    require(busyThenReady.paused == 2 && busyThenReady.now == 2, "Only busy results consume retry pauses");
    FakeOutput eventUsedBudget{249, {89}};
    const auto expired = eventUsedBudget.run();
    require(!expired.ready && expired.expired && expired.nativeStatus == 89, "Busy exhaustion preserves the actual busy status");
    require(eventUsedBudget.calls == 1 && eventUsedBudget.paused == 1 && eventUsedBudget.now == 250, "Event wait and retry share one 250ms budget");
    FakeOutput atDeadline{250, {17}};
    const auto noAttempt = atDeadline.run();
    require(noAttempt.expired && !noAttempt.ready && noAttempt.attempts == 0 && atDeadline.calls == 0, "No API lock attempt starts after the deadline");
    FakeOutput fatal{10, {128, 17}};
    const auto failed = fatal.run();
    require(!failed.ready && !failed.expired && failed.nativeStatus == 128 && fatal.calls == 1 && fatal.paused == 0, "Fatal API status is neither retried nor converted to success");
    FakeOutput nearlyReady{249, {17}};
    require(nearlyReady.run().ready && nearlyReady.paused == 0, "Actual completed output at the last millisecond is accepted");
    FakeOutput backwards{9, {17}};
    require(backwards.run({10}).expired && backwards.calls == 0, "Backwards clock fails closed");
    FakeOutput frozen{0, {89}}; frozen.frozen = true;
    const auto finite = frozen.run();
    require(finite.expired && frozen.calls == 250 && frozen.paused == 250, "Frozen clock still has a finite attempt and pause budget");
    require(nvencFlushAllowsRelease(false, false, false, false), "Unsubmitted partial construction can release its resources");
    require(nvencFlushAllowsRelease(true, true, true, true), "Accepted and completed async flush permits SDK free-before-destroy");
    require(!nvencFlushAllowsRelease(true, true, false, false), "Rejected EOS cannot justify resource release");
    require(!nvencFlushAllowsRelease(true, true, true, false), "Accepted EOS without completion must retain resources");
    require(!nvencFlushAllowsRelease(false, true, true, false), "An incomplete EOS itself prevents release even with no prior picture");
    require(!nvencFlushAllowsRelease(true, false, false, false), "A pending picture without a flush event must retain resources");
    NvencProcessAdmission normal;
    require(normal.acquire() && normal.acquire() && normal.live == 2, "Normal dual NVENC states remain allowed");
    require(normal.release() && normal.release() && normal.live == 0 && !normal.fenced, "Successful teardown permits subsequent sessions");
    require(normal.acquire() && normal.release(), "Clean session teardown does not permanently disable NVENC");
    NvencProcessAdmission quarantine;
    require(quarantine.acquire() && quarantine.acquire(), "Two states can already be live before the first failure");
    require(quarantine.retain() && quarantine.fenced && quarantine.retained == 1 && quarantine.retentionLimit == 2, "First failure fixes the retention budget to existing live admission");
    require(!quarantine.acquire(), "No new NVENC state can accumulate after quarantine");
    require(quarantine.retain() && quarantine.live == 0 && quarantine.retained == 2, "The other already-admitted dual state can also be safely retained");
    require(!quarantine.retain() && !quarantine.release() && !quarantine.acquire(), "Duplicate retirement cannot grow process retention");
    std::vector<int> order;
    const auto freeResources = [&]() -> NvencNativeResult { order.push_back(2); return {true, 17}; };
    const auto destroy = [&]() -> NvencNativeResult { order.push_back(3); return {true, 17}; };
    const auto completedDrain = [&]() -> NvencDrainResult { order.push_back(1); return {true, true, true, 17}; };
    const auto cleanClose = nvencCloseInSdkOrder(true, completedDrain, freeResources, destroy);
    require(cleanClose.released && order == std::vector<int>({1, 2, 3}), "Production teardown helper enforces completed EOS then free-before-destroy");
    order.clear();
    const auto timeoutClose = nvencCloseInSdkOrder(true,
      [&]() -> NvencDrainResult { order.push_back(1); return {true, true, false, 258}; }, freeResources, destroy);
    require(!timeoutClose.released && timeoutClose.nativeStatus == 258 && order == std::vector<int>({1}), "Flush timeout never invokes resource free or DestroyEncoder");
    order.clear();
    const auto rejectedClose = nvencCloseInSdkOrder(true,
      [&]() -> NvencDrainResult { order.push_back(1); return {true, false, false, 128}; }, freeResources, destroy);
    require(!rejectedClose.released && rejectedClose.nativeStatus == 128 && order == std::vector<int>({1}), "Failed EOS preserves its status without pretending destruction is drain");
    order.clear();
    const auto failedFree = nvencCloseInSdkOrder(true, completedDrain,
      [&]() -> NvencNativeResult { order.push_back(2); return {false, 129}; }, destroy);
    require(!failedFree.released && failedFree.nativeStatus == 129 && order == std::vector<int>({1, 2}), "Failed buffer release retains the remaining graph without destroying the session");
    order.clear();
    const auto failedDestroy = nvencCloseInSdkOrder(true, completedDrain, freeResources,
      [&]() -> NvencNativeResult { order.push_back(3); return {false, 130}; });
    require(!failedDestroy.released && failedDestroy.nativeStatus == 130 && order == std::vector<int>({1, 2, 3}), "Failed DestroyEncoder must retain its DLL, D3D and event ownership");
    require(metadataAfterSuccessfulApi(&nvencOutputMetadataFailure, 2, true, 100, 1000, 55, 55, 3) == NvencMetadataFailure::none,
      "Successful API output with raw hwEncodeStatus2 must reach real metadata validation");
    FakeOutput actualApiError{0, {2}};
    const auto apiError = actualApiError.run();
    require(!apiError.ready && !apiError.expired && apiError.nativeStatus == 2 && actualApiError.calls == 1,
      "An actual failing API return2 remains a fatal lock result");
    for (const uint32_t type : {0U, 2U, 3U, 7U})
      require(metadataAfterSuccessfulApi(&nvencOutputMetadataFailure, 0, true, 100, 1000, 55, 55, type) == NvencMetadataFailure::none, "SDK forward P/I/IDR/NONREF_P types are valid exact-PTS metadata");
    for (const uint32_t type : {1U, 4U, 5U, 6U, 255U})
      require(metadataAfterSuccessfulApi(&nvencOutputMetadataFailure, 0, true, 100, 1000, 55, 55, type) == NvencMetadataFailure::pictureType, "B/BI/skipped/intra-refresh/unknown remain outside the zero-reorder contract");
    require(metadataAfterSuccessfulApi(&nvencOutputMetadataFailure, 128, true, 100, 1000, 55, 55, 7) == NvencMetadataFailure::none, "Separate opaque metadata is not reinterpreted as an API return code");
    require(metadataAfterSuccessfulApi(&nvencOutputMetadataFailure, 2, false, 100, 1000, 55, 55, 7) == NvencMetadataFailure::buffer, "Raw status2 never bypasses the real output-buffer check");
    require(metadataAfterSuccessfulApi(&nvencOutputMetadataFailure, 2, true, 0, 1000, 55, 55, 7) == NvencMetadataFailure::buffer, "Raw status2 never bypasses empty-output rejection");
    require(metadataAfterSuccessfulApi(&nvencOutputMetadataFailure, 2, true, 1001, 1000, 55, 55, 7) == NvencMetadataFailure::buffer, "Raw status2 never bypasses oversize rejection");
    require(metadataAfterSuccessfulApi(&nvencOutputMetadataFailure, 2, true, 100, 1000, 56, 55, 7) == NvencMetadataFailure::timestamp, "Raw status2 never bypasses exact input-PTS association");
    require(metadataAfterSuccessfulApi(&nvencOutputMetadataFailure, 2, true, 100, 1000, 55, 55, 1) == NvencMetadataFailure::pictureType, "Raw status2 never permits a B frame");
    std::cout << "{\"kind\":\"nvenc-output-policy\",\"checks\":" << checks << ",\"passed\":true,\"actualGpu\":false,\"nativeCalls\":false}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
