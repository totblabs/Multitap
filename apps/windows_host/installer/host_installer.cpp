// Multitap Host installer (Multitap-Host-<version>-windows-x64.exe).
// It carries the Host Windows Installer package and runs it with msiexec:
//   (no switch) | /MultitapManual | --manual        interactive install with progress, result shown
//   /MultitapUpdate | --update                      same, started from Host settings
//   /MultitapUpdate /MultitapService [/MultitapDowngrade] | --service-update [--allow-downgrade]
//                                                   unattended, started by the Host service updater;
//                                                   writes result.json into its working directory
// Every failure says why: a message box in interactive modes, result.json in service mode,
// and the msiexec log path in both.
// An interactive install always has one window on screen that moves: this program's status
// window (StatusWindow below) while it unpacks the package, closes running Host programs and,
// afterwards, tidies up and opens Host settings; Windows Installer's own progress window
// (msiexec /qb, the real progress of the package) in between. Nothing is drawn in service mode.
// The package holds files and the service only. What a first install needs besides them (the
// virtual display driver with its one certificate consent, WinFsp) is finished by the installed
// Host settings, which an interactive install therefore opens when it succeeds
// (apps/windows_host/src/host_prerequisites.hpp). Service mode opens nothing and changes no
// driver or trust; the updated service reports a Host that still needs that step.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <sddl.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "host_installer_text.hpp"

#ifndef MULTITAP_HOST_VERSION
#error "Define MULTITAP_HOST_VERSION, e.g. /DMULTITAP_HOST_VERSION=\"1.0.21\""
#endif
#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)

namespace fs = std::filesystem;
namespace {

constexpr int kPackageResource = 201;
const wchar_t* const kVersion = WIDEN(MULTITAP_HOST_VERSION);
// Every text a person sees (host_installer_text.hpp), in the Windows display
// language of whoever runs the installer.
using multitap::installer_text::Text;
const wchar_t* text(Text id) { return multitap::installer_text::text(id, multitap::ui::processLanguage()); }

enum class Mode { manual, service_update, invalid };

Mode parse(const std::vector<std::wstring>& a) {
  using V = std::vector<std::wstring>;
  if (a.empty() || a == V{L"/MultitapManual"} || a == V{L"--manual"} || a == V{L"/MultitapUpdate"} || a == V{L"--update"}) return Mode::manual;
  if (a == V{L"/MultitapUpdate", L"/MultitapService"} || a == V{L"--service-update"} ||
      a == V{L"/MultitapUpdate", L"/MultitapService", L"/MultitapDowngrade"} || a == V{L"--service-update", L"--allow-downgrade"})
    return Mode::service_update;
  return Mode::invalid;
}

bool elevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION value{}; DWORD size = 0;
  const bool ok = GetTokenInformation(token, TokenElevation, &value, sizeof(value), &size) && value.TokenIsElevated;
  CloseHandle(token); return ok;
}

std::wstring knownFolder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr; std::wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) out = raw;
  CoTaskMemFree(raw); return out;
}

// Only SYSTEM and Administrators may change [path]: owned by one of them, not a link, and no
// allow ACE grants a write right to anyone else (CREATOR OWNER as inherit-only template is fine).
bool adminOnly(const fs::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
  PSID owner = nullptr; PACL dacl = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
  const auto admin = [](PSID sid) { return IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid); };
  bool ok = owner && dacl && admin(owner);
  constexpr ACCESS_MASK write = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE | FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
  for (DWORD i = 0; ok && i < dacl->AceCount; ++i) {
    void* raw = nullptr; if (!GetAce(dacl, i, &raw)) { ok = false; break; }
    const auto* header = static_cast<ACE_HEADER*>(raw);
    if ((header->AceFlags & INHERIT_ONLY_ACE) || header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { ok = false; break; }
    const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
    if ((ace->Mask & write) && !admin(const_cast<DWORD*>(&ace->SidStart))) ok = false;
  }
  LocalFree(descriptor); return ok;
}

