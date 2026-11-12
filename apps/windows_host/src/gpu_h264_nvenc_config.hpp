#pragma once
// The NVENC streaming configuration on top of the driver's preset, separate from the
// native calls so it can be checked without a GPU (gpu_h264_nvenc_policy_test.cpp).
#include "reference_history.hpp"
#include <nvEncodeAPI.h>
#include <algorithm>
#include <cstdint>

namespace host::gpu {
// NV_ENC_CAPS answers the configuration depends on.
struct NvencCaps { bool invalidation = false, multipleReferences = false; };

// CBR at the target with a one-frame VBV (a frame never waits for the buffer to drain).
inline void nvencRate(NV_ENC_CONFIG& config, int bitrate, int fps) {
  const uint32_t target = uint32_t(std::max(bitrate, 1));
  config.rcParams.averageBitRate = target; config.rcParams.maxBitRate = target;
  config.rcParams.vbvBufferSize = std::max<uint32_t>(target / uint32_t(std::max(fps, 1)), 1); config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
}

// `config` holds the P1 ultra-low-latency preset the driver returned. `wide`: older
// references for LOSS recovery, only once the client has sent a LOSS (media.hpp).
inline void nvencStreamingConfig(NV_ENC_CONFIG& config, int width, int height, int fps, int bitrate, NvencCaps caps, bool wide = false) {
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
  // udp-media-v1 LOSS: older reference frames in the DPB for nvEncInvalidateRefFrames to fall
  // back on, while motion search keeps using one (NVIDIA's own advice for this API).
  if (wide && caps.invalidation && caps.multipleReferences) { h264.maxNumRefFrames = uint32_t(recoveryReferenceFrames(width, height)); h264.numRefL0 = NV_ENC_NUM_REF_FRAMES_1; }
  auto& vui = h264.h264VUIParameters;
  vui.videoSignalTypePresentFlag = 1; vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED; vui.videoFullRangeFlag = 0;
  vui.colourDescriptionPresentFlag = 1; vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_UNSPECIFIED;
  vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_UNSPECIFIED; vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_SMPTE170M;
}
// Reference recovery is on only where the configuration above kept older frames.
inline bool nvencRecovers(NvencCaps caps) { return caps.invalidation && caps.multipleReferences; }
}
