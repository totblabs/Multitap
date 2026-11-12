#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace host::gpu {
enum class Vendor : uint8_t { unknown, nvidia, amd, intel };
inline constexpr Vendor vendorFromId(uint32_t value) {
  return value == 0x10de ? Vendor::nvidia : value == 0x1002 ? Vendor::amd : value == 0x8086 ? Vendor::intel : Vendor::unknown;
}
inline constexpr const char* vendorName(Vendor value) {
  return value == Vendor::nvidia ? "nvidia" : value == Vendor::amd ? "amd" : value == Vendor::intel ? "intel" : "unknown";
}
// A DXGI LUID is a current-machine adapter identity, not an ordinal or a
// promise that this device has a working hardware video encoder.
inline std::optional<uint64_t> parseAdapterLuid(std::string_view text) {
  if (text.size() != 17 || text[8] != ':') return {};
  uint64_t value = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if (i == 8) continue;
    const char c = text[i];
    const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (digit < 0) return {};
    value = (value << 4) | uint64_t(digit);
  }
  return value;
}
inline std::string adapterLuidText(uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result(17, '0'); result[8] = ':';
  for (int i = 16; i >= 0; --i) if (i != 8) { result[size_t(i)] = digits[value & 15]; value >>= 4; }
  return result;
}
struct Limits {
  uint32_t minimumWidth = 0, minimumHeight = 0, maximumWidth = 0, maximumHeight = 0;
  uint64_t maximumMacroblocks = 0, maximumMacroblocksPerSecond = 0;
};
inline constexpr bool pictureFits(const Limits& limits, uint32_t width, uint32_t height, uint32_t fps) {
  if (!width || !height || !fps || (width & 1) || (height & 1) || !limits.maximumWidth || !limits.maximumHeight ||
      width < limits.minimumWidth || height < limits.minimumHeight || width > limits.maximumWidth || height > limits.maximumHeight) return false;
  const uint64_t macroblocks = ((uint64_t(width) + 15) / 16) * ((uint64_t(height) + 15) / 16);
  if (limits.maximumMacroblocks && macroblocks > limits.maximumMacroblocks) return false;
  // Division avoids overflow for untrusted requests. Zero means the API did
  // not provide this optional limit, not an invented product-level ceiling.
  return !limits.maximumMacroblocksPerSecond || macroblocks <= limits.maximumMacroblocksPerSecond / fps;
}
struct AccessUnit {
  bool valid = false, idr = false, sps = false, pps = false;
  uint8_t spsProfile = 0, spsLevel = 0;
};
inline AccessUnit inspectAnnexB(std::span<const uint8_t> bytes, size_t maximumBytes) {
  AccessUnit result;
  if (bytes.empty() || bytes.size() > maximumBytes) return result;
  const auto start = [&](size_t at) -> size_t {
    if (at + 3 <= bytes.size() && bytes[at] == 0 && bytes[at + 1] == 0 && bytes[at + 2] == 1) return 3;
    if (at + 4 <= bytes.size() && bytes[at] == 0 && bytes[at + 1] == 0 && bytes[at + 2] == 0 && bytes[at + 3] == 1) return 4;
    return 0;
  };
  size_t at = 0; bool picture = false;
  while (at < bytes.size()) {
    const size_t prefix = start(at);
    if (!prefix || prefix >= bytes.size() - at) return {};
    at += prefix;
    const uint8_t header = bytes[at], type = header & 31;
    if ((header & 128) || !type || type >= 24) return {};
    size_t next = at + 1;
    while (next < bytes.size() && !start(next)) ++next;
    if (next == at + 1) return {};
    if (type == 5) { result.idr = true; picture = true; }
    else if (type == 1) picture = true;
    else if (type == 7) {
      if (next - at < 4 || (result.sps && (result.spsProfile != bytes[at + 1] || result.spsLevel != bytes[at + 3]))) return {};
      result.sps = true; result.spsProfile = bytes[at + 1]; result.spsLevel = bytes[at + 3];
    } else if (type == 8) result.pps = true;
    at = next;
  }
  result.valid = picture; return result;
}
}