// Exactly what protectedFolder makes: a folder (not a link) owned by SYSTEM or Administrators, a
// protected list, SYSTEM and Administrators any right, Users only read and execute (0x1200a9).
bool exactProtectedFolder(const fs::path& path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
  PSID owner = nullptr; PACL dacl = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
  const auto admin = [](PSID sid) { return IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid); };
  SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
  bool ok = owner && admin(owner) && dacl && GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED);
  for (DWORD i = 0; ok && i < dacl->AceCount; ++i) {
    void* raw = nullptr; if (!GetAce(dacl, i, &raw)) { ok = false; break; }
    const auto* header = static_cast<ACE_HEADER*>(raw);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { ok = false; break; }
    const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw); const PSID sid = const_cast<DWORD*>(&ace->SidStart);
    if (admin(sid)) continue;
    ok = IsWellKnownSid(sid, WinBuiltinUsersSid) && (ace->Mask & ~ACCESS_MASK(0x1200a9)) == 0;
  }
  LocalFree(descriptor); return ok && adminOnly(path);
}
// A folder only SYSTEM and Administrators may change (Users read). One
// that is already there but is not exactly that (ProgramData lets every user create folders; one made
// first by a user keeps its files, links and open handles even after its list is reset) is never
// adopted: a link or an empty folder is removed, anything else moved aside to
// "<name>.untrusted-{GUID}" (never deleted), and a new one is created.
bool protectedFolder(const fs::path& path) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)", SDDL_REVISION_1, &descriptor, nullptr)) return false;
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  bool ok = false;
  for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
    if (CreateDirectoryW(path.c_str(), &attributes)) { ok = exactProtectedFolder(path); break; }
    if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    if (exactProtectedFolder(path)) { ok = true; break; }
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) && ((a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str()))) continue;
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && RemoveDirectoryW(path.c_str())) continue;   // empty
    GUID guid{}; wchar_t suffix[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, suffix, 40) || !MoveFileExW(path.c_str(), (path.wstring() + L".untrusted-" + suffix).c_str(), 0)) break;
  }
  LocalFree(descriptor);
  return ok;
}
// Writes a new file (never an existing one) with [sddl] as its list from the start.
// [written] is told how many bytes are on their way to the disk, after each megabyte and at the
// end (the status window's bar: bytes of a known size).
using BytesWritten = std::function<void(DWORD done, DWORD total)>;
bool writeNewFile(const fs::path& path, const void* bytes, DWORD size, const wchar_t* sddl, const BytesWritten& written = {}) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (sddl && !ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr)) return false;
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, descriptor ? &attributes : nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (descriptor) LocalFree(descriptor);
  if (file == INVALID_HANDLE_VALUE) return false;
  constexpr DWORD kChunk = 1024 * 1024;
  bool ok = true; DWORD done = 0;
  while (ok && done < size) {
    const DWORD part = (std::min)(kChunk, size - done); DWORD count = 0;
    ok = WriteFile(file, static_cast<const BYTE*>(bytes) + done, part, &count, nullptr) && count == part;
    if (ok) { done += part; if (written) written(done, size); }
  }
  ok = ok && FlushFileBuffers(file);
  CloseHandle(file);
  if (!ok) DeleteFileW(path.c_str());
  return ok;
}
// Opened for reading, shared for reading only: nobody can change, replace or delete the package while
// this handle is held (until msiexec has finished with it).
HANDLE holdPackage(const fs::path& path) {
  const HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (held == INVALID_HANDLE_VALUE) return nullptr;
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (!GetFileInformationByHandleEx(held, FileAttributeTagInfo, &tag, sizeof(tag)) || (tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) { CloseHandle(held); return nullptr; }
  return held;
}

// The package stays where Windows Installer finds it again for Repair and for removal:
// ProgramData\Multitap\Installer\Host\<version>\package.msi, a chain only administrators can
// change (else Repair would run a package a user swapped in). Without such a chain it falls
// back to a per-run TEMP folder (Repair then needs the installer again).
fs::path packageCacheRoot() {
  const std::wstring data = knownFolder(FOLDERID_ProgramData);
  return data.empty() ? fs::path() : fs::path(data) / L"Multitap" / L"Installer" / L"Host";
}
bool cachedPackage(fs::path& package, const void* bytes, DWORD size, const BytesWritten& written = {}) {
  const fs::path root = packageCacheRoot();
  if (root.empty()) return false;
  const fs::path vendor = root.parent_path().parent_path();  // ProgramData\Multitap
  if (!adminOnly(vendor) || !protectedFolder(root.parent_path()) || !protectedFolder(root)) return false;
  const fs::path folder = root / kVersion;
  if (!protectedFolder(folder)) return false;
  std::error_code ignored;
  for (fs::directory_iterator it(folder, ignored), end; it != end; it.increment(ignored)) fs::remove_all(it->path(), ignored);
  const fs::path temporary = folder / L"package.msi.new";
  package = folder / L"package.msi";
  return writeNewFile(temporary, bytes, size, L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)", written) &&
         MoveFileExW(temporary.c_str(), package.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
// After a successful install only this version's package is kept.
void removeOtherCachedPackages() {
  const fs::path root = packageCacheRoot();
  std::error_code error;
  if (root.empty() || !adminOnly(root)) return;
  for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
    std::error_code e;
    if (it->is_directory(e) && !it->is_symlink(e) && _wcsicmp(it->path().filename().c_str(), kVersion) != 0) fs::remove_all(it->path(), e);
  }
}

// The log goes to a fresh per-run TEMP folder; the package to the cache (or there as well).
// [held] keeps the package from being changed until msiexec has finished.
bool extract(fs::path& folder, fs::path& package, bool& cached, HANDLE& held, const BytesWritten& written = {}) {
  held = nullptr;
  const HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(kPackageResource), RT_RCDATA);
  const HGLOBAL loaded = found ? LoadResource(nullptr, found) : nullptr;
  const void* bytes = loaded ? LockResource(loaded) : nullptr;
  const DWORD size = found ? SizeofResource(nullptr, found) : 0;
  if (!bytes || size < 1024) return false;
  wchar_t temp[MAX_PATH + 1]{};
  if (!GetTempPathW(MAX_PATH, temp)) return false;
  folder = fs::path(temp) / (L"MultitapHostSetup-" + std::wstring(kVersion) + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  if (!CreateDirectoryW(folder.c_str(), nullptr)) return false;
  cached = cachedPackage(package, bytes, size, written);
  if (!cached) {
    package = folder / L"package.msi";
    if (!writeNewFile(package, bytes, size, nullptr, written)) return false;
  }
  held = holdPackage(package);
  return held != nullptr;
}

// A window of [process] that is on screen: Windows Installer's progress window, or Host
// settings once it has drawn its first picture (it is created hidden and shown then).
bool hasVisibleWindow(DWORD process) {
  struct Search { DWORD process; bool found; } search{process, false};
  EnumWindows([](HWND window, LPARAM value) -> BOOL {
    auto* s = reinterpret_cast<Search*>(value); DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != s->process || !IsWindowVisible(window)) return TRUE;
    s->found = true; return FALSE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.found;
}

// How long msiexec gets to put its progress window on screen before the status window says
// "installing" itself (it then keeps looking for that window).
constexpr ULONGLONG kMsiexecWindowWaitMs = 15000;
// [window] is told once with true when msiexec's own progress window is on screen, and once
// with false when it still is not after kMsiexecWindowWaitMs (never in quiet mode, where it
// has none).
DWORD runMsiexec(const fs::path& package, const fs::path& log, bool quiet, const std::function<void(bool shown)>& window = {}) {
  wchar_t system[MAX_PATH + 1]{};
  if (!GetSystemDirectoryW(system, MAX_PATH)) return ERROR_FILE_NOT_FOUND;
  const fs::path msiexec = fs::path(system) / L"msiexec.exe";
  std::wstring command = L"\"" + msiexec.wstring() + L"\" /i \"" + package.wstring() + L"\" " + (quiet ? L"/qn" : L"/qb") +
                         L" /norestart REBOOT=ReallySuppress /l*v \"" + log.wstring() + L"\"";
  STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
  if (!CreateProcessW(msiexec.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return GetLastError();
  CloseHandle(process.hThread);
  DWORD code = ERROR_TIMEOUT;
  const ULONGLONG started = GetTickCount64(), deadline = started + 30ull * 60 * 1000;
  bool watching = !quiet && window, late = false;
  for (;;) {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) break;
    const DWORD wait = watching ? 100 : DWORD(deadline - now);
    if (WaitForSingleObject(process.hProcess, wait) == WAIT_OBJECT_0) { GetExitCodeProcess(process.hProcess, &code); break; }
    if (!watching) continue;
    if (hasVisibleWindow(process.dwProcessId)) { watching = false; window(true); }
    else if (!late && GetTickCount64() - started >= kMsiexecWindowWaitMs) { late = true; window(false); }
  }
  CloseHandle(process.hProcess); return code;
}

bool succeeded(DWORD code) { return code == ERROR_SUCCESS || code == ERROR_SUCCESS_REBOOT_REQUIRED; }

// The single-instance lock Global\MultitapHostInstaller (also read by the Host's removal cleanup,
// host_removal_native.hpp). Any user may create a named object in Global\ first,
// so it is created with a SYSTEM/Administrators list, and one that already exists counts
// as another installer of ours only when its owner is SYSTEM or Administrators. One that is not ours,
// or cannot be opened at all, is noted and ignored: it never blocks an install or an update.
enum class SingleInstance { ours, running, foreign };
bool administrativeOwner(HANDLE object) {
  PSID owner = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetSecurityInfo(object, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS) return false;
  const bool ok = owner && (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid));
  LocalFree(descriptor); return ok;
}
SingleInstance takeSingleInstance(HANDLE& held) {
  held = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) return SingleInstance::foreign;
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  held = CreateMutexW(&security, TRUE, L"Global\\MultitapHostInstaller");
  const DWORD error = GetLastError();
  LocalFree(descriptor);
  if (!held) return SingleInstance::foreign;  // e.g. access denied, or the name is another kind of object
  if (error != ERROR_ALREADY_EXISTS) return SingleInstance::ours;
  if (administrativeOwner(held)) return SingleInstance::running;
  CloseHandle(held); held = nullptr; return SingleInstance::foreign;
}
// A note next to the msiexec log of this run (the log folder is the installer's own).
void note(const fs::path& folder, const char* text) {
  const HANDLE file = CreateFileW((folder / L"installer-notes.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0; WriteFile(file, text, DWORD(lstrlenA(text)), &written, nullptr); WriteFile(file, "\r\n", 2, &written, nullptr);
  CloseHandle(file);
}

std::wstring reason(DWORD code) {
  switch (code) {
    case ERROR_INSTALL_USEREXIT: return text(Text::Cancelled);
    case ERROR_INSTALL_ALREADY_RUNNING: return text(Text::OtherInstall);
    case ERROR_PRODUCT_VERSION: return text(Text::SameVersion);
    case ERROR_INSTALL_PLATFORM_UNSUPPORTED: return text(Text::Unsupported);
    case ERROR_INSTALL_FAILURE: return text(Text::RolledBack);
    case ERROR_TIMEOUT: return text(Text::TimedOut);
    default: return text(Text::Failed);
  }
}

fs::path hostRoot() {
  const std::wstring programs = knownFolder(FOLDERID_ProgramFiles);
  return programs.empty() ? fs::path() : fs::path(programs) / L"Multitap" / L"Host";
}
// The one fixed install folder; earlier versions used Host\<version>\bin.
fs::path trayImage() { return hostRoot() / L"bin" / L"MultitapHost.exe"; }

// Host programs in user sessions (tray, settings, stream workers) run from the install folder,
// which the package now overwrites in place. Close every executable under
// <Program Files>\Multitap\Host\ outside session 0 before msiexec, so no file is in use
// (otherwise Windows Installer would ask for a restart). Returns the sessions that had a tray.
std::vector<DWORD> closeSessionProcesses() {
  std::vector<DWORD> traySessions;
  const fs::path rootPath = hostRoot();
  if (rootPath.empty()) return traySessions;
  const std::wstring root = rootPath.wstring() + L"\\";
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return traySessions;
  PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
  for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
    DWORD session = 0;
    if (entry.th32ProcessID == GetCurrentProcessId() || !ProcessIdToSessionId(entry.th32ProcessID, &session) || session == 0) continue;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
    if (!process) continue;
    wchar_t image[MAX_PATH * 2]{}; DWORD length = MAX_PATH * 2;
    if (QueryFullProcessImageNameW(process, 0, image, &length)) {
      const std::wstring path(image, length);
      if (CompareStringOrdinal(path.c_str(), int(root.size()), root.c_str(), int(root.size()), TRUE) == CSTR_EQUAL &&
          path.size() >= 4 && _wcsicmp(path.c_str() + path.size() - 4, L".exe") == 0) {
        // The tray (and a stream worker) is MultitapHost.exe: a session that had one gets a tray back,
        // as the Startup shortcut would have given it at sign-in.
        if (_wcsicmp(fs::path(path).filename().c_str(), L"MultitapHost.exe") == 0 &&
            std::find(traySessions.begin(), traySessions.end(), session) == traySessions.end())
          traySessions.push_back(session);
        TerminateProcess(process, 0); WaitForSingleObject(process, 5000);
      }
    }
    CloseHandle(process);
  }
  CloseHandle(snapshot);
  return traySessions;
}

void startTray();

// Service mode runs as SYSTEM: start the tray as the signed-in user of each session that had one.
// Without SYSTEM rights (an administrator ran the service command line) only the own session's
// tray can be started, through Explorer.
void startTrayInSessions(const std::vector<DWORD>& sessions) {
  const fs::path image = trayImage();
  std::error_code ignored;
  if (sessions.empty() || !fs::exists(image, ignored)) return;
  DWORD own = 0; ProcessIdToSessionId(GetCurrentProcessId(), &own);
  for (const DWORD session : sessions) {
    HANDLE token = nullptr;
    if (!WTSQueryUserToken(session, &token)) { if (session == own) startTray(); continue; }
    void* environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, token, FALSE)) environment = nullptr;
    std::wstring command = L"\"" + image.wstring() + L"\" --tray";
    wchar_t desktop[] = L"winsta0\\default";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.lpDesktop = desktop;
    PROCESS_INFORMATION process{};
    if (CreateProcessAsUserW(token, image.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                             environment, image.parent_path().c_str(), &startup, &process)) {
      CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    if (environment) DestroyEnvironmentBlock(environment);
    CloseHandle(token);
  }
}

// Earlier Host versions used a <Program Files>\Multitap\Host\<x.y.z> folder each (upgrades
// left the ones whose files were in use) and failed installs a ".failed-*" one.
// The program lives in Host\bin, so nothing points at them after a successful
// install: remove them, best effort. Only those folders directly under Host\; Host data lives in
// ProgramData and is never touched here.
void removeOldVersionFolders() {
  const fs::path root = hostRoot();
  if (root.empty()) return;
  std::error_code error;
  for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
    std::error_code e;
    if (!it->is_directory(e) || it->is_symlink(e)) continue;
    const std::wstring name = it->path().filename().wstring();
    bool version = !name.empty();
    int dots = 0;
    for (wchar_t c : name) { if (c == L'.') ++dots; else if (c < L'0' || c > L'9') version = false; }
    if ((version && dots == 2 && name.front() != L'.' && name.back() != L'.') || name.rfind(L".failed-", 0) == 0) fs::remove_all(it->path(), e);
  }
}

// Starts the new tray for the signed-in user through Explorer, so it runs unelevated.
void startTray() {
  const fs::path link = fs::path(knownFolder(FOLDERID_CommonStartup)) / L"Multitap Host Tray.lnk";
  std::error_code ignored;
  if (!fs::exists(link, ignored)) return;
  wchar_t windows[MAX_PATH + 1]{};
  if (!GetWindowsDirectoryW(windows, MAX_PATH)) return;
  const std::wstring explorer = (fs::path(windows) / L"explorer.exe").wstring(), argument = L"\"" + link.wstring() + L"\"";
  ShellExecuteW(nullptr, L"open", explorer.c_str(), argument.c_str(), nullptr, SW_SHOWNORMAL);
}

// Result for the Host service updater (host_update.hpp parseHostUpdateResult: exactly these 8 fields).
// It is written only into the stage the service downloaded this installer to:
// ProgramData\Multitap-Updates\{GUID}\ (host_update.hpp hostUpdateStageRoot, hostUpdateStageName).
bool updateStage(fs::path& stage) {
  wchar_t image[MAX_PATH * 2]{};
  const DWORD length = GetModuleFileNameW(nullptr, image, MAX_PATH * 2);
  if (!length || length >= MAX_PATH * 2) return false;
  stage = fs::path(image).parent_path();
  const fs::path root = fs::path(knownFolder(FOLDERID_ProgramData)) / L"Multitap-Updates";
  const std::wstring name = stage.filename().wstring();
  bool guid = name.size() == 38 && name.front() == L'{' && name.back() == L'}';
  for (size_t i = 1; guid && i + 1 < name.size(); ++i) {
    const wchar_t c = name[i];
    guid = (i == 9 || i == 14 || i == 19 || i == 24) ? c == L'-' : ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F'));
  }
  return guid && !root.empty() && _wcsicmp(stage.parent_path().c_str(), root.c_str()) == 0;
}

void writeServiceResult(bool completed, bool reboot, const char* code) {
  fs::path stage;
  if (!updateStage(stage)) return;
  std::string version; for (const wchar_t* c = kVersion; *c; ++c) version.push_back(char(*c));
  const std::string json = std::string("{\"schema\":1,\"product\":\"host\",\"outcome\":\"") + (completed ? "completed" : "failed") +
      "\",\"completed\":" + (completed ? "6" : "0") + ",\"rebootRequired\":" + (reboot ? "true" : "false") +
      ",\"attention\":false,\"version\":\"" + version + "\",\"code\":\"" + code + "\"}";
  const fs::path temporary = stage / L"result.json.new", result = stage / L"result.json";
  const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  const bool ok = WriteFile(file, json.data(), DWORD(json.size()), &written, nullptr) && written == json.size() && FlushFileBuffers(file);
  CloseHandle(file);
  if (ok) MoveFileExW(temporary.c_str(), result.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// The status window of an interactive install: one sentence (what is being done now), a bar,
// and a line that asks to wait. Charcoal and amber, drawn here (no common control). The bar
// is filled only from a real measure (the bytes of the package written so far); for every other
// step, whose length this program cannot know, a short piece runs along it.
// The install runs on its own thread and only posts to this window, so the window always
// answers Windows (never "응답 없음"). It has no close button: closing it would not stop
// Windows Installer.
struct StatusView { Text stage = Text::StatusUnpacking; bool measured = false; double fraction = 0; };
struct StatusColors {
  static constexpr COLORREF canvas = RGB(0x14, 0x1C, 0x21), ink = RGB(0xE9, 0xF1, 0xF3), muted = RGB(0x9A, 0xAD, 0xB6),
                            track = RGB(0x2B, 0x39, 0x41), amber = RGB(0xF5, 0xA6, 0x23);
};
constexpr int kStatusWidth = 440, kStatusHeight = 132;  // client size at 96 DPI
// The filled part of the bar, in pixels of a [width] wide track: from the left for a measured
// fraction; else a piece a third of the track wide that crosses it every 1.4 s ([tick] ms).
void statusBarSpan(const StatusView& view, int width, ULONGLONG tick, int& left, int& right) {
  if (view.measured) { left = 0; right = int(width * (std::min)(1.0, (std::max)(0.0, view.fraction)) + 0.5); return; }
  const int piece = width / 3; const double at = double(tick % 1400) / 1400.0;
  left = int((width + piece) * at) - piece; right = left + piece;
  left = (std::max)(0, left); right = (std::min)(width, right);
}
void paintStatus(HDC dc, const RECT& client, const StatusView& view, UINT dpi, ULONGLONG tick) {
  const auto px = [dpi](int value) { return MulDiv(value, int(dpi), 96); };
  const HBRUSH canvas = CreateSolidBrush(StatusColors::canvas); FillRect(dc, &client, canvas); DeleteObject(canvas);
  const wchar_t* face = multitap::ui::korean() ? L"Malgun Gothic" : L"Segoe UI";
  const HFONT title = CreateFontW(-px(16), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
  const HFONT body = CreateFontW(-px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
  SetBkMode(dc, TRANSPARENT);
  const int left = px(24), right = client.right - px(24);
  const HGDIOBJ before = SelectObject(dc, body);
  SetTextColor(dc, StatusColors::muted);
  RECT line{left, px(20), right, px(38)};
  const std::wstring product = multitap::ui::fill(text(Text::StatusVersion), {kVersion});
  DrawTextW(dc, product.c_str(), -1, &line, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  SelectObject(dc, title); SetTextColor(dc, StatusColors::ink);
  line = RECT{left, px(42), right, px(66)};
  DrawTextW(dc, text(view.stage), -1, &line, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  RECT track{left, px(78), right, px(84)};
  const HBRUSH trackBrush = CreateSolidBrush(StatusColors::track); FillRect(dc, &track, trackBrush); DeleteObject(trackBrush);
  int from = 0, to = 0; statusBarSpan(view, track.right - track.left, tick, from, to);
  if (to > from) { RECT bar{track.left + from, track.top, track.left + to, track.bottom}; const HBRUSH amber = CreateSolidBrush(StatusColors::amber); FillRect(dc, &bar, amber); DeleteObject(amber); }
  SelectObject(dc, body); SetTextColor(dc, StatusColors::muted);
  line = RECT{left, px(94), right, px(114)};
  DrawTextW(dc, text(Text::StatusWait), -1, &line, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  SelectObject(dc, before); DeleteObject(title); DeleteObject(body);
}

class StatusWindow {
 public:
  // Any thread. Without a window (service mode, or one that could not be made) these do nothing.
  void stage(Text id) { set([&](StatusView& v) { v.stage = id; v.measured = false; v.fraction = 0; }); }
  void bytes(DWORD done, DWORD total) { set([&](StatusView& v) { v.measured = total != 0; v.fraction = total ? double(done) / double(total) : 0; }); }
  void hide() { if (window_) SendMessageW(window_, kVisible, FALSE, 0); }
  void show(Text id) { stage(id); if (window_) SendMessageW(window_, kVisible, TRUE, 0); }
  StatusView view() { std::lock_guard<std::mutex> lock(mutex_); return view_; }

  // The window's thread: makes the window, runs [work] on another thread and answers Windows
  // until that thread has ended. Returns what [work] returned.
  int run(const std::function<int()>& work) {
    WNDCLASSEXW type{}; type.cbSize = sizeof(type); type.lpfnWndProc = procedure; type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.lpszClassName = L"MultitapHostInstallerStatus";
    type.hIcon = LoadIconW(type.hInstance, MAKEINTRESOURCEW(1)); type.hIconSm = type.hIcon;
    RegisterClassExW(&type);
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_MINIMIZEBOX;
    window_ = CreateWindowExW(0, type.lpszClassName, text(Text::Title), style, CW_USEDEFAULT, CW_USEDEFAULT, kStatusWidth, kStatusHeight, nullptr, nullptr, type.hInstance, this);
    if (window_) {
      const UINT dpi = GetDpiForWindow(window_);
      RECT frame{0, 0, MulDiv(kStatusWidth, int(dpi), 96), MulDiv(kStatusHeight, int(dpi), 96)};
      AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
      MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
      GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTOPRIMARY), &monitor);
      const int width = frame.right - frame.left, height = frame.bottom - frame.top;
      SetWindowPos(window_, nullptr, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
                   monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
      // On screen at once, before the first byte is unpacked.
      ShowWindow(window_, SW_SHOWNORMAL); UpdateWindow(window_); SetForegroundWindow(window_);
      SetTimer(window_, 1, 33, nullptr);
    }
    struct Job { const std::function<int()>* work; int result; } job{&work, 0};
    const HANDLE thread = CreateThread(nullptr, 0, [](void* raw) -> DWORD { auto* j = static_cast<Job*>(raw); j->result = (*j->work)(); return 0; }, &job, 0, nullptr);
    if (!thread) { job.result = work(); }
    else {
      for (;;) {
        const DWORD woke = MsgWaitForMultipleObjects(1, &thread, FALSE, INFINITE, QS_ALLINPUT);
        if (woke == WAIT_OBJECT_0 || woke == WAIT_FAILED) break;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
      }
      CloseHandle(thread);
    }
    if (window_) { const HWND window = window_; window_ = nullptr; DestroyWindow(window); }
    return job.result;
  }

 private:
  static constexpr UINT kChanged = WM_APP + 1, kVisible = WM_APP + 2;
  template <class Change> void set(Change&& change) {
    { std::lock_guard<std::mutex> lock(mutex_); change(view_); }
    if (window_) PostMessageW(window_, kChanged, 0, 0);
  }
  static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<StatusWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
      case WM_ERASEBKGND: return 1;
      case WM_PAINT: {
        PAINTSTRUCT paint{}; const HDC dc = BeginPaint(window, &paint);
        RECT client{}; GetClientRect(window, &client);
        // Drawn off screen first: the moving piece never flickers.
        const HDC memory = CreateCompatibleDC(dc); const HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        const HGDIOBJ before = SelectObject(memory, bitmap);
        paintStatus(memory, client, self ? self->view() : StatusView{}, GetDpiForWindow(window), GetTickCount64());
        BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, before); DeleteObject(bitmap); DeleteDC(memory);
        EndPaint(window, &paint); return 0;
      }
      case WM_TIMER: case kChanged: InvalidateRect(window, nullptr, FALSE); return 0;
      case kVisible:
        if (wp) { ShowWindow(window, SW_SHOWNORMAL); SetForegroundWindow(window); UpdateWindow(window); }
        else ShowWindow(window, SW_HIDE);
        return 0;
      case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
      }
      case WM_CLOSE: return 0;  // Alt+F4: the install goes on, so does its window
    }
    return DefWindowProcW(window, message, wp, lp);
  }
  std::mutex mutex_; StatusView view_; HWND window_ = nullptr;
};
StatusWindow* statusWindow = nullptr;  // the interactive install's window; null in service mode

// A message box is the only window while it is open: the status window steps aside.
void show(const std::wstring& message, UINT icon) {
  if (statusWindow) statusWindow->hide();
  MessageBoxW(nullptr, message.c_str(), text(Text::Title), MB_OK | icon | MB_SETFOREGROUND);
}

// Opens the installed Host settings. It needs administrator rights and this installer already
// has them, so Windows asks nothing again. Only the program this package just installed, in a
// folder only administrators can change.
// [settled] waits (at most 30 s) until Host settings has drawn its window, so the status
// window stays until the next window is really there.
bool openHostSettings(bool settled = false) {
  const fs::path root = hostRoot();
  if (root.empty()) return false;
  const fs::path folder = root / L"bin", image = folder / L"MultitapHostSettings.exe";
  if (!adminOnly(image)) return false;
  SHELLEXECUTEINFOW launch{}; launch.cbSize = sizeof(launch);
  launch.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS; launch.lpVerb = L"open";
  launch.lpFile = image.c_str(); launch.lpParameters = L"--host-settings"; launch.lpDirectory = folder.c_str(); launch.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&launch)) return false;
  if (launch.hProcess) {
    const DWORD process = GetProcessId(launch.hProcess);
    for (int poll = 0; settled && process && poll < 300 && !hasVisibleWindow(process); ++poll)
      if (WaitForSingleObject(launch.hProcess, 100) == WAIT_OBJECT_0) break;  // it ended: nothing to wait for
    CloseHandle(launch.hProcess);
  }
  return true;
}

int installPackage(bool quiet, bool foreignLock);

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // First thing: this program is started from Downloads, where anyone may
  // leave a DLL next to it. Every library it loads from now on comes from System32; wtsapi32 and userenv
  // are delay-loaded (tools/host-installer/build.ps1) so none of them is resolved before this line.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
  int count = 0; wchar_t** values = CommandLineToArgvW(GetCommandLineW(), &count);
  std::vector<std::wstring> args; for (int n = 1; values && n < count; ++n) args.emplace_back(values[n]);
  if (values) LocalFree(values);
  const Mode mode = parse(args);
  const bool quiet = mode == Mode::service_update;
  if (mode == Mode::invalid) { show(text(Text::UnknownOption), MB_ICONERROR); return ERROR_INVALID_PARAMETER; }

  HANDLE single = nullptr;
  const SingleInstance instance = takeSingleInstance(single);
  if (instance == SingleInstance::running) {
    if (quiet) writeServiceResult(false, false, "launch_failed"); else show(text(Text::AlreadyRunning), MB_ICONINFORMATION);
    return ERROR_INSTALL_ALREADY_RUNNING;
  }
  if (!elevated()) {
    if (quiet) writeServiceResult(false, false, "launch_failed"); else show(text(Text::NeedsAdmin), MB_ICONERROR);
    return ERROR_ELEVATION_REQUIRED;
  }

  // Interactive: the install runs on its own thread under the status window, which this
  // (first) thread keeps answering. Service mode: no window, the same steps on this thread.
  const auto install = [&]() -> int { return installPackage(quiet, instance == SingleInstance::foreign); };
  if (quiet) return install();
  StatusWindow status; statusWindow = &status;
  const int result = status.run(install);
  statusWindow = nullptr;
  return result;
}

