#pragma once
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <optional>
#include <regex>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "../../../../native/windows_admin/release_signature.hpp"
#include "../../../../native/windows_admin/update_relaunch.hpp"

namespace multitap {
// Update: the installer's unattended update mode (no Install click; the
// seamless handoff when the app wrote its record). Manual: the ordinary
// graphical installer with its own Install click and confirmations.
enum class InstallerMode { Update, Manual };
struct ProductUpdateSpec {
  std::wstring source, filename;
  // signature: the catalog's release signature (base64 Ed25519), verified
  // against the pinned project key before any installer starts.
  std::string product, version, digest, signature;
  uint64_t size = 0;
};
inline bool ValidProductUpdate(const ProductUpdateSpec& spec) {
  if ((spec.product != "host" && spec.product != "client") ||
      !std::regex_match(spec.version, std::regex("[0-9]{1,6}\\.[0-9]{1,6}\\.[0-9]{1,6}")) ||
      !std::regex_match(spec.digest, std::regex("[a-f0-9]{64}")) || !spec.size || spec.size > 128ULL*1024*1024) return false;
  const auto name = "Multitap" + std::string(spec.product == "host" ? "-Host" : "") + "-" + spec.version + "-windows-x64.exe";
  return spec.filename == std::wstring(name.begin(), name.end()) && spec.source.size() < 32760 &&
      spec.source.size() > 3 && spec.source[1] == L':' && spec.source[2] == L'\\' &&
      ((spec.source[0] >= L'A' && spec.source[0] <= L'Z') || (spec.source[0] >= L'a' && spec.source[0] <= L'z')) &&
      spec.source.find(L':', 2) == std::wstring::npos && spec.source.find(L'\0') == std::wstring::npos;
}
// The project's own release signature over product, version, file name, exact
// size and SHA-256 (multitap-windows-release-signature-v1). A missing, malformed
// or mismatching signature refuses the update ("signature_invalid"). The
// staged bytes are bound to these fields by the exact size/SHA-256 check.
inline bool ProductUpdateSignatureValid(const ProductUpdateSpec& spec,
    const release_signature::Key& key = release_signature::PinnedKey()) {
  if (!ValidProductUpdate(spec)) return false;
  std::string filename;
  for (const wchar_t c : spec.filename) { if (c > 0x7f) return false; filename.push_back(char(c)); }
  return release_signature::VerifyRelease(spec.product, spec.version, filename, spec.size, spec.digest, spec.signature, key);
}
namespace update_detail {
struct Handle { HANDLE value = INVALID_HANDLE_VALUE; ~Handle(){if(value != INVALID_HANDLE_VALUE)CloseHandle(value);} };
struct Descriptor { PSECURITY_DESCRIPTOR value = nullptr; ~Descriptor(){if(value)LocalFree(value);} };
inline void Require(bool ok) { if(!ok) throw std::runtime_error("installer_verification_failed"); }
inline void TrustedDirectory(HANDLE handle) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  Require(GetFileInformationByHandleEx(handle,FileAttributeTagInfo,&attributes,sizeof(attributes)) &&
      (attributes.FileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(attributes.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT));
  Descriptor descriptor; PSID owner=nullptr; PACL acl=nullptr;
  Require(GetSecurityInfo(handle,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
      &owner,nullptr,&acl,nullptr,&descriptor.value)==ERROR_SUCCESS && acl && owner);
  SECURITY_DESCRIPTOR_CONTROL control{};DWORD revision=0;
  Require(GetSecurityDescriptorControl(descriptor.value,&control,&revision) && (control&SE_DACL_PROTECTED));
  std::array<uint8_t,SECURITY_MAX_SID_SIZE> admins{},system{};DWORD a=DWORD(admins.size()),s=DWORD(system.size());
  Require(CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admins.data(),&a) &&
      CreateWellKnownSid(WinLocalSystemSid,nullptr,system.data(),&s) && EqualSid(owner,admins.data()));
  bool hasAdmins=false,hasSystem=false;
  for(DWORD i=0;i<acl->AceCount;++i) {
    void* raw=nullptr;Require(GetAce(acl,i,&raw));const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
    Require(ace->Header.AceType==ACCESS_ALLOWED_ACE_TYPE && !(ace->Header.AceFlags&INHERITED_ACE));
    const auto sid=const_cast<DWORD*>(&ace->SidStart);
    Require(IsValidSid(sid) && (EqualSid(sid,admins.data()) || EqualSid(sid,system.data())) && (ace->Mask&FILE_ALL_ACCESS)==FILE_ALL_ACCESS);
    hasAdmins=hasAdmins || EqualSid(sid,admins.data());hasSystem=hasSystem || EqualSid(sid,system.data());
  }
  Require(hasAdmins&&hasSystem);
}
inline HANDLE DirectoryHandle(const std::wstring& path) {
  const auto handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE,
      nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  Require(handle!=INVALID_HANDLE_VALUE);return handle;
}
inline bool TrustedDirectoryPath(const std::wstring& path) {
  try { Handle handle{DirectoryHandle(path)}; TrustedDirectory(handle.value); return true; }
  catch (const std::runtime_error&) { return false; }
}
// ProgramData\Multitap-Updates. Standard users may create folders in
// ProgramData, so a root someone else made (wrong owner/ACL, a file or a
// link) is never used: an empty folder or a link is removed, anything else
// is moved aside (never deleted recursively), and a fresh root is created
// with [security]. Installers verify exactly this root, so its path stays.
inline void SecureUpdateRoot(const std::wstring& root, SECURITY_ATTRIBUTES& security) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (CreateDirectoryW(root.c_str(), &security)) { Require(TrustedDirectoryPath(root)); return; }
    Require(GetLastError() == ERROR_ALREADY_EXISTS);
    if (TrustedDirectoryPath(root)) return;
    if (RemoveDirectoryW(root.c_str()) || DeleteFileW(root.c_str())) continue;
    GUID guid{}; Require(SUCCEEDED(CoCreateGuid(&guid)));
    wchar_t suffix[40]{}; Require(StringFromGUID2(guid, suffix, 40) > 0);
    Require(MoveFileExW(root.c_str(), (root + L".untrusted-" + suffix).c_str(), 0) != FALSE);
  }
  Require(false);
}
// The installer's window belongs to a child of the self-extractor. Grant
// foreground permission to that installer process alone (it replaces
// AllowSetForegroundWindow(ASFW_ANY), which let every process take focus).
inline bool SameProcessName(HANDLE process, const wchar_t* name) {
  wchar_t image[32768]{}; DWORD length = 32768;
  return QueryFullProcessImageNameW(process, 0, image, &length) &&
      _wcsicmp(std::filesystem::path(image).filename().c_str(), name) == 0;
}
inline void AllowInstallerForeground(HANDLE extractor, DWORD extractorId) {
  HANDLE watched = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), extractor, GetCurrentProcess(), &watched,
          SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) return;
  try {
    std::thread([watched, extractorId] {
      FILETIME started{}, unused1{}, unused2{}, unused3{};
      if (GetProcessTimes(watched, &started, &unused1, &unused2, &unused3)) {
        // Up to a minute: the payload is unpacked before the setup UI starts.
        for (int i = 0; i < 600 && WaitForSingleObject(watched, 100) == WAIT_TIMEOUT; ++i) {
          Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
          if (snapshot.value == INVALID_HANDLE_VALUE) continue;
          PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry); DWORD child = 0;
          for (BOOL more = Process32FirstW(snapshot.value, &entry); more && !child; more = Process32NextW(snapshot.value, &entry)) {
            if (entry.th32ParentProcessID != extractorId) continue;
            Handle candidate{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)};
            if (!candidate.value) { candidate.value = INVALID_HANDLE_VALUE; continue; }
            FILETIME created{};
            // A process the reused parent id named before the extractor existed is not its child.
            if (GetProcessTimes(candidate.value, &created, &unused1, &unused2, &unused3) && CompareFileTime(&created, &started) >= 0 &&
                SameProcessName(candidate.value, L"MultitapClientSetup.exe"))
              child = entry.th32ProcessID;
          }
          if (child) { AllowSetForegroundWindow(child); break; }
        }
      }
      CloseHandle(watched);
    }).detach();
  } catch (...) { CloseHandle(watched); }
}
struct Hash {
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::vector<uint8_t> object;
  Hash(){try{Require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0);
    DWORD bytes=0,got=0;Require(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&bytes),sizeof(bytes),&got,0)>=0);
    object.resize(bytes);Require(BCryptCreateHash(algorithm,&hash,object.data(),bytes,nullptr,0,0)>=0);}
    catch(...){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);throw;}}
  ~Hash(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
  void Add(uint8_t* bytes,DWORD count){Require(BCryptHashData(hash,bytes,count,0)>=0);}
  std::string Finish(){std::array<uint8_t,32> bytes{};Require(BCryptFinishHash(hash,bytes.data(),DWORD(bytes.size()),0)>=0);
    std::string out;constexpr char hex[]="0123456789abcdef";for(auto b:bytes){out+=hex[b>>4];out+=hex[b&15];}return out;}
};
inline bool ProcessElevated() {
  Handle token; TOKEN_ELEVATION elevation{}; DWORD size = 0;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return false;
  return GetTokenInformation(token.value, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
}
// The same hash over an open handle from the start (verification through the
// very handle that is then held while the installer starts).
inline bool HandleHashes(HANDLE file, uint64_t size, const std::string& digest, const std::function<bool()>& cancelled) {
  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return false;
  Hash hash; std::array<uint8_t, 64 * 1024> buffer{}; uint64_t total = 0;
  for (;;) {
    if (cancelled()) return false;
    DWORD read = 0; if (!ReadFile(file, buffer.data(), DWORD(buffer.size()), &read, nullptr)) return false;
    if (!read) break;
    total += read; if (total > size) return false; hash.Add(buffer.data(), read);
  }
  return total == size && hash.Finish() == digest;
}
}
// The installer EXE's own version resource names exactly the catalog version
// ("x.y.z.0", from VIProductVersion). Checked on the downloaded file before
// the app offers the restart and again on the staged copy before it starts.
inline bool InstallerVersionMatches(const std::wstring& path, const std::string& version) {
  return relaunch::FileVersionIs(path, version, true);
}
// --- Elevation without trusting anything the user can write -----------------
// The unelevated client cannot write the
// administrator-only stage ProgramData\Multitap-Updates. An installer that runs
// elevated from a user-writable folder, however, can be hijacked while the UAC
// prompt is open (a same-user process plants a DLL the self-extractor loads,
// or swaps the file), so the only thing the client ever starts with "runas" is
// its own administrator-protected EXE in Program Files:
//   Multitap.exe --apply-staged-update <mode> <product> <version> <size> <sha256>
//       <signature> <token|-> "<downloaded installer>"
// That elevated instance (RunStagedUpdateHelper) opens no window and takes no
// part in the single-instance logic. It re-verifies the project release
// signature (pinned key), then runs the same code the administrator-owned
// client always used: copy the bytes into the protected ProgramData stage while
// hashing them, hold the staged file, check its version resource and start it
// from there. The user's download folder is only ever read, never executed.

