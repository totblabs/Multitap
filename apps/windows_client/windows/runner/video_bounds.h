#ifndef MULTITAP_VIDEO_BOUNDS_H_
#define MULTITAP_VIDEO_BOUNDS_H_
#include <cstdint>
#include <cstddef>
#include <limits>
#include "../../../../native/windows_video/frame_layout.hpp"
namespace multitap {
// ABI/arithmetic bounds only. These are NOT a supported-resolution rectangle.
// Exact decoder, texture feature-level and available-memory admission is done
// for the requested shape(s), including simultaneous independent outputs.
inline constexpr uint32_t kMaximumVideoWidth = (uint32_t(std::numeric_limits<int32_t>::max()) / 4) & ~1u;
inline constexpr uint32_t kMaximumVideoHeight = uint32_t(std::numeric_limits<int32_t>::max()) & ~1u;
inline constexpr uint64_t kMaximumVideoPixels = uint64_t(std::numeric_limits<uint32_t>::max()) * 2 / 3;
inline constexpr uint32_t kMaximumNv12Buffer = std::numeric_limits<uint32_t>::max();
struct VideoLayout { size_t rgbaBytes=0;uint32_t rgbaStride=0,nv12Bytes=0; };
inline constexpr bool CheckedVideoLayout(uint32_t width,uint32_t height,VideoLayout& out) {
  video::FrameLayout value{};if(!video::frameLayout(width,height,value))return false;
  out={value.rgbaBytes,value.stride,value.nv12Bytes};return true;
}
inline constexpr bool ValidVideoSize(uint32_t width, uint32_t height) {
  VideoLayout layout{};return CheckedVideoLayout(width,height,layout);
}
}
#endif
