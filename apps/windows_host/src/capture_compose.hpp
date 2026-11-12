#pragma once
#include "owned_output_size.hpp"
#include "../../../native/windows_video/bgra_nv12_sse2.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>

namespace host::capture {
// Compose an exact-size surface without the GDI HALFTONE resampling path.
// The two buffers must be separate. The caller flushes pending GDI writes
// before accessing a DIB's pixels directly; every frame restores the base
// surface, including when only the separately drawn cursor has moved.
inline void copyUnscaledSurface(std::span<uint8_t> target,int targetWidth,int targetHeight,
                                std::span<const uint8_t> source,int sourceWidth,int sourceHeight,
                                int left,int top) {
  const auto targetLayout=checkedFrameLayout(uint64_t(targetWidth),uint64_t(targetHeight),false);
  const auto sourceLayout=checkedFrameLayout(uint64_t(sourceWidth),uint64_t(sourceHeight),false);
  if(!targetLayout||!sourceLayout||
     sourceWidth<=0||sourceHeight<=0||sourceWidth>targetWidth||sourceHeight>targetHeight||
     left<0||top<0||left>targetWidth-sourceWidth||top>targetHeight-sourceHeight||
     targetLayout->bgraBytes>target.size()||sourceLayout->bgraBytes>source.size())
    throw std::runtime_error("Invalid unscaled capture surface");
  const size_t sourceStride=sourceLayout->bgraStride,targetStride=targetLayout->bgraStride;
  auto* destination=target.data()+size_t(top)*targetStride+size_t(left)*4;
  if(sourceWidth==targetWidth)std::memcpy(destination,source.data(),sourceStride*sourceHeight);
  else for(int row=0;row<sourceHeight;++row)
    std::memcpy(destination+size_t(row)*targetStride,source.data()+size_t(row)*sourceStride,sourceStride);
}

// Frame-space rectangle [left,right) x [top,bottom).
struct PixelRect {
  int left=0,top=0,right=0,bottom=0;
  int width()const{return right-left;}
  int height()const{return bottom-top;}
  bool empty()const{return right<=left||bottom<=top;}
  friend bool operator==(const PixelRect&,const PixelRect&)=default;
};
inline bool evenRect(const PixelRect& r){return !r.empty()&&((r.left|r.top|r.right|r.bottom)&1)==0;}
inline std::optional<PixelRect> intersect(const PixelRect& a,const PixelRect& b){
  const PixelRect r{std::max(a.left,b.left),std::max(a.top,b.top),std::min(a.right,b.right),std::min(a.bottom,b.bottom)};
  if(r.empty())return std::nullopt;return r;
}
// a minus b as at most four disjoint rectangles; returns their count.
inline int subtract(const PixelRect& a,const PixelRect& b,PixelRect (&out)[4]){
  const auto cut=intersect(a,b);if(!cut){out[0]=a;return 1;}
  int n=0;
  if(cut->top>a.top)out[n++]={a.left,a.top,a.right,cut->top};
  if(cut->bottom<a.bottom)out[n++]={a.left,cut->bottom,a.right,a.bottom};
  if(cut->left>a.left)out[n++]={a.left,cut->top,cut->left,cut->bottom};
  if(cut->right<a.right)out[n++]={cut->right,cut->top,a.right,cut->bottom};
  return n;
}
// Receives disjoint pieces of the composed BGRA frame: the pixel at the
// piece's top-left, the row pitch in bytes and the piece in frame coordinates.
using ComposedVisitor=std::function<void(const uint8_t* origin,size_t pitch,const PixelRect& piece)>;
// One base surface's readback placed in frame coordinates (origin = placed top-left).
struct ComposedSurface {PixelRect placed;const uint8_t* pixels=nullptr;size_t pitch=0;};
// The composed frame of the direct capture path: base surfaces outside the
// cursor patch, the composed cursor canvas (origin = patch top-left) inside it.
// Pieces are disjoint and exactly cover each rect when the surfaces tile it.
inline void visitComposedPieces(std::span<const PixelRect> rects,std::span<const ComposedSurface> surfaces,const std::optional<PixelRect>& patch,
                                const uint8_t* patchPixels,size_t patchPitch,const ComposedVisitor& visitor){
  for(const auto& rect:rects){
    for(const auto& surface:surfaces){
      const auto overlap=intersect(rect,surface.placed);if(!overlap)continue;
      PixelRect pieces[4];int parts=1;pieces[0]=*overlap;if(patch)parts=subtract(*overlap,*patch,pieces);
      for(int k=0;k<parts;++k){const auto& piece=pieces[k];
        visitor(surface.pixels+size_t(piece.top-surface.placed.top)*surface.pitch+size_t(piece.left-surface.placed.left)*4,surface.pitch,piece);}
    }
    if(patch)if(const auto inside=intersect(rect,*patch))
      visitor(patchPixels+size_t(inside->top-patch->top)*patchPitch+size_t(inside->left-patch->left)*4,patchPitch,*inside);
  }
}
// The cursor rectangle clipped to the frame and widened to even coordinates,
// so it covers every complete NV12 2x2 chroma block the cursor can touch.
// Frame dimensions are even, so the widened rectangle stays inside the frame.
inline std::optional<PixelRect> evenCursorPatch(int x,int y,int width,int height,int frameWidth,int frameHeight){
  if(width<=0||height<=0||frameWidth<=0||frameHeight<=0||(frameWidth|frameHeight)&1)return std::nullopt;
  const int64_t left=std::max<int64_t>(0,x),top=std::max<int64_t>(0,y);
  const int64_t right=std::min<int64_t>(frameWidth,int64_t(x)+width),bottom=std::min<int64_t>(frameHeight,int64_t(y)+height);
  if(left>=right||top>=bottom)return std::nullopt;
  return PixelRect{int(left&~int64_t(1)),int(top&~int64_t(1)),int((right+1)&~int64_t(1)),int((bottom+1)&~int64_t(1))};
}
// Convert one even-aligned BGRA block (any row pitch) into its place in a
// complete NV12 frame. Bytes outside the block are untouched.
inline void convertBlockToNv12(std::span<uint8_t> nv12,int frameWidth,int frameHeight,
                               const uint8_t* source,size_t sourceStride,const PixelRect& block){
  const auto layout=checkedFrameLayout(uint64_t(frameWidth),uint64_t(frameHeight));
  if(!source||!layout||
     !evenRect(block)||block.left<0||block.top<0||block.right>frameWidth||block.bottom>frameHeight||
     sourceStride<size_t(block.width())*4||layout->nv12Bytes>nv12.size())
    throw std::runtime_error("Invalid NV12 block conversion");
  auto* luma=nv12.data()+size_t(block.top)*frameWidth+block.left;
  auto* chroma=nv12.data()+size_t(frameWidth)*frameHeight+size_t(block.top/2)*frameWidth+block.left;
  pixels::bgraToNv12Block(source,sourceStride,block.width(),block.height(),luma,chroma,size_t(frameWidth));
}
}
