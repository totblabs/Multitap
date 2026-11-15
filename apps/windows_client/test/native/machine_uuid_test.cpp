// SMBIOS system UUID reader (windows/runner/machine_uuid.h): the same text
// the Host registers for the same PC. `HostReference` below is the Host's own
// reader (apps/windows_host/src/support.hpp machineUuid) on a given table;
// every table here must read the same through both.
// Optional argument: the UUID Windows itself reports for this PC
// (Win32_ComputerSystemProduct.UUID); the live read must equal it.
#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <optional>
#include <string>
#include <vector>

#include "machine_uuid.h"

namespace {
int failures = 0, checks = 0;
void Check(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}

std::optional<std::string> HostReference(const std::vector<uint8_t>& data) {
  const DWORD size = static_cast<DWORD>(data.size());
  if (size < 8) return std::nullopt;
  DWORD length = 0; std::memcpy(&length, data.data() + 4, 4); const size_t end = (std::min)(size_t(size), size_t(length) + 8);
  for (size_t at = 8; at + 4 <= end;) {
    const uint8_t type = data[at], count = data[at + 1]; if (count < 4 || at + count > end) break;
    if (type == 1 && count >= 24) {
      const auto* bytes = data.data() + at + 8;
      if (std::all_of(bytes, bytes + 16, [](uint8_t b) { return b == 0; }) || std::all_of(bytes, bytes + 16, [](uint8_t b) { return b == 255; })) break;
      GUID guid{}; std::memcpy(&guid, bytes, 16);
      if (data[1] < 2 || (data[1] == 2 && data[2] < 6)) { guid.Data1 = (DWORD(bytes[0]) << 24) | (DWORD(bytes[1]) << 16) | (DWORD(bytes[2]) << 8) | bytes[3]; guid.Data2 = WORD((WORD(bytes[4]) << 8) | bytes[5]); guid.Data3 = WORD((WORD(bytes[6]) << 8) | bytes[7]); }
      wchar_t text[40]{}; StringFromGUID2(guid, text, 40); std::wstring value(text + 1, text + 37); for (auto& ch : value) ch = wchar_t(towlower(ch));
      std::string narrow; for (const auto ch : value) narrow.push_back(static_cast<char>(ch));
      return narrow;
    }
    at += count; while (at + 1 < end && (data[at] != 0 || data[at + 1] != 0)) at++; at += 2;
  }
  return std::nullopt;
}

// One structure: type, formatted length, a handle, the remaining formatted
// bytes, then its strings and the closing double NUL.
std::vector<uint8_t> Structure(uint8_t type, const std::vector<uint8_t>& body, const std::vector<std::string>& strings = {}) {
  std::vector<uint8_t> out{type, static_cast<uint8_t>(4 + body.size()), 0, 0};
  out.insert(out.end(), body.begin(), body.end());
  for (const auto& text : strings) { out.insert(out.end(), text.begin(), text.end()); out.push_back(0); }
  if (strings.empty()) out.push_back(0);
  out.push_back(0);
  return out;
}
// A type 1 structure of SMBIOS 2.1 and later: four string numbers, the UUID,
// the wake-up type.
std::vector<uint8_t> System(const uint8_t uuid[16], bool strings = true) {
  std::vector<uint8_t> body{1, 2, 3, 4};
  body.insert(body.end(), uuid, uuid + 16);
  body.push_back(6);
  return Structure(1, body, strings ? std::vector<std::string>{"Maker", "Model", "1.0", "SN"} : std::vector<std::string>{});
}
std::vector<uint8_t> Table(uint8_t major, uint8_t minor, const std::vector<std::vector<uint8_t>>& structures) {
  std::vector<uint8_t> data{0, major, minor, 0, 0, 0, 0, 0};
  for (const auto& item : structures) data.insert(data.end(), item.begin(), item.end());
  const uint32_t length = static_cast<uint32_t>(data.size() - 8);
  std::memcpy(data.data() + 4, &length, 4);
  return data;
}
std::optional<std::string> Read(const std::vector<uint8_t>& data) { return multitap::MachineUuidFromSmbios(data.data(), data.size()); }
bool Same(const std::vector<uint8_t>& data) { return Read(data) == HostReference(data); }
std::string Lower(std::string text) { for (auto& ch : text) if (ch >= 'A' && ch <= 'F') ch = static_cast<char>(ch - 'A' + 'a'); return text; }
}  // namespace

