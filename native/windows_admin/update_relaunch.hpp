#pragma once
// One-process update handoff (update-handoff-v1) between an elevated product
// window (client or Host settings) and its staged installer:
//
//  1. The app writes a record (versions, its window placement, appearance and
//     a random token) under HKCU, creates the named event of that token and
//     launches the verified staged installer in update mode.
//  2. The installer shows a small splash on the app's monitor, signals the
//     event once its own pre-checks passed ("ready") or with a fixed failure
//     code, runs the existing unattended installation, then writes the final
//     outcome and itself relaunches the new (or, on failure, the previous)
//     installed app.
//  3. The relaunched app restores the saved placement and shows a
//     non-blocking notice; it consumes the record.
//
// The record carries no path, origin, command or free text: every executable
// is rebuilt from validated versions under the fixed installation roots and
// verified again before it is started. A forged record can therefore only
// change a window position or a fixed notice. The record lives in HKCU, which
// any program of this user can write: it may name an older "previous"
// version, so only an administrator-only installation is ever started with
// the installer's elevated token (AppLaunch::elevated); a per-user one opens
// through the shell and Windows asks for approval.
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <string>
#include <vector>
#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace multitap::relaunch {
enum class Product { Client, HostSettings };

inline bool ValidVersion(const std::string& value) {
  static const std::regex pattern("[0-9]{1,6}\\.[0-9]{1,6}\\.[0-9]{1,6}");
  return value.size() <= 20 && std::regex_match(value, pattern);
}
inline bool ValidToken(const std::wstring& value) {
  if (value.size() != 32) return false;
  for (const wchar_t ch : value)
    if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f'))) return false;
  return true;
}
// "" before the installer answered; "ready" once it accepted (the app closes);
// then exactly one of completed / failed / blocked.
inline bool ValidOutcome(const std::string& value) {
  return value.empty() || value == "ready" || value == "completed" || value == "failed" || value == "blocked";
}
// Fixed installer codes only; anything else is rejected when read back.
inline bool ValidCode(const std::string& value) {
  static const char* const codes[] = {
      "install_failed", "rollback_failed", "installer_missing", "progress_channel_failed",
      "installer_start_failed", "package_version_mismatch", "app_launch_failed",
      "existing_install_required", "certificate_trust_required", "driver_install_required",
      // The setup's own version is lower than the installed one (no downgrade).
      "update_not_newer"};
  if (value.empty()) return true;
  for (const char* code : codes)
    if (value == code) return true;
  return false;
}
inline bool BlockedCode(const std::string& value) {
  return value == "existing_install_required" || value == "certificate_trust_required" ||
      value == "driver_install_required";
}

struct Record {
  std::string fromVersion, toVersion;
  std::wstring token;
  WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
  bool hasPlacement = false;
  RECT bounds{};  // outer window rectangle in screen pixels (splash monitor)
  bool hasBounds = false;
  bool dark = true;
  DWORD processId = 0;
  uint64_t created = 0;  // FILETIME, UTC
  std::string outcome, code;
  DWORD stage = 0;  // completed installer steps, 0..6
  bool attention = false, restartRequired = false;
};

