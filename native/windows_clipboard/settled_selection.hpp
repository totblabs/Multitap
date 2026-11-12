#pragma once
#include <cstdint>
#include <stdexcept>
namespace multitap::clipboard {
inline uint32_t settledQuietRemaining(uint64_t observed, uint64_t now) {
  const auto elapsed = now >= observed ? now - observed : 0;
  return elapsed >= 250 ? 0 : uint32_t(250 - elapsed);
}
inline bool settledTextImageHint(bool negotiated, bool textAllowed, bool imageAllowed, uint32_t formats) {
  return negotiated && (formats & 7u) == 0 && ((textAllowed && (formats & 8u)) || (imageAllowed && (formats & 16u)));
}
// Metadata only. Used again under the clipboard read lock before any format
// data is fetched. Files/shell/virtual sources never enter this profile.
inline void validateSettledSelection(uint32_t expected, uint32_t actual, uint32_t formats) {
  if (actual != expected) throw std::runtime_error("clipboard_changed");
  if ((formats & 7u) != 0 || (formats & 24u) == 0) throw std::runtime_error("unsupported_clipboard_format");
}
}
