#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

// Checked representation/admission only. These helpers cannot establish that
// an AMD device, driver, encoder, output frame or installed session worked.
namespace host::gpu::amf_detail {
struct Timing { int64_t pts = 0, duration = 0; };
inline constexpr std::optional<Timing> timing(uint64_t ptsUs, uint32_t fps, bool havePrevious, uint64_t previousUs) {
  if (!fps || fps > 60 || ptsUs > uint64_t(INT64_MAX) / 10 || (havePrevious && ptsUs <= previousUs)) return {};
  return Timing{int64_t(ptsUs * 10), int64_t(10000000 / fps)}; // AMF uses100ns, media uses microseconds.
}
inline constexpr std::optional<uint64_t> macroblocksPerSecond(uint32_t width, uint32_t height, uint32_t fps) {
  if (!width || !height || !fps || (width & 1) || (height & 1)) return {};
  const uint64_t blocks = ((uint64_t(width) + 15) / 16) * ((uint64_t(height) + 15) / 16);
  if (blocks > UINT64_MAX / fps) return {};
  return blocks * fps;
}
inline constexpr bool throughputFits(std::optional<uint64_t> requested, int64_t maximum, int64_t inUse) {
  return requested && maximum > 0 && inUse >= 0 && inUse <= maximum && *requested <= uint64_t(maximum - inUse);
}
// H.264 Annex A limits, cross-checked with the project's pinned GPLv2-or-later
// x264 b35605a common/tables.c (x264_levels) and encoder/set.c. Numeric limits
// are Baseline/Main, without High-profile bitrate multipliers. AMF has no 1b
// enumerator. Our encoder explicitly requests progressive, one reference,
// no B, no LTR and one temporal layer; its actual driver/output still decide.
struct Level { int64_t value; uint64_t macroblocksPerSecond, macroblocksPerFrame, bitrate, cpb; };
inline constexpr Level levels[] = {
  {10,1485,99,64000,175000}, {11,3000,396,192000,500000},
  {12,6000,396,384000,1000000}, {13,11880,396,768000,2000000},
  {20,11880,396,2000000,2000000}, {21,19800,792,4000000,4000000},
  {22,20250,1620,4000000,4000000}, {30,40500,1620,10000000,10000000},
  {31,108000,3600,14000000,14000000}, {32,216000,5120,20000000,20000000},
  {40,245760,8192,20000000,25000000}, {41,245760,8192,50000000,62500000},
  {42,522240,8704,50000000,62500000}, {50,589824,22080,135000000,135000000},
  {51,983040,36864,240000000,240000000}, {52,2073600,36864,240000000,240000000},
  {60,4177920,139264,240000000,240000000}, {61,8355840,139264,480000000,480000000},
  {62,16711680,139264,800000000,800000000}
};
inline constexpr std::optional<Level> selectLevel(uint32_t width, uint32_t height, uint32_t fps,
    uint64_t bitrate, uint64_t vbv, int64_t reportedMaximum) {
  bool knownMaximum = false;
  for (const auto& level : levels) if (level.value == reportedMaximum) knownMaximum = true;
  const auto rate = macroblocksPerSecond(width, height, fps);
  if (!knownMaximum || !rate || !bitrate || !vbv) return {};
  const uint64_t columns = (uint64_t(width) + 15) / 16, rows = (uint64_t(height) + 15) / 16;
  for (const auto& level : levels) {
    if (level.value > reportedMaximum) break;
    if (columns * rows <= level.macroblocksPerFrame && columns * columns <= level.macroblocksPerFrame * 8 &&
        rows * rows <= level.macroblocksPerFrame * 8 && *rate <= level.macroblocksPerSecond &&
        bitrate <= level.bitrate && vbv <= level.cpb) return level;
  }
  return {};
}
struct PlaneLayout { size_t pitch = 0, rows = 0, bytes = 0; };
inline constexpr std::optional<PlaneLayout> planeLayout(uint32_t pixels, uint32_t rows, uint32_t pixelBytes,
    int32_t actualPixels, int32_t actualRows, int32_t actualPixelBytes,
    int32_t offsetX, int32_t offsetY, int32_t pitch, int32_t verticalPitch) {
  if (!pixels || !rows || !pixelBytes || actualPixels <= 0 || actualRows <= 0 ||
      uint32_t(actualPixels) < pixels || uint32_t(actualRows) < rows || actualPixelBytes <= 0 || uint32_t(actualPixelBytes) != pixelBytes ||
      offsetX || offsetY || pitch <= 0 || verticalPitch <= 0 || uint32_t(verticalPitch) < rows ||
      uint64_t(pitch) < uint64_t(pixels) * pixelBytes) return {};
  const uint64_t bytes = uint64_t(pitch) * uint32_t(verticalPitch);
  if (bytes > std::numeric_limits<size_t>::max()) return {};
  return PlaneLayout{size_t(pitch), size_t(rows), size_t(bytes)};
}
inline constexpr bool planesFit(const PlaneLayout& y, const PlaneLayout& uv, uint64_t available) {
  return uint64_t(y.bytes) <= UINT64_MAX - uint64_t(uv.bytes) && uint64_t(y.bytes) + uint64_t(uv.bytes) <= available;
}
}
