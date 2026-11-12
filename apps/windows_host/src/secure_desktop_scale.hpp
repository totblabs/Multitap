#pragma once
// BGRA scale for Role::secure: the SYSTEM helper captures the owned output at its
// native size; the worker scales to the encoder size (they differ when the
// stream encodes below the output's size) before the NV12 conversion. Shrinking
// averages every source pixel a destination pixel covers, so the prompt's text
// stays readable; enlarging repeats the nearest pixel. Kept in its own light
// header (no Windows, no DXGI) so it is unit-tested without the VM.
#include <cstdint>
#include <cstring>

namespace host::secure {

inline void scaleBgra(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, uint32_t dw, uint32_t dh) {
  if (!src || !dst || !sw || !sh || !dw || !dh) return;
  if (sw == dw && sh == dh) { std::memcpy(dst, src, size_t(sw) * sh * 4); return; }
  for (uint32_t y = 0; y < dh; ++y) {
    // Source rows [y0, y1) this destination row covers (at least one).
    const uint32_t y0 = uint32_t(uint64_t(y) * sh / dh);
    uint32_t y1 = uint32_t(uint64_t(y + 1) * sh / dh);
    if (y1 <= y0) y1 = y0 + 1;
    if (y1 > sh) y1 = sh;
    uint8_t* drow = dst + size_t(y) * dw * 4;
    for (uint32_t x = 0; x < dw; ++x) {
      const uint32_t x0 = uint32_t(uint64_t(x) * sw / dw);
      uint32_t x1 = uint32_t(uint64_t(x + 1) * sw / dw);
      if (x1 <= x0) x1 = x0 + 1;
      if (x1 > sw) x1 = sw;
      if (x1 - x0 == 1 && y1 - y0 == 1) { std::memcpy(drow + size_t(x) * 4, src + (size_t(y0) * sw + x0) * 4, 4); continue; }
      uint32_t sum[4] = {0, 0, 0, 0};
      for (uint32_t sy = y0; sy < y1; ++sy) {
        const uint8_t* p = src + (size_t(sy) * sw + x0) * 4;
        for (uint32_t sx = x0; sx < x1; ++sx, p += 4) { sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3]; }
      }
      const uint32_t count = (x1 - x0) * (y1 - y0);
      uint8_t* d = drow + size_t(x) * 4;
      d[0] = uint8_t((sum[0] + count / 2) / count); d[1] = uint8_t((sum[1] + count / 2) / count);
      d[2] = uint8_t((sum[2] + count / 2) / count); d[3] = uint8_t((sum[3] + count / 2) / count);
    }
  }
}

}  // namespace host::secure
