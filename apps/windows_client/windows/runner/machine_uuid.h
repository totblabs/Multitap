#pragma once
// The SMBIOS system UUID of the PC this app runs on, read the way the Host
// reads its own (apps/windows_host/src/support.hpp machineUuid): the raw
// 'RSMB' firmware table, the type 1 (System Information) structure, as
// lowercase GUID text without braces, with the first three fields taken
// big-endian before SMBIOS 2.6 and little-endian from 2.6 on.
// The app compares a value derived from it with each listed desktop
// (lib/data/same_pc.dart) and refuses to connect to the desktop whose Host
// runs on the same PC. Nothing here leaves the PC.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace multitap {

// `data`: a whole RawSMBIOSData block (calling method, major version, minor
// version, DMI revision, a 32-bit table length, then the structures).
// No value for a table without a usable UUID: no type 1 structure, one too
// short to hold a UUID, or a UUID of all 0x00 or all 0xFF (not set).
inline std::optional<std::string> MachineUuidFromSmbios(const uint8_t* data, size_t size) {
  if (!data || size < 8) return std::nullopt;
  uint32_t length = 0;
  std::memcpy(&length, data + 4, 4);
  const size_t end = (std::min)(size, static_cast<size_t>(length) + 8);
  for (size_t at = 8; at + 4 <= end;) {
    const uint8_t type = data[at], count = data[at + 1];
    if (count < 4 || at + count > end) break;
    if (type == 1 && count >= 24) {
      const uint8_t* bytes = data + at + 8;
      if (std::all_of(bytes, bytes + 16, [](uint8_t b) { return b == 0; }) ||
          std::all_of(bytes, bytes + 16, [](uint8_t b) { return b == 255; })) {
        break;
      }
      const bool bigEndian = data[1] < 2 || (data[1] == 2 && data[2] < 6);
      const uint8_t ordered[8] = {
          bytes[bigEndian ? 0 : 3], bytes[bigEndian ? 1 : 2], bytes[bigEndian ? 2 : 1], bytes[bigEndian ? 3 : 0],
          bytes[bigEndian ? 4 : 5], bytes[bigEndian ? 5 : 4],
          bytes[bigEndian ? 6 : 7], bytes[bigEndian ? 7 : 6]};
      char text[37]{};
      std::snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    ordered[0], ordered[1], ordered[2], ordered[3], ordered[4], ordered[5], ordered[6], ordered[7],
                    bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
      return std::string(text, 36);
    }
    // The formatted area, then its strings up to the double NUL.
    at += count;
    while (at + 1 < end && (data[at] != 0 || data[at + 1] != 0)) at++;
    at += 2;
  }
  return std::nullopt;
}

// No value when Windows gives no SMBIOS table or the table has no usable UUID.
inline std::optional<std::string> MachineUuid() {
  constexpr DWORD rawSmbios = 0x52534d42;  // 'RSMB'
  const DWORD size = GetSystemFirmwareTable(rawSmbios, 0, nullptr, 0);
  if (size < 8 || size > 1024 * 1024) return std::nullopt;
  std::vector<uint8_t> data(size);
  if (GetSystemFirmwareTable(rawSmbios, 0, data.data(), size) != size) return std::nullopt;
  return MachineUuidFromSmbios(data.data(), data.size());
}

}  // namespace multitap
