#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
namespace multitap::video {
struct FrameLayout {size_t rgbaBytes=0;uint32_t stride=0,nv12Bytes=0;};
// Signed MF stride, unsigned MF buffer length, size_t/vector addressing. No
// product-resolution limit: exact decoder/device admission is independent.
inline constexpr bool frameLayout(uint32_t width,uint32_t height,FrameLayout& out,bool even=true){
  if(!width||!height||(even&&((width&1)||(height&1)))||width>uint32_t(std::numeric_limits<int32_t>::max())/4||height>uint32_t(std::numeric_limits<int32_t>::max()))return false;
  const uint64_t pixels=uint64_t(width)*height;
  if(pixels>uint64_t(std::numeric_limits<std::ptrdiff_t>::max())/4||pixels>uint64_t(std::numeric_limits<size_t>::max())/4||
      (even&&pixels>uint64_t(std::numeric_limits<uint32_t>::max())*2/3))return false;
  const auto nv12=pixels+pixels/2;
  out={size_t(pixels*4),width*4,!(width&1)&&!(height&1)&&nv12<=std::numeric_limits<uint32_t>::max()?uint32_t(nv12):0};return true;
}
inline constexpr bool checkedAdd(uint64_t a,uint64_t b,uint64_t& out){if(a>std::numeric_limits<uint64_t>::max()-b)return false;out=a+b;return true;}
inline constexpr bool checkedMultiply(uint64_t a,uint64_t b,uint64_t& out){if(b&&a>std::numeric_limits<uint64_t>::max()/b)return false;out=a*b;return true;}
}
