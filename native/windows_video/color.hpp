#pragma once
#include <emmintrin.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace multitap::video {
// Windows x64 SSE2. The caller validates both planes and output dimensions.
// Matches the scalar integer conversion byte for byte, including superblack
// clamping and saturated output. No colour-space guess from frame dimensions.
inline void nv12ToRgba(const uint8_t* nv12,uint8_t* rgba,uint32_t width,uint32_t height,
                       uint32_t stride,uint32_t planeHeight,bool bt709,bool fullRange) {
  const int lumaScale=fullRange?256:298;
  const int redV=fullRange?(bt709?403:359):(bt709?459:409);
  const int greenU=fullRange?(bt709?48:88):(bt709?55:100);
  const int greenV=fullRange?(bt709?120:183):(bt709?136:208);
  const int blueU=fullRange?(bt709?475:454):(bt709?541:516);
  const auto weights=[](int n){return _mm_setr_epi16(short(n),0,short(n),0,short(n),0,short(n),0);};
  const __m128i zero=_mm_setzero_si128(),round=_mm_set1_epi32(128);
  const auto yWeights=weights(lumaScale),rvWeights=weights(redV),guWeights=weights(greenU),gvWeights=weights(greenV),buWeights=weights(blueU);
  const auto pack=[&](__m128i value){value=_mm_srai_epi32(_mm_add_epi32(value,round),8);return _mm_packus_epi16(_mm_packs_epi32(value,zero),zero);};
  const uint8_t* chromaPlane=nv12+size_t(stride)*planeHeight;
  for(uint32_t y=0;y<height;++y){
    const uint8_t* row=nv12+size_t(y)*stride;
    const uint8_t* uv=chromaPlane+size_t(y/2)*stride;
    uint8_t* target=rgba+size_t(y)*width*4;
    uint32_t x=0;
    for(;x+4<=width;x+=4){
      uint32_t yBytes=0,uvBytes=0;std::memcpy(&yBytes,row+x,4);std::memcpy(&uvBytes,uv+x,4);
      auto luma=_mm_unpacklo_epi8(_mm_cvtsi32_si128(int(yBytes)),zero);
      if(!fullRange)luma=_mm_subs_epu16(luma,_mm_set1_epi16(16));
      const auto chroma=_mm_sub_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(int(uvBytes)),zero),_mm_set1_epi16(128));
      const auto u=_mm_unpacklo_epi16(_mm_shufflelo_epi16(chroma,_MM_SHUFFLE(2,2,0,0)),zero);
      const auto v=_mm_unpacklo_epi16(_mm_shufflelo_epi16(chroma,_MM_SHUFFLE(3,3,1,1)),zero);
      const auto yy=_mm_madd_epi16(_mm_unpacklo_epi16(luma,zero),yWeights);
      const auto r=pack(_mm_add_epi32(yy,_mm_madd_epi16(v,rvWeights)));
      const auto g=pack(_mm_sub_epi32(_mm_sub_epi32(yy,_mm_madd_epi16(u,guWeights)),_mm_madd_epi16(v,gvWeights)));
      const auto b=pack(_mm_add_epi32(yy,_mm_madd_epi16(u,buWeights)));
      const auto rg=_mm_unpacklo_epi8(r,g),ba=_mm_unpacklo_epi8(b,_mm_set1_epi8(char(-1)));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(target+size_t(x)*4),_mm_unpacklo_epi16(rg,ba));
    }
    for(;x<width;++x){
      const int yy=std::max(0,int(row[x])-(fullRange?0:16)),u=int(uv[x&~1u])-128,v=int(uv[(x&~1u)+1])-128;
      const auto clip=[](int n){return uint8_t(std::clamp(n,0,255));};
      target[size_t(x)*4]=clip((lumaScale*yy+redV*v+128)>>8);
      target[size_t(x)*4+1]=clip((lumaScale*yy-greenU*u-greenV*v+128)>>8);
      target[size_t(x)*4+2]=clip((lumaScale*yy+blueU*u+128)>>8);
      target[size_t(x)*4+3]=255;
    }
  }
}
}
