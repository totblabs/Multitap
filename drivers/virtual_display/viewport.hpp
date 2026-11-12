#pragma once
#include <aclapi.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <sddl.h>
#include <set>
#include "common_modes.hpp"
#include "../../native/windows_video/frame_layout.hpp"

// The signed driver only loads XML modes from EvtDeviceAdd. This is a guarded
// PnP restart fallback, not a live IddCx mode-update or a system reboot.
namespace viewport {
constexpr char originalHash[] =
    "37b3bf7f87e8938b6ec3ab370272ea0f71ff1640f8061fef4b1b74e833e5efa9";
constexpr char beginMark[] = "<!-- Multitap viewport modes begin -->\n";
constexpr char endMark[] = "<!-- Multitap viewport modes end -->\n";
constexpr wchar_t backupName[] = L"multitap-viewport-original.xml";
constexpr wchar_t journalName[] = L"multitap-viewport-pending.txt";
static bool observedRestartRequired = false;
struct Handle {
  HANDLE h = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : h(value) {}
  ~Handle() {
    if (h && h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  }
  Handle(const Handle &) = delete;
};
struct Source {
  std::wstring name;
  UINT32 connector{};
  DISPLAYCONFIG_PATH_INFO path{};
};
struct CcdData {
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};
struct Owned {
  UINT32 connector{};
  Display display;
};
struct Snapshot {
  std::wstring instance;
  std::string xml;
  UINT32 target{};
  std::vector<Owned> owned;
  std::vector<Display> external;
  CcdData ccd;
  std::vector<std::pair<UINT32, UINT32>> ownedPaths;
};
static bool sameLuid(const LUID &a, const LUID &b) {
  return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}
static UINT32 sourceModeIndex(const DISPLAYCONFIG_PATH_INFO &p) {
  return p.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE
             ? p.sourceInfo.sourceModeInfoIdx
             : p.sourceInfo.modeInfoIdx;
}
static UINT32 targetModeIndex(const DISPLAYCONFIG_PATH_INFO &p) {
  return p.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE
             ? p.targetInfo.targetModeInfoIdx
             : p.targetInfo.modeInfoIdx;
}
static CcdData queryCcd(UINT32 selection) {
  const auto flags = selection | QDC_VIRTUAL_MODE_AWARE;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    UINT32 paths = 0, modes = 0;
    require(GetDisplayConfigBufferSizes(flags, &paths, &modes) ==
                    ERROR_SUCCESS &&
                paths <= 4096 && modes <= 8192,
            "viewport_ccd_query_failed");
    CcdData out;
    out.paths.resize(paths);
    out.modes.resize(modes);
    const auto error = QueryDisplayConfig(flags, &paths, out.paths.data(),
                                          &modes, out.modes.data(), nullptr);
    if (error == ERROR_INSUFFICIENT_BUFFER)
      continue;
    require(error == ERROR_SUCCESS, "viewport_ccd_query_failed");
    out.paths.resize(paths);
    out.modes.resize(modes);
    return out;
  }
  throw std::runtime_error("viewport_ccd_query_failed");
}
static const DISPLAYCONFIG_SOURCE_MODE &
ccdSource(const CcdData &ccd, const DISPLAYCONFIG_PATH_INFO &path) {
  const auto index = sourceModeIndex(path);
  require(index < ccd.modes.size() &&
              ccd.modes[index].infoType ==
                  DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
              sameLuid(ccd.modes[index].adapterId, path.sourceInfo.adapterId) &&
              ccd.modes[index].id == path.sourceInfo.id,
          "viewport_ccd_source_invalid");
  return ccd.modes[index].sourceMode;
}
template <class T> static std::string encodeRaw(const std::vector<T> &values) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(values.size() * sizeof(T) * 2);
  const auto *bytes = reinterpret_cast<const unsigned char *>(values.data());
  for (size_t i = 0; i < values.size() * sizeof(T); ++i) {
    out += hex[bytes[i] >> 4];
    out += hex[bytes[i] & 15];
  }
  return out;
}
template <class T>
static std::vector<T> decodeRaw(const std::string &encoded, size_t maximum) {
  require(encoded.size() % (sizeof(T) * 2) == 0 &&
              encoded.size() / (sizeof(T) * 2) <= maximum,
          "viewport_journal_invalid");
  std::vector<T> out(encoded.size() / (sizeof(T) * 2));
  auto *bytes = reinterpret_cast<unsigned char *>(out.data());
  const auto digit = [](char c) {
    require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'),
            "viewport_journal_invalid");
    return c <= '9' ? c - '0' : c - 'a' + 10;
  };
  for (size_t i = 0; i < encoded.size() / 2; ++i)
    bytes[i] = static_cast<unsigned char>((digit(encoded[i * 2]) << 4) |
                                          digit(encoded[i * 2 + 1]));
  return out;
}
static std::wstring wide(const std::string &value) {
  if (value.empty())
    return {};
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          int(value.size()), nullptr, 0);
  require(length > 0, "viewport_invalid_utf8");
  std::wstring out(size_t(length), 0);
  require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                              int(value.size()), out.data(), length) == length,
          "viewport_invalid_utf8");
  return out;
}
static bool sameMode(const Mode &a, const Mode &b) {
  return a.width == b.width && a.height == b.height &&
         a.refreshRate == b.refreshRate;
}
static std::string modeLine(const Mode &m) {
  return "<resolution><width>" + std::to_string(m.width) + "</width><height>" +
         std::to_string(m.height) + "</height><refresh_rate>" +
         std::to_string(m.refreshRate) + "</refresh_rate></resolution>\n";
}
static void validMode(const Mode &m) {
  multitap::video::FrameLayout layout;
  require(m.width>=320&&m.height>=180&&multitap::video::frameLayout(m.width,m.height,layout)&&m.refreshRate==60,
          "invalid_viewport_arguments");
}
static std::string generated(const std::string &base,
                             const std::vector<Mode> &modes) {
  const auto at = base.find("</resolutions>");
  require(at != std::string::npos &&
              base.find("</resolutions>", at + 1) == std::string::npos,
          "viewport_original_settings_invalid");
  require(modes.size() <= 32, "viewport_mode_limit");
  if (modes.empty())
    return base;
  std::string extra = beginMark;
  for (const auto &m : modes) {
    validMode(m);
    extra += modeLine(m);
  }
  extra += endMark;
  auto out = base;
  out.insert(at, extra);
  return out;
}
static std::vector<Mode> addedModes(const std::string &base,
                                    const std::string &current) {
  if (base == current)
    return {};
  const auto begin = current.find(beginMark), end = current.find(endMark);
  require(begin != std::string::npos && end != std::string::npos &&
              end >= begin + strlen(beginMark),
          "viewport_settings_modified_externally");
  std::vector<Mode> modes;
  size_t at = begin + strlen(beginMark);
  const auto read = [&](const char *prefix, const char *suffix, unsigned min,
                        unsigned max) {
    require(current.compare(at, strlen(prefix), prefix) == 0,
            "viewport_settings_modified_externally");
    at += strlen(prefix);
    const auto finish = current.find(suffix, at);
    require(finish != std::string::npos && finish - at <= 8,
            "viewport_settings_modified_externally");
    const auto digits = wide(current.substr(at, finish - at));
    const auto result = number(digits.c_str(), min, max);
    at = finish + strlen(suffix);
    return result;
  };
  while (at < end) {
    Mode m;
    m.width = read("<resolution><width>", "</width>", 320, DWORD(INT32_MAX/4));
    m.height = read("<height>", "</height>", 180, DWORD(INT32_MAX));
    m.refreshRate =
        read("<refresh_rate>", "</refresh_rate></resolution>\n", 60, 60);
    validMode(m);
    require(modes.size() < 32 &&
                std::none_of(modes.begin(), modes.end(),
                             [&](const Mode &old) { return sameMode(old, m); }),
            "viewport_mode_limit");
    modes.push_back(m);
  }
  require(at == end && generated(base, modes) == current,
          "viewport_settings_modified_externally");
  return modes;
}
static bool protectedAcl(HANDLE h);
static std::string readFile(const fs::path &p, size_t maximum = 65536) {
  Handle file(CreateFileW(p.c_str(), GENERIC_READ | READ_CONTROL,
                          FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  require(file.h != INVALID_HANDLE_VALUE, "viewport_settings_unavailable");
  FILE_ATTRIBUTE_TAG_INFO info{};
  LARGE_INTEGER size{};
  require(GetFileInformationByHandleEx(file.h, FileAttributeTagInfo, &info,
                                       sizeof(info)) &&
              !(info.FileAttributes &
                (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
              GetFileSizeEx(file.h, &size) && size.QuadPart > 0 &&
              uint64_t(size.QuadPart) <= maximum && protectedAcl(file.h),
          "viewport_settings_untrusted");
  std::string out(size_t(size.QuadPart), 0);
  DWORD got = 0;
  require(ReadFile(file.h, out.data(), DWORD(out.size()), &got, nullptr) &&
              got == out.size(),
          "viewport_settings_read_failed");
  return out;
}
static bool privilegedSid(PSID sid) {
  if (IsWellKnownSid(sid, WinLocalSystemSid) ||
      IsWellKnownSid(sid, WinBuiltinAdministratorsSid))
    return true;
  PSID installer = nullptr;
  const bool ok =
      ConvertStringSidToSidW(
          L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",
          &installer) &&
      EqualSid(installer, sid);
  if (installer)
    LocalFree(installer);
  return ok;
}
static bool protectedAcl(HANDLE h) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetSecurityInfo(h, SE_FILE_OBJECT,
                      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                      &owner, nullptr, &dacl, nullptr,
                      &descriptor) != ERROR_SUCCESS)
    return false;
  bool safe = owner && privilegedSid(owner) && dacl;
  constexpr DWORD writes = GENERIC_ALL | GENERIC_WRITE | DELETE | WRITE_DAC |
                           WRITE_OWNER | FILE_WRITE_DATA | FILE_APPEND_DATA |
                           FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
                           FILE_DELETE_CHILD;
  if (safe)
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
      void *raw = nullptr;
      if (!GetAce(dacl, i, &raw)) {
        safe = false;
        break;
      }
      const auto *header = static_cast<const ACE_HEADER *>(raw);
      if (header->AceFlags & INHERIT_ONLY_ACE)
        continue;
      if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
        const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(raw);
        if ((ace->Mask & writes) &&
            !privilegedSid(const_cast<DWORD *>(&ace->SidStart))) {
          safe = false;
          break;
        }
      } else if (header->AceType != ACCESS_DENIED_ACE_TYPE) {
        safe = false;
        break;
      }
    }
  LocalFree(descriptor);
  return safe;
}
struct DirectoryLock {
  std::vector<HANDLE> handles;
  explicit DirectoryLock(const fs::path &p) {
    try {
      require(p.is_absolute(), "viewport_package_path_invalid");
      auto at = p.root_path();
      for (const auto &part : p.relative_path()) {
        require(part != L"." && part != L"..", "viewport_package_path_invalid");
        at /= part;
        Handle h(CreateFileW(
            at.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        FILE_ATTRIBUTE_TAG_INFO info{};
        require(h.h != INVALID_HANDLE_VALUE &&
                    GetFileInformationByHandleEx(h.h, FileAttributeTagInfo,
                                                 &info, sizeof(info)) &&
                    (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
                "viewport_package_path_untrusted");
        if (at == p)
          require(protectedAcl(h.h),
                  "viewport_package_not_administrator_controlled");
        handles.push_back(h.h);
        h.h = INVALID_HANDLE_VALUE;
      }
    } catch (...) {
      for (const auto h : handles)
        CloseHandle(h);
      throw;
    }
  }
  ~DirectoryLock() {
    for (const auto h : handles)
      CloseHandle(h);
  }
};
static void writeNew(const fs::path &p, const std::string &bytes,
                     bool publicConfig = false) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
              publicConfig ? L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;AU)"
                           : L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)",
              SDDL_REVISION_1, &descriptor, nullptr),
          "viewport_acl_failed");
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  Handle file(CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                          CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH |
                              FILE_FLAG_OPEN_REPARSE_POINT,
                          nullptr));
  LocalFree(descriptor);
  require(file.h != INVALID_HANDLE_VALUE,
          "viewport_private_file_create_failed");
  DWORD count = 0;
  const bool ok =
      WriteFile(file.h, bytes.data(), DWORD(bytes.size()), &count, nullptr) &&
      count == bytes.size() && FlushFileBuffers(file.h);
  if (!ok) {
    CloseHandle(file.h);
    file.h = INVALID_HANDLE_VALUE;
    DeleteFileW(p.c_str());
    throw std::runtime_error("viewport_private_file_write_failed");
  }
}
static void replace(const fs::path &p, const std::string &bytes) {
  unsigned long long nonce = 0;
  require(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&nonce),
                          sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0,
          "viewport_random_failed");
  const auto temporary =
      p.parent_path() /
      (L"multitap-viewport-write-" + std::to_wstring(GetCurrentProcessId()) +
       L"-" + std::to_wstring(nonce) + L".tmp");
  writeNew(temporary, bytes, true);
  if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
    const auto reason = GetLastError();
    if ((reason == ERROR_FILE_NOT_FOUND || reason == ERROR_PATH_NOT_FOUND) &&
        MoveFileExW(temporary.c_str(), p.c_str(), MOVEFILE_WRITE_THROUGH))
      return;
    DeleteFileW(temporary.c_str());
    throw std::runtime_error("viewport_settings_replace_failed");
  }
  if (!ReplaceFileW(p.c_str(), temporary.c_str(), nullptr, 0, nullptr,
                    nullptr)) {
    // ReplaceFile may exceptionally leave its destination missing (1176).
    // Keep the journal and repair the exact path before an explicit rollback.
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
      if (!MoveFileExW(temporary.c_str(), p.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("viewport_settings_recovery_required");
    } else
      DeleteFileW(temporary.c_str());
    throw std::runtime_error("viewport_settings_replace_failed");
  }
}
static fs::path ownedDirectory(const Status &s, bool recovery = false) {
  require(s.owned && (recovery || s.ready) && s.deviceCount == 1 &&
              !s.foreignDeviceCount && !s.vendorPathConflict &&
              (recovery || !s.restartRequired),
          "no_exclusive_owned_virtual_display");
  const auto path = regRead(kOwner, L"PackagePath");
  require(!path.empty() && samePath(path, regRead(kVendor, L"VDDPATH")),
          "shared_vendor_path_conflict");
  return fs::path(path);
}
static void verifyPayload(const fs::path &dir) {
  for (const auto &entry :
       std::vector<std::pair<const wchar_t *, const char *>>{
           {L"MttVDD.inf",
            "550d211fe481e74dfe3f9d724ed78be48b3a9113405965d683d9373e8d672f5d"},
           {L"MttVDD.dll",
            "c9ca837f57a98fbd43bc416a7f535a95843626e7759eaf85cf0cd7ce334dbb05"},
           {L"mttvdd.cat", "08a0093fc9b2e32b287a6f8a77ca4de0a31830d29fc33d2b13a"
                           "918dc859468f6"}}) {
    regularPath(dir / entry.first);
    require(sha256(dir / entry.first) == entry.second, "driver_hash_mismatch");
  }
  require(trusted(dir / L"mttvdd.cat") && trusted(dir / L"MttVDD.dll") &&
              catalogMember(dir / L"mttvdd.cat", dir / L"MttVDD.inf") &&
              catalogMember(dir / L"mttvdd.cat", dir / L"MttVDD.dll"),
          "driver_signature_untrusted");
}
static void verifyInstalledBinding(const std::wstring &owner) {
  Devices devices{SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr,
                                       DIGCF_PRESENT)};
  require(devices.h != INVALID_HANDLE_VALUE,
          "viewport_installed_driver_unavailable");
  SP_DEVINFO_DATA device{sizeof(device)};
  bool found = false;
  for (DWORD i = 0; SetupDiEnumDeviceInfo(devices.h, i, &device); ++i)
    if (virtualDevice(devices.h, device) &&
        !_wcsicmp(instance(devices.h, device).c_str(), owner.c_str())) {
      found = true;
      break;
    }
  require(found, "viewport_installed_driver_unavailable");
  Reg software;
  software.h = SetupDiOpenDevRegKey(devices.h, &device, DICS_FLAG_GLOBAL, 0,
                                    DIREG_DRV, KEY_READ);
  require(software.h && software.h != INVALID_HANDLE_VALUE,
          "viewport_installed_driver_unavailable");
  wchar_t inf[512]{};
  DWORD bytes = sizeof(inf), type = 0;
  require(RegQueryValueExW(software.h, L"InfPath", nullptr, &type,
                           reinterpret_cast<BYTE *>(inf),
                           &bytes) == ERROR_SUCCESS &&
              type == REG_SZ && bytes >= sizeof(wchar_t) && bytes < sizeof(inf),
          "viewport_installed_driver_unavailable");
  const std::wstring leaf(inf);
  require(!leaf.empty() && leaf.find_first_of(L"/\\:") == std::wstring::npos &&
              leaf != L"." && leaf != L"..",
          "viewport_installed_driver_untrusted");
  wchar_t windows[32768]{};
  require(GetWindowsDirectoryW(windows, DWORD(std::size(windows))) > 0,
          "viewport_installed_driver_unavailable");
  const auto published = fs::path(windows) / L"INF" / leaf;
  require(
      sha256(published) ==
          "550d211fe481e74dfe3f9d724ed78be48b3a9113405965d683d9373e8d672f5d",
      "viewport_installed_driver_mismatch");
  wchar_t stored[32768]{};
  require(SetupGetInfDriverStoreLocationW(published.c_str(), nullptr, nullptr,
                                          stored, DWORD(std::size(stored)),
                                          nullptr),
          "viewport_installed_driver_unavailable");
  verifyPayload(fs::path(stored).parent_path());
}
static std::string baseXml(const fs::path &dir) {
  const auto backup = dir / backupName;
  const bool present =
      GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES;
  const auto path = present ? backup : dir / L"vdd_settings.xml";
  require(sha256(path) == originalHash, "viewport_original_settings_modified");
  return readFile(path);
}
// QDC_ALL_PATHS contains possible inactive source/target permutations as well
// as the active configuration. Only verified owned-adapter candidates enter
// here; inactive alternatives must not invalidate an unambiguous active path.
static std::vector<Source> selectSources(const std::vector<Source> &candidates) {
  std::vector<Source> unique, out;
  const auto sameEndpoint = [](const Source &a, const Source &b) {
    return a.name == b.name && a.connector == b.connector &&
           a.path.sourceInfo.id == b.path.sourceInfo.id &&
           a.path.targetInfo.id == b.path.targetInfo.id &&
           sameLuid(a.path.sourceInfo.adapterId, b.path.sourceInfo.adapterId) &&
           sameLuid(a.path.targetInfo.adapterId, b.path.targetInfo.adapterId);
  };
  for (const auto &candidate : candidates) {
    require(candidate.name.rfind(L"\\\\.\\DISPLAY", 0) == 0 &&
                sameLuid(candidate.path.sourceInfo.adapterId,
                         candidate.path.targetInfo.adapterId) &&
                (unique.empty() ||
                 sameLuid(candidate.path.sourceInfo.adapterId,
                          unique.front().path.sourceInfo.adapterId)),
            "viewport_connector_identity_ambiguous");
    // A GDI name identifies one source, and a connector identifies one target,
    // even when Windows reports multiple possible source/target pairings.
    for (const auto &previous : unique)
      require((candidate.name == previous.name) ==
                  (candidate.path.sourceInfo.id == previous.path.sourceInfo.id) &&
                  (candidate.connector == previous.connector) ==
                  (candidate.path.targetInfo.id == previous.path.targetInfo.id),
              "viewport_connector_identity_ambiguous");
    const auto duplicate = std::find_if(unique.begin(), unique.end(),
        [&](const Source &previous) { return sameEndpoint(previous, candidate); });
    if (duplicate == unique.end()) unique.push_back(candidate);
    else if (candidate.path.flags & DISPLAYCONFIG_PATH_ACTIVE)
      duplicate->path = candidate.path;
  }
  std::set<UINT32> connectors;
  for (const auto &candidate : unique) connectors.insert(candidate.connector);
  require(connectors.size() == 2, "owned_virtual_sources_unavailable");
  const auto conflicts = [](const Source &a, const Source &b) {
    return a.name == b.name || a.connector == b.connector ||
           a.path.sourceInfo.id == b.path.sourceInfo.id ||
           a.path.targetInfo.id == b.path.targetInfo.id;
  };
  for (const auto &candidate : unique) {
    if (!(candidate.path.flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
    require(std::none_of(out.begin(), out.end(),
                [&](const Source &selected) { return conflicts(selected, candidate); }),
            "viewport_connector_identity_ambiguous");
    out.push_back(candidate);
  }
  for (const auto connector : connectors) {
    if (std::any_of(out.begin(), out.end(),
          [&](const Source &selected) { return selected.connector == connector; })) continue;
    const Source *available = nullptr;
    for (const auto &candidate : unique) {
      if (candidate.connector != connector ||
          std::any_of(out.begin(), out.end(),
            [&](const Source &selected) { return conflicts(selected, candidate); })) continue;
      require(!available, "viewport_connector_identity_ambiguous");
      available = &candidate;
    }
    require(available, "owned_virtual_sources_unavailable");
    out.push_back(*available);
  }
  require(out.size() == 2, "owned_virtual_sources_unavailable");
  std::sort(out.begin(), out.end(), [](const Source &a, const Source &b) {
    return a.connector < b.connector;
  });
  return out;
}
static std::vector<Source> sources(const std::wstring &owner) {
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    UINT32 pathCount = 0, modeCount = 0;
    require(GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount,
                                        &modeCount) == ERROR_SUCCESS &&
                pathCount <= 4096 && modeCount <= 8192,
            "viewport_sources_unavailable");
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    const auto error =
        QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount,
                           modes.data(), nullptr);
    if (error == ERROR_INSUFFICIENT_BUFFER)
      continue;
    require(error == ERROR_SUCCESS, "viewport_sources_unavailable");
    std::vector<Source> out;
    for (UINT32 i = 0; i < pathCount; ++i) {
      const auto &p = paths[i];
      if (!p.targetInfo.targetAvailable)
        continue;
      DISPLAYCONFIG_ADAPTER_NAME adapter{};
      adapter.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME,
                        sizeof(adapter), p.sourceInfo.adapterId, 0};
      if (DisplayConfigGetDeviceInfo(&adapter.header) != ERROR_SUCCESS ||
          _wcsicmp(adapterInstance(adapter.adapterDevicePath).c_str(),
                   owner.c_str()))
        continue;
      DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
      source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                       sizeof(source), p.sourceInfo.adapterId, p.sourceInfo.id};
      DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
      target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
                       sizeof(target), p.targetInfo.adapterId, p.targetInfo.id};
      require(DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
                  DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS,
              "viewport_connector_identity_unavailable");
      const std::wstring name = source.viewGdiDeviceName;
      require(name.rfind(L"\\\\.\\DISPLAY", 0) == 0,
              "viewport_connector_identity_unavailable");
      out.push_back({name, target.connectorInstance, p});
    }
    return selectSources(out);
  }
  throw std::runtime_error("viewport_sources_unavailable");
}
static void context(const std::wstring &owner) {
  require(consoleDesktop() && unlockedDefaultDesktop(),
          "interactive_console_required");
  const auto current = status();
  require(current.owned && current.deviceCount == 1 &&
              !current.foreignDeviceCount && !current.vendorPathConflict &&
              !_wcsicmp(current.id.c_str(), owner.c_str()),
          "owned_source_identity_changed");
  if (current.restartRequired)
    observedRestartRequired = true;
  require(!current.restartRequired, "reboot_required");
}
static Snapshot snapshot(const Status &s, const std::string &xml,
                         const std::wstring &target) {
  Snapshot out;
  out.instance = s.id;
  out.xml = xml;
  const auto mapping = sources(s.id);
  const auto active = activeDisplays();
  bool found = false;
  for (const auto &d : active) {
    const auto source =
        std::find_if(mapping.begin(), mapping.end(),
                     [&](const Source &v) { return v.name == d.name; });
    if (source == mapping.end())
      out.external.push_back(d);
    else {
      out.owned.push_back({source->connector, d});
      if (d.name == target) {
        out.target = source->connector;
        found = true;
      }
    }
  }
  require(found && !out.owned.empty() && out.owned.size() <= 2,
          "monitor_not_available");
  if (std::any_of(out.owned.begin(), out.owned.end(),
                  [](const Owned &item) { return item.display.primary; })) {
    out.ccd = queryCcd(QDC_ONLY_ACTIVE_PATHS);
    require(out.ccd.paths.size() <= 64 && out.ccd.modes.size() <= 192,
            "viewport_ccd_snapshot_limit");
    for (size_t i = 0; i < out.ccd.paths.size(); ++i) {
      const auto &path = out.ccd.paths[i];
      const auto source =
          std::find_if(mapping.begin(), mapping.end(), [&](const Source &v) {
            return sameLuid(v.path.targetInfo.adapterId,
                            path.targetInfo.adapterId) &&
                   v.path.targetInfo.id == path.targetInfo.id;
          });
      const auto &mode = ccdSource(out.ccd, path);
      DISPLAYCONFIG_SOURCE_DEVICE_NAME label{};
      label.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(label),
                      path.sourceInfo.adapterId, path.sourceInfo.id};
      require(DisplayConfigGetDeviceInfo(&label.header) == ERROR_SUCCESS,
              "viewport_ccd_source_invalid");
      const auto display =
          std::find_if(active.begin(), active.end(), [&](const Display &d) {
            return d.name == label.viewGdiDeviceName;
          });
      require(display != active.end() && display->left == mode.position.x &&
                  display->top == mode.position.y &&
                  display->width == int(mode.width) &&
                  display->height == int(mode.height),
              "viewport_topology_changed");
      if (source != mapping.end())
        out.ownedPaths.push_back({source->connector, UINT32(i)});
    }
    require(out.ownedPaths.size() == out.owned.size(),
            "viewport_ccd_owned_identity_invalid");
  }
  return out;
}
static std::vector<Owned> plan(const Snapshot &s, unsigned width,
                               unsigned height) {
  auto out = s.owned;
  auto target = std::find_if(out.begin(), out.end(), [&](const Owned &v) {
    return v.connector == s.target;
  });
  require(target != out.end(), "monitor_not_available");
  target->display.width = int(width);
  target->display.height = int(height);
  target->display.frequency = 60;
  const auto overlaps = [](const Display &a, const Display &b) {
    RECT x{a.left, a.top, a.left + a.width, a.top + a.height},
        y{b.left, b.top, b.left + b.width, b.top + b.height}, intersection{};
    return IntersectRect(&intersection, &x, &y) != FALSE;
  };
  for (auto &item : out)
    if (item.connector != s.target && item.display.primary &&
        overlaps(item.display, target->display))
      target->display.left = item.display.left - target->display.width;
  for (auto &item : out)
    if (item.connector != s.target && overlaps(item.display, target->display)) {
      require(!item.display.primary, "viewport_would_move_primary");
      item.display.left = target->display.left + target->display.width;
    }
  for (const auto &item : out)
    for (const auto &other : s.external)
      require(!overlaps(item.display, other),
              "resize_would_overlap_existing_display");
  return out;
}
static void cacheMode(std::vector<Mode> &extra, const Mode &requested,
                      const std::vector<Owned> &active) {
  if (std::none_of(extra.begin(), extra.end(),
                   [&](const Mode &m) { return sameMode(m, requested); }))
    extra.push_back(requested);
  while (extra.size() > 32) {
    const auto retired =
        std::find_if(extra.begin(), extra.end(), [&](const Mode &m) {
          return !sameMode(m, requested) && !common_modes::contains(m) &&
                 std::none_of(active.begin(), active.end(),
                              [&](const Owned &item) {
                                return m.width == DWORD(item.display.width) &&
                                       m.height == DWORD(item.display.height) &&
                                       m.refreshRate == item.display.frequency;
                              });
        });
    require(retired != extra.end(), "viewport_mode_limit");
    extra.erase(retired);
  }
}
static void writeDisplay(std::ostream &out, const Display &d) {
  out << std::quoted(utf8(d.name)) << ' ' << d.left << ' ' << d.top << ' '
      << d.width << ' ' << d.height << ' ' << d.primary << ' ' << d.frequency
      << '\n';
}
static Display readDisplay(std::istream &in) {
  std::string name;
  Display d;
  multitap::video::FrameLayout layout;
  require(bool(in >> std::quoted(name) >> d.left >> d.top >> d.width >>
               d.height >> d.primary >> d.frequency) &&
              name.size() < 80 && d.width >= 320 && d.height >= 180 &&
              multitap::video::frameLayout(d.width,d.height,layout,false) &&
              std::abs(int64_t(d.left)) < 1000000 &&
              std::abs(int64_t(d.top)) < 1000000,
          "viewport_journal_invalid");
  d.name = wide(name);
  require(d.name.rfind(L"\\\\.\\DISPLAY", 0) == 0, "viewport_journal_invalid");
  return d;
}
static std::string journal(const Snapshot &s) {
  std::ostringstream out;
  out << "MULTITAP-VIEWPORT-2\n"
      << std::quoted(utf8(s.instance)) << '\n'
      << std::quoted(s.xml) << '\n'
      << s.target << ' ' << s.owned.size() << ' ' << s.external.size() << '\n';
  for (const auto &d : s.owned) {
    out << d.connector << ' ';
    writeDisplay(out, d.display);
  }
  for (const auto &d : s.external)
    writeDisplay(out, d);
  out << std::quoted(encodeRaw(s.ccd.paths)) << '\n'
      << std::quoted(encodeRaw(s.ccd.modes)) << '\n'
      << s.ownedPaths.size() << '\n';
  for (const auto &item : s.ownedPaths)
    out << item.first << ' ' << item.second << '\n';
  require(out.str().size() <= 131072, "viewport_journal_limit");
  return out.str();
}
static Snapshot readJournal(const std::string &bytes) {
  std::istringstream in(bytes);
  std::string magic, id;
  Snapshot s;
  size_t owned = 0, external = 0;
  require(bool(std::getline(in, magic)) && magic == "MULTITAP-VIEWPORT-2" &&
              bool(in >> std::quoted(id) >> std::quoted(s.xml) >> s.target >>
                   owned >> external) &&
              id.size() < 4096 && s.xml.size() <= 65536 && owned >= 1 &&
              owned <= 2 && external <= 64,
          "viewport_journal_invalid");
  s.instance = wide(id);
  std::set<UINT32> connectors;
  for (size_t i = 0; i < owned; ++i) {
    UINT32 connector = 0;
    require(bool(in >> connector) && connectors.insert(connector).second,
            "viewport_journal_invalid");
    s.owned.push_back({connector, readDisplay(in)});
  }
  for (size_t i = 0; i < external; ++i)
    s.external.push_back(readDisplay(in));
  std::string rawPaths, rawModes;
  size_t ownedPaths = 0;
  require(bool(in >> std::quoted(rawPaths) >> std::quoted(rawModes) >>
               ownedPaths) &&
              ownedPaths <= 2,
          "viewport_journal_invalid");
  s.ccd.paths = decodeRaw<DISPLAYCONFIG_PATH_INFO>(rawPaths, 64);
  s.ccd.modes = decodeRaw<DISPLAYCONFIG_MODE_INFO>(rawModes, 192);
  std::set<UINT32> pathIndexes, pathConnectors;
  for (size_t i = 0; i < ownedPaths; ++i) {
    UINT32 connector = 0, pathIndex = 0;
    require(bool(in >> connector >> pathIndex) &&
                connectors.contains(connector) &&
                pathConnectors.insert(connector).second &&
                pathIndex < s.ccd.paths.size() &&
                pathIndexes.insert(pathIndex).second,
            "viewport_journal_invalid");
    s.ownedPaths.push_back({connector, pathIndex});
  }
  if (std::any_of(s.owned.begin(), s.owned.end(),
                  [](const Owned &v) { return v.display.primary; }))
    require(s.ownedPaths.size() == s.owned.size() && !s.ccd.paths.empty(),
            "viewport_journal_invalid");
  for (const auto &path : s.ccd.paths)
    ccdSource(s.ccd, path);
  in >> std::ws;
  require(in.eof() && connectors.contains(s.target),
          "viewport_journal_invalid");
  return s;
}
static void restart(const std::wstring &owner) {
  context(owner);
  verifyInstalledBinding(owner);
  Devices set{SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr,
                                   DIGCF_PRESENT)};
  require(set.h != INVALID_HANDLE_VALUE, "owned_device_restart_failed");
  SP_DEVINFO_DATA d{sizeof(d)};
  bool found = false;
  for (DWORD i = 0; SetupDiEnumDeviceInfo(set.h, i, &d); ++i)
    if (virtualDevice(set.h, d) &&
        !_wcsicmp(instance(set.h, d).c_str(), owner.c_str())) {
      found = true;
      break;
    }
  require(found, "owned_device_restart_failed");
  SP_PROPCHANGE_PARAMS p{};
  p.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
  p.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
  p.StateChange = DICS_PROPCHANGE;
  p.Scope = DICS_FLAG_CONFIGSPECIFIC;
  p.HwProfile = 0;
  require(SetupDiSetClassInstallParamsW(set.h, &d, &p.ClassInstallHeader,
                                        sizeof(p)) &&
              SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set.h, &d),
          "owned_device_restart_failed");
  SP_DEVINSTALL_PARAMS_W parameters{};
  parameters.cbSize = sizeof(parameters);
  require(SetupDiGetDeviceInstallParamsW(set.h, &d, &parameters),
          "owned_device_restart_status_unavailable");
  if (parameters.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART))
    observedRestartRequired = true;
  require(!observedRestartRequired, "reboot_required");
}
static std::vector<Source> waitReady(const std::wstring &owner,
                                     const Mode *requested, UINT32 target) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    require(consoleDesktop() && unlockedDefaultDesktop(),
            "interactive_console_required");
    const auto s = status();
    require(!s.restartRequired, "reboot_required");
    if (s.owned && s.ready && s.deviceCount == 1 && !s.foreignDeviceCount &&
        !s.vendorPathConflict && !_wcsicmp(s.id.c_str(), owner.c_str()))
      try {
        const auto mapping = sources(owner);
        const auto source =
            std::find_if(mapping.begin(), mapping.end(), [&](const Source &v) {
              return v.connector == target;
            });
        require(source != mapping.end(), "viewport_connector_identity_changed");
        if (!requested) {
          verifyInstalledBinding(owner);
          return mapping;
        }
        const auto available = supportedModes(source->name);
        if (std::any_of(available.begin(), available.end(), [&](const Mode &m) {
              return sameMode(m, *requested);
            })) {
          verifyInstalledBinding(owner);
          return mapping;
        }
      } catch (const std::exception &) {
      }
    Sleep(100);
  }
  throw std::runtime_error("viewport_mode_registration_timeout");
}
static void unchangedExternal(const Snapshot &s,
                              const std::vector<Source> &mapping) {
  const auto active = activeDisplays();
  std::vector<Display> external;
  for (const auto &d : active)
    if (std::none_of(mapping.begin(), mapping.end(),
                     [&](const Source &v) { return v.name == d.name; }))
      external.push_back(d);
  require(external.size() == s.external.size() &&
              std::all_of(s.external.begin(), s.external.end(),
                          [&](const Display &before) {
                            return std::any_of(external.begin(), external.end(),
                                               [&](const Display &after) {
                                                 return sameDisplay(before,
                                                                    after);
                                               });
                          }),
          "existing_display_changed");
}
static CcdData suppliedCcd(const Snapshot &s, const std::vector<Owned> &desired,
                           const CcdData &fresh,
                           const std::vector<Source> &mapping) {
  require(!s.ccd.paths.empty() && !s.ccd.modes.empty() &&
              s.ownedPaths.size() == s.owned.size(),
          "viewport_ccd_snapshot_invalid");
  CcdData out;
  std::map<UINT32, UINT32> copied;
  const auto copyMode = [&](UINT32 index, DISPLAYCONFIG_MODE_INFO_TYPE type) {
    require(index < s.ccd.modes.size() && s.ccd.modes[index].infoType == type,
            "viewport_ccd_mode_invalid");
    if (copied.contains(index))
      return copied[index];
    require(out.modes.size() < 192, "viewport_ccd_snapshot_limit");
    const auto destination = UINT32(out.modes.size());
    out.modes.push_back(s.ccd.modes[index]);
    copied[index] = destination;
    return destination;
  };
  for (size_t i = 0; i < s.ccd.paths.size(); ++i) {
    auto path = s.ccd.paths[i];
    const auto owned =
        std::find_if(s.ownedPaths.begin(), s.ownedPaths.end(),
                     [&](const auto &entry) { return entry.second == i; });
    if (owned == s.ownedPaths.end()) {
      require(std::any_of(fresh.paths.begin(), fresh.paths.end(),
                          [&](const DISPLAYCONFIG_PATH_INFO &p) {
                            return sameLuid(p.targetInfo.adapterId,
                                            path.targetInfo.adapterId) &&
                                   p.targetInfo.id == path.targetInfo.id &&
                                   p.sourceInfo.id == path.sourceInfo.id &&
                                   p.targetInfo.targetAvailable;
                          }),
              "viewport_external_identity_changed");
      const auto source =
          copyMode(sourceModeIndex(path), DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE);
      const auto target =
          copyMode(targetModeIndex(path), DISPLAYCONFIG_MODE_INFO_TYPE_TARGET);
      if (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) {
        path.sourceInfo.sourceModeInfoIdx = source;
        path.targetInfo.targetModeInfoIdx = target;
        if (path.targetInfo.desktopModeInfoIdx !=
            DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID)
          path.targetInfo.desktopModeInfoIdx =
              copyMode(path.targetInfo.desktopModeInfoIdx,
                       DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE);
      } else {
        path.sourceInfo.modeInfoIdx = source;
        path.targetInfo.modeInfoIdx = target;
      }
    } else {
      const auto wanted =
          std::find_if(desired.begin(), desired.end(), [&](const Owned &v) {
            return v.connector == owned->first;
          });
      if (wanted == desired.end())
        continue;
      const auto source =
          std::find_if(mapping.begin(), mapping.end(), [&](const Source &v) {
            return v.connector == wanted->connector;
          });
      require(source != mapping.end(), "viewport_connector_identity_changed");
      const auto available =
          std::find_if(fresh.paths.begin(), fresh.paths.end(),
                       [&](const DISPLAYCONFIG_PATH_INFO &p) {
                         return sameLuid(p.targetInfo.adapterId,
                                         source->path.targetInfo.adapterId) &&
                                p.targetInfo.id == source->path.targetInfo.id &&
                                p.sourceInfo.id == source->path.sourceInfo.id &&
                                p.targetInfo.targetAvailable;
                       });
      require(available != fresh.paths.end(),
              "viewport_ccd_owned_identity_invalid");
      path = *available;
      const auto &d = wanted->display;
      DISPLAYCONFIG_MODE_INFO mode{};
      mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
      mode.id = path.sourceInfo.id;
      mode.adapterId = path.sourceInfo.adapterId;
      mode.sourceMode.width = UINT32(d.width);
      mode.sourceMode.height = UINT32(d.height);
      mode.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
      mode.sourceMode.position = {d.left, d.top};
      const auto sourceIndex = UINT32(out.modes.size());
      out.modes.push_back(mode);
      if (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) {
        path.sourceInfo.cloneGroupId = DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;
        path.sourceInfo.sourceModeInfoIdx = sourceIndex;
        path.targetInfo.desktopModeInfoIdx =
            DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
        path.targetInfo.targetModeInfoIdx =
            DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;
      } else {
        path.sourceInfo.modeInfoIdx = sourceIndex;
        path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
      }
      path.targetInfo.refreshRate = {d.frequency, 1};
      path.targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
    }
    path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
    out.paths.push_back(path);
  }
  require(out.paths.size() == s.ccd.paths.size() && out.paths.size() <= 64 &&
              out.modes.size() <= 192,
          "viewport_ccd_count_changed");
  return out;
}
static void externalRebaseOnly(const Snapshot &s,
                               const std::vector<Source> &mapping) {
  const auto current = activeDisplays();
  std::vector<Display> external;
  for (const auto &d : current)
    if (std::none_of(mapping.begin(), mapping.end(), [&](const Source &source) {
          return source.name == d.name;
        }))
      external.push_back(d);
  require(external.size() == s.external.size(),
          "viewport_external_identity_changed");
  bool first = true;
  int dx = 0, dy = 0;
  for (const auto &before : s.external) {
    const auto after =
        std::find_if(external.begin(), external.end(),
                     [&](const Display &d) { return d.name == before.name; });
    require(after != external.end() && after->width == before.width &&
                after->height == before.height &&
                after->frequency == before.frequency,
            "existing_display_changed");
    if (first) {
      dx = after->left - before.left;
      dy = after->top - before.top;
      first = false;
    }
    require(after->left - before.left == dx && after->top - before.top == dy,
            "existing_display_changed");
  }
}
static std::wstring verifyApplied(const Snapshot &s,
                                  const std::vector<Owned> &desired) {
  context(s.instance);
  const auto fresh = sources(s.instance);
  unchangedExternal(s, fresh);
  const auto active = activeDisplays();
  size_t count = 0;
  std::wstring target;
  for (const auto &source : fresh) {
    const auto actual =
        std::find_if(active.begin(), active.end(),
                     [&](const Display &d) { return d.name == source.name; });
    const auto wanted =
        std::find_if(desired.begin(), desired.end(), [&](const Owned &v) {
          return v.connector == source.connector;
        });
    if (wanted == desired.end()) {
      require(actual == active.end(), "viewport_active_count_changed");
      continue;
    }
    require(actual != active.end(), "viewport_mode_not_applied");
    auto expected = wanted->display;
    expected.name = source.name;
    require(sameDisplay(expected, *actual), "viewport_mode_not_applied");
    ++count;
    if (source.connector == s.target)
      target = source.name;
  }
  require(count == desired.size() && !target.empty(),
          "viewport_active_count_changed");
  return target;
}
static std::wstring applyCcd(const Snapshot &s,
                             const std::vector<Owned> &desired) {
  context(s.instance);
  const auto mapping = sources(s.instance);
  externalRebaseOnly(s, mapping);
  const auto fresh = queryCcd(QDC_ALL_PATHS);
  auto supplied = suppliedCcd(s, desired, fresh, mapping);
  constexpr UINT32 options =
      SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE;
  require(SetDisplayConfig(UINT32(supplied.paths.size()), supplied.paths.data(),
                           UINT32(supplied.modes.size()), supplied.modes.data(),
                           SDC_VALIDATE | options) == ERROR_SUCCESS,
          "viewport_ccd_validate_failed");
  context(s.instance);
  externalRebaseOnly(s, mapping);
  verifyInstalledBinding(s.instance);
  require(SetDisplayConfig(UINT32(supplied.paths.size()), supplied.paths.data(),
                           UINT32(supplied.modes.size()), supplied.modes.data(),
                           SDC_APPLY | SDC_SAVE_TO_DATABASE | options) ==
              ERROR_SUCCESS,
          "viewport_ccd_apply_failed");
  return verifyApplied(s, desired);
}
static std::wstring apply(const Snapshot &s,
                          const std::vector<Owned> &desired) {
  if (!s.ccd.paths.empty())
    return applyCcd(s, desired);
  context(s.instance);
  const auto mapping = sources(s.instance);
  unchangedExternal(s, mapping);
  std::vector<DEVMODEW> modes;
  for (const auto &source : mapping) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    const auto wanted =
        std::find_if(desired.begin(), desired.end(), [&](const Owned &v) {
          return v.connector == source.connector;
        });
    mode.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
    if (wanted != desired.end()) {
      const auto &d = wanted->display;
      mode.dmPosition = {d.left, d.top};
      mode.dmPelsWidth = DWORD(d.width);
      mode.dmPelsHeight = DWORD(d.height);
      mode.dmBitsPerPel = 32;
      mode.dmDisplayFrequency = d.frequency;
      mode.dmFields |= DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
      const auto available = supportedModes(source.name);
      require(std::any_of(available.begin(), available.end(),
                          [&](const Mode &m) {
                            return m.width == DWORD(d.width) &&
                                   m.height == DWORD(d.height) &&
                                   m.refreshRate == d.frequency;
                          }),
              "viewport_required_mode_unavailable");
    }
    require(ChangeDisplaySettingsExW(source.name.c_str(), &mode, nullptr,
                                     CDS_TEST,
                                     nullptr) == DISP_CHANGE_SUCCESSFUL,
            "viewport_mode_test_failed");
    modes.push_back(mode);
  }
  for (size_t i = 0; i < mapping.size(); ++i) {
    context(s.instance);
    const auto wanted =
        std::find_if(desired.begin(), desired.end(), [&](const Owned &v) {
          return v.connector == mapping[i].connector;
        });
    const DWORD primary = wanted != desired.end() && wanted->display.primary
                              ? CDS_SET_PRIMARY
                              : 0;
    require(ChangeDisplaySettingsExW(mapping[i].name.c_str(), &modes[i],
                                     nullptr,
                                     CDS_UPDATEREGISTRY | CDS_NORESET | primary,
                                     nullptr) == DISP_CHANGE_SUCCESSFUL,
            "viewport_mode_apply_failed");
  }
  require(ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr) ==
              DISP_CHANGE_SUCCESSFUL,
          "viewport_mode_apply_failed");
  context(s.instance);
  const auto fresh = sources(s.instance);
  unchangedExternal(s, fresh);
  const auto active = activeDisplays();
  size_t activeOwned = 0;
  std::wstring targetName;
  for (const auto &source : fresh) {
    const auto actual =
        std::find_if(active.begin(), active.end(),
                     [&](const Display &d) { return d.name == source.name; });
    const auto wanted =
        std::find_if(desired.begin(), desired.end(), [&](const Owned &v) {
          return v.connector == source.connector;
        });
    if (wanted == desired.end()) {
      require(actual == active.end(), "viewport_active_count_changed");
      continue;
    }
    require(actual != active.end(), "viewport_mode_not_applied");
    ++activeOwned;
    auto expected = wanted->display;
    expected.name = source.name;
    require(sameDisplay(expected, *actual), "viewport_mode_not_applied");
    if (source.connector == s.target)
      targetName = source.name;
  }
  require(activeOwned == desired.size() && !targetName.empty(),
          "viewport_active_count_changed");
  return targetName;
}
static void outcome(bool ok, bool restarted, const Mode &m,
                    const std::wstring &name, const char *error, bool attempted,
                    bool restored, bool recovered = false) {
  const auto state = status();
  std::wstring monitor;
  for (size_t i = 0; i < state.displays.size(); ++i)
    if (state.displays[i].name == name) {
      monitor = logicalMonitorId(state, state.displays[i], i);
      break;
    }
  std::cout << std::boolalpha << "{\"viewportApplied\":" << ok
            << ",\"deviceRestarted\":" << restarted
            << ",\"targetDisplayDeviceName\":" << json(name)
            << ",\"targetMonitorId\":"
            << (!monitor.empty() ? json(monitor) : "null")
            << ",\"width\":" << m.width << ",\"height\":" << m.height
            << ",\"refreshRate\":" << m.refreshRate
            << ",\"configurationPending\":" << (!ok && !restored)
            << ",\"rebootRequired\":"
            << (state.restartRequired || observedRestartRequired)
            << ",\"error\":\"" << error
            << "\",\"rollbackAttempted\":" << attempted
            << ",\"rollbackSucceeded\":" << restored
            << ",\"recovered\":" << recovered
            << ",\"recoveryRequired\":" << viewportRecoveryPending(state)
            << ",\"state\":" << statusJson(state) << "}\n";
}
static bool rollback(const fs::path &dir, const Snapshot &s, bool &restarted) {
  try {
    const bool reload =
        GetFileAttributesW((dir / L"vdd_settings.xml").c_str()) ==
            INVALID_FILE_ATTRIBUTES ||
        readFile(dir / L"vdd_settings.xml") != s.xml || !status().ready;
    replace(dir / L"vdd_settings.xml", s.xml);
    context(s.instance);
    if (reload) {
      restart(s.instance);
      restarted = true;
      waitReady(s.instance, nullptr, s.target);
    }
    apply(s, s.owned);
    require(DeleteFileW((dir / journalName).c_str()),
            "viewport_journal_cleanup_failed");
    return true;
  } catch (...) {
    return false;
  }
}
static int support() {
  const auto s = status();
  try {
    const auto dir = ownedDirectory(s);
    require(consoleDesktop() && unlockedDefaultDesktop(),
            "interactive_console_required");
    DirectoryLock lock(dir);
    verifyPayload(dir);
    verifyInstalledBinding(s.id);
    const auto base = baseXml(dir);
    addedModes(base, readFile(dir / L"vdd_settings.xml"));
    require(GetFileAttributesW((dir / journalName).c_str()) ==
                INVALID_FILE_ATTRIBUTES,
            "viewport_recovery_required");
    const auto mapping = sources(s.id);
    const auto active = activeDisplays();
    require(!s.displays.empty(), "monitor_not_available");
    const auto before = snapshot(s, readFile(dir / L"vdd_settings.xml"),
                                 s.displays.front().name);
    if (!before.ccd.paths.empty()) {
      const auto fresh = queryCcd(QDC_ALL_PATHS);
      auto supplied = suppliedCcd(before, before.owned, fresh, mapping);
      require(
          SetDisplayConfig(UINT32(supplied.paths.size()), supplied.paths.data(),
                           UINT32(supplied.modes.size()), supplied.modes.data(),
                           SDC_VALIDATE | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                               SDC_VIRTUAL_MODE_AWARE) == ERROR_SUCCESS,
          "viewport_ccd_validate_failed");
    }
    std::cout << "{\"viewportProvisioningSupported\":true,"
                 "\"deviceRestartRequiredForNewMode\":true,\"minWidth\":320,"
                 "\"viewportSizeProtocol\":\"runtime-verified-v1\",\"minHeight\":180,"
                 "\"widthAlignment\":2,\"heightAlignment\":2,\"refreshRate\":"
                 "60,\"maxRefreshRate\":60,"
                 "\"displayModesApplied\":false,\"state\":"
              << statusJson(s) << "}\n";
    return 0;
  } catch (const std::exception &failure) {
    std::cout << std::boolalpha
              << "{\"viewportProvisioningSupported\":false,"
                 "\"displayModesApplied\":false,\"recoveryRequired\":"
              << viewportRecoveryPending(s) << ",\"error\":" << display_json::quoteUtf8(failure.what())
              << ",\"state\":" << statusJson(s) << "}\n";
    return 1;
  }
}
static int change(const std::wstring &name, Mode requested,
                  bool recover = false) {
  require(elevated(), "administrator_required");
  if (!recover)
    validMode(requested);
  const auto initial = status();
  const auto dir = ownedDirectory(initial, recover);
  require(consoleDesktop() && unlockedDefaultDesktop(),
          "interactive_console_required");
  DirectoryLock directoryLock(dir);
  verifyPayload(dir);
  verifyInstalledBinding(initial.id);
  PSECURITY_DESCRIPTOR mutexSecurity = nullptr;
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
              L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &mutexSecurity,
              nullptr),
          "viewport_acl_failed");
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), mutexSecurity, FALSE};
  Handle mutex(
      CreateMutexW(&attributes, FALSE, L"Global\\Multitap.Display.Viewport"));
  LocalFree(mutexSecurity);
  require(mutex.h && mutex.h != INVALID_HANDLE_VALUE, "viewport_busy");
  const auto acquired = WaitForSingleObject(mutex.h, 0);
  require(acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED,
          "viewport_busy");
  struct Release {
    HANDLE h;
    ~Release() { ReleaseMutex(h); }
  } release{mutex.h};
  const auto base = baseXml(dir);
  const auto pending = dir / journalName;
  if (recover) {
    require(GetFileAttributesW(pending.c_str()) != INVALID_FILE_ATTRIBUTES,
            "viewport_no_recovery_pending");
    const auto old = readJournal(readFile(pending, 131072));
    require(!_wcsicmp(old.instance.c_str(), initial.id.c_str()),
            "owned_source_identity_changed");
    addedModes(base, old.xml);
    bool restarted = false;
    const bool restored = rollback(dir, old, restarted);
    outcome(false, restarted, requested, L"",
            observedRestartRequired ? "reboot_required"
            : restored              ? ""
                                    : "viewport_rollback_incomplete",
            true, restored, restored);
    return restored ? 0 : 1;
  }
  require(GetFileAttributesW(pending.c_str()) == INVALID_FILE_ATTRIBUTES,
          "viewport_recovery_required");
  const auto current = readFile(dir / L"vdd_settings.xml");
  auto extra = addedModes(base, current);
  const auto before = snapshot(initial, current, name);
  const auto desired = plan(before, requested.width, requested.height);
  const auto available = supportedModes(name);
  const bool needsRestart =
      std::none_of(available.begin(), available.end(),
                   [&](const Mode &m) { return sameMode(m, requested); });
  if (needsRestart) {
    cacheMode(extra, requested, before.owned);
  }
  const auto next = needsRestart ? generated(base, extra) : current;
  if (GetFileAttributesW((dir / backupName).c_str()) == INVALID_FILE_ATTRIBUTES)
    writeNew(dir / backupName, base);
  writeNew(pending, journal(before));
  bool restarted = false;
  try {
    if (needsRestart) {
      replace(dir / L"vdd_settings.xml", next);
      restart(before.instance);
      restarted = true;
      waitReady(before.instance, &requested, before.target);
    }
    const auto target = apply(before, desired);
    require(DeleteFileW(pending.c_str()), "viewport_journal_cleanup_failed");
    outcome(true, restarted, requested, target, "", false, false);
    return 0;
  } catch (const std::exception &failure) {
    const std::string reason = failure.what();
    const bool restored = rollback(dir, before, restarted);
    outcome(false, restarted, requested, L"",
            observedRestartRequired ? "reboot_required"
            : restored              ? reason.c_str()
                                    : "viewport_rollback_incomplete",
            true, restored);
    return 1;
  }
}
static void ccdPlanTest(bool virtualAware) {
  Snapshot s;
  s.instance = L"OWNED-QA-NOT-A-DEVICE";
  s.xml = "fixture";
  s.target = 7;
  s.owned = {{7, {L"\\\\.\\DISPLAY2", 0, 0, 1920, 1080, true, 60}},
             {9, {L"\\\\.\\DISPLAY3", 1920, 0, 1920, 1080, false, 60}}};
  s.external = {{L"\\\\.\\DISPLAY1", -1024, 0, 1024, 768, false, 60}};
  const LUID owned{8, 0}, external{2, 0}, renewed{88, 0};
  for (unsigned i = 0; i < 3; ++i) {
    const bool foreign = i == 1;
    const LUID adapter = foreign ? external : owned;
    const UINT32 sourceId = i == 2 ? 1 : 0, targetId = foreign  ? 20
                                                       : i == 2 ? 11
                                                                : 10;
    const Display &d =
        foreign ? s.external[0] : s.owned[i == 2 ? 1 : 0].display;
    DISPLAYCONFIG_MODE_INFO source{};
    source.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    source.id = sourceId;
    source.adapterId = adapter;
    source.sourceMode.width = UINT32(d.width);
    source.sourceMode.height = UINT32(d.height);
    source.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
    source.sourceMode.position = {d.left, d.top};
    DISPLAYCONFIG_MODE_INFO target{};
    target.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
    target.id = targetId;
    target.adapterId = adapter;
    target.targetMode.targetVideoSignalInfo.pixelRate =
        foreign ? 654321 : 123456;
    target.targetMode.targetVideoSignalInfo.activeSize = {UINT32(d.width),
                                                          UINT32(d.height)};
    const auto at = UINT32(s.ccd.modes.size());
    s.ccd.modes.push_back(source);
    s.ccd.modes.push_back(target);
    DISPLAYCONFIG_PATH_INFO path{};
    path.flags = DISPLAYCONFIG_PATH_ACTIVE |
                 (virtualAware ? DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE : 0);
    path.sourceInfo.adapterId = adapter;
    path.sourceInfo.id = sourceId;
    path.targetInfo.adapterId = adapter;
    path.targetInfo.id = targetId;
    path.targetInfo.targetAvailable = TRUE;
    path.targetInfo.refreshRate = foreign ? DISPLAYCONFIG_RATIONAL{60000, 1001}
                                          : DISPLAYCONFIG_RATIONAL{60, 1};
    path.targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
    path.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
    if (virtualAware) {
      path.sourceInfo.cloneGroupId = DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;
      path.sourceInfo.sourceModeInfoIdx = at;
      path.targetInfo.targetModeInfoIdx = at + 1;
      path.targetInfo.desktopModeInfoIdx =
          DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
    } else {
      path.sourceInfo.modeInfoIdx = at;
      path.targetInfo.modeInfoIdx = at + 1;
    }
    s.ccd.paths.push_back(path);
    if (!foreign)
      s.ownedPaths.push_back({i == 0 ? 7u : 9u, i});
  }
  if (virtualAware) {
    DISPLAYCONFIG_MODE_INFO desktop{};
    desktop.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE;
    desktop.id = s.ccd.paths[1].sourceInfo.id;
    desktop.adapterId = external;
    desktop.desktopImageInfo.PathSourceSize = {1024, 768};
    desktop.desktopImageInfo.DesktopImageRegion = {0, 0, 1024, 768};
    desktop.desktopImageInfo.DesktopImageClip = {0, 0, 1024, 768};
    s.ccd.paths[1].targetInfo.desktopModeInfoIdx = UINT32(s.ccd.modes.size());
    s.ccd.modes.push_back(desktop);
  }
  CcdData fresh = s.ccd;
  std::vector<Source> mapping;
  for (size_t i : {size_t(0), size_t(2)}) {
    auto &p = fresh.paths[i];
    p.sourceInfo.adapterId = renewed;
    p.targetInfo.adapterId = renewed;
    p.sourceInfo.id += 2;
    p.targetInfo.id += 100;
    mapping.push_back({i == 0 ? L"\\\\.\\DISPLAY20" : L"\\\\.\\DISPLAY19",
                       i == 0 ? 7u : 9u, p});
  }
  const auto desired = plan(s, 2200, 1100);
  const auto supplied = suppliedCcd(s, desired, fresh, mapping);
  require(supplied.paths.size() == 3, "viewport_ccd_paths_lost");
  const auto &foreignPath = supplied.paths[1];
  const auto &foreignSource = supplied.modes[sourceModeIndex(foreignPath)];
  const auto &foreignTarget = supplied.modes[targetModeIndex(foreignPath)];
  require(std::memcmp(&foreignSource, &s.ccd.modes[2], sizeof(foreignSource)) ==
                  0 &&
              std::memcmp(&foreignTarget, &s.ccd.modes[3],
                          sizeof(foreignTarget)) == 0,
          "viewport_ccd_changed_external_modes");
  auto reindexed = foreignPath;
  if (virtualAware) {
    const auto oldDesktop = s.ccd.paths[1].targetInfo.desktopModeInfoIdx;
    const auto newDesktop = foreignPath.targetInfo.desktopModeInfoIdx;
    require(newDesktop < supplied.modes.size() &&
                std::memcmp(&supplied.modes[newDesktop],
                            &s.ccd.modes[oldDesktop],
                            sizeof(DISPLAYCONFIG_MODE_INFO)) == 0,
            "viewport_ccd_changed_external_desktop_image");
    reindexed.sourceInfo.sourceModeInfoIdx =
        s.ccd.paths[1].sourceInfo.sourceModeInfoIdx;
    reindexed.targetInfo.targetModeInfoIdx =
        s.ccd.paths[1].targetInfo.targetModeInfoIdx;
    reindexed.targetInfo.desktopModeInfoIdx = oldDesktop;
  } else {
    reindexed.sourceInfo.modeInfoIdx = s.ccd.paths[1].sourceInfo.modeInfoIdx;
    reindexed.targetInfo.modeInfoIdx = s.ccd.paths[1].targetInfo.modeInfoIdx;
  }
  require(std::memcmp(&reindexed, &s.ccd.paths[1], sizeof(reindexed)) == 0,
          "viewport_ccd_changed_external_path");
  const auto &primary = ccdSource(supplied, supplied.paths[0]);
  require(primary.width == 2200 && primary.height == 1100 &&
              primary.position.x == 0 && primary.position.y == 0 &&
              sameLuid(supplied.paths[0].sourceInfo.adapterId, renewed) &&
              targetModeIndex(supplied.paths[0]) ==
                  (virtualAware ? DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID
                                : DISPLAYCONFIG_PATH_MODE_IDX_INVALID),
          "viewport_ccd_owned_remap_failed");
  const auto &secondary = ccdSource(supplied, supplied.paths[2]);
  require(secondary.position.x == 2200 && secondary.width == 1920,
          "viewport_ccd_secondary_layout_failed");
  const auto recovered = readJournal(journal(s));
  require(recovered.ownedPaths == s.ownedPaths &&
              encodeRaw(recovered.ccd.paths) == encodeRaw(s.ccd.paths) &&
              encodeRaw(recovered.ccd.modes) == encodeRaw(s.ccd.modes),
          "viewport_ccd_journal_roundtrip_failed");
}
static void test() {
  const std::string base =
      "<vdd_settings><monitors><count>2</count></"
      "monitors><resolutions><resolution><width>1920</width><height>1080</"
      "height><refresh_rate>60</refresh_rate></resolution></"
      "resolutions><options><logging>false</logging></options></vdd_settings>";
  const Mode a{1584, 884, 60}, b{2200, 1100, 60};
  const auto xml = generated(base, {a, b});
  const auto modes = addedModes(base, xml);
  require(modes.size() == 2 && sameMode(modes[0], a) && sameMode(modes[1], b) &&
              generated(base, modes) == xml,
          "viewport_xml_roundtrip_failed");
  auto altered = xml;
  altered.replace(altered.find("false"), 5, "true");
  bool refused = false;
  try {
    addedModes(base, altered);
  } catch (...) {
    refused = true;
  }
  require(refused, "viewport_foreign_xml_edit_accepted");
  refused = false;
  try {
    validMode({1585, 884, 60});
  } catch (...) {
    refused = true;
  }
  require(refused, "viewport_odd_mode_accepted");
  for(const auto& mode:std::array<Mode,3>{{{5120,1440,60},{5120,2880,60},{4320,7680,60}}}){
    validMode(mode);
    const auto large=addedModes(base,generated(base,{mode}));
    require(large.size()==1&&sameMode(large.front(),mode),"viewport_large_geometry_roundtrip_failed");
  }
  Snapshot s;
  s.instance = L"ROOT\\DISPLAY\\OWNED";
  s.xml = xml;
  s.target = 7;
  s.owned = {{7, {L"\\\\.\\DISPLAY2", 1024, 0, 1920, 1080, false, 60}},
             {9, {L"\\\\.\\DISPLAY3", 2944, 0, 1920, 1080, false, 60}}};
  s.external = {{L"\\\\.\\DISPLAY1", 0, 0, 1024, 768, true, 60}};
  const auto p = plan(s, 2200, 1100);
  require(p.size() == 2 && p[0].display.width == 2200 &&
              p[1].display.left == 3224 && s.external[0].width == 1024,
          "viewport_owned_layout_failed");
  const auto decoded = readJournal(journal(s));
  require(decoded.instance == s.instance && decoded.xml == s.xml &&
              decoded.target == s.target && decoded.owned.size() == 2 &&
              sameDisplay(decoded.external[0], s.external[0]),
          "viewport_journal_roundtrip_failed");
  {
    auto large=s;large.external[0].width=32768;
    const auto reread=readJournal(journal(large));
    require(reread.external[0].width==32768,"viewport_recovery_larger_foreign_mode_preserved");
    large.external[0].width=std::numeric_limits<DWORD>::max();
    bool overflow=false;try{(void)readJournal(journal(large));}catch(...){overflow=true;}
    require(overflow,"viewport_recovery_stride_overflow_rejected");
  }
  std::vector<Mode> cache;
  for (unsigned i = 0; i < 32; ++i)
    cache.push_back({320 + i * 2, 180, 60});
  const std::vector<Owned> active = {
      {7, {L"\\\\.\\DISPLAY2", 1024, 0, 320, 180, false, 60}}};
  cacheMode(cache, a, active);
  require(cache.size() == 32 &&
              std::any_of(cache.begin(), cache.end(),
                          [&](const Mode &m) { return sameMode(m, a); }) &&
              std::any_of(cache.begin(), cache.end(),
                          [](const Mode &m) {
                            return m.width == 320 && m.height == 180;
                          }),
          "viewport_cache_retired_active_mode");
  auto single = s;
  single.owned.resize(1);
  require(plan(single, 1584, 884).size() == 1,
          "viewport_single_count_not_preserved");
  auto collision = s;
  collision.external.push_back(
      {L"\\\\.\\DISPLAY4", 2200, 0, 320, 180, false, 60});
  refused = false;
  try {
    plan(collision, 2200, 1100);
  } catch (...) {
    refused = true;
  }
  require(refused, "viewport_external_overlap_not_refused");
  ccdPlanTest(false);
  ccdPlanTest(true);
  std::cout
      << "{\"viewportPlanTestsPassed\":true,\"xmlPreservationTest\":true,"
         "\"journalRoundtripTest\":true,\"boundedCachePreservesActiveModes\":"
         "true,\"externalOverlapRejected\":true,\"singleCountPreserved\":true,"
         "\"ownedPrimaryCcdPlanTest\":true,\"externalCcdModesBytePreserved\":"
         "true,\"freshConnectorRemapTest\":true,\"operatingSystemChanged\":"
         "false,"
         "\"virtualDisplayRuntimeTest\":false}\n";
}
} // namespace viewport
