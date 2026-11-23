#pragma once
// The NVENC streaming configuration on top of the driver's preset, separate from the
// native calls so it can be checked without a GPU (gpu_h264_nvenc_policy_test.cpp).
#include "reference_history.hpp"
#include <nvEncodeAPI.h>
#include <algorithm>
#include <cstdint>
#include <optional>

namespace host::gpu {
// NV_ENC_CAPS answers the configuration depends on.
struct NvencCaps { int longTermFrames = 0; };  // NV_ENC_CAPS_NUM_MAX_LTR_FRAMES

// CBR at the target with a one-frame VBV (a frame never waits for the buffer to drain).
inline void nvencRate(NV_ENC_CONFIG& config, int bitrate, int fps) {
  const uint32_t target = uint32_t(std::max(bitrate, 1));
  config.rcParams.averageBitRate = target; config.rcParams.maxBitRate = target;
  config.rcParams.vbvBufferSize = std::max<uint32_t>(target / uint32_t(std::max(fps, 1)), 1); config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
}

// The long-term reference slots a LOSS is answered from (reference_history.hpp): `levelIdc`
// is the level in the SPS of the stream the encoder is coding.
inline int nvencLongTermSlots(NvencCaps caps, int levelIdc, int width, int height) { return longTermSlots(levelIdc, width, height, caps.longTermFrames); }

// `config` holds the P1 ultra-low-latency preset the driver returned. `longTermSlots`: long-term
// references for LOSS recovery, only once the client has sent a LOSS (media.hpp).
inline void nvencStreamingConfig(NV_ENC_CONFIG& config, int fps, int bitrate, int longTermSlots = 0) {
  config.version = NV_ENC_CONFIG_VER; config.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
  // No periodic IDR: a key frame comes only when asked for (the first frame, dropped video,
  // a LOSS without reference recovery). The one-minute GOP sent an IDR squeezed into the
  // one-frame VBV every minute, and P-frames after it only follow from a one-frame budget.
  // NVENC requires frameIntervalP 1 with an infinite GOP: no B-frames.
  config.gopLength = NVENC_INFINITE_GOPLENGTH; config.frameIntervalP = 1;
  config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
  auto& rate = config.rcParams; rate.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  rate.enableLookahead = 0; rate.lookaheadDepth = 0; rate.zeroReorderDelay = 1; rate.enableAQ = 0; rate.enableTemporalAQ = 0;
  nvencRate(config, bitrate, fps);
  auto& h264 = config.encodeCodecConfig.h264Config;
  h264.idrPeriod = NVENC_INFINITE_GOPLENGTH; h264.repeatSPSPPS = 1; h264.disableSPSPPS = 0;
  h264.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC; h264.level = NV_ENC_LEVEL_AUTOSELECT;
  h264.chromaFormatIDC = 1;
  // udp-media-v1 LOSS: long-term reference frames marked picture by picture (the mode NVIDIA
  // prefers), beside the one short-term frame every P-frame is coded from. This works on
  // every NVENC generation; older reference frames to invalidate back to need a GPU with
  // several reference frames and keep 16 at most, which above 60 fps a LOSS outruns.
  // maxNumRefFrames stays the preset's: the encoder sizes the DPB for the slots itself (the
  // SPS then says slots + 1), and a Tesla P40 refuses a reconfiguration that names a size.
  if (longTermSlots > 0) { h264.enableLTR = 1; h264.ltrNumFrames = uint32_t(longTermSlots); h264.ltrTrustMode = 0; }
  auto& vui = h264.h264VUIParameters;
  vui.videoSignalTypePresentFlag = 1; vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED; vui.videoFullRangeFlag = 0;
  vui.colourDescriptionPresentFlag = 1; vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_UNSPECIFIED;
  vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_UNSPECIFIED; vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_SMPTE170M;
}
// What the encoder reports for a coded picture, against what was asked of it: the slot it
// was to be marked into and the slot a recovery frame was to be coded from. A recovery frame
// coded from anything else would show a picture the client cannot rebuild; an IDR needs none.
struct NvencLongTermOutput { bool marked = false; uint32_t markedSlot = 0, usedBitmap = 0; };
inline bool nvencRecoveryHonored(std::optional<uint32_t> asked, bool idr, const NvencLongTermOutput& output) {
  return !asked || idr || output.usedBitmap == uint32_t(1) << *asked;
}
inline std::optional<uint32_t> nvencMarkedSlot(std::optional<uint32_t> asked, const NvencLongTermOutput& output) {
  return asked && output.marked && output.markedSlot == *asked ? asked : std::nullopt;
}
}
