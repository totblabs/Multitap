#pragma once
#include "support.hpp"
#include <functional>

namespace host {
// The one fixed install: <Program Files>\Multitap\Host\bin, with the
// marker multitap-host.json in Host\. Everything a caller needs to believe
// before it starts MultitapHostTool.exe with the service's rights:
//  - the asking process is the installed service image itself;
//  - Multitap, Host and bin can be changed only by SYSTEM, Administrators or
//    Windows servicing (trustedInstalledPath). Windows Installer creates these
//    folders, so they inherit TrustedInstaller's entry from Program Files;
//    checking them with trustedMachineFile refused every install, and the
//    start-time repair of an interrupted automatic-login change never ran;
//  - the marker names this product, this service and this exact version;
//  - both program files are protected the same way.
// File access is injected so the decision can be tested without an install.
struct InstalledHostFiles {
  std::function<bool(const std::filesystem::path&)> trusted;
  std::function<std::string(const std::filesystem::path&)> read;  // at most 8193 bytes
};
inline std::string readInstalledMarker(const std::filesystem::path& marker) {
  std::ifstream input(marker, std::ios::binary); std::string bytes(8193, '\0');
  input.read(bytes.data(), std::streamsize(bytes.size())); bytes.resize(size_t(input.gcount()));
  return bytes;
}
inline InstalledHostFiles installedHostFiles() { return {trustedInstalledPath, readInstalledMarker}; }
// The installed MultitapHostTool.exe, or an empty path when `own` is not the
// installed service of `version` in a protected install.
inline std::filesystem::path installedHostTool(const std::filesystem::path& programFiles, const std::filesystem::path& own,
    const std::string& version, const InstalledHostFiles& files) noexcept {
  try {
    if (!files.trusted || !files.read || programFiles.empty()) return {};
    const auto root = programFiles / L"Multitap" / L"Host";
    const auto bin = root / L"bin";
    // However the asking image is spelled, it is this one file; the checks below use the install's own spelling.
    const auto service = bin / L"MultitapHost.exe";
    if (_wcsicmp(own.lexically_normal().c_str(), service.lexically_normal().c_str()) != 0) return {};
    for (const auto& directory : {root.parent_path(), root, bin}) if (!files.trusted(directory)) return {};
    const auto marker = root / L"multitap-host.json";
    if (!files.trusted(marker)) return {};
    auto bytes = files.read(marker);
    if (bytes.empty() || bytes.size() > 8192) return {};
    if (bytes.size() >= 3 && uint8_t(bytes[0]) == 0xef && uint8_t(bytes[1]) == 0xbb && uint8_t(bytes[2]) == 0xbf) bytes.erase(0, 3);
    const auto metadata = JsonParser::parse(bytes);
    if (metadata.get("product").string() != "MultitapHost" || metadata.get("version").string() != version || metadata.get("service").string() != "MultitapHost") return {};
    const auto tool = bin / L"MultitapHostTool.exe";
    if (!files.trusted(service) || !files.trusted(tool)) return {};
    return tool;
  } catch (...) { return {}; }
}
} // namespace host
