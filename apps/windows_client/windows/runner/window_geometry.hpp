#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace multitap {
// Outer-window pixels, including the caption/borders. The work rectangle has
// already excluded the taskbar. No image/remote-desktop scaling happens here.
struct WindowWorkArea {
  int64_t left=0,top=0,right=0,bottom=0;
};
inline std::optional<WindowWorkArea> insetWindowWorkArea(const RECT& work,UINT dpi){
  const int64_t width=int64_t(work.right)-work.left,height=int64_t(work.bottom)-work.top;
  if(width<=0||height<=0)return {};
  const auto scaled=uint64_t(16)*(dpi?dpi:96)/96;
  const auto x=std::min<int64_t>(int64_t(scaled),width/8),y=std::min<int64_t>(int64_t(scaled),height/8);
  return WindowWorkArea{work.left+x,work.top+y,work.right-x,work.bottom-y};
}
inline int64_t windowLogicalDimension(UINT value,UINT dpi){
  return std::max<int64_t>(1,int64_t(uint64_t(value)*(dpi?dpi:96)/96));
}
inline std::optional<RECT> initialWindowFrame(const RECT& work,UINT dpi,LONG originX,LONG originY,UINT width,UINT height){
  const auto area=insetWindowWorkArea(work,dpi);if(!area)return {};
  const auto limit=int64_t((std::numeric_limits<LONG>::max)());
  const auto w=std::min({windowLogicalDimension(width,dpi),area->right-area->left,limit});
  const auto h=std::min({windowLogicalDimension(height,dpi),area->bottom-area->top,limit});
  const auto x=std::clamp(int64_t(originX)*(dpi?dpi:96)/96,area->left,area->right-w);
  const auto y=std::clamp(int64_t(originY)*(dpi?dpi:96)/96,area->top,area->bottom-h);
  return RECT{LONG(x),LONG(y),LONG(x+w),LONG(y+h)};
}
// A new session window opens centred over the launcher window, kept inside
// the work area of the launcher's monitor (shrunk to it when larger).
inline std::optional<RECT> centeredWindowFrame(const RECT& owner,const RECT& work,LONG width,LONG height){
  const int64_t areaWidth=int64_t(work.right)-work.left,areaHeight=int64_t(work.bottom)-work.top;
  if(areaWidth<=0||areaHeight<=0||width<=0||height<=0||owner.right<owner.left||owner.bottom<owner.top)return {};
  const int64_t w=std::min<int64_t>(width,areaWidth),h=std::min<int64_t>(height,areaHeight);
  const int64_t centerX=(int64_t(owner.left)+owner.right)/2,centerY=(int64_t(owner.top)+owner.bottom)/2;
  const int64_t x=std::clamp<int64_t>(centerX-w/2,work.left,int64_t(work.right)-w);
  const int64_t y=std::clamp<int64_t>(centerY-h/2,work.top,int64_t(work.bottom)-h);
  return RECT{LONG(x),LONG(y),LONG(x+w),LONG(y+h)};
}
inline std::optional<SIZE> minimumWindowTrackSize(const RECT& work,UINT dpi,UINT width,UINT height){
  const auto area=insetWindowWorkArea(work,dpi);if(!area)return {};
  const auto limit=int64_t((std::numeric_limits<LONG>::max)());
  return SIZE{LONG(std::min({windowLogicalDimension(width,dpi),area->right-area->left,limit})),
              LONG(std::min({windowLogicalDimension(height,dpi),area->bottom-area->top,limit}))};
}
}
