#pragma once
#include "frame_layout.hpp"
#include "rgba_upscale.hpp"
#include <vector>
namespace multitap::video {
// Initial tracks-v2 encoded picture -> unchanged source workspace. No epochs,
// decode claims, display changes or rescale renegotiation are introduced here.
inline bool sourcePicture(uint32_t width,uint32_t height,uint32_t sourceWidth,uint32_t sourceHeight,
    std::vector<uint8_t>& rgba,uint64_t available){
  FrameLayout encoded{},source{};
  if(!frameLayout(width,height,encoded)||!frameLayout(sourceWidth,sourceHeight,source,false)||
      sourceWidth<width||sourceHeight<height||rgba.size()!=encoded.rgbaBytes)return false;
  if(width==sourceWidth&&height==sourceHeight)return true;
  if(!available||source.rgbaBytes>available)return false;
  BilinearUpscaler scale;if(!scale.Configure(width,height,sourceWidth,sourceHeight))return false;
  std::vector<uint8_t> frame(source.rgbaBytes);
  scale.Resample(rgba.data(),encoded.stride,frame.data(),source.stride,0,0,sourceWidth,sourceHeight);
  rgba.swap(frame);return true;
}
}