namespace {
int installPackage(bool quiet, bool foreignLock) {
  StatusWindow inert; StatusWindow& status = statusWindow ? *statusWindow : inert;
  status.stage(Text::StatusUnpacking);
  fs::path folder, package; bool cached = false; HANDLE heldPackage = nullptr;
  if (!extract(folder, package, cached, heldPackage, [&](DWORD done, DWORD total) { status.bytes(done, total); })) {
    if (quiet) writeServiceResult(false, false, "stage_failed"); else show(text(Text::Unpack), MB_ICONERROR);
    return ERROR_WRITE_FAULT;
  }
  const fs::path log = folder / L"install.log";
  if (foreignLock) note(folder, "Global\\MultitapHostInstaller exists but is not owned by SYSTEM or Administrators (or cannot be opened): not ours, ignored");
  status.stage(Text::StatusClosing);
  const std::vector<DWORD> traySessions = closeSessionProcesses();
  // Until Windows Installer's own progress window is on screen this one stays; then it steps
  // aside (one progress window at a time) and comes back when msiexec has ended.
  status.stage(Text::StatusStarting);
  const DWORD code = runMsiexec(package, log, quiet, [&](bool shown) { if (shown) status.hide(); else status.stage(Text::StatusInstalling); });
  // Back only for what follows a successful install. After a failure or a cancel nothing
  // long follows: the message box (or nothing, for a cancel) is next.
  if (succeeded(code)) status.show(Text::StatusFinishing); else status.hide();
  if (heldPackage) CloseHandle(heldPackage);
  // The log stays for diagnosis; a cached package stays for Repair and removal.
  std::error_code ignored; if (!cached) fs::remove(package, ignored);
  const auto restartTray = [&] { if (quiet) startTrayInSessions(traySessions); else startTray(); };

  if (succeeded(code)) {
    if (cached) removeOtherCachedPackages();
    removeOldVersionFolders();
    restartTray();
    if (quiet) { writeServiceResult(true, code == ERROR_SUCCESS_REBOOT_REQUIRED, ""); return 0; }
    // Host settings is the result screen: it shows the installed version and, on a first
    // install, finishes it (virtual display driver, WinFsp) after one confirmation. A message
    // box comes first only when Windows asked for a restart, or instead when it did not open.
    // The fixed install folder: the window says where it went.
    const std::wstring done = multitap::ui::fill(text(Text::Done), {kVersion}) + (hostRoot().empty() ? std::wstring() : multitap::ui::fill(text(Text::Location), {hostRoot().wstring()}));
    if (code == ERROR_SUCCESS_REBOOT_REQUIRED) show(done + text(Text::Restart), MB_ICONINFORMATION);
    status.show(Text::StatusOpening);
    if (!openHostSettings(true) && code != ERROR_SUCCESS_REBOOT_REQUIRED)
      show(done + text(Text::OpenSettings), MB_ICONINFORMATION);
    return 0;
  }
  restartTray(); // the previous version is back after a rollback
  if (quiet) writeServiceResult(false, false, code == ERROR_PRODUCT_VERSION ? "not_newer" : "install_failed");
  else if (code != ERROR_INSTALL_USEREXIT)
    show(reason(code) + multitap::ui::fill(text(Text::ErrorDetail), {std::to_wstring(code), log.wstring()}), MB_ICONERROR);
  return int(code);
}
} // namespace
