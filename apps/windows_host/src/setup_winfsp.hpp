#pragma once
// Installing WinFsp (the signed user-mode file-system driver of drive sharing,
// docs/drive-redirection.md "WinFsp backend") from Host settings: the pinned,
// unmodified WinFsp release in the Host's bin\winfsp, accepted only by exact size,
// SHA-256 and signature. Core feature only (msiexec's default); no reboot is
// requested or forced. A failure is never fatal for the Host: drive sharing
// then uses the WebDAV path.
#include "setup_payload.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <msi.h>
#include <wintrust.h>
#include <softpub.h>
#pragma comment(lib, "msi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "wintrust.lib")

namespace multitap::winfsp {
// Pins; tests compare them with apps/windows_host/third_party/winfsp/upstream.json.
inline constexpr const wchar_t* MsiName = L"winfsp-2.1.25156.msi";
inline constexpr const char* MsiSha256 = "073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a";
inline constexpr unsigned long long MsiSize = 2191360ULL;
inline constexpr const char* PackagedVersion = "2.1.25156";
inline constexpr const wchar_t* UpgradeCode = L"{5466A3D8-3AA1-4240-B6A0-3A051940A3EC}";
inline constexpr const wchar_t* OwnerKey = L"SOFTWARE\\Multitap\\Host\\WinFsp";  // InstalledByHost (DWORD), Version (string)

struct Version {
  uint32_t major = 0, minor = 0, build = 0;
  bool operator==(const Version& o) const { return major == o.major && minor == o.minor && build == o.build; }
  bool operator<(const Version& o) const { return major != o.major ? major < o.major : minor != o.minor ? minor < o.minor : build < o.build; }
};
// "2.1.25156" or "2.1.25156.0"; anything else is unreadable.
inline std::optional<Version> parseVersion(const std::string& text) {
  uint64_t parts[4]{}; size_t count = 0; size_t at = 0;
  while (at <= text.size() && count < 4) {
    size_t end = at; uint64_t value = 0;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') { value = value * 10 + uint64_t(text[end] - '0'); if (value > 0xffffffffULL) return std::nullopt; ++end; }
    if (end == at) return std::nullopt;
    parts[count++] = value; at = end;
    if (at == text.size()) break;
    if (text[at] != '.') return std::nullopt;
    ++at;
  }
  if (at != text.size() || count < 3 || (count == 4 && parts[3] != 0)) return std::nullopt;
  return Version{uint32_t(parts[0]), uint32_t(parts[1]), uint32_t(parts[2])};
}

enum class State { none, older, current };
// What is installed against what this setup carries: nothing -> install, older
// -> upgrade, the same or newer (or present but unreadable) -> leave it alone.
inline State stateOf(const std::optional<Version>& installed, bool present, const Version& packaged) {
  if (!present) return State::none;
  if (!installed) return State::current;  // present but unidentifiable: never touched
  return *installed < packaged ? State::older : State::current;
}

class Backend {
 public:
  virtual ~Backend() = default;
  // Registered WinFsp: `present` false when nothing is installed; `version` empty when unreadable.
  virtual void installed(bool& present, std::optional<Version>& version) = 0;
  virtual bool packageIntact(const std::wstring& path) = 0;
  // msiexec's exit code; -1 when it could not run.
  virtual int installMsi(const std::wstring& path) = 0;
  // "" when the DLL loads, exports what the Host uses and the driver service is registered; else a fixed code.
  virtual std::string verifyInstalled() = 0;
  virtual void recordOwnership(const std::string& version) = 0;
};

struct Outcome { std::string error; const char* action = "skipped"; bool restart = false; };

// Pure orchestration (tested with a fake Backend).
inline Outcome ensureInstalled(Backend& backend, const std::wstring& msi) {
  Outcome out;
  const auto packaged = *parseVersion(PackagedVersion);
  bool present = false; std::optional<Version> version;
  backend.installed(present, version);
  const State state = stateOf(version, present, packaged);
  if (state == State::current) {
    // Already there (same or newer): nothing is installed, but it must still be usable.
    out.error = backend.verifyInstalled();
    return out;
  }
  if (!backend.packageIntact(msi)) { out.error = "winfsp_package_mismatch"; return out; }
  const int code = backend.installMsi(msi);
  if (code == 0 || code == 3010) { out.restart = code == 3010; out.action = state == State::none ? "installed" : "upgraded"; }
  else {
    out.error = code == 1618 ? "winfsp_busy" : code == 1625 || code == 1643 ? "winfsp_blocked" : code == 1638 || code == 1603 ? "winfsp_install_failed" : code < 0 ? "winfsp_unavailable" : "winfsp_install_failed";
    return out;
  }
  out.error = backend.verifyInstalled();
  // Only a fresh install is ours; an upgrade of someone else's WinFsp stays theirs.
  if (out.error.empty() && state == State::none) backend.recordOwnership(PackagedVersion);
  return out;
}

// Tests never touch the machine: a scripted stand-in.
// mode: "" (nothing installed, install works), "old", "current", "newer", "unreadable",
// "install-fails", "busy", "reboot", "reboot-verify-fails", "verify-fails", "package-bad".
class Fake final : public Backend {
 public:
  explicit Fake(std::string mode) : mode_(std::move(mode)) {}
  size_t installs = 0, verifies = 0, owned = 0, hashes = 0; bool installedNow = false;
  void installed(bool& present, std::optional<Version>& version) override {
    present = installedNow || mode_ == "old" || mode_ == "current" || mode_ == "newer" || mode_ == "unreadable";
    if (installedNow) version = parseVersion(PackagedVersion);
    else if (mode_ == "old") version = parseVersion("2.0.20000");
    else if (mode_ == "current") version = parseVersion(PackagedVersion);
    else if (mode_ == "newer") version = parseVersion("2.2.26000");
  }
  bool packageIntact(const std::wstring&) override { ++hashes; return mode_ != "package-bad"; }
  int installMsi(const std::wstring&) override {
    ++installs;
    if (mode_ == "install-fails") return 1603; if (mode_ == "busy") return 1618;
    installedNow = true; return mode_ == "reboot" || mode_ == "reboot-verify-fails" ? 3010 : 0;
  }
  std::string verifyInstalled() override { ++verifies; return mode_ == "verify-fails" || mode_ == "reboot-verify-fails" ? "winfsp_load_failed" : std::string(); }
  void recordOwnership(const std::string&) override { ++owned; }
 private:
  std::string mode_;
};

// The machine: Windows Installer for the version, msiexec for the install, the
// DLL for the check.
class Machine final : public Backend {
 public:
  void installed(bool& present, std::optional<Version>& version) override {
    present = false; version.reset();
    for (DWORD index = 0;; ++index) {
      wchar_t product[40]{};
      if (MsiEnumRelatedProductsW(UpgradeCode, 0, index, product) != ERROR_SUCCESS) break;
      wchar_t text[64]{}; DWORD size = DWORD(std::size(text));
      present = true;
      if (MsiGetProductInfoW(product, INSTALLPROPERTY_VERSIONSTRING, text, &size) != ERROR_SUCCESS) continue;
      const auto parsed = parseVersion(payload::utf8(text));
      if (parsed && (!version || *version < *parsed)) version = parsed;
    }
    if (present) return;
    // Not registered with Windows Installer (copied or installed another way): the DLL still decides.
    const auto dll = library();
    if (dll.empty()) return;
    present = true;
    DWORD ignored = 0; const DWORD size = GetFileVersionInfoSizeW(dll.c_str(), &ignored);
    if (!size) return;
    std::vector<BYTE> data(size); VS_FIXEDFILEINFO* info = nullptr; UINT length = 0;
    if (GetFileVersionInfoW(dll.c_str(), 0, size, data.data()) && VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) && info && length >= sizeof(*info))
      version = Version{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS)};
  }
  bool packageIntact(const std::wstring& path) override {
    try {
      payload::Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
      LARGE_INTEGER size{};
      if (!file || (payload::attributesOf(file.get()) & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !GetFileSizeEx(file.get(), &size) || static_cast<unsigned long long>(size.QuadPart) != MsiSize) return false;
      const auto digest = payload::parseDigest(MsiSha256);
      if (!digest || payload::hashOf(file.get(), MsiSize) != *digest) return false;
    } catch (...) { return false; }
    // The release is signed by the WinFsp project; check the signature as well (the
    // signing certificate itself has expired, the timestamp keeps it valid).
    WINTRUST_FILE_INFO fileInfo{sizeof(fileInfo), path.c_str(), nullptr, nullptr};
    WINTRUST_DATA data{}; data.cbStruct = sizeof(data); data.dwUIChoice = WTD_UI_NONE; data.fdwRevocationChecks = WTD_REVOKE_NONE; data.dwUnionChoice = WTD_CHOICE_FILE; data.pFile = &fileInfo; data.dwStateAction = WTD_STATEACTION_VERIFY; data.dwProvFlags = WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr, &action, &data);
    return status == ERROR_SUCCESS;
  }
  int installMsi(const std::wstring& path) override {
    wchar_t system[MAX_PATH]{}; if (!GetSystemDirectoryW(system, MAX_PATH)) return -1;
    const std::wstring msiexec = std::wstring(system) + L"\\msiexec.exe";
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    const std::wstring log = std::wstring(temp) + L"Multitap-winfsp-install.log";
    std::wstring command = L"\"" + msiexec + L"\" /i \"" + path + L"\" /qn /norestart REBOOT=ReallySuppress /l*v \"" + log + L"\"";
    for (int attempt = 0; attempt < 3; ++attempt) {
      STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
      if (!CreateProcessW(msiexec.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, system, &startup, &process)) return -1;
      CloseHandle(process.hThread);
      DWORD code = 1;
      if (WaitForSingleObject(process.hProcess, 10 * 60 * 1000) != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); code = 1460; }  // ERROR_TIMEOUT
      else GetExitCodeProcess(process.hProcess, &code);
      CloseHandle(process.hProcess);
      if (code == 1618 && attempt < 2) { Sleep(5000); continue; }  // another installation is running: wait and retry
      if (code == 0 || code == 3010) DeleteFileW(log.c_str());
      return int(code);
    }
    return 1618;
  }
  std::string verifyInstalled() override {
    const auto dll = library();
    if (dll.empty()) return "winfsp_dll_missing";
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) return "winfsp_load_failed";
    std::string result;
    if (!GetProcAddress(module, "FspFileSystemCreate") || !GetProcAddress(module, "FspFileSystemStartDispatcher") || !GetProcAddress(module, "FspFileSystemAcquireDirectoryBuffer")) result = "winfsp_load_failed";
    else {
      using Version32 = LONG(WINAPI*)(UINT32*);
      const auto service = reinterpret_cast<Version32>(GetProcAddress(module, "FspFsctlServiceVersion"));
      UINT32 driver = 0;
      if (!service || service(&driver) < 0 || !driver) result = "winfsp_driver_missing";  // the kernel driver's service is not registered
    }
    FreeLibrary(module);
    return result;
  }
  void recordOwnership(const std::string& version) override {
    DWORD one = 1; const auto text = payload::widen(version).value_or(L"");
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, OwnerKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(key, L"InstalledByHost", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegSetValueExW(key, L"Version", 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()), DWORD((text.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
  }
 private:
  static std::wstring library() {
    wchar_t path[MAX_PATH]{}; DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\WinFsp", L"InstallDir", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) return {};
    std::wstring directory = path; if (directory.empty()) return {};
    if (directory.back() != L'\\') directory += L'\\';
    directory += L"bin\\winfsp-x64.dll";
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) ? std::wstring() : directory;
  }
};
}  // namespace multitap::winfsp