inline uint64_t NowFileTime() {
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  return (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}
constexpr uint64_t kMinute = 60ULL * 10000000ULL;
// An update handoff is meaningful for one hour; a clock skew of five minutes
// is tolerated. Older records are discarded, never acted on.
inline bool Fresh(const Record& record, uint64_t now) {
  return record.created != 0 && record.created <= now + 5 * kMinute && now - record.created <= 60 * kMinute;
}

inline std::wstring Widen(const std::string& value) { return std::wstring(value.begin(), value.end()); }
inline std::optional<std::string> Narrow(const std::wstring& value) {
  std::string out;
  for (const wchar_t ch : value) {
    if (ch < 0x20 || ch > 0x7e) return std::nullopt;
    out.push_back(char(ch));
  }
  return out;
}
// Whether dotted version [a] is lower than [b] (both already ValidVersion).
// The unelevated client refuses to run a signed but older installer as an
// "update": a server cannot roll the app back with a genuine old release.
inline bool VersionLess(const std::string& a, const std::string& b) {
  unsigned long x[3]{}, y[3]{};
  if (sscanf_s(a.c_str(), "%lu.%lu.%lu", &x[0], &x[1], &x[2]) != 3 || sscanf_s(b.c_str(), "%lu.%lu.%lu", &y[0], &y[1], &y[2]) != 3) return true;
  for (int i = 0; i < 3; ++i) if (x[i] != y[i]) return x[i] < y[i];
  return false;
}
inline std::wstring EventName(const std::wstring& token) { return L"Local\\Multitap.Update." + token; }
inline std::wstring NewToken() {
  GUID id{};
  if (FAILED(CoCreateGuid(&id))) return {};
  const auto* bytes = reinterpret_cast<const BYTE*>(&id);
  constexpr wchar_t hex[] = L"0123456789abcdef";
  std::wstring token;
  for (size_t i = 0; i < sizeof(id); ++i) {
    token += hex[bytes[i] >> 4];
    token += hex[bytes[i] & 15];
  }
  return token;
}

class Store {
 public:
  explicit Store(std::wstring root = L"Software\\Multitap\\UpdateHandoff") : root_(std::move(root)) {}
  std::wstring Key(Product product) const {
    return root_ + (product == Product::Client ? L"\\Client" : L"\\HostSettings");
  }
  const std::wstring& Root() const { return root_; }

  bool Write(Product product, const Record& record) const {
    if (!ValidVersion(record.fromVersion) || !ValidVersion(record.toVersion) || !ValidToken(record.token) ||
        !ValidOutcome(record.outcome) || !ValidCode(record.code))
      return false;
    Clear(product);
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, Key(product).c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
      return false;
    bool ok = Dword(key, L"Schema", 1) && Text(key, L"FromVersion", Widen(record.fromVersion)) &&
        Text(key, L"ToVersion", Widen(record.toVersion)) && Text(key, L"Token", record.token) &&
        Dword(key, L"Dark", record.dark ? 1 : 0) && Dword(key, L"ProcessId", record.processId) &&
        RegSetValueExW(key, L"Created", 0, REG_QWORD, reinterpret_cast<const BYTE*>(&record.created),
            sizeof(record.created)) == ERROR_SUCCESS;
    if (ok && record.hasPlacement)
      ok = RegSetValueExW(key, L"Placement", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&record.placement),
               sizeof(record.placement)) == ERROR_SUCCESS;
    if (ok && record.hasBounds)
      ok = RegSetValueExW(key, L"Bounds", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&record.bounds),
               sizeof(record.bounds)) == ERROR_SUCCESS;
    if (ok) ok = Result(key, record.outcome, record.code, record.stage, record.attention, record.restartRequired);
    RegCloseKey(key);
    if (!ok) Clear(product);
    return ok;
  }

  // The installer's answer. Only an existing record is updated.
  bool WriteOutcome(Product product, const std::string& outcome, const std::string& code, DWORD stage,
      bool attention, bool restartRequired) const {
    if (!ValidOutcome(outcome) || !ValidCode(code)) return false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, Key(product).c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
      return false;
    const bool ok = Result(key, outcome, code, stage, attention, restartRequired);
    RegCloseKey(key);
    return ok;
  }

  std::optional<Record> Read(Product product) const {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, Key(product).c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
      return std::nullopt;
    Record record;
    DWORD schema = 0, dark = 0, attention = 0, restart = 0;
    std::wstring from, to, outcome, code;
    bool ok = ReadDword(key, L"Schema", schema) && schema == 1 && ReadText(key, L"FromVersion", from) &&
        ReadText(key, L"ToVersion", to) && ReadText(key, L"Token", record.token) &&
        ReadDword(key, L"Dark", dark) && ReadDword(key, L"ProcessId", record.processId) &&
        ReadQword(key, L"Created", record.created) && ReadText(key, L"Outcome", outcome) &&
        ReadText(key, L"Code", code) && ReadDword(key, L"Stage", record.stage) &&
        ReadDword(key, L"Attention", attention) && ReadDword(key, L"RestartRequired", restart);
    if (ok) {
      record.hasPlacement = ReadBinary(key, L"Placement", &record.placement, sizeof(record.placement));
      record.hasBounds = ReadBinary(key, L"Bounds", &record.bounds, sizeof(record.bounds));
    }
    RegCloseKey(key);
    if (!ok) return std::nullopt;
    const auto narrowFrom = Narrow(from), narrowTo = Narrow(to), narrowOutcome = Narrow(outcome),
               narrowCode = Narrow(code);
    if (!narrowFrom || !narrowTo || !narrowOutcome || !narrowCode) return std::nullopt;
    record.fromVersion = *narrowFrom;
    record.toVersion = *narrowTo;
    record.outcome = *narrowOutcome;
    record.code = *narrowCode;
    record.dark = dark != 0;
    record.attention = attention != 0;
    record.restartRequired = restart != 0;
    if (!ValidVersion(record.fromVersion) || !ValidVersion(record.toVersion) || !ValidToken(record.token) ||
        !ValidOutcome(record.outcome) || !ValidCode(record.code) || record.stage > 6 || dark > 1 ||
        attention > 1 || restart > 1)
      return std::nullopt;
    if (record.hasPlacement && record.placement.length != sizeof(WINDOWPLACEMENT)) record.hasPlacement = false;
    return record;
  }

  bool Clear(Product product) const {
    const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, Key(product).c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
  }
  // QA only: remove an isolated root created for a self-test, then the parent
  // keys it implicitly created while they hold nothing else at all.
  bool ClearRoot() const {
    const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, root_.c_str());
    const bool removed = status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
    for (std::wstring parent = root_;;) {
      const auto split = parent.find_last_of(L'\\');
      if (split == std::wstring::npos) break;
      parent.resize(split);
      if (_wcsicmp(parent.c_str(), L"Software") == 0 || !RemoveIfEmpty(parent)) break;
    }
    return removed;
  }

 private:
  std::wstring root_;
  static bool RemoveIfEmpty(const std::wstring& path) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    DWORD subkeys = 0, values = 0;
    const bool empty = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr,
                           nullptr, nullptr, nullptr) == ERROR_SUCCESS && subkeys == 0 && values == 0;
    RegCloseKey(key);
    return empty && RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS;
  }
  static bool Dword(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) ==
        ERROR_SUCCESS;
  }
  static bool Text(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
               DWORD((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
  }
  static bool Result(HKEY key, const std::string& outcome, const std::string& code, DWORD stage, bool attention,
      bool restart) {
    return Text(key, L"Outcome", Widen(outcome)) && Text(key, L"Code", Widen(code)) &&
        Dword(key, L"Stage", stage > 6 ? 6 : stage) && Dword(key, L"Attention", attention ? 1 : 0) &&
        Dword(key, L"RestartRequired", restart ? 1 : 0);
  }
  static bool ReadDword(HKEY key, const wchar_t* name, DWORD& value) {
    DWORD size = sizeof(value);
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
  }
  static bool ReadQword(HKEY key, const wchar_t* name, uint64_t& value) {
    DWORD size = sizeof(value);
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_QWORD, nullptr, &value, &size) == ERROR_SUCCESS;
  }
  static bool ReadText(HKEY key, const wchar_t* name, std::wstring& value) {
    wchar_t buffer[65]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return false;
    value = buffer;
    return value.size() <= 64;
  }
  static bool ReadBinary(HKEY key, const wchar_t* name, void* out, DWORD expected) {
    DWORD size = expected;
    DWORD type = 0;
    std::vector<BYTE> bytes(expected);
    if (RegQueryValueExW(key, name, nullptr, &type, bytes.data(), &size) != ERROR_SUCCESS || type != REG_BINARY ||
        size != expected)
      return false;
    std::memcpy(out, bytes.data(), expected);
    return true;
  }
};

