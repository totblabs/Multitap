#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

namespace host {
// The input side of x264's mb_info hint (X264_MBINFO_CONSTANT, docs/x264-encoder.md):
// flags[i] = 1 for every 16x16 macroblock (row-major, ceil(width/16) per row,
// ceil(height/16) rows) whose NV12 pixels (16x16 luma and its 16x8 bytes of
// interleaved UV) equal `previous`, otherwise 0. `previous` then holds `current`;
// only differing 16-byte runs are written. Even width and height; a partial
// macroblock at the right or bottom edge compares the pixels that exist (x264
// pads the picture by repeating its edge). Returns the constant macroblocks.
inline size_t markConstantMacroblocks(const uint8_t* current, uint8_t* previous, int width, int height, uint8_t* flags) {
  const size_t columns = size_t(width + 15) / 16, rows = size_t(height + 15) / 16;
  std::fill(flags, flags + columns * rows, uint8_t(1));
  const auto row = [&](const uint8_t* now, uint8_t* before, uint8_t* rowFlags) {
    size_t column = 0; int x = 0;
    for (; x + 16 <= width; x += 16, ++column) {
      const auto a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(now + x));
      if (_mm_movemask_epi8(_mm_cmpeq_epi8(a, _mm_loadu_si128(reinterpret_cast<const __m128i*>(before + x)))) != 0xffff) {
        rowFlags[column] = 0; _mm_storeu_si128(reinterpret_cast<__m128i*>(before + x), a);
      }
    }
    if (x < width && std::memcmp(now + x, before + x, size_t(width - x)) != 0) { rowFlags[column] = 0; std::memcpy(before + x, now + x, size_t(width - x)); }
  };
  const size_t plane = size_t(width) * size_t(height);
  for (int y = 0; y < height; ++y) row(current + size_t(y) * width, previous + size_t(y) * width, flags + size_t(y / 16) * columns);
  for (int y = 0; y < height / 2; ++y) row(current + plane + size_t(y) * width, previous + plane + size_t(y) * width, flags + size_t(y / 8) * columns);
  return size_t(std::count(flags, flags + columns * rows, uint8_t(1)));
}
}