// Exit codes of the elevated instance; low two bits are never zero so they can
// not be mistaken for a handle value or a small Win32 error.
constexpr DWORD kStagedBadArguments = 0xE0000001, kStagedNotElevated = 0xE0000002, kStagedUntrustedHelper = 0xE0000003,
    kStagedSignatureInvalid = 0xE0000004, kStagedStartFailed = 0xE0000005, kStagedInstallerExited = 0xE0000006,
    kStagedDowngrade = 0xE0000007;
constexpr wchar_t kApplyStagedUpdateFlag[] = L"--apply-staged-update";
// The fixed update code for an elevated instance's exit code ("" for none/other).
inline std::string StagedUpdateExitFailure(DWORD code) {
  switch (code) {
    case kStagedSignatureInvalid: return "signature_invalid";
    case kStagedInstallerExited: return "installer_exited";
    case kStagedDowngrade: return "update_not_newer";
    case kStagedBadArguments: case kStagedNotElevated: case kStagedUntrustedHelper: case kStagedStartFailed:
      return "installer_start_failed";
    default: return {};
  }
}
// The fixed update code for a failed "runas" start: the user declining the
// administrator approval is its own, harmless outcome.
inline const char* ElevationFailureCode(DWORD error) { return error == ERROR_CANCELLED ? "elevation_cancelled" : "installer_start_failed"; }
namespace update_detail {
// Every directory from [top] down to [path]'s parent, and [path], owned by an
// administrator-class principal with write access for nobody else, no links.
inline bool ProtectedChain(const std::filesystem::path& top, const std::filesystem::path& path) {
  std::vector<std::filesystem::path> folders;
  for (auto folder = path.parent_path(); ; folder = folder.parent_path()) {
    folders.push_back(folder);
    if (_wcsicmp(folder.c_str(), top.c_str()) == 0) break;
    if (folder == folder.parent_path() || folders.size() > 16) return false;  // never reached [top]
  }
  for (const auto& folder : folders) if (!relaunch::ProtectedEntry(folder, true)) return false;
  return relaunch::ProtectedEntry(path, false);
}
// <programFiles>\Multitap\Client\Multitap.exe (one fixed folder) or the
// earlier <programFiles>\Multitap\Client\<version>\Multitap.exe, spelled exactly so.
inline bool InstalledClientLayout(const std::filesystem::path& exe, const std::filesystem::path& programFiles) {
  if (programFiles.empty() || _wcsicmp(exe.filename().c_str(), L"Multitap.exe") != 0) return false;
  {
    const auto root = exe.parent_path(), vendor = root.parent_path();
    if (_wcsicmp(root.filename().c_str(), L"Client") == 0 && _wcsicmp(vendor.filename().c_str(), L"Multitap") == 0 &&
        _wcsicmp(vendor.parent_path().c_str(), programFiles.c_str()) == 0)
      return true;
  }
  const auto directory = exe.parent_path(), root = directory.parent_path(), vendor = root.parent_path();
  const auto version = relaunch::Narrow(directory.filename().wstring());
  return version && relaunch::ValidVersion(*version) && !programFiles.empty() &&
      _wcsicmp(exe.filename().c_str(), L"Multitap.exe") == 0 && _wcsicmp(root.filename().c_str(), L"Client") == 0 &&
      _wcsicmp(vendor.filename().c_str(), L"Multitap") == 0 && _wcsicmp(vendor.parent_path().c_str(), programFiles.c_str()) == 0;
}
// The only EXE the client may start with "runas": the installed client under
// Program Files, whose whole folder chain only an administrator can change.
inline bool ElevationTargetTrusted(const std::filesystem::path& exe, const std::filesystem::path& programFiles) {
  return InstalledClientLayout(exe, programFiles) && ProtectedChain(programFiles, exe);
}
// This process's environment with TEMP and TMP replaced by [temporary], as a
// sorted CreateProcess block.
inline std::wstring InstallerEnvironment(const std::wstring& temporary) {
  std::vector<std::wstring> entries;
  if (LPWCH current = GetEnvironmentStringsW()) {
    for (const wchar_t* at = current; *at; at += wcslen(at) + 1) {
      if (_wcsnicmp(at, L"TEMP=", 5) == 0 || _wcsnicmp(at, L"TMP=", 4) == 0) continue;
      entries.emplace_back(at);
    }
    FreeEnvironmentStringsW(current);
  }
  entries.push_back(L"TEMP=" + temporary);
  entries.push_back(L"TMP=" + temporary);
  const auto name = [](const std::wstring& entry) { const auto end = entry.find(L'=', 1); return entry.substr(0, end == std::wstring::npos ? entry.size() : end); };
  std::stable_sort(entries.begin(), entries.end(), [&](const std::wstring& a, const std::wstring& b) {
    const auto left = name(a), right = name(b);
    return CompareStringOrdinal(left.c_str(), int(left.size()), right.c_str(), int(right.size()), TRUE) == CSTR_LESS_THAN;
  });
  std::wstring block;
  for (const auto& entry : entries) { block += entry; block.push_back(L'\0'); }
  block.push_back(L'\0');
  return block;
}
inline std::filesystem::path ProgramFilesFolder() {
  PWSTR folder = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &folder))) return {};
  std::filesystem::path path(folder); CoTaskMemFree(folder); return path;
}
inline std::filesystem::path OwnImage() {
  std::wstring image(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, image.data(), DWORD(image.size()));
  if (!length || length >= image.size()) return {};
  image.resize(length); return image;
}
inline bool SafeArgument(const std::string& value, const char* extra) {
  if (value.empty() || value.size() > 256) return false;
  for (const char c : value)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || std::strchr(extra, c))) return false;
  return true;
}
}
// The elevated instance's command line (everything after the program name).
// Empty when a value is not a plain token (nothing is ever composed from free text).
inline std::wstring StagedUpdateArguments(const ProductUpdateSpec& spec, InstallerMode mode, const std::wstring& token) {
  using namespace update_detail;
  if (!ValidProductUpdate(spec) || !SafeArgument(spec.signature, "+/=") || spec.source.find(L'"') != std::wstring::npos ||
      (!token.empty() && !relaunch::ValidToken(token)))
    return {};
  const auto widen = [](const std::string& value) { return std::wstring(value.begin(), value.end()); };
  return std::wstring(kApplyStagedUpdateFlag) + (mode == InstallerMode::Manual ? L" manual " : L" update ") + widen(spec.product) + L" " +
      widen(spec.version) + L" " + std::to_wstring(spec.size) + L" " + widen(spec.digest) + L" " + widen(spec.signature) + L" " +
      (token.empty() ? L"-" : token) + L" \"" + spec.source + L"\"";
}
struct StagedUpdateRequest { ProductUpdateSpec spec; InstallerMode mode = InstallerMode::Update; std::wstring token; };
inline std::optional<StagedUpdateRequest> ParseStagedUpdateArguments(int argc, wchar_t** argv) {
  using namespace update_detail;
  if (argc != 10 || std::wstring(argv[1]) != kApplyStagedUpdateFlag) return std::nullopt;
  const std::wstring mode = argv[2];
  if (mode != L"update" && mode != L"manual") return std::nullopt;
  const auto product = relaunch::Narrow(argv[3]), version = relaunch::Narrow(argv[4]), size = relaunch::Narrow(argv[5]),
      digest = relaunch::Narrow(argv[6]), signature = relaunch::Narrow(argv[7]);
  if (!product || !version || !size || !digest || !signature || size->empty() || size->size() > 12 ||
      size->find_first_not_of("0123456789") != std::string::npos || !SafeArgument(*signature, "+/="))
    return std::nullopt;
  StagedUpdateRequest request;
  request.mode = mode == L"manual" ? InstallerMode::Manual : InstallerMode::Update;
  auto& spec = request.spec;
  spec.product = *product; spec.version = *version; spec.digest = *digest; spec.signature = *signature;
  spec.size = std::stoull(*size); spec.source = argv[9];
  const std::string name = "Multitap" + std::string(spec.product == "host" ? "-Host" : "") + "-" + spec.version + "-windows-x64.exe";
  spec.filename = std::wstring(name.begin(), name.end());
  if (std::wstring(argv[8]) != L"-") request.token = argv[8];
  if (!ValidProductUpdate(spec) || (!request.token.empty() && !relaunch::ValidToken(request.token))) return std::nullopt;
  return request;
}
// The unelevated client's launch. The downloaded file is verified here first
// (signature, exact size, SHA-256 and version resource, through the handle it is
// read from) so a bad file never costs the user an approval prompt, but nothing
// elevated starts from this folder: the "runas" target is the installed,
// administrator-protected client EXE (see above), which verifies everything
// again against the very bytes it copies. Declining the prompt reports
// "elevation_cancelled" and leaves everything as it was (nothing was staged).
// [process_out] receives the elevated instance's handle: with a handoff token
// it stays alive until the installer accepted the update (the caller waits for
// it and reads its exit code); without one it has finished and exited 0.
inline bool LaunchVerifiedProductUpdateUnelevated(const ProductUpdateSpec& spec,
    const std::function<bool()>& cancelled, const std::function<bool()>& commit,
    InstallerMode mode, HANDLE* process_out, std::string* failure, const std::wstring& token) {
  using namespace update_detail;
  const auto fail = [&](const char* code) { if (failure) *failure = code; return false; };
  try {
    Require(!cancelled());
    // Never elevate an EXE the user (or anything running as the user) can replace.
    const auto self = OwnImage();
    Require(!self.empty() && ElevationTargetTrusted(self, ProgramFilesFolder()));
    const auto arguments = StagedUpdateArguments(spec, mode, token);
    Require(!arguments.empty());
    {
      Handle source{CreateFileW(spec.source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
      Require(source.value != INVALID_HANDLE_VALUE && GetFileType(source.value) == FILE_TYPE_DISK);
      FILE_ATTRIBUTE_TAG_INFO attrs{}; LARGE_INTEGER length{};
      Require(GetFileInformationByHandleEx(source.value, FileAttributeTagInfo, &attrs, sizeof(attrs)) &&
          !(attrs.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
          GetFileSizeEx(source.value, &length) && uint64_t(length.QuadPart) == spec.size);
      Require(HandleHashes(source.value, spec.size, spec.digest, cancelled));
      std::array<uint8_t, 2> mz{}; LARGE_INTEGER zero{}; DWORD read = 0;
      Require(SetFilePointerEx(source.value, zero, nullptr, FILE_BEGIN) && ReadFile(source.value, mz.data(), 2, &read, nullptr) && read == 2 && mz[0] == 'M' && mz[1] == 'Z');
    }
    Require(InstallerVersionMatches(spec.source, spec.version));
    Require(!cancelled() && commit());
    const auto directory = self.parent_path().wstring(), program = self.wstring();
    SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC; info.lpVerb = L"runas"; info.lpFile = program.c_str();
    info.lpParameters = arguments.c_str(); info.lpDirectory = directory.c_str(); info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
      // The user declined the administrator approval: nothing was started or written.
      return fail(ElevationFailureCode(GetLastError()));
    }
    if (token.empty()) {
      // Nobody else waits for this launch: it ends when the elevated instance has
      // started the installer (or failed to). The bound covers copying the file.
      DWORD code = STILL_ACTIVE;
      if (WaitForSingleObject(info.hProcess, 180000) != WAIT_OBJECT_0 || !GetExitCodeProcess(info.hProcess, &code) || code != 0) {
        CloseHandle(info.hProcess);
        const auto reason = code == STILL_ACTIVE ? std::string() : StagedUpdateExitFailure(code);
        return fail(reason.empty() ? "installer_start_failed" : reason.c_str());
      }
    }
    if (process_out) *process_out = info.hProcess; else CloseHandle(info.hProcess);
    return true;
  } catch (...) {
    return fail("installer_start_failed");
  }
}
// Reverify and stage under an administrator-owned protected directory before
// executing. A user-writable temp path is never passed to CreateProcess.
// [process] receives the installer's process handle (the caller closes it).
inline bool LaunchVerifiedProductUpdate(const ProductUpdateSpec& spec,
    const std::function<bool()>& cancelled = []{return false;},
    const std::function<bool()>& commit = []{return true;},
    InstallerMode mode = InstallerMode::Update, HANDLE* process_out = nullptr, std::string* failure = nullptr,
    const std::wstring& handoffToken = std::wstring()) {
  using namespace update_detail;
  if(process_out) *process_out = nullptr;
  if(failure) failure->clear();
  // Signed by the project's release key (the window answers signature_invalid
  // before this point; this is the last gate before anything is staged).
  if(!ValidProductUpdate(spec) || !ProductUpdateSignatureValid(spec)) { if(failure) *failure = "installer_start_failed"; return false; }
  // Without administrator rights (the client since 2026-10-27) the download is
  // verified and the installed, administrator-protected client EXE is started
  // through UAC; that elevated instance runs this very function again and
  // stages into the administrator-only folder below. Host settings are elevated
  // already and take the same path directly.
  if(!ProcessElevated()) return LaunchVerifiedProductUpdateUnelevated(spec,cancelled,commit,mode,process_out,failure,handoffToken);
  std::wstring destination,stage;
  bool createdStage=false,createdTarget=false,createdTemporary=false;
  std::wstring temporary;
  try {
    Require(!cancelled());
    Handle source{CreateFileW(spec.source.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr)};
    Require(source.value!=INVALID_HANDLE_VALUE && GetFileType(source.value)==FILE_TYPE_DISK);
    FILE_ATTRIBUTE_TAG_INFO attrs{};LARGE_INTEGER length{};
    Require(GetFileInformationByHandleEx(source.value,FileAttributeTagInfo,&attrs,sizeof(attrs)) &&
        !(attrs.FileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) &&
        GetFileSizeEx(source.value,&length) && uint64_t(length.QuadPart)==spec.size);
    PWSTR programData=nullptr;Require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData,0,nullptr,&programData)));
    const std::wstring root=std::wstring(programData)+L"\\Multitap-Updates";CoTaskMemFree(programData);
    Descriptor security;Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)",SDDL_REVISION_1,&security.value,nullptr));
    SECURITY_ATTRIBUTES sa{sizeof(sa),security.value,FALSE};
    // A root a standard user created first is replaced, not trusted.
    SecureUpdateRoot(root,sa);
    Handle rootHandle{DirectoryHandle(root)};TrustedDirectory(rootHandle.value);
    GUID guid{};Require(SUCCEEDED(CoCreateGuid(&guid)));wchar_t suffix[40]{};Require(StringFromGUID2(guid,suffix,40)>0);
    stage=root+L"\\"+suffix;Require(CreateDirectoryW(stage.c_str(),&sa));createdStage=true;
    Handle stageHandle{DirectoryHandle(stage)};TrustedDirectory(stageHandle.value);
    // The installer is a self-extractor that unpacks its payload (setup EXE and
    // the scripts it then runs elevated) into TEMP. A TEMP inside the user's
    // profile lets a same-user process swap those files after they are written,
    // so it gets its own TEMP/TMP inside this administrator-only stage.
    Descriptor inheritable;Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",SDDL_REVISION_1,&inheritable.value,nullptr));
    SECURITY_ATTRIBUTES temporarySecurity{sizeof(temporarySecurity),inheritable.value,FALSE};
    temporary=stage+L"\\tmp";Require(CreateDirectoryW(temporary.c_str(),&temporarySecurity));createdTemporary=true;
    {Handle temporaryHandle{DirectoryHandle(temporary)};TrustedDirectory(temporaryHandle.value);}
    auto environment=InstallerEnvironment(temporary);
    destination=stage+L"\\"+spec.filename;
    {
      Handle target{CreateFileW(destination.c_str(),GENERIC_WRITE|GENERIC_READ,FILE_SHARE_READ,&sa,CREATE_NEW,
          FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
      Require(target.value!=INVALID_HANDLE_VALUE);
      createdTarget=true;
      Hash hash;std::array<uint8_t,64*1024> buffer{};uint64_t total=0;
      for(;;){Require(!cancelled());DWORD read=0;Require(ReadFile(source.value,buffer.data(),DWORD(buffer.size()),&read,nullptr));if(!read)break;
        total+=read;Require(total<=spec.size);DWORD written=0;Require(WriteFile(target.value,buffer.data(),read,&written,nullptr)&&written==read);hash.Add(buffer.data(),read);}
      Require(total==spec.size && hash.Finish()==spec.digest && FlushFileBuffers(target.value));
      LARGE_INTEGER zero{};Require(SetFilePointerEx(target.value,zero,nullptr,FILE_BEGIN));
      std::array<uint8_t,2> signature{};DWORD read=0;Require(ReadFile(target.value,signature.data(),2,&read,nullptr)&&read==2&&signature[0]=='M'&&signature[1]=='Z');
    }
    Handle executable{CreateFileW(destination.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};Require(executable.value!=INVALID_HANDLE_VALUE);
    // The staged copy (write access denied to everyone but SYSTEM and
    // Administrators, held open for reading) must still name this version.
    Require(InstallerVersionMatches(destination,spec.version));
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    // Both installers run their update mode from this verified stage: no Install
    // click or second confirmation. /MultitapManual opens the ordinary
    // graphical installer (Install click, every confirmation).
    auto command=L"\""+destination+(mode==InstallerMode::Manual?L"\" /MultitapManual":L"\" /MultitapUpdate");
    Require(!cancelled()&&commit());
    // The installer may bring its splash to the front over this window: only
    // the self-extractor and its setup child get that permission. It starts
    // suspended so the grant exists before it can run.
    Require(CreateProcessW(destination.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_UNICODE_ENVIRONMENT,environment.data(),stage.c_str(),&startup,&process));
    AllowSetForegroundWindow(process.dwProcessId);
    AllowInstallerForeground(process.hProcess,process.dwProcessId);
    if(ResumeThread(process.hThread)==DWORD(-1)){
      TerminateProcess(process.hProcess,ERROR_CANCELLED);WaitForSingleObject(process.hProcess,5000);
      CloseHandle(process.hThread);CloseHandle(process.hProcess);
      Require(false);
    }
    CloseHandle(process.hThread);
    if(process_out)*process_out=process.hProcess;else CloseHandle(process.hProcess);
    return true;
  } catch(...) {
    // Only exact paths generated for this invocation are removed; never recurse.
    if(createdTarget)DeleteFileW(destination.c_str());
    if(createdTemporary)RemoveDirectoryW(temporary.c_str());
    if(createdStage)RemoveDirectoryW(stage.c_str());
    if(failure) *failure = "installer_start_failed";
    return false;
  }
}
// What the elevated instance does for --apply-staged-update. Returns the process
// exit code (0: the installer started and, with a handoff token, accepted the
// update). No window, no OLE, no single-instance mutex: the caller (wWinMain)
// dispatches here before any of them exists.
inline int RunStagedUpdateHelper(int argc, wchar_t** argv) {
  using namespace update_detail;
  if (!ProcessElevated()) return int(kStagedNotElevated);
  const auto parsed = ParseStagedUpdateArguments(argc, argv);
  if (!parsed) return int(kStagedBadArguments);
  const auto& spec = parsed->spec;
  const InstallerMode mode = parsed->mode;
  const std::wstring& token = parsed->token;
  // Only the installed, administrator-protected client acts as this helper.
  const auto programFiles = ProgramFilesFolder();
  if (!ElevationTargetTrusted(OwnImage(), programFiles)) return int(kStagedUntrustedHelper);
  if (!ProductUpdateSignatureValid(spec)) return int(kStagedSignatureInvalid);
  // An unattended update never installs an older version than the installed one.
  if (mode == InstallerMode::Update &&
      relaunch::DowngradeRefused(spec.product == "host" ? relaunch::Product::HostSettings : relaunch::Product::Client, spec.version, programFiles))
    return int(kStagedDowngrade);
  HANDLE installer = nullptr; std::string failure;
  if (!LaunchVerifiedProductUpdate(spec, [] { return false; }, [] { return true; }, mode, &installer, &failure, std::wstring()) || !installer)
    return int(kStagedStartFailed);
  Handle process{installer};
  if (token.empty()) return 0;
  // Stay until the installer accepted the update, so the caller's wait sees
  // this process end exactly then; the installer closes verified client
  // processes (this one included) right after that.
  Handle ready{OpenEventW(SYNCHRONIZE, FALSE, relaunch::EventName(token).c_str())};
  if (!ready.value) { ready.value = INVALID_HANDLE_VALUE; return 0; }
  HANDLE waits[]{ready.value, process.value};
  const DWORD signal = WaitForMultipleObjects(2, waits, FALSE, 15 * 60 * 1000);
  if (signal == WAIT_OBJECT_0 || signal == WAIT_TIMEOUT) return 0;
  return WaitForSingleObject(ready.value, 0) == WAIT_OBJECT_0 ? 0 : int(kStagedInstallerExited);
}
}