// What a starting app does with a record.
enum class Decision { None, Updated, Failed, Stale };
inline Decision Decide(const Record& record, const std::string& running, uint64_t now) {
  if (!Fresh(record, now)) return Decision::Stale;
  if (!ValidVersion(running)) return Decision::None;
  const bool failed = record.outcome == "failed" || record.outcome == "blocked";
  if (running == record.toVersion && !failed) return Decision::Updated;
  if (running == record.fromVersion && failed) return Decision::Failed;
  return Decision::None;
}
// Placement is reused only by the app the installer itself relaunched.
inline bool PlacementUsable(const Record& record, uint64_t now) {
  return record.hasPlacement && Fresh(record, now) &&
      (record.outcome == "completed" || record.outcome == "failed" || record.outcome == "blocked");
}
// A restored window must still land on a monitor at a sane size; a minimized
// window reopens normally.
inline bool PlacementSane(const WINDOWPLACEMENT& placement) {
  const RECT& r = placement.rcNormalPosition;
  const LONG width = r.right - r.left, height = r.bottom - r.top;
  if (placement.length != sizeof(WINDOWPLACEMENT) || width < 200 || height < 150 || width > 32767 || height > 32767)
    return false;
  return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
}
inline int ShowCommandFor(const WINDOWPLACEMENT& placement) {
  return placement.showCmd == SW_SHOWMAXIMIZED || (placement.flags & WPF_RESTORETOMAXIMIZED)
      ? SW_SHOWMAXIMIZED
      : SW_SHOWNORMAL;
}

