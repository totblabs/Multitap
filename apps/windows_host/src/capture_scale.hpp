#pragma once
#include "owned_output_size.hpp"
#include <emmintrin.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace host::capture {
// Area-average BGRA resampler for the scaled capture path (encoded video smaller
// than the captured owned output). It replaces GDI StretchDIBits(HALFTONE),
// which re-filtered the whole source on EVERY tick: field data
// (2560x1440 -> 1920x1080) showed ~30 ms per tick, ~20 fps and ~100 ms
// input-to-frame. Each output pixel is the exact coverage-weighted average of
// the source pixels its footprint covers (a box filter, like HALFTONE), in two
// separable passes: vertical (rows -> one source-width row) then horizontal.
// Fixed point: per-axis 8-bit weights summing to exactly 256 (cumulative
// rounding), each pass rounds to 8 bits. Alpha is filtered like the colour
// channels. scale() and scaleReference() produce identical bytes.
class AreaScaler {
 public:
  AreaScaler(int sourceWidth, int sourceHeight, int targetWidth, int targetHeight)
      : sourceWidth_(sourceWidth), sourceHeight_(sourceHeight), targetWidth_(targetWidth), targetHeight_(targetHeight) {
    if (!checkedFrameLayout(uint64_t(sourceWidth),uint64_t(sourceHeight),false) ||
        !checkedFrameLayout(uint64_t(targetWidth),uint64_t(targetHeight),false))
      throw std::runtime_error("Invalid capture scale geometry");
    axis(sourceWidth, targetWidth, columns_, columnWeights_, columnTaps_);
    axis(sourceHeight, targetHeight, rows_, rowWeights_, rowTaps_);
    // Horizontal taps are read in pairs of pixels; the last pair may run one
    // pixel past the row (weight 0), so the row buffer is padded.
    pairs_ = (columnTaps_ + 1) / 2;
    pairWeights_.assign(count<uint16_t>(uint64_t(targetWidth),uint64_t(pairs_)*8), 0);
    for (int x = 0; x < targetWidth; ++x)
      for (int t = 0; t < columnTaps_; ++t)
        for (int lane = 0; lane < 4; ++lane)
          pairWeights_[(size_t(x) * pairs_ + t / 2) * 8 + (t & 1) * 4 + lane] = columnWeights_[size_t(x) * columnTaps_ + t];
    const uint64_t rowBytes=uint64_t(sourceWidth)*4+uint64_t(pairs_)*8+16;
    row_.assign(count<uint8_t>(rowBytes,1), 0);
  }
  int sourceWidth() const { return sourceWidth_; }
  int sourceHeight() const { return sourceHeight_; }
  int targetWidth() const { return targetWidth_; }
  int targetHeight() const { return targetHeight_; }
  // source: sourceHeight rows of sourceWidth BGRA pixels, `sourcePitch` bytes
  // apart; target: targetHeight rows of targetWidth pixels, `targetPitch` apart.
  void scale(const uint8_t* source, size_t sourcePitch, uint8_t* target, size_t targetPitch) {
    check(source, sourcePitch, target, targetPitch);
    const size_t bytes = size_t(sourceWidth_) * 4;
    const __m128i zero = _mm_setzero_si128(), bias = _mm_set1_epi16(128);
    for (int y = 0; y < targetHeight_; ++y) {
      const int first = rows_[y];
      const uint16_t* weights = rowWeights_.data() + size_t(y) * rowTaps_;
      int taps = rowTaps_;
      while (taps > 1 && !weights[taps - 1]) --taps;
      uint8_t* row = row_.data();
      size_t x = 0;
      for (; x + 16 <= bytes; x += 16) {
        __m128i lo = _mm_setzero_si128(), hi = _mm_setzero_si128();
        for (int t = 0; t < taps; ++t) {
          const __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + size_t(first + t) * sourcePitch + x));
          const __m128i weight = _mm_set1_epi16(short(weights[t]));
          lo = _mm_add_epi16(lo, _mm_mullo_epi16(_mm_unpacklo_epi8(value, zero), weight));
          hi = _mm_add_epi16(hi, _mm_mullo_epi16(_mm_unpackhi_epi8(value, zero), weight));
        }
        lo = _mm_srli_epi16(_mm_add_epi16(lo, bias), 8);
        hi = _mm_srli_epi16(_mm_add_epi16(hi, bias), 8);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(row + x), _mm_packus_epi16(lo, hi));
      }
      for (; x < bytes; ++x) {
        uint32_t sum = 0;
        for (int t = 0; t < taps; ++t) sum += uint32_t(weights[t]) * source[size_t(first + t) * sourcePitch + x];
        row[x] = uint8_t((sum + 128) >> 8);
      }
      uint8_t* out = target + size_t(y) * targetPitch;
      const auto* pairWeights = reinterpret_cast<const __m128i*>(pairWeights_.data());
      for (int column = 0; column < targetWidth_; ++column) {
        const uint8_t* pixels = row + size_t(columns_[column]) * 4;
        const __m128i* weight = pairWeights + size_t(column) * pairs_;
        __m128i sum = _mm_setzero_si128();
        for (int p = 0; p < pairs_; ++p) {
          const __m128i two = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(pixels + size_t(p) * 8)), zero);
          sum = _mm_add_epi16(sum, _mm_mullo_epi16(two, _mm_loadu_si128(weight + p)));
        }
        sum = _mm_add_epi16(sum, _mm_srli_si128(sum, 8));
        sum = _mm_srli_epi16(_mm_add_epi16(sum, bias), 8);
        const int packed = _mm_cvtsi128_si32(_mm_packus_epi16(sum, sum));
        std::memcpy(out + size_t(column) * 4, &packed, 4);
      }
    }
  }
  // Plain scalar statement of the same arithmetic (tests only).
  void scaleReference(const uint8_t* source, size_t sourcePitch, uint8_t* target, size_t targetPitch) const {
    check(source, sourcePitch, target, targetPitch);
    std::vector<uint8_t> row(size_t(sourceWidth_) * 4);
    for (int y = 0; y < targetHeight_; ++y) {
      for (size_t x = 0; x < row.size(); ++x) {
        uint32_t sum = 0;
        for (int t = 0; t < rowTaps_ && rows_[y] + t < sourceHeight_; ++t) sum += uint32_t(rowWeights_[size_t(y) * rowTaps_ + t]) * source[size_t(rows_[y] + t) * sourcePitch + x];
        row[x] = uint8_t((sum + 128) >> 8);
      }
      for (int column = 0; column < targetWidth_; ++column)
        for (int c = 0; c < 4; ++c) {
          uint32_t sum = 0;
          for (int t = 0; t < columnTaps_ && columns_[column] + t < sourceWidth_; ++t) sum += uint32_t(columnWeights_[size_t(column) * columnTaps_ + t]) * row[size_t(columns_[column] + t) * 4 + c];
          target[size_t(y) * targetPitch + size_t(column) * 4 + c] = uint8_t((sum + 128) >> 8);
        }
    }
  }
 private:
  template<class T> static size_t count(uint64_t a,uint64_t b){
    const auto maximum=std::vector<T>{}.max_size();
    if(a>maximum||(b&&a>maximum/b))throw std::runtime_error("Invalid capture scale allocation");
    return size_t(a*b);
  }
  void check(const uint8_t* source, size_t sourcePitch, const uint8_t* target, size_t targetPitch) const {
    const auto maximum=uint64_t(std::numeric_limits<ptrdiff_t>::max());
    if (!source || !target || sourcePitch < size_t(sourceWidth_) * 4 || targetPitch < size_t(targetWidth_) * 4 ||
        sourcePitch>maximum/uint64_t(sourceHeight_)||targetPitch>maximum/uint64_t(targetHeight_))
      throw std::runtime_error("Invalid capture scale buffers");
  }
  // Output i covers source [i*S/T, (i+1)*S/T). In units of 1/T source pixel the
  // coverage of source pixel j is an exact integer; 8-bit weights come from the
  // rounded cumulative coverage, so they sum to exactly 256.
  static void axis(int source, int target, std::vector<int>& first, std::vector<uint16_t>& weights, int& taps) {
    first.assign(count<int>(uint64_t(target),1), 0); taps = 1;
    for (int i = 0; i < target; ++i) {
      const int64_t a = int64_t(i) * source, b = int64_t(i + 1) * source;
      const int j0 = int(a / target), j1 = int(std::min<int64_t>(source, (b + target - 1) / target));
      taps = std::max(taps, j1 - j0);
    }
    weights.assign(count<uint16_t>(uint64_t(target),uint64_t(taps)), 0);
    for (int i = 0; i < target; ++i) {
      const int64_t a = int64_t(i) * source, b = int64_t(i + 1) * source;
      const int j0 = int(a / target), j1 = int(std::min<int64_t>(source, (b + target - 1) / target));
      first[size_t(i)] = j0;
      int64_t covered = 0; int previous = 0;
      for (int j = j0; j < j1; ++j) {
        const int64_t overlap = std::max<int64_t>(0, std::min<int64_t>(int64_t(j + 1) * target, b) - std::max<int64_t>(int64_t(j) * target, a));
        covered += overlap;
        const int cumulative = j + 1 == j1 ? 256 : int((covered * 256 + source / 2) / source);
        weights[size_t(i) * taps + (j - j0)] = uint16_t(cumulative - previous); previous = cumulative;
      }
    }
  }
  int sourceWidth_, sourceHeight_, targetWidth_, targetHeight_;
  std::vector<int> columns_, rows_;
  std::vector<uint16_t> columnWeights_, rowWeights_, pairWeights_;
  int columnTaps_ = 1, rowTaps_ = 1, pairs_ = 1;
  std::vector<uint8_t> row_;
};
}
