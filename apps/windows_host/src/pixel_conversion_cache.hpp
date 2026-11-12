#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace host {
// A conservative hint only avoids cache work during active capture/cursor
// changes. Reuse still requires exact final pixels, after cursor composition.
// One instance belongs to one immutable Capture geometry; resize recreates it.
class PixelConversionCache {
 public:
  bool reuse(const uint8_t* pixels, size_t bytes, bool eligible) {
    if (!pixels || !bytes || bytes / 4 > std::numeric_limits<uint32_t>::max() || bytes % 4)
      throw std::invalid_argument("Invalid pixel conversion cache frame");
    if (!eligible) { valid_ = false; return false; }
    if (valid_ && pixels_.size() == bytes && std::memcmp(pixels_.data(), pixels, bytes) == 0)
      return true;
    pixels_.assign(pixels, pixels + bytes);
    valid_ = true;
    return false;
  }
 private:
  std::vector<uint8_t> pixels_;
  bool valid_ = false;
};
}
