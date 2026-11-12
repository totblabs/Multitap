#pragma once
#include <windows.h>
#include "capture_compose.hpp"

namespace host::capture {
// A small owned top-down 32bpp DIB in which the cursor is composed over the
// exact base pixels it covers, with the capture DC's GDI state (HALFTONE and a
// brush origin aligned to frame coordinates). DrawIconEx only writes inside
// its destination rectangle and each written pixel depends only on that
// pixel's previous value, so composing a frame-aligned patch yields the same
// bytes as drawing into a full-frame DIB.
class CursorCanvas {
 public:
  CursorCanvas()=default;
  CursorCanvas(const CursorCanvas&)=delete;
  CursorCanvas& operator=(const CursorCanvas&)=delete;
  ~CursorCanvas(){release();}
  bool reserve(int width,int height,HDC compatible){
    if(width<=0||height<=0||width>4096||height>4096)return false;
    if(pixels_&&width<=capacityWidth_&&height<=capacityHeight_)return true;
    const int w=std::max({width,capacityWidth_,64}),h=std::max({height,capacityHeight_,64});
    release();
    dc_=CreateCompatibleDC(compatible);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    if(dc_)bitmap_=CreateDIBSection(compatible?compatible:dc_,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels_),nullptr,0);
    if(!dc_||!bitmap_||!pixels_){release();return false;}
    previous_=SelectObject(dc_,bitmap_);SetStretchBltMode(dc_,HALFTONE);
    capacityWidth_=w;capacityHeight_=h;return true;
  }
  // Brush phase of frame pixel (left,top) equals the full-frame DC's (origin 0,0).
  void align(int left,int top){SetBrushOrgEx(dc_,((-left)%8+8)%8,((-top)%8+8)%8,nullptr);}
  HDC dc()const{return dc_;}
  uint8_t* pixels()const{return pixels_;}
  size_t stride()const{return size_t(capacityWidth_)*4;}
 private:
  void release()noexcept{
    if(dc_&&previous_)SelectObject(dc_,previous_);if(bitmap_)DeleteObject(bitmap_);if(dc_)DeleteDC(dc_);
    dc_=nullptr;bitmap_=nullptr;previous_=nullptr;pixels_=nullptr;capacityWidth_=capacityHeight_=0;
  }
  HDC dc_=nullptr;HBITMAP bitmap_=nullptr;HGDIOBJ previous_=nullptr;uint8_t* pixels_=nullptr;int capacityWidth_=0,capacityHeight_=0;
};

struct CursorDraw {HCURSOR shape=nullptr;int x=0,y=0,width=0,height=0;};
// Whether a capture tick draws the cursor into the video: composition is on
// (no cursor-v1, the unrepresentable fallback, or cursor-compose-v1 while the
// client holds the mouse) and Windows shows it now. An application that hides
// it (a game's own crosshair) gets nothing drawn.
inline bool drawsCursor(bool compose,bool known,DWORD flags){return compose&&known&&(flags&CURSOR_SHOWING)!=0;}

