#pragma once
#include "support.hpp"
#include "owned_output_size.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

// The encoded video size of a viewport stream (viewport-encoder-limits
// separate-v1). Fitting the captured source into the client's
// quality box (1920x1080 by default) would encode any window wider than 1920
// smaller, to be upscaled by the client: 2960x760 (2.25 MP, about one
// 1080p frame) becomes 1920x492 and looks blurry. The cost
// of a frame is its pixel count, not its width: the source is encoded at
// its own size unless it exceeds the box (a codec safety ceiling) or the
// Host's pixel budget, and only then scaled once with its aspect kept.
namespace host {
// The conservative released startup work budget is retained. An explicit
// administrator videoPixelBudget may use the buffer ABI range; it is not a
// universal display/codec capability and does not impose a workspace ceiling.
inline constexpr uint64_t streamPixelBudgetDefault=4194304;
inline constexpr uint64_t streamPixelBudgetMinimum=921600,streamPixelBudgetMaximum=uint64_t(UINT32_MAX)*2/3;
inline uint64_t streamPixelBudget(const Json& settings){
  const auto value=settings.get("videoPixelBudget").number(-1);
  if(!std::isfinite(value)||value!=std::floor(value)||value<double(streamPixelBudgetMinimum)||value>double(streamPixelBudgetMaximum))return streamPixelBudgetDefault;
  return uint64_t(value);
}
inline uint64_t streamMacroblocks(int width,int height){return width>0&&height>0?((uint64_t(width)+15)/16)*((uint64_t(height)+15)/16):0;}
// `budget` is pixels per frame at 60 fps; a lower frame rate gets the same
// macroblock rate (subject to the DWORD NV12 buffer length).
inline std::pair<int,int> streamEncodeSize(int sourceWidth,int sourceHeight,int boxWidth,int boxHeight,int fps,uint64_t budget){
  if(sourceWidth<16||sourceHeight<16)return {std::max(16,sourceWidth&~1),std::max(16,sourceHeight&~1)};
  fps=std::clamp(fps,1,60);budget=std::clamp(budget,streamPixelBudgetMinimum,streamPixelBudgetMaximum);
  const uint64_t frameBudget=std::min<uint64_t>(budget*60/uint64_t(fps),streamPixelBudgetMaximum);
  const uint64_t frameMacroblocks=frameBudget/256;
  const double pixels=double(sourceWidth)*sourceHeight;
  double scale=std::min({1.0,double(boxWidth)/sourceWidth,double(boxHeight)/sourceHeight,std::sqrt(double(frameBudget)/pixels)});
  const auto at=[&](double value){return std::pair{std::max(16,int(sourceWidth*value)&~1),std::max(16,int(sourceHeight*value)&~1)};};
  auto size=at(scale);
  // Padding to whole macroblocks can still exceed the budget by a row.
  for(int step=0;step<64&&scale<1.0&&streamMacroblocks(size.first,size.second)>frameMacroblocks;++step){scale*=0.995;size=at(scale);}
  return size;
}
}
