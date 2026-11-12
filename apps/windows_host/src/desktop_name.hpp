#pragma once
// The VM's name as the broker knows it (the Proxmox / administrator name the
// clients show, e.g. "Code-agent"). The broker sends it in
// host.capabilities.ack as desktopName; the service keeps it here so Host
// settings can title the overview with it instead of the desktop id.
// A missing or unreadable file means "unknown": the settings
// screen then shows the desktop id as before.
#include "support.hpp"
#include <fstream>
#include <string>

namespace host {
inline std::filesystem::path desktopNamePath() { return machineDirectory() / L"private" / L"desktop-name.txt"; }

// UTF-8, 1..240 bytes (the broker caps names at 80 characters), no control characters.
inline bool validDesktopName(const std::string& value) {
  if (value.empty() || value.size() > 240) return false;
  for (const unsigned char c : value) if (c < 0x20 || c == 0x7f) return false;
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0) > 0;
}

inline std::string readDesktopName() noexcept {
  try {
    const auto path = desktopNamePath();
    if (!trustedMachineFile(path)) return {};
    std::ifstream file(path, std::ios::binary);
    std::string value(256, '\0');
    file.read(value.data(), std::streamsize(value.size()));
    value.resize(size_t(file.gcount()));
    return validDesktopName(value) ? value : std::string();
  } catch (...) { return {}; }
}

// Writes only a valid name that differs from the kept one: temporary file,
// then an atomic replace inside the SYSTEM/Administrators-only folder.
inline void saveDesktopName(const std::string& value) {
  if (!validDesktopName(value) || readDesktopName() == value) return;
  prepareMachineDirectory();
  const auto path = desktopNamePath();
  const auto temporary = path.wstring() + temporarySuffix();
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  winCheck(file != INVALID_HANDLE_VALUE, "Create desktop name file");
  DWORD written = 0;
  const BOOL saved = WriteFile(file, value.data(), DWORD(value.size()), &written, nullptr) && written == value.size() && FlushFileBuffers(file);
  CloseHandle(file);
  if (!saved || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    winCheck(FALSE, "Save desktop name");
  }
}
}  // namespace host
