#pragma once
#include <cstdint>
#include "gpu_h264_nvenc_observation.hpp"

namespace host {
enum class EncoderGpuFailureSite : uint32_t {
  none = 0,
  candidateOpen = 1, candidateIdentity = 2, outputIdentity = 3,
  outputTiming = 4, outputAnnexB = 5, outputHeaders = 6,
  outputDelayed = 7, candidateReconfigure = 8, encoderInitialize = 9,
  encoderSubmit = 10, encoderReconfigure = 11,
  // gpu-capture-v1 texture input (gpu_capture.hpp): register, map, unmap.
  textureRegister = 12, textureMap = 13, textureUnmap = 14,
  nvencInputTimestamp = 100, nvencInputPitch = 101, nvencNeedMoreInput = 102,
  nvencOutputLock = 103, nvencOutputMetadata = 104, nvencOutputAnnexB = 105,
  nvencOutputUnlock = 106, nvencOutputWait = 107, nvencEndOfStream = 108,
  nvencInitialize = 109, nvencSubmit = 110, nvencReconfigure = 111,
  nvencOutputHardwareStatus = 112, nvencOutputBuffer = 113,
  nvencOutputTimestamp = 114, nvencOutputPictureType = 115,
  nvencOutputBaseline = 116, nvencOutputHeaders = 117,
  nvencOutputForcedIdr = 118,
};
inline constexpr bool validEncoderGpuFailureSite(uint32_t site) {
  // Fixed provider ranges; unused slots stay reserved for source-defined
  // substeps. Values outside these families never become private wire data.
  return site == 0 || (site >= 1 && site <= 15) ||
    (site >= 100 && site <= 119) || (site >= 200 && site <= 219) ||
    (site >= 300 && site <= 319);
}
struct EncoderGpuFailure {
  uint32_t site = 0, nativeStatus = 0, vendorId = 0, deviceId = 0;
  // Private observation only, never part of the released four-field failure
  // wire shape. The mutable candidate state outlives its existing close; only
  // the immutable scalar snapshot is retained after CPU fallback reset.
  gpu::NvencObservationSnapshot nvencObservation;
  std::shared_ptr<gpu::NvencObservationState> nvencPending;
  bool operator==(const EncoderGpuFailure&) const = default;
};
inline void bindEncoderGpuObservationPhase(EncoderGpuFailure& value, bool established) noexcept {
  if (value.nvencPending) value.nvencPending->phase(established);
}
inline void freezeEncoderGpuObservation(EncoderGpuFailure& value) noexcept {
  if (!value.nvencPending) return;
  value.nvencObservation = value.nvencPending->snapshot();
  value.nvencPending.reset();
}
inline constexpr bool encoderGpuFailureKnownVendor(uint32_t vendor) {
  return vendor == 0x10de || vendor == 0x1002 || vendor == 0x8086;
}
inline constexpr bool encoderGpuFailureDetailKnown(const EncoderGpuFailure& value) {
  if (!validEncoderGpuFailureSite(value.site) || !encoderGpuFailureKnownVendor(value.vendorId)) return false;
  if (value.site >= 100 && value.site <= 119) return value.vendorId == 0x10de;
  if (value.site >= 200 && value.site <= 219) return value.vendorId == 0x1002;
  if (value.site >= 300 && value.site <= 319) return value.vendorId == 0x8086;
  return true;  // Common sites (including unknown0) still need a known candidate vendor.
}
}
