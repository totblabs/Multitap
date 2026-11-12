#pragma once
#include <emmintrin.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include "frame_layout.hpp"

namespace multitap::video {
// Separable bilinear RGBA upscaling with pixel-centre alignment and 8-bit
// fixed-point weights (truncating), SSE2. Used when lossless tiles describe the
// unscaled source while the decoded video is downscaled: regions without a
// lossless tile are resampled to source size. `ReferenceResample` is the scalar
// definition the SIMD path must match bit for bit. C++17, Windows x64.
class BilinearUpscaler {
 public:
  bool Configure(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t targetWidth, uint32_t targetHeight) {
    FrameLayout source{},target{};
    if (sourceWidth < 2 || sourceHeight < 2 || targetWidth < sourceWidth || targetHeight < sourceHeight ||
        !frameLayout(sourceWidth,sourceHeight,source,false)||!frameLayout(targetWidth,targetHeight,target,false)) return false;
    sw_ = sourceWidth; sh_ = sourceHeight; tw_ = targetWidth; th_ = targetHeight;
    columns_.resize(tw_); weightsX_.resize(tw_); rows_.resize(th_); weightsY_.resize(th_); vectors_.resize(tw_);
    for (uint32_t x = 0; x < tw_; ++x) {
      map(x, sw_, tw_, columns_[x], weightsX_[x]);
      const short w = short(weightsX_[x]), inverse = short(256 - weightsX_[x]);
      vectors_[x] = _mm_setr_epi16(inverse, w, inverse, w, inverse, w, inverse, w);
    }
    for (uint32_t y = 0; y < th_; ++y) map(y, sh_, th_, rows_[y], weightsY_[y]);
    return true;
  }
  // Writes target rows [y0,y1) x columns [x0,x1) from the source image.
  void Resample(const uint8_t* source, size_t sourcePitch, uint8_t* target, size_t targetPitch, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    if (x1 > tw_ || y1 > th_ || x0 >= x1 || y0 >= y1) return;
    const size_t bytes = size_t(x1 - x0) * 4;
    for (auto& slot : slots_) { slot.row = ~0u; slot.pixels.resize(bytes + 16); }
    for (uint32_t y = y0; y < y1; ++y) {
      const uint8_t* top = horizontal(source, sourcePitch, rows_[y], x0, x1);
      const uint8_t* bottom = horizontal(source, sourcePitch, rows_[y] + 1, x0, x1);
      vertical(top, bottom, weightsY_[y], target + size_t(y) * targetPitch + size_t(x0) * 4, bytes);
    }
  }
  static void ReferenceResample(const uint8_t* source, size_t sourcePitch, uint32_t sourceWidth, uint32_t sourceHeight, uint8_t* target, size_t targetPitch,
                                uint32_t targetWidth, uint32_t targetHeight, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y) {
      uint32_t row = 0, wy = 0; map(y, sourceHeight, targetHeight, row, wy);
      for (uint32_t x = x0; x < x1; ++x) {
        uint32_t column = 0, wx = 0; map(x, sourceWidth, targetWidth, column, wx);
        for (int c = 0; c < 4; ++c) {
          const auto at = [&](uint32_t r, uint32_t k) { return uint32_t(source[size_t(r) * sourcePitch + size_t(k) * 4 + c]); };
          const uint32_t top = (at(row, column) * (256 - wx) + at(row, column + 1) * wx) >> 8;
          const uint32_t bottom = (at(row + 1, column) * (256 - wx) + at(row + 1, column + 1) * wx) >> 8;
          target[size_t(y) * targetPitch + size_t(x) * 4 + c] = uint8_t((top * (256 - wy) + bottom * wy) >> 8);
        }
      }
    }
  }
  uint32_t targetWidth() const { return tw_; }
  uint32_t targetHeight() const { return th_; }
 private:
  // Centre of target pixel i in source pixels, as index + weight/256; the last
  // source pixel uses (s-2, 256) so both taps stay inside the image.
  static void map(uint32_t i, uint32_t s, uint32_t t, uint32_t& index, uint32_t& weight) {
    // Quotient/remainder order keeps the bilinear position exact without the
    // old 2*i and intermediate *256 overflow at larger ABI-valid dimensions.
    const uint64_t numerator=(uint64_t(i)*2+1)*s,denominator=uint64_t(t)*2;
    const int64_t position = std::max<int64_t>(0, int64_t((numerator/denominator)*256+(numerator%denominator)*256/denominator)-128);
    index = uint32_t(position >> 8); weight = uint32_t(position & 255);
    if (index >= s - 1) { index = s - 2; weight = 256; }
  }
  struct Slot { uint32_t row = ~0u; std::vector<uint8_t> pixels; };
  const uint8_t* horizontal(const uint8_t* source, size_t pitch, uint32_t row, uint32_t x0, uint32_t x1) {
    for (auto& slot : slots_) if (slot.row == row) return slot.pixels.data();
    // Upscaling advances at most one source row per target row: use an empty
    // slot, else evict the older row (never the other row of this target row).
    Slot& slot = slots_[0].row == ~0u ? slots_[0] : slots_[1].row == ~0u ? slots_[1] : slots_[0].row < slots_[1].row ? slots_[0] : slots_[1];
    slot.row = row;
    const uint8_t* line = source + size_t(row) * pitch;
    uint8_t* out = slot.pixels.data();
    const __m128i zero = _mm_setzero_si128();
    uint32_t x = x0;
    for (; x + 4 <= x1; x += 4) {
      __m128i channel[4];
      for (int k = 0; k < 4; ++k) {
        const __m128i pair = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(line + size_t(columns_[x + k]) * 4));
        const __m128i wide = _mm_unpacklo_epi8(pair, zero);
        channel[k] = _mm_srli_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(wide, _mm_srli_si128(wide, 8)), vectors_[x + k]), 8);
      }
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + size_t(x - x0) * 4),
                       _mm_packus_epi16(_mm_packs_epi32(channel[0], channel[1]), _mm_packs_epi32(channel[2], channel[3])));
    }
    for (; x < x1; ++x) {
      const uint8_t* a = line + size_t(columns_[x]) * 4; const uint32_t w = weightsX_[x];
      for (int c = 0; c < 4; ++c) out[size_t(x - x0) * 4 + c] = uint8_t((uint32_t(a[c]) * (256 - w) + uint32_t(a[4 + c]) * w) >> 8);
    }
    return out;
  }
  static void vertical(const uint8_t* top, const uint8_t* bottom, uint32_t weight, uint8_t* out, size_t bytes) {
    const __m128i zero = _mm_setzero_si128(), w0 = _mm_set1_epi16(short(256 - weight)), w1 = _mm_set1_epi16(short(weight));
    size_t i = 0;
    for (; i + 16 <= bytes; i += 16) {
      const __m128i t = _mm_loadu_si128(reinterpret_cast<const __m128i*>(top + i)), b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bottom + i));
      const __m128i lo = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(t, zero), w0), _mm_mullo_epi16(_mm_unpacklo_epi8(b, zero), w1)), 8);
      const __m128i hi = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(t, zero), w0), _mm_mullo_epi16(_mm_unpackhi_epi8(b, zero), w1)), 8);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), _mm_packus_epi16(lo, hi));
    }
    for (; i < bytes; ++i) out[i] = uint8_t((uint32_t(top[i]) * (256 - weight) + uint32_t(bottom[i]) * weight) >> 8);
  }
  uint32_t sw_ = 0, sh_ = 0, tw_ = 0, th_ = 0;
  std::vector<uint32_t> columns_, weightsX_, rows_, weightsY_;
  std::vector<__m128i> vectors_;
  Slot slots_[2];
};
}
