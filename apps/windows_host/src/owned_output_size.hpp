#pragma once
#include <cstdint>
#include <cstddef>
#include <limits>
#include <optional>
#include "../../../native/windows_video/frame_layout.hpp"

// Owned geometry and checked in-process frame layout. These checks do not
// claim support by the installed driver, CCD, capture device or encoder.
namespace host {
// Representability bounds only. Actual SetDisplayConfig/capture/encoder calls
// decide support; a representable request is not a display capability claim.
// BGRA scanline and encoder strides use signed 32-bit byte counts.
inline constexpr uint32_t ownedOutputMaxSide = uint32_t(std::numeric_limits<int32_t>::max() / 4);
// Desktop coordinates are LONG endpoints, while CCD source sizes are DWORD.
// Subtract only after validating coordinates so neither addition nor narrowing
// can overflow. A representable rectangle is not device/mode admission.
inline constexpr bool displayRectangleFits(int64_t left,int64_t top,uint64_t width,uint64_t height){
  return width&&height&&left>=INT32_MIN&&left<=INT32_MAX&&top>=INT32_MIN&&top<=INT32_MAX&&
    width<=uint64_t(int64_t(INT32_MAX)-left)&&height<=uint64_t(int64_t(INT32_MAX)-top);
}
struct FrameLayout { std::size_t bgraBytes=0,nv12Bytes=0; uint32_t bgraStride=0; };
inline constexpr std::optional<FrameLayout> checkedFrameLayout(uint64_t width,uint64_t height,bool even=true){
  if(width>UINT32_MAX||height>UINT32_MAX)return {};
  multitap::video::FrameLayout layout;
  if(!multitap::video::frameLayout(uint32_t(width),uint32_t(height),layout,even))return {};
  return FrameLayout{layout.rgbaBytes,layout.nv12Bytes,layout.stride};
}
inline constexpr bool frameMemoryFits(const FrameLayout& layout,uint64_t available,uint64_t bgraCopies,uint64_t nv12Copies){
  const auto max=std::numeric_limits<uint64_t>::max();
  if(!layout.bgraBytes||(!layout.nv12Bytes&&nv12Copies)||(bgraCopies&&layout.bgraBytes>max/bgraCopies)||(nv12Copies&&layout.nv12Bytes>max/nv12Copies))return false;
  const auto bgra=uint64_t(layout.bgraBytes)*bgraCopies,nv12=uint64_t(layout.nv12Bytes)*nv12Copies;
  return nv12<=max-bgra&&bgra+nv12<=available;
}
inline constexpr bool ownedOutputSize(uint64_t width, uint64_t height) {
  return width >= 320 && height >= 180 && checkedFrameLayout(width,height).has_value();
}
// Admission compatibility only. Never use this to read a recovery journal,
// inspect actual foreign geometry or restore an already-saved larger mode.
inline constexpr bool legacyStreamOutputSize(uint64_t width,uint64_t height){
  return ownedOutputSize(width,height)&&width<=3840&&height<=3840&&width*height<=8294400;
}
// An encoded picture: even sides of at least 16, same buffer ABI checks.
inline constexpr bool encodedPictureSize(uint64_t width, uint64_t height) {
  return width >= 16 && height >= 16 && checkedFrameLayout(width,height).has_value();
}
}
