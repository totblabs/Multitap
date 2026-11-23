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
  for (const int slots : {0, 3}) {
    NV_ENC_CONFIG c = preset; nvencStreamingConfig(c, 60, 30000000, slots);
    const auto& h = c.encodeCodecConfig.h264Config;
    require(c.gopLength == NVENC_INFINITE_GOPLENGTH && h.idrPeriod == NVENC_INFINITE_GOPLENGTH, "A periodic IDR remains: key frames must come only on request");
    require(c.frameIntervalP == 1 && c.rcParams.zeroReorderDelay == 1 && !c.rcParams.enableLookahead && !c.rcParams.lookaheadDepth, "B-frames, reordering or lookahead delay a frame");
    require(c.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CBR && c.rcParams.averageBitRate == 30000000 && c.rcParams.maxBitRate == 30000000, "Not CBR at the target");
    require(c.rcParams.vbvBufferSize == 500000 && c.rcParams.vbvInitialDelay == 500000, "The VBV is not one frame (30 Mbps / 60 fps)");
    require(!c.rcParams.enableAQ && !c.rcParams.enableTemporalAQ, "AQ left on");
    require(IsEqualGUID(c.profileGUID, NV_ENC_H264_PROFILE_BASELINE_GUID) && h.entropyCodingMode == NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC && h.repeatSPSPPS == 1, "Not the Baseline/CAVLC stream with headers before every IDR the client decodes");
    // The preset's DPB until a client sent LOSS: other decoders keep the stream they had.
    require(slots ? h.enableLTR && h.ltrNumFrames == 3 && !h.ltrTrustMode : !h.enableLTR && !h.ltrNumFrames, "Long-term references are wrong, or set before any LOSS");
    require(h.maxNumRefFrames == 2, "The DPB size was named: the encoder sizes it for the slots itself");
  }
  // The slots follow the level of the stream the encoder codes (measured on a Tesla P40: where a fourth slot
  // would need a higher level than the SPS has, every slot is marked and no answer is coded from one).
  require(host::levelReferenceFrames(42, 1920, 1080) == 4 && host::levelReferenceFrames(40, 1920, 1080) == 4 && host::levelReferenceFrames(42, 1680, 1050) == 5 && host::levelReferenceFrames(51, 1920, 1080) == 16
    && host::levelReferenceFrames(31, 1280, 720) == 5 && host::levelReferenceFrames(51, 2560, 1440) == 12 && host::levelReferenceFrames(52, 3840, 2160) == 5 && host::levelReferenceFrames(51, 5120, 2880) == 3
    && host::levelReferenceFrames(0, 1280, 720) == 0 && host::levelReferenceFrames(43, 1280, 720) == 0, "The DPB of a level is not the standard's");
  require(nvencLongTermSlots({8}, 42, 1920, 1080) == 3 && nvencLongTermSlots({8}, 40, 1920, 1080) == 3 && nvencLongTermSlots({8}, 42, 1680, 1050) == 4 && nvencLongTermSlots({8}, 51, 1920, 1080) == 4
    && nvencLongTermSlots({8}, 42, 1280, 720) == 4 && nvencLongTermSlots({8}, 52, 3840, 2160) == 4 && nvencLongTermSlots({8}, 51, 5120, 2880) == 2 && nvencLongTermSlots({8}, 52, 7680, 4320) == 0
    && nvencLongTermSlots({2}, 51, 1920, 1080) == 2 && nvencLongTermSlots({0}, 51, 1920, 1080) == 0 && nvencLongTermSlots({8}, 0, 1920, 1080) == 0, "Slots beyond the stream's level, the GPU's own limit, or with no level known");
  // What the encoder reports decides: a recovery frame coded from anything but its slot is refused, an IDR never.
  require(nvencRecoveryHonored({}, false, {}) && nvencRecoveryHonored(2, false, {false, 0, 4}) && nvencRecoveryHonored(2, true, {}), "An honoured recovery frame was refused");
  require(!nvencRecoveryHonored(2, false, {}) && !nvencRecoveryHonored(2, false, {false, 0, 2}) && !nvencRecoveryHonored(0, false, {false, 0, 3}), "A recovery frame coded from another reference passed");
  require(nvencMarkedSlot(1, {true, 1, 0}) == 1u && !nvencMarkedSlot(1, {false, 1, 0}) && !nvencMarkedSlot(1, {true, 0, 0}) && !nvencMarkedSlot({}, {true, 0, 0}), "A mark the encoder did not make was counted");
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
// Which kept frame answers a LOSS, and when only an IDR helps.
void longTermReferences() {
  using host::LongTermReferences;
  const uint64_t frame = 1000000 / 144;
  // The encoder as the bookkeeping drives it: marks what is due, every mark honoured.
  const auto code = [](LongTermReferences& l, uint64_t pts, bool idr = false) { const auto mark = l.due(pts, idr); l.encoded(pts, idr, mark); return mark; };
  LongTermReferences l(4);
  require(l.intervalUs() == 125000 && !l.recover(0), "A slot answered before any frame");
  require(code(l, 1000, true) == 0u && !l.recover(1000), "The IDR did not take the first slot, or one slot answered while the others were empty");
  require(code(l, 1000 + frame) == 1u && code(l, 1000 + 2 * frame) == 2u && !l.recover(1000 + 2 * frame), "The slots were not filled by the frames after the IDR");
  require(code(l, 1000 + 3 * frame) == 3u && !code(l, 1000 + 4 * frame), "The last slot was not filled, or a mark came before its interval");
  // A LOSS that names a frame from before the last slot was filled: the client's DPB may lack a slot.
  require(!l.recover(1000 + 2 * frame) && l.recover(1000 + 3 * frame) == 3u && l.recover(1000 + 50 * frame) == 3u, "The fill was not required, or the newest slot the client holds was not chosen");
  // One interval after the last mark the oldest slot is marked again.
  uint64_t pts = 1000 + 4 * frame; std::vector<std::pair<uint64_t, uint32_t>> marks;
  for (; pts < 1000 + 3 * frame + 4 * 125000 + 5 * frame; pts += frame) if (const auto mark = code(l, pts)) marks.push_back({pts, *mark});
  require(marks.size() == 4 && marks[0].second == 0 && marks[1].second == 1 && marks[2].second == 2 && marks[3].second == 3 && marks[0].first >= 1000 + 3 * frame + 125000 && marks[0].first < 1000 + 3 * frame + 125000 + frame
    && marks[1].first - marks[0].first >= 125000 && marks[1].first - marks[0].first < 125000 + frame, "The oldest slot was not marked again once per interval");
  // A mark the client never got: the slot is not used, and the next frame fills it again.
  LongTermReferences lost = l;
  require(lost.recover(marks[3].first - 1) == 2u && lost.due(pts, false) == 3u, "A slot marked after the last good frame answered, or was not marked again at once");
  require(lost.recover(pts) == 2u, "A slot the client never got came back without a new mark");
  require(code(lost, pts) == 3u && lost.recover(pts) == 3u && !lost.due(pts + frame, false), "A slot marked again did not answer");
  // Two marks lost, then the answer from the newest of the rest; both are filled by the next two frames.
  lost = l; require(lost.recover(marks[2].first - 1) == 1u && code(lost, pts) == 2u && code(lost, pts + frame) == 3u && !code(lost, pts + 2 * frame), "Two lost marks were not both made again");
  // Older than every slot: only an IDR.
  lost = l; require(!lost.recover(marks[0].first - 1) && lost.due(pts, false) == 0u, "A LOSS older than every slot was answered");
  // An IDR empties the slots; a LOSS from before it (the client lost that IDR) gets another.
  require(code(l, pts, true) == 0u && !l.recover(pts - frame) && !l.recover(pts), "A frame before the IDR, or an unfilled DPB, answered");
  // The reach, at 60, 144 and 240 fps: a LOSS up to 375 ms late always finds a slot the client holds, and once
  // every slot has been marked again that slot is at most one interval older than the last good frame.
  for (const uint64_t step : {uint64_t(1000000 / 60), uint64_t(1000000 / 144), uint64_t(1000000 / 240)}) {
    LongTermReferences run(4); std::vector<uint64_t> when(4, 0); uint64_t at = 0, filled = 0; code(run, at, true);
    for (int n = 1; n < 2000; ++n) {
      at += step; if (const auto mark = code(run, at)) { when[*mark] = at; if (n == 3) filled = at; }
      if (n < 3) continue;
      for (const uint64_t late : {uint64_t(0), step, uint64_t(100000), uint64_t(200000), uint64_t(375000)}) {
        if (at < filled + late) continue;
        const uint64_t lastGood = at - late; LongTermReferences copy = run; const auto slot = copy.recover(lastGood);
        require(slot.has_value() && when[*slot] <= lastGood, "A LOSS within the reach found no slot, or one newer than the last good frame");
        require(at < filled + 4 * 125000 + step || lastGood - when[*slot] <= 125000 + step, "The slot is more than an interval older than the last good frame");
      }
    }
  }
  // Fewer slots: the GPU's limit, a refusal, a huge picture.
  LongTermReferences two(2); require(two.intervalUs() == 375000 && code(two, 0, true) == 0u && code(two, frame) == 1u && two.recover(frame) == 1u, "Two slots do not cover the same reach");
  LongTermReferences one(1); require(one.intervalUs() == 375000 && code(one, 0, true) == 0u && one.recover(frame) == 0u && !code(one, frame), "One slot does not answer from its IDR");
  LongTermReferences refused(4); refused.encoded(0, true, refused.due(0, true)); refused.encoded(frame, false, refused.due(frame, false));
  require(refused.due(2 * frame, false) == 2u && !refused.recover(frame), "A slot was skipped");
  refused.refused(2); refused.encoded(2 * frame, false, {});
  require(refused.slots() == 2 && refused.recover(2 * frame) == 1u && !refused.due(3 * frame, false), "A refused slot was asked for again, or the slots below it stopped answering");
  LongTermReferences none(4); none.refused(0); none.encoded(0, true, {}); require(none.slots() == 0 && !none.due(frame, false) && !none.due(frame, true) && !none.recover(0), "An encoder that marks nothing was asked to recover");
  LongTermReferences empty(0); require(!empty.due(0, true) && !empty.recover(0), "No slots, yet one answered");
}
int main() {
  try {
    using namespace host::gpu;
    streamingConfig(); referenceHistory(); longTermReferences();
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