int main(int argc, char** argv) {
  const uint8_t uuid[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  const uint8_t zero[16] = {};
  uint8_t unset[16]; std::memset(unset, 0xff, sizeof(unset));
  uint8_t nearlyZero[16] = {}; nearlyZero[15] = 1;
  uint8_t capitals[16] = {0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89};
  const auto bios = Structure(0, std::vector<uint8_t>(20, 7), {"Vendor", "Version", "01/01/2024"});
  const auto board = Structure(2, std::vector<uint8_t>(11, 3), {"Board"});
  const auto endOfTable = Structure(127, {});

  // SMBIOS 2.6 and later: the first three fields are little-endian.
  const uint8_t later[][2] = {{2, 6}, {2, 8}, {3, 0}, {3, 6}};
  for (const auto& version : later) {
    const auto table = Table(version[0], version[1], {bios, System(uuid), board, endOfTable});
    Check(Read(table) == std::string("33221100-5544-7766-8899-aabbccddeeff"), "2.6+ reads the fields little-endian");
    Check(Same(table), "2.6+ equals the Host's reader");
  }
  // Before 2.6: as written.
  const uint8_t earlier[][2] = {{2, 1}, {2, 4}, {2, 5}, {1, 9}};
  for (const auto& version : earlier) {
    const auto table = Table(version[0], version[1], {bios, System(uuid), endOfTable});
    Check(Read(table) == std::string("00112233-4455-6677-8899-aabbccddeeff"), "before 2.6 reads the fields as written");
    Check(Same(table), "before 2.6 equals the Host's reader");
  }
  // Lowercase text, 36 characters, hyphens in place.
  {
    const auto text = Read(Table(3, 2, {System(capitals)}));
    Check(text == std::string("01efcdab-4523-8967-abcd-ef0123456789"), "lowercase hexadecimal");
    Check(text && text->size() == 36 && (*text)[8] == '-' && (*text)[13] == '-' && (*text)[18] == '-' && (*text)[23] == '-', "GUID text without braces");
    Check(Same(Table(3, 2, {System(capitals)})), "capitals equal the Host's reader");
  }
  // The system structure first, without strings, or after several others.
  Check(Read(Table(3, 0, {System(uuid, false)})) == std::string("33221100-5544-7766-8899-aabbccddeeff"), "a structure without strings");
  Check(Read(Table(3, 0, {bios, board, bios, System(uuid), endOfTable})) == std::string("33221100-5544-7766-8899-aabbccddeeff"), "after several structures");
  // Not set, or not there: no value (the app then never refuses a connection).
  Check(!Read(Table(3, 0, {bios, System(zero), endOfTable})), "all zero is not a UUID");
  Check(!Read(Table(3, 0, {bios, System(unset), endOfTable})), "all 0xFF is not a UUID");
  Check(Read(Table(3, 0, {System(nearlyZero)})) == std::string("00000000-0000-0000-0000-000000000001"), "one set bit is a UUID");
  Check(Same(Table(3, 0, {System(nearlyZero)})), "one set bit equals the Host's reader");
  Check(!Read(Table(3, 0, {bios, board, endOfTable})), "no system structure");
  Check(!Read(Table(2, 0, {bios, Structure(1, {1, 2, 3, 4}, {"Maker", "Model", "1.0", "SN"}), endOfTable})), "a 2.0 system structure has no UUID");
  Check(!Read(Table(3, 0, {})), "an empty table");
  Check(!multitap::MachineUuidFromSmbios(nullptr, 0), "no table");
  {
    const std::vector<uint8_t> tiny{0, 3, 0, 0, 0, 0, 0};
    Check(!Read(tiny), "a header cut short");
  }
  // A table cut inside the system structure, and a length field that ends
  // before it: never read past either end.
  {
    auto table = Table(3, 0, {bios, System(uuid), endOfTable});
    auto cut = table;
    cut.resize(8 + bios.size() + 12);
    Check(!Read(cut), "cut inside the structure");
    Check(Same(cut), "cut inside the structure equals the Host's reader");
    auto shortLength = table;
    const uint32_t length = static_cast<uint32_t>(bios.size());
    std::memcpy(shortLength.data() + 4, &length, 4);
    Check(!Read(shortLength), "the length field ends the table");
    auto longLength = table;
    const uint32_t tooLong = 0x00ffffff;
    std::memcpy(longLength.data() + 4, &tooLong, 4);
    Check(Read(longLength) == std::string("33221100-5544-7766-8899-aabbccddeeff"), "a length beyond the block stops at the block");
    Check(Same(longLength), "a length beyond the block equals the Host's reader");
    auto badCount = Table(3, 0, {std::vector<uint8_t>{0, 2, 0, 0, 0, 0}, System(uuid)});
    Check(!Read(badCount), "a structure shorter than its header ends the read");
    Check(Same(badCount), "a short structure equals the Host's reader");
  }

  // This PC: the reader agrees with the Host's on the live table, and with
  // Windows' own answer when one was passed. The UUID is never printed.
  constexpr DWORD rawSmbios = 0x52534d42;
  const DWORD size = GetSystemFirmwareTable(rawSmbios, 0, nullptr, 0);
  std::vector<uint8_t> live(size);
  if (size >= 8 && GetSystemFirmwareTable(rawSmbios, 0, live.data(), size) == size) {
    const auto mine = multitap::MachineUuid();
    Check(mine == HostReference(live), "the live table equals the Host's reader");
    Check(mine == Read(live), "the live read is the parsed live table");
    std::printf("note: live SMBIOS UUID %s\n", mine ? "read" : "not available");
    if (argc > 1) {
      Check(mine && *mine == Lower(argv[1]), "the live read equals the UUID Windows reports");
    }
  } else {
    std::printf("note: no SMBIOS table on this PC; the live read was not checked\n");
    Check(!multitap::MachineUuid(), "no table gives no value");
    if (argc > 1) Check(false, "a UUID was passed but no table could be read");
  }

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
