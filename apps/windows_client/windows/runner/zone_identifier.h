#pragma once
// Mark-of-the-Web for files this PC writes on behalf of the remote desktop
// (a shared folder's files, a "PDF로 저장" print): an Internet zone
// Zone.Identifier stream, so SmartScreen, Office Protected View and the
// attachment checks treat them as downloaded. The text is the one
// native/windows_clipboard writes for received clipboard files: fixed URLs,
// no server, account or file name.
#include <windows.h>

#include <cstring>
#include <string>

namespace multitap {

inline constexpr char kInternetZoneIdentifier[] =
    "[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=about:internet\r\nHostUrl=about:internet\r\n";

namespace zone_detail {
struct Handle {
  HANDLE value;
  ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
// An existing Internet (3) or Restricted (4) zone mark is kept as it is.
inline bool AlreadyMarked(HANDLE marker) {
  char text[4096]{};
  DWORD got = 0;
  if (!ReadFile(marker, text, sizeof(text) - 1, &got, nullptr)) return false;
  const std::string content(text, got);
  const auto at = content.find("ZoneId=");
  if (at == std::string::npos || at + 7 >= content.size()) return false;
  const char zone = content[at + 7];
  const bool ended = at + 8 == content.size() || content[at + 8] == '\r' || content[at + 8] == '\n';
  return ended && (zone == '3' || zone == '4');
}
}  // namespace zone_detail

// Marks the file `path` names; `file` is an open handle to it (any access),
// used to ask whether its volume has named streams at all (FAT and exFAT do
// not: nothing to do there). The stream is opened on the object itself
// (FILE_FLAG_OPEN_REPARSE_POINT). ERROR_SUCCESS when marked or nothing to
// do, else the Win32 error. Sharing is per stream: an exclusive handle on
// the file's content does not block this.
inline DWORD MarkInternetZone(HANDLE file, const std::wstring& path) {
  DWORD flags = 0;
  if (GetVolumeInformationByHandleW(file, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) && !(flags & FILE_NAMED_STREAMS)) {
    return ERROR_SUCCESS;
  }
  // The long (\\?\) form, so a path near MAX_PATH still takes the suffix.
  std::wstring stream = path;
  if (stream.rfind(L"\\\\?\\", 0) != 0) {
    if (stream.rfind(L"\\\\", 0) == 0) stream = L"\\\\?\\UNC\\" + stream.substr(2);
    else if (stream.size() > 2 && stream[1] == L':' && stream[2] == L'\\') stream = L"\\\\?\\" + stream;
  }
  stream += L":Zone.Identifier";
  zone_detail::Handle marker{CreateFileW(stream.c_str(), GENERIC_READ | GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (marker.value == INVALID_HANDLE_VALUE) return GetLastError();
  if (zone_detail::AlreadyMarked(marker.value)) return ERROR_SUCCESS;
  LARGE_INTEGER start{};
  if (!SetFilePointerEx(marker.value, start, nullptr, FILE_BEGIN) || !SetEndOfFile(marker.value)) return GetLastError();
  constexpr DWORD size = sizeof(kInternetZoneIdentifier) - 1;
  DWORD written = 0;
  if (!WriteFile(marker.value, kInternetZoneIdentifier, size, &written, nullptr)) return GetLastError();
  return written == size ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
}

}  // namespace multitap
