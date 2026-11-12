#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <immintrin.h>
#include <intrin.h>

namespace host::pixels {
// Same BT.601 limited-range integer conversion as the host. Callers provide
// positive even dimensions and complete BGRA/NV12 planes.
// Process two source rows together so their loaded pixels supply both planes.
// Every 2x2 block is converted independently (luma per pixel, chroma from the
// block's own four pixels), so converting any even-aligned sub-rectangle gives
// exactly the bytes a full-frame conversion produces for that rectangle.
// SSE2 kernel (every x86-64 CPU). bgraToNv12RowPair below picks the AVX2
// kernel, which produces the same bytes, when the CPU and Windows support it.
inline void bgraToNv12RowPairSse2(const uint8_t* top,const uint8_t* bottom,uint8_t* y0,uint8_t* y1,uint8_t* uv,int width) {
  const auto zero=_mm_setzero_si128();
  const auto yw=_mm_setr_epi16(25,129,66,0,25,129,66,0);
  const auto uw=_mm_setr_epi16(112,-74,-38,0,112,-74,-38,0);
  const auto vw=_mm_setr_epi16(-18,-94,112,0,-18,-94,112,0);
  const auto pair=[](__m128i value){return _mm_add_epi32(value,_mm_shuffle_epi32(value,_MM_SHUFFLE(2,3,0,1)));};
  const auto luma=[&](__m128i bytes) {
    const auto lo=pair(_mm_madd_epi16(_mm_unpacklo_epi8(bytes,zero),yw));
    const auto hi=pair(_mm_madd_epi16(_mm_unpackhi_epi8(bytes,zero),yw));
    auto sums=_mm_unpacklo_epi64(_mm_shuffle_epi32(lo,_MM_SHUFFLE(2,0,2,0)),_mm_shuffle_epi32(hi,_MM_SHUFFLE(2,0,2,0)));
    sums=_mm_add_epi32(_mm_srli_epi32(_mm_add_epi32(sums,_mm_set1_epi32(128)),8),_mm_set1_epi32(16));
    return uint32_t(_mm_cvtsi128_si32(_mm_packus_epi16(_mm_packs_epi32(sums,zero),zero)));
  };
  const auto clamp=[](int value){return uint8_t(std::clamp(value,0,255));};
  // Eight pixels per row per step, same integer formulas with fewer shuffles:
  // ((s+128)>>8)+16 == (s+128+16*256)>>8 for luma (s>=0), and the arithmetic
  // ((c+128)>>8)+128 == (c+128+128*256)>>8 for chroma. Adjacent 32-bit madd
  // halves are summed with two lane-selecting shuffles instead of four.
  const auto lumaBias=_mm_set1_epi32(128+(16<<8)),chromaBias=_mm_set1_epi32(128+(128<<8));
  const auto adjacent=[](__m128i a,__m128i b){
    const auto fa=_mm_castsi128_ps(a),fb=_mm_castsi128_ps(b);
    return _mm_add_epi32(_mm_castps_si128(_mm_shuffle_ps(fa,fb,_MM_SHUFFLE(2,0,2,0))),_mm_castps_si128(_mm_shuffle_ps(fa,fb,_MM_SHUFFLE(3,1,3,1))));
  };
  int x=0;
  for(;x+8<=width;x+=8) {
    const auto* t=top+size_t(x)*4;const auto* d=bottom+size_t(x)*4;
    const auto t0=_mm_loadu_si128(reinterpret_cast<const __m128i*>(t)),t1=_mm_loadu_si128(reinterpret_cast<const __m128i*>(t+16));
    const auto b0=_mm_loadu_si128(reinterpret_cast<const __m128i*>(d)),b1=_mm_loadu_si128(reinterpret_cast<const __m128i*>(d+16));
    const auto t0l=_mm_unpacklo_epi8(t0,zero),t0h=_mm_unpackhi_epi8(t0,zero),t1l=_mm_unpacklo_epi8(t1,zero),t1h=_mm_unpackhi_epi8(t1,zero);
    const auto b0l=_mm_unpacklo_epi8(b0,zero),b0h=_mm_unpackhi_epi8(b0,zero),b1l=_mm_unpacklo_epi8(b1,zero),b1h=_mm_unpackhi_epi8(b1,zero);
    const auto row=[&](__m128i p0,__m128i p1,__m128i p2,__m128i p3){
      const auto first=_mm_srli_epi32(_mm_add_epi32(adjacent(_mm_madd_epi16(p0,yw),_mm_madd_epi16(p1,yw)),lumaBias),8);
      const auto second=_mm_srli_epi32(_mm_add_epi32(adjacent(_mm_madd_epi16(p2,yw),_mm_madd_epi16(p3,yw)),lumaBias),8);
      return _mm_packs_epi32(first,second);
    };
    const auto lumaBytes=_mm_packus_epi16(row(t0l,t0h,t1l,t1h),row(b0l,b0h,b1l,b1h));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(y0+x),lumaBytes);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(y1+x),_mm_srli_si128(lumaBytes,8));
    // Vertical sums per pixel, then each 2x2 block's sum and floor average.
    const auto s0=_mm_add_epi16(t0l,b0l),s1=_mm_add_epi16(t0h,b0h),s2=_mm_add_epi16(t1l,b1l),s3=_mm_add_epi16(t1h,b1h);
    const auto c01=_mm_srli_epi16(_mm_add_epi16(_mm_unpacklo_epi64(s0,s1),_mm_unpackhi_epi64(s0,s1)),2);
    const auto c23=_mm_srli_epi16(_mm_add_epi16(_mm_unpacklo_epi64(s2,s3),_mm_unpackhi_epi64(s2,s3)),2);
    const auto u=_mm_srai_epi32(_mm_add_epi32(adjacent(_mm_madd_epi16(c01,uw),_mm_madd_epi16(c23,uw)),chromaBias),8);
    const auto v=_mm_srai_epi32(_mm_add_epi32(adjacent(_mm_madd_epi16(c01,vw),_mm_madd_epi16(c23,vw)),chromaBias),8);
    const auto pairs=_mm_packs_epi32(_mm_unpacklo_epi32(u,v),_mm_unpackhi_epi32(u,v));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(uv+x),_mm_packus_epi16(pairs,pairs));
  }
  for(;x+4<=width;x+=4) {
    const auto a=_mm_loadu_si128(reinterpret_cast<const __m128i*>(top+size_t(x)*4));
    const auto b=_mm_loadu_si128(reinterpret_cast<const __m128i*>(bottom+size_t(x)*4));
    const uint32_t first=luma(a),second=luma(b);
    std::memcpy(y0+x,&first,sizeof(first));std::memcpy(y1+x,&second,sizeof(second));
    auto lo=_mm_add_epi16(_mm_unpacklo_epi8(a,zero),_mm_unpacklo_epi8(b,zero));
    auto hi=_mm_add_epi16(_mm_unpackhi_epi8(a,zero),_mm_unpackhi_epi8(b,zero));
    lo=_mm_srli_epi16(_mm_add_epi16(lo,_mm_srli_si128(lo,8)),2);
    hi=_mm_srli_epi16(_mm_add_epi16(hi,_mm_srli_si128(hi,8)),2);
    const auto rgb=_mm_unpacklo_epi64(lo,hi);
    const auto u=pair(_mm_madd_epi16(rgb,uw)),v=pair(_mm_madd_epi16(rgb,vw));
    auto sums=_mm_unpacklo_epi64(_mm_unpacklo_epi32(u,v),_mm_unpackhi_epi32(u,v));
    sums=_mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(sums,_mm_set1_epi32(128)),8),_mm_set1_epi32(128));
    const uint32_t packed=uint32_t(_mm_cvtsi128_si32(_mm_packus_epi16(_mm_packs_epi32(sums,zero),zero)));
    std::memcpy(uv+x,&packed,sizeof(packed));
  }
  for(;x<width;x+=2) {
    int red=0,green=0,blue=0;
    for(int row=0;row<2;++row)for(int dx=0;dx<2;++dx) {
      const auto* pixel=(row?bottom:top)+size_t(x+dx)*4;
      (row?y1:y0)[x+dx]=clamp(((66*pixel[2]+129*pixel[1]+25*pixel[0]+128)>>8)+16);
      red+=pixel[2];green+=pixel[1];blue+=pixel[0];
    }
    red/=4;green/=4;blue/=4;
    uv[x]=clamp(((-38*red-74*green+112*blue+128)>>8)+128);
    uv[x+1]=clamp(((112*red-94*green-18*blue+128)>>8)+128);
  }
}
// AVX2: the SSE2 kernel's eight-pixel step twice side by side, one step per
// 128-bit lane (pixels x..x+7 in the low lane, x+8..x+15 in the high lane), so
// every intermediate is the same integer as above and the bytes are identical.
// The remaining (< 16) pixels go through the SSE2 kernel.
inline void bgraToNv12RowPairAvx2(const uint8_t* top,const uint8_t* bottom,uint8_t* y0,uint8_t* y1,uint8_t* uv,int width) {
  const auto zero=_mm256_setzero_si256();
  const auto yw=_mm256_setr_epi16(25,129,66,0,25,129,66,0,25,129,66,0,25,129,66,0);
  const auto uw=_mm256_setr_epi16(112,-74,-38,0,112,-74,-38,0,112,-74,-38,0,112,-74,-38,0);
  const auto vw=_mm256_setr_epi16(-18,-94,112,0,-18,-94,112,0,-18,-94,112,0,-18,-94,112,0);
  const auto lumaBias=_mm256_set1_epi32(128+(16<<8)),chromaBias=_mm256_set1_epi32(128+(128<<8));
  const auto adjacent=[](__m256i a,__m256i b){
    const auto fa=_mm256_castsi256_ps(a),fb=_mm256_castsi256_ps(b);
    return _mm256_add_epi32(_mm256_castps_si256(_mm256_shuffle_ps(fa,fb,_MM_SHUFFLE(2,0,2,0))),_mm256_castps_si256(_mm256_shuffle_ps(fa,fb,_MM_SHUFFLE(3,1,3,1))));
  };
  int x=0;
  for(;x+16<=width;x+=16) {
    const auto* t=top+size_t(x)*4;const auto* d=bottom+size_t(x)*4;
    const auto ta=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(t)),tb=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(t+32));
    const auto ba=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(d)),bb=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(d+32));
    // t0/b0: pixels 0-3 | 8-11, t1/b1: pixels 4-7 | 12-15 (the SSE2 step's t0/t1 per lane).
    const auto t0=_mm256_permute2x128_si256(ta,tb,0x20),t1=_mm256_permute2x128_si256(ta,tb,0x31);
    const auto b0=_mm256_permute2x128_si256(ba,bb,0x20),b1=_mm256_permute2x128_si256(ba,bb,0x31);
    const auto t0l=_mm256_unpacklo_epi8(t0,zero),t0h=_mm256_unpackhi_epi8(t0,zero),t1l=_mm256_unpacklo_epi8(t1,zero),t1h=_mm256_unpackhi_epi8(t1,zero);
    const auto b0l=_mm256_unpacklo_epi8(b0,zero),b0h=_mm256_unpackhi_epi8(b0,zero),b1l=_mm256_unpacklo_epi8(b1,zero),b1h=_mm256_unpackhi_epi8(b1,zero);
    const auto row=[&](__m256i p0,__m256i p1,__m256i p2,__m256i p3){
      const auto first=_mm256_srli_epi32(_mm256_add_epi32(adjacent(_mm256_madd_epi16(p0,yw),_mm256_madd_epi16(p1,yw)),lumaBias),8);
      const auto second=_mm256_srli_epi32(_mm256_add_epi32(adjacent(_mm256_madd_epi16(p2,yw),_mm256_madd_epi16(p3,yw)),lumaBias),8);
      return _mm256_packs_epi32(first,second);
    };
    // Per lane: 8 top then 8 bottom luma bytes; reorder to 16 top, 16 bottom.
    const auto lumaBytes=_mm256_permute4x64_epi64(_mm256_packus_epi16(row(t0l,t0h,t1l,t1h),row(b0l,b0h,b1l,b1h)),_MM_SHUFFLE(3,1,2,0));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(y0+x),_mm256_castsi256_si128(lumaBytes));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(y1+x),_mm256_extracti128_si256(lumaBytes,1));
    const auto s0=_mm256_add_epi16(t0l,b0l),s1=_mm256_add_epi16(t0h,b0h),s2=_mm256_add_epi16(t1l,b1l),s3=_mm256_add_epi16(t1h,b1h);
    const auto c01=_mm256_srli_epi16(_mm256_add_epi16(_mm256_unpacklo_epi64(s0,s1),_mm256_unpackhi_epi64(s0,s1)),2);
    const auto c23=_mm256_srli_epi16(_mm256_add_epi16(_mm256_unpacklo_epi64(s2,s3),_mm256_unpackhi_epi64(s2,s3)),2);
    const auto u=_mm256_srai_epi32(_mm256_add_epi32(adjacent(_mm256_madd_epi16(c01,uw),_mm256_madd_epi16(c23,uw)),chromaBias),8);
    const auto v=_mm256_srai_epi32(_mm256_add_epi32(adjacent(_mm256_madd_epi16(c01,vw),_mm256_madd_epi16(c23,vw)),chromaBias),8);
    const auto pairs=_mm256_packs_epi32(_mm256_unpacklo_epi32(u,v),_mm256_unpackhi_epi32(u,v));
    // Per lane: 8 UV bytes in the low half; join both lanes' low halves.
    const auto uvBytes=_mm256_permute4x64_epi64(_mm256_packus_epi16(pairs,pairs),_MM_SHUFFLE(3,1,2,0));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(uv+x),_mm256_castsi256_si128(uvBytes));
  }
  _mm256_zeroupper();
  if(x<width)bgraToNv12RowPairSse2(top+size_t(x)*4,bottom+size_t(x)*4,y0+x,y1+x,uv+x,width-x);
}
// CPU has AVX2 and Windows saves the YMM state (CPUID leaf 7 EBX bit 5, OSXSAVE + AVX, XCR0 bits 1-2).
inline bool avx2Supported() {
  static const bool value=[]{
    int r[4]{};__cpuid(r,0);if(r[0]<7)return false;
    __cpuid(r,1);if(!(r[2]&(1<<27))||!(r[2]&(1<<28)))return false;
    if((_xgetbv(0)&6)!=6)return false;
    __cpuidex(r,7,0);return (r[1]&(1<<5))!=0;
  }();
  return value;
}
// Tests only: true runs the SSE2 kernel on an AVX2 machine too.
inline std::atomic<bool> forceSse2Kernel{false};
using RowPairKernel=void(*)(const uint8_t*,const uint8_t*,uint8_t*,uint8_t*,uint8_t*,int);
inline RowPairKernel rowPairKernel(){return avx2Supported()&&!forceSse2Kernel.load(std::memory_order_relaxed)?bgraToNv12RowPairAvx2:bgraToNv12RowPairSse2;}
inline void bgraToNv12RowPair(const uint8_t* top,const uint8_t* bottom,uint8_t* y0,uint8_t* y1,uint8_t* uv,int width){rowPairKernel()(top,bottom,y0,y1,uv,width);}
// Converts an even-sized BGRA block with any source row pitch. `luma` and
// `chroma` address the block origin in each NV12 plane; both planes advance by
// `planeStride` bytes per row (the NV12 frame width).
inline void bgraToNv12Block(const uint8_t* source,size_t sourceStride,int width,int height,uint8_t* luma,uint8_t* chroma,size_t planeStride) {
  const auto kernel=rowPairKernel();
  for(int y=0;y<height;y+=2) {
    const auto* top=source+size_t(y)*sourceStride;
    auto* y0=luma+size_t(y)*planeStride;
    kernel(top,top+sourceStride,y0,y0+planeStride,chroma+size_t(y/2)*planeStride,width);
  }
}
inline void bgraToNv12PairedRows(const uint8_t* source,uint8_t* target,int width,int height) {
  bgraToNv12Block(source,size_t(width)*4,width,height,target,target+size_t(width)*height,size_t(width));
}
} // namespace host::pixels