// --- Installed product executables ------------------------------------------
// The four parts of the fixed file version in an executable's version resource.
inline bool FileVersion(const std::wstring& path, std::array<uint16_t, 4>& out) {
  DWORD ignored = 0;
  const DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path.c_str(), &ignored);
  if (!size || size > 1024 * 1024) return false;
  std::vector<BYTE> data(size);
  if (!GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path.c_str(), 0, size, data.data())) return false;
  VS_FIXEDFILEINFO* info = nullptr;
  UINT length = 0;
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info ||
      length < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xFEEF04BD)
    return false;
  out = {uint16_t(HIWORD(info->dwFileVersionMS)), uint16_t(LOWORD(info->dwFileVersionMS)),
      uint16_t(HIWORD(info->dwFileVersionLS)), uint16_t(LOWORD(info->dwFileVersionLS))};
  return true;
}
// x.y.z equals the first three parts; the fourth must be 0 when required
// (installer EXEs carry "x.y.z.0"; the client carries its build number).
inline bool FileVersionIs(const std::wstring& path, const std::string& version, bool zeroRevision) {
  if (!ValidVersion(version)) return false;
  std::array<uint16_t, 4> parts{};
  if (!FileVersion(path, parts)) return false;
  unsigned a = 0, b = 0, c = 0;
  if (sscanf_s(version.c_str(), "%u.%u.%u", &a, &b, &c) != 3 || a > 65535 || b > 65535 || c > 65535) return false;
  return parts[0] == a && parts[1] == b && parts[2] == c && (!zeroRevision || parts[3] == 0);
}
inline std::optional<std::filesystem::path> KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR folder = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &folder))) return std::nullopt;
  std::filesystem::path path(folder);
  CoTaskMemFree(folder);
  return path;
}
inline bool PlainEntry(const std::filesystem::path& path, bool directory) {
  const DWORD flags = GetFileAttributesW(path.c_str());
  return flags != INVALID_FILE_ATTRIBUTES && !(flags & FILE_ATTRIBUTE_REPARSE_POINT) &&
      bool(flags & FILE_ATTRIBUTE_DIRECTORY) == directory;
}
inline bool MarkerMatches(const std::filesystem::path& marker, const std::string& product, const std::string& version) {
  if (!PlainEntry(marker, false)) return false;
  std::error_code error;
  const auto size = std::filesystem::file_size(marker, error);
  if (error || size > 4096) return false;
  std::ifstream file(marker, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(file)), {});
  return std::regex_search(text, std::regex("\"product\"\\s*:\\s*\"" + product + "\"")) &&
      std::regex_search(text, std::regex("\"version\"\\s*:\\s*\"" + std::regex_replace(version, std::regex("\\."), "\\.") + "\""));
}
// The version a plain marker of [product] names, or empty.
inline std::string MarkerVersion(const std::filesystem::path& marker, const std::string& product) {
  if (!PlainEntry(marker, false)) return {};
  std::error_code error;
  const auto size = std::filesystem::file_size(marker, error);
  if (error || size > 4096) return {};
  std::ifstream file(marker, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(file)), {});
  if (!std::regex_search(text, std::regex("\"product\"\\s*:\\s*\"" + product + "\""))) return {};
  std::smatch match;
  if (!std::regex_search(text, match, std::regex("\"version\"\\s*:\\s*\"([0-9]{1,6}\\.[0-9]{1,6}\\.[0-9]{1,6})\""))) return {};
  return match[1].str();
}
// Only an administrator can change it: the entry itself (never a link) is
// owned by SYSTEM, Administrators or TrustedInstaller and grants write access
// to nobody else (CREATOR OWNER only as an inherit-only template). The same
// rule as the setup's protectedDirectory (setup_payload.hpp).
inline bool SidIs(PSID sid, const wchar_t* text) {
  PSID expected = nullptr;
  if (!ConvertStringSidToSidW(text, &expected)) return false;
  const bool same = EqualSid(sid, expected) != FALSE;
  LocalFree(expected);
  return same;
}
inline bool AdministratorSid(PSID sid) {
  return SidIs(sid, L"S-1-5-18") || SidIs(sid, L"S-1-5-32-544") ||
      SidIs(sid, L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464");
}
inline bool ProtectedEntry(const std::filesystem::path& path, bool directory) {
  const HANDLE handle = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  bool ok = false;
  FILE_ATTRIBUTE_TAG_INFO tag{};
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) &&
      !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && bool(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory &&
      GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
          &dacl, nullptr, &descriptor) == ERROR_SUCCESS &&
      owner && dacl && AdministratorSid(owner)) {
    constexpr ACCESS_MASK write = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE |
        FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
    ok = true;
    for (DWORD index = 0; ok && index < dacl->AceCount; ++index) {
      void* raw = nullptr;
      if (!GetAce(dacl, index, &raw)) { ok = false; break; }
      const auto* header = static_cast<ACE_HEADER*>(raw);
      if ((header->AceFlags & INHERIT_ONLY_ACE) || header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
      if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { ok = false; break; }
      const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
      const PSID sid = const_cast<DWORD*>(&ace->SidStart);
      if ((ace->Mask & write) && !AdministratorSid(sid) && !SidIs(sid, L"S-1-3-0")) ok = false;
    }
  }
  if (descriptor) LocalFree(descriptor);
  CloseHandle(handle);
  return ok;
}
struct AppLaunch {
  std::filesystem::path executable, directory;
  std::wstring arguments;
  // False for the client (it runs unelevated) and for the
  // per-user client of an earlier layout under %LOCALAPPDATA%: anything
  // the user can write is never started with an elevated token. The setup
  // starts those through the shell instead, with the desktop user's token.
  // True only for Host settings, which require administrator approval.
  bool elevated = true;
};
// The client of [version], verified; never a path from a record.
//   %ProgramFiles%\Multitap\Client\<version>\Multitap.exe (an earlier layout):
//     every folder from Multitap down and the EXE are administrator-only.
//   %LOCALAPPDATA%\Multitap\Client\<version>\Multitap.exe (the earliest layout):
//     plain folders only, and AppLaunch::elevated is false.
inline std::optional<AppLaunch> InstalledClientApp(const std::filesystem::path& programFiles,
    const std::filesystem::path& localAppData, const std::string& version) {
  if (!ValidVersion(version)) return std::nullopt;
  if (!programFiles.empty()) {
    // One fixed folder: %ProgramFiles%\Multitap\Client\Multitap.exe, marker beside it.
    const auto vendor = programFiles / L"Multitap", root = vendor / L"Client", executable = root / L"Multitap.exe";
    if (ProtectedEntry(vendor, true) && ProtectedEntry(root, true) && ProtectedEntry(executable, false) &&
        MarkerMatches(root / L"multitap-client.json", "Multitap", version) && FileVersionIs(executable.wstring(), version, false))
      return AppLaunch{executable, root, L"", false};
  }
  if (!programFiles.empty()) {
    const auto vendor = programFiles / L"Multitap", root = vendor / L"Client", directory = root / Widen(version),
               executable = directory / L"Multitap.exe";
    if (ProtectedEntry(vendor, true) && ProtectedEntry(root, true) && ProtectedEntry(directory, true) &&
        ProtectedEntry(executable, false) && MarkerMatches(directory / L"multitap-client.json", "Multitap", version) &&
        FileVersionIs(executable.wstring(), version, false))
      // The client runs unelevated: the setup, which holds the
      // administrator token, starts it through the shell, never with its own.
      return AppLaunch{executable, directory, L"", false};
  }
  if (localAppData.empty()) return std::nullopt;
  const auto vendor = localAppData / L"Multitap", root = vendor / L"Client", directory = root / Widen(version),
             executable = directory / L"Multitap.exe";
  if (!PlainEntry(vendor, true) || !PlainEntry(root, true) || !PlainEntry(directory, true) ||
      !PlainEntry(executable, false) || !MarkerMatches(directory / L"multitap-client.json", "Multitap", version) ||
      !FileVersionIs(executable.wstring(), version, false))
    return std::nullopt;
  return AppLaunch{executable, directory, L"", false};
}
// The installed app of [version], verified; never a path from a record.
//   client: InstalledClientApp under the known Program Files / LocalAppData
//   Host:   %ProgramFiles%\Multitap\Host\<version>\bin\MultitapHostSettings.exe
inline std::optional<AppLaunch> InstalledApp(Product product, const std::string& version) {
  if (!ValidVersion(version)) return std::nullopt;
  if (product == Product::Client) {
    const auto programs = KnownFolder(FOLDERID_ProgramFiles);
    const auto local = KnownFolder(FOLDERID_LocalAppData);
    return InstalledClientApp(programs ? *programs : std::filesystem::path(), local ? *local : std::filesystem::path(), version);
  }
  const auto programs = KnownFolder(FOLDERID_ProgramFiles);
  if (!programs) return std::nullopt;
  const auto vendor = *programs / L"Multitap", root = vendor / L"Host", directory = root / Widen(version),
             bin = directory / L"bin", executable = bin / L"MultitapHostSettings.exe";
  if (!PlainEntry(vendor, true) || !PlainEntry(root, true) || !PlainEntry(directory, true) || !PlainEntry(bin, true) ||
      !PlainEntry(executable, false) || !MarkerMatches(directory / L"multitap-host.json", "MultitapHost", version))
    return std::nullopt;
  return AppLaunch{executable, bin, L"--host-settings"};
}
// The Host version of a running Host settings EXE, from its installed path
// (...\Host\<version>\bin\MultitapHostSettings.exe). Empty when not installed.
inline std::string InstalledHostVersion(const std::filesystem::path& executable) {
  const auto bin = executable.parent_path();
  if (_wcsicmp(executable.filename().c_str(), L"MultitapHostSettings.exe") || _wcsicmp(bin.filename().c_str(), L"bin"))
    return {};
  const auto name = Narrow(bin.parent_path().filename().wstring());
  return name && ValidVersion(*name) ? *name : std::string();
}
// The highest version of [product] installed under [programFiles]\Multitap
// (Client\<version> or Host\<version>): a plain folder with a version name and
// the product's ownership marker. Program Files is administrator-only, so this
// cannot be forged by the user the way the HKCU handoff record can. Used to
// refuse an unattended update to a lower version (no downgrade).
inline std::optional<std::string> HighestInstalledVersion(Product product, const std::filesystem::path& programFiles) {
  if (programFiles.empty()) return std::nullopt;
  const bool client = product == Product::Client;
  const auto root = programFiles / L"Multitap" / (client ? L"Client" : L"Host");
  if (!PlainEntry(programFiles / L"Multitap", true) || !PlainEntry(root, true)) return std::nullopt;
  std::optional<std::string> best;
  // The fixed install folder keeps its marker in the root.
  if (const auto fixed = MarkerVersion(root / (client ? L"multitap-client.json" : L"multitap-host.json"), client ? "Multitap" : "MultitapHost");
      ValidVersion(fixed))
    best = fixed;
  std::error_code error;
  for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
    const auto name = Narrow(it->path().filename().wstring());
    if (!name || !ValidVersion(*name) || !PlainEntry(it->path(), true)) continue;
    if (!MarkerMatches(it->path() / (client ? L"multitap-client.json" : L"multitap-host.json"), client ? "Multitap" : "MultitapHost", *name))
      continue;
    if (!best || VersionLess(*best, *name)) best = *name;
  }
  return best;
}
// True when an unattended update to [ownVersion] would install an older
// version than the one already installed. An unreadable [ownVersion] counts as
// older; with nothing installed there is nothing to downgrade.
inline bool DowngradeRefused(Product product, const std::string& ownVersion, const std::filesystem::path& programFiles) {
  const auto installed = HighestInstalledVersion(product, programFiles);
  return installed && (!ValidVersion(ownVersion) || VersionLess(ownVersion, *installed));
}
// Started with the caller's (elevated) token: no second approval prompt.
// Only for an administrator-only installation; a per-user one is refused here.
inline bool Launch(const AppLaunch& app) {
  if (!app.elevated) return false;
  std::wstring command = L"\"" + app.executable.wstring() + L"\"";
  if (!app.arguments.empty()) command += L" " + app.arguments;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  AllowSetForegroundWindow(ASFW_ANY);
  if (!CreateProcessW(app.executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
          app.directory.c_str(), &startup, &process))
    return false;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}
// Whether the app that wrote the record is still running.
inline bool AppAlive(Product product, DWORD processId) {
  if (!processId || processId == GetCurrentProcessId()) return false;
  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
  if (!process) return false;
  wchar_t image[32768]{};
  DWORD length = 32768;
  const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT &&
      QueryFullProcessImageNameW(process, 0, image, &length) &&
      !_wcsicmp(std::filesystem::path(image).filename().c_str(),
          product == Product::Client ? L"Multitap.exe" : L"MultitapHostSettings.exe");
  CloseHandle(process);
  return alive;
}
}  // namespace multitap::relaunch