// Keeps a complete NV12 frame byte-identical to bgraToNv12(base + DrawIconEx
// cursor) while the base surfaces are converted directly from their readback
// memory. Only the cursor's even-aligned patch is composed in BGRA; the
// previous patch is restored from the base. `visit(rect, f)` must call
// f(pixelAtOverlapOrigin, rowPitch, overlap) for each base surface that
// overlaps rect, covering it completely.
class Nv12CursorPatch {
 public:
  bool reserve(int width,int height,HDC compatible){return canvas_.reserve(width,height,compatible);}
  // Forget the composed patch, e.g. after every base pixel was rewritten.
  void reset(){previous_.reset();}
  // The currently composed patch (base + cursor) and its BGRA pixels, whose
  // origin is the patch's top-left. Valid until the next apply().
  std::optional<PixelRect> patch()const{return previous_;}
  const uint8_t* patchPixels()const{return canvas_.pixels();}
  size_t patchStride()const{return canvas_.stride();}
  // Returns false when DrawIconEx failed (the patch then holds what GDI left).
  template<class Visit>
  bool apply(std::span<uint8_t> nv12,int frameWidth,int frameHeight,const std::optional<CursorDraw>& cursor,Visit&& visit,HDC compatible){
    std::optional<PixelRect> next;
    if(cursor)next=evenCursorPatch(cursor->x,cursor->y,cursor->width,cursor->height,frameWidth,frameHeight);
    if(previous_&&previous_!=next)
      visit(*previous_,[&](const uint8_t* source,size_t pitch,const PixelRect& overlap){convertBlockToNv12(nv12,frameWidth,frameHeight,source,pitch,overlap);});
    previous_.reset();
    if(!next)return true;
    if(!canvas_.reserve(next->width(),next->height(),compatible))throw std::runtime_error("Desktop capture is unavailable");
    GdiFlush();
    const auto stride=canvas_.stride();auto* pixels=canvas_.pixels();
    visit(*next,[&](const uint8_t* source,size_t pitch,const PixelRect& overlap){
      auto* row=pixels+size_t(overlap.top-next->top)*stride+size_t(overlap.left-next->left)*4;
      for(int y=0;y<overlap.height();++y)std::memcpy(row+size_t(y)*stride,source+size_t(y)*pitch,size_t(overlap.width())*4);
    });
    canvas_.align(next->left,next->top);
    const bool drawn=DrawIconEx(canvas_.dc(),cursor->x-next->left,cursor->y-next->top,cursor->shape,cursor->width,cursor->height,0,nullptr,DI_NORMAL)!=FALSE;
    GdiFlush();
    convertBlockToNv12(nv12,frameWidth,frameHeight,pixels,stride,*next);
    previous_=next;
    return drawn;
  }
 private:
  CursorCanvas canvas_;std::optional<PixelRect> previous_;
};

// The cursor composed over BGRA base surfaces (no NV12), e.g. at the unscaled
// source resolution of a scaled capture for lossless tiles. Same canvas, base
// copy, brush alignment and DrawIconEx as the full-frame DIB, so the pieces
// from visitComposedPieces(bases, patch(), pixels(), stride()) equal it.
class CursorComposer {
 public:
  // Returns false when DrawIconEx failed (the canvas then holds what GDI left).
  bool compose(const std::optional<CursorDraw>& cursor,int frameWidth,int frameHeight,std::span<const ComposedSurface> bases,HDC compatible){
    patch_.reset();if(!cursor)return true;
    const auto next=evenCursorPatch(cursor->x,cursor->y,cursor->width,cursor->height,frameWidth,frameHeight);if(!next)return true;
    if(!canvas_.reserve(next->width(),next->height(),compatible))throw std::runtime_error("Desktop capture is unavailable");
    GdiFlush();const auto stride=canvas_.stride();auto* pixels=canvas_.pixels();
    for(const auto& base:bases){
      const auto overlap=intersect(*next,base.placed);if(!overlap)continue;
      for(int y=0;y<overlap->height();++y)std::memcpy(pixels+size_t(overlap->top-next->top+y)*stride+size_t(overlap->left-next->left)*4,base.pixels+size_t(overlap->top-base.placed.top+y)*base.pitch+size_t(overlap->left-base.placed.left)*4,size_t(overlap->width())*4);
    }
    canvas_.align(next->left,next->top);
    const bool drawn=DrawIconEx(canvas_.dc(),cursor->x-next->left,cursor->y-next->top,cursor->shape,cursor->width,cursor->height,0,nullptr,DI_NORMAL)!=FALSE;
    GdiFlush();patch_=next;return drawn;
  }
  std::optional<PixelRect> patch()const{return patch_;}
  const uint8_t* pixels()const{return canvas_.pixels();}
  size_t stride()const{return canvas_.stride();}
 private:
  CursorCanvas canvas_;std::optional<PixelRect> patch_;
};
}
