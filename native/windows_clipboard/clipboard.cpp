#include "clipboard.hpp"
#include "storage.hpp"
#include "pull.hpp"
#include "ack_window.hpp"
#include "publication_retry.hpp"
#include "settled_selection.hpp"
#include <bcrypt.h>
#include <winternl.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <ole2.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <thread>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
namespace multitap::clipboard {
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  Handle() = default; explicit Handle(HANDLE value) : value(value) {}
  ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
  Handle(const Handle&) = delete;
};
// The Host's worker of an administrator runs elevated
// (apps/windows_host/src/worker_token.hpp), while the folders it receives into
// belong to the same account, which its ordinary (medium) programs can change.
// So every folder and file below a transfer folder is created and removed
// relative to a handle this process holds, never by a path looked up again: a
// link put in between is refused (OBJ_DONT_REPARSE, FILE_OPEN_REPARSE_POINT)
// instead of followed. An elevated process also labels what it creates High
// (no write up), so the account's medium programs can read it but not change it.
using NtCreateFileProc = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using RtlNtStatusToDosErrorProc = ULONG(NTAPI*)(LONG);
constexpr ULONG objectDontReparse = 0x00001000;  // OBJ_DONT_REPARSE (Windows 10 1709+)
constexpr ULONG createNew = 2, openExisting = 1;  // FILE_CREATE, FILE_OPEN
constexpr ULONG directoryOnly = 0x00000001, fileOnly = 0x00000040, synchronous = 0x00000020, reparseItself = 0x00200000;
constexpr DWORD shareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
// A long-held folder handle asks for no data or DELETE access, so that it takes part in no sharing check
// (the store's quota walk, expiry and removal open the same folders); what is created or opened relative to
// it is checked against the folder's own list, not against the handle's access. A folder is removed through
// its parent.
constexpr ACCESS_MASK folderAccess = FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | READ_CONTROL;
// [relative] below the folder [parent] (one name or several joined by '\'),
// never through a reparse point. INVALID_HANDLE_VALUE with the Win32 error set.
HANDLE openBelow(HANDLE parent, const std::wstring& relative, ACCESS_MASK access, ULONG share, ULONG disposition, ULONG options,
                 PSECURITY_DESCRIPTOR security = nullptr) {
  static const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  static const auto create = reinterpret_cast<NtCreateFileProc>(ntdll ? GetProcAddress(ntdll, "NtCreateFile") : nullptr);
  static const auto toWin32 = reinterpret_cast<RtlNtStatusToDosErrorProc>(ntdll ? GetProcAddress(ntdll, "RtlNtStatusToDosError") : nullptr);
  if (!create || !toWin32 || !parent || parent == INVALID_HANDLE_VALUE || relative.empty() || relative.size() > 32000 ||
      relative.front() == L'\\' || relative.find(L"\\\\") != std::wstring::npos) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  UNICODE_STRING name{USHORT(relative.size() * sizeof(wchar_t)), USHORT(relative.size() * sizeof(wchar_t)), const_cast<PWSTR>(relative.c_str())};
  OBJECT_ATTRIBUTES object{}; InitializeObjectAttributes(&object, &name, OBJ_CASE_INSENSITIVE | objectDontReparse, parent, security);
  IO_STATUS_BLOCK io{}; HANDLE out = nullptr;
  const LONG status = create(&out, access | SYNCHRONIZE, &object, &io, nullptr, FILE_ATTRIBUTE_NORMAL, share, disposition,
                             options | synchronous | reparseItself, nullptr, 0);
  if (status < 0) { SetLastError(toWin32(status)); return INVALID_HANDLE_VALUE; }
  return out;
}
// The object behind [handle] is not a reparse point and is (or is not) a folder.
bool plainObject(HANDLE handle, bool folder) {
  FILE_ATTRIBUTE_TAG_INFO tag{};
  return handle != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) &&
         !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && bool(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == folder;
}
// Deletes the object of [handle] (opened with DELETE) when it is closed; a
// folder only when it is empty. [clearReadOnly] needs FILE_WRITE_ATTRIBUTES.
bool disposeHandle(HANDLE handle, bool clearReadOnly = false) {
  if (clearReadOnly) {
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) && (basic.FileAttributes & FILE_ATTRIBUTE_READONLY)) {
      FILE_BASIC_INFO change{}; change.FileAttributes = basic.FileAttributes & ~DWORD(FILE_ATTRIBUTE_READONLY);
      if (!change.FileAttributes) change.FileAttributes = FILE_ATTRIBUTE_NORMAL;
      SetFileInformationByHandle(handle, FileBasicInfo, &change, sizeof(change));
    }
  }
  // POSIX semantics, as DeleteFileW / RemoveDirectoryW use them on current Windows: the name goes at
  // once even while another handle is open. The classic disposition where a volume does not support it.
  FILE_DISPOSITION_INFO_EX posix{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS};
  if (SetFileInformationByHandle(handle, FileDispositionInfoEx, &posix, sizeof(posix))) return true;
  FILE_DISPOSITION_INFO disposition{TRUE};
  return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
}
// An existing folder, opened as itself (never through a final link). Whatever is
// done relative to the handle stays in this folder even if it is renamed later.
HANDLE openFolder(const std::filesystem::path& path) {
  const HANDLE folder = CreateFileW(longPath(path).c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (folder != INVALID_HANDLE_VALUE && !plainObject(folder, true)) { CloseHandle(folder); SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
  return folder;
}
// Removes the folder [name] of the held folder [parent] when it is empty; a link there is left alone.
bool removeEmptyBelow(HANDLE parent, const std::wstring& name) {
  Handle folder(openBelow(parent, name, DELETE | FILE_READ_ATTRIBUTES, shareAll, openExisting, directoryOnly));
  return plainObject(folder.value, true) && disposeHandle(folder.value);
}
// Everything below the folder [directory] (a handle with FILE_LIST_DIRECTORY),
// children first, each opened relative to its folder: a link is removed as the
// link and never entered. Bounded depth.
void removeBelow(HANDLE directory, int depth) noexcept {
  if (depth > 600) return;
  try {
    struct Child { std::wstring name; DWORD attributes; };
    std::vector<Child> children;
    std::vector<uint64_t> buffer(8192);  // 64 KiB, 8-byte aligned
    FILE_INFO_BY_HANDLE_CLASS kind = FileFullDirectoryRestartInfo;
    while (GetFileInformationByHandleEx(directory, kind, buffer.data(), DWORD(buffer.size() * sizeof(uint64_t)))) {
      kind = FileFullDirectoryInfo;
      const auto* bytes = reinterpret_cast<const uint8_t*>(buffer.data());
      for (size_t at = 0;;) {
        const auto* entry = reinterpret_cast<const FILE_FULL_DIR_INFO*>(bytes + at);
        const std::wstring name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
        if (name != L"." && name != L"..") children.push_back({name, entry->FileAttributes});
        if (!entry->NextEntryOffset) break;
        at += entry->NextEntryOffset;
      }
      if (children.size() > 250000) return;
    }
    for (const auto& child : children) {
      const bool folder = (child.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0, link = (child.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
      Handle item(openBelow(directory, child.name, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | (folder && !link ? FILE_LIST_DIRECTORY : 0),
                            shareAll, openExisting, folder ? directoryOnly : fileOnly));
      if (item.value == INVALID_HANDLE_VALUE) continue;
      if (folder && !link && plainObject(item.value, true)) removeBelow(item.value, depth + 1);
      disposeHandle(item.value, true);
    }
  } catch (...) {}
}
// The entry [name] of the held folder [parent] and everything below it; true only when it is gone.
bool removeTreeBelow(HANDLE parent, const std::wstring& name) noexcept {
  if (name.empty() || name.find_first_of(L"\\/:") != std::wstring::npos || name == L"." || name == L"..") return false;
  {
    Handle item(openBelow(parent, name, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | FILE_LIST_DIRECTORY, shareAll, openExisting, 0));
    if (item.value == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if (plainObject(item.value, true)) removeBelow(item.value, 0);
    if (!disposeHandle(item.value, true)) return false;
  }
  // Gone, or (still open elsewhere) pending deletion.
  Handle again(openBelow(parent, name, FILE_READ_ATTRIBUTES, shareAll, openExisting, 0));
  return again.value == INVALID_HANDLE_VALUE && (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_ACCESS_DENIED);
}
// Why this thread's last clipboard write or open failed (clipboard.hpp writeFailureDetail).
thread_local uint32_t failureDetail = 0;
constexpr uint32_t healedBit = 1u << 20;
// Records the Win32 error of the call that just failed (call it first thing after the failure) and who holds the clipboard.
void noteWin32(DWORD error, uint32_t holder = 0) {
  failureDetail = (failureDetail & healedBit) | (holder << 16) | (error & 0xffff);
}
// 0 nobody, 1 another process, 2 this thread (a clipboard we left open), 3 another thread of this process.
uint32_t clipboardHolder() {
  const HWND open = GetOpenClipboardWindow();
  if (!open) return 0;
  DWORD pid = 0; const DWORD thread = GetWindowThreadProcessId(open, &pid);
  if (pid != GetCurrentProcessId()) return 1;
  return thread == GetCurrentThreadId() ? 2 : 3;
}
// Other listeners (clipboard history, shell, the copying application's delayed rendering) hold the clipboard briefly right
// after every change, which is exactly when automatic sharing reads or writes. Reads retry for about a second before
// reporting the transient busy state; a write, which the user is waiting for, for about two.
constexpr unsigned readBudgetMs = 1100, writeBudgetMs = 2000;
struct Clipboard {
  explicit Clipboard(HWND owner, unsigned budgetMs = readBudgetMs) {
    require(owner && IsWindow(owner), "clipboard_owner_unavailable");
    DWORD pid = 0; GetWindowThreadProcessId(owner, &pid);
    require(pid == GetCurrentProcessId(), "clipboard_owner_not_this_process");
    DWORD error = ERROR_SUCCESS;
    for (unsigned waited = 0, attempt = 0;; ++attempt) {
      if (OpenClipboard(owner)) { opened = true; break; }
      error = GetLastError();
      if (waited >= budgetMs) break;
      const unsigned step = attempt < 10 ? 10 : 25;
      Sleep(step); waited += step;
    }
    if (!opened) noteWin32(error, clipboardHolder());
    require(opened, "clipboard_busy");
  }
  ~Clipboard() { if (opened) CloseClipboard(); }
  bool opened = false;
};
void validText(const std::wstring& text) {
  require(text.size() <= maxTextUnits, "clipboard_text_too_large");
  require(text.find(L'\0') == std::wstring::npos, "clipboard_text_too_large_or_invalid");
  if (!text.empty()) {
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    require(bytes > 0, "clipboard_invalid_unicode");
    require(size_t(bytes) <= maxTextBytes, "clipboard_text_too_large");
  }
}
struct Memory {
  HGLOBAL value = nullptr;
  explicit Memory(size_t size) : value(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size)) {
    require(value != nullptr, "clipboard_allocation_failed");
  }
  struct Adopt {};
  Memory(HGLOBAL adopted, Adopt) : value(adopted) { require(value != nullptr, "clipboard_allocation_failed"); }
  Memory(const Memory&) = delete;
  ~Memory() { if (value) GlobalFree(value); }
  void put(const void* data, size_t bytes, size_t offset = 0) {
    require(offset <= GlobalSize(value) && bytes <= GlobalSize(value) - offset, "clipboard_data_bounds");
    void* destination = GlobalLock(value); require(destination != nullptr, "clipboard_lock_failed");
    std::memcpy(static_cast<uint8_t*>(destination) + offset, data, bytes); GlobalUnlock(value);
  }
  void transfer(UINT format) {
    if (!EmptyClipboard()) { noteWin32(GetLastError(), clipboardHolder()); throw std::runtime_error("clipboard_empty_failed"); }
    give(format);
  }
  // Adds one more format to content published with a first transfer() (the clipboard stays open).
  void give(UINT format) {
    if (!SetClipboardData(format, value)) { noteWin32(GetLastError()); throw std::runtime_error("clipboard_write_failed"); }
    value = nullptr;
  }
  uint8_t* lock() { auto* p = static_cast<uint8_t*>(GlobalLock(value)); require(p != nullptr, "clipboard_lock_failed"); return p; }
  HGLOBAL release() { HGLOBAL memory = value; value = nullptr; return memory; }
  void unlock() { GlobalUnlock(value); }
};
class Hash {
 public:
  Hash() {
    require(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
            "clipboard_hash_provider_failed");
    DWORD size = 0, read = 0;
    if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &read, 0) < 0) {
      BCryptCloseAlgorithmProvider(algorithm_, 0); algorithm_ = nullptr;
      throw std::runtime_error("clipboard_hash_property_failed");
    }
    object_.resize(size);
    if (BCryptCreateHash(algorithm_, &hash_, object_.data(), size, nullptr, 0, 0) < 0) {
      BCryptCloseAlgorithmProvider(algorithm_, 0); algorithm_ = nullptr;
      throw std::runtime_error("clipboard_hash_create_failed");
    }
  }
  ~Hash() { if (hash_) BCryptDestroyHash(hash_); if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0); }
  void add(const uint8_t* data, size_t size) {
    require(size <= maxChunkBytes && BCryptHashData(hash_, const_cast<PUCHAR>(data), ULONG(size), 0) >= 0,
            "clipboard_hash_update_failed");
  }
  std::string finish() {
    if (!digest_.empty()) return digest_;
    std::array<uint8_t, 32> bytes{};
    require(BCryptFinishHash(hash_, bytes.data(), ULONG(bytes.size()), 0) >= 0, "clipboard_hash_finish_failed");
    constexpr char hex[] = "0123456789abcdef";
    for (auto value : bytes) { digest_ += hex[value >> 4]; digest_ += hex[value & 15]; }
    return digest_;
  }
 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr; BCRYPT_HASH_HANDLE hash_ = nullptr;
  std::vector<uint8_t> object_; std::string digest_;
};
struct PrivateSecurity {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
  PrivateSecurity() {
    HANDLE raw = nullptr; require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != FALSE,
                                "clipboard_user_token_failed");
    Handle token(raw); DWORD size = 0; GetTokenInformation(raw, TokenUser, nullptr, 0, &size);
    require(size > 0 && size < 65536, "clipboard_user_identity_failed");
    std::vector<uint8_t> bytes(size);
    require(GetTokenInformation(raw, TokenUser, bytes.data(), size, &size) != FALSE, "clipboard_user_identity_failed");
    LPWSTR sid = nullptr;
    require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid) != FALSE,
            "clipboard_user_identity_failed");
    // An elevated worker labels its folders and files High (no write up): see openBelow.
    const auto dacl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + std::wstring(sid) + L")" +
                      (processAboveMedium() ? L"S:(ML;OICI;NW;;;HI)" : L"");
    LocalFree(sid);
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(), SDDL_REVISION_1,
        &descriptor, nullptr) != FALSE, "clipboard_private_acl_failed");
    attributes.lpSecurityDescriptor = descriptor;
  }
  ~PrivateSecurity() { if (descriptor) LocalFree(descriptor); }
};
// Mark-of-the-Web: a file received from the other
// side of a remote session is an Internet-zone file, so SmartScreen, Office
// Protected View and attachment checks apply when it is opened. The URLs are
// fixed: no file name, server, account or content is recorded in the stream.
constexpr char kZoneIdentifier[] =
    "[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=about:internet\r\nHostUrl=about:internet\r\n";
// The stream is opened relative to the received file's own handle.
void markInternetZone(HANDLE file) {
  Handle marker(openBelow(file, L":Zone.Identifier", GENERIC_WRITE, 0, createNew, fileOnly));
  require(marker.value != INVALID_HANDLE_VALUE, "clipboard_transfer_file_create_failed");
  DWORD written = 0;
  require(WriteFile(marker.value, kZoneIdentifier, DWORD(sizeof(kZoneIdentifier) - 1), &written, nullptr) &&
      written == sizeof(kZoneIdentifier) - 1, "clipboard_transfer_file_create_failed");
}
void existingDirectoryChain(const std::filesystem::path& path) {
  require(path.is_absolute(), "clipboard_transfer_parent_not_absolute");
  auto current = path.root_path();
  for (const auto& part : path.relative_path()) {
    require(part != L"." && part != L"..", "clipboard_transfer_parent_invalid"); current /= part;
    const DWORD attributes = GetFileAttributesW(longPath(current).c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "clipboard_transfer_parent_untrusted");
  }
}
std::wstring randomName() {
  std::array<uint8_t, 16> bytes{};
  require(BCryptGenRandom(nullptr, bytes.data(), ULONG(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0,
          "clipboard_transfer_id_failed");
  std::wstring value = L"transfer-"; constexpr wchar_t hex[] = L"0123456789abcdef";
  for (auto byte : bytes) { value += hex[byte >> 4]; value += hex[byte & 15]; }
  return value;
}
}

bool processAboveMedium() noexcept {
  HANDLE raw = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
  Handle token(raw); DWORD size = 0; GetTokenInformation(raw, TokenIntegrityLevel, nullptr, 0, &size);
  if (!size || size > 1024) return false;
  std::vector<uint8_t> bytes(size);
  if (!GetTokenInformation(raw, TokenIntegrityLevel, bytes.data(), size, &size)) return false;
  const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(bytes.data())->Label.Sid;
  if (!IsValidSid(sid) || !*GetSidSubAuthorityCount(sid)) return false;
  return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1) >= SECURITY_MANDATORY_HIGH_RID;
}
bool removeWithoutFollowing(const std::filesystem::path& parent, const std::wstring& name) noexcept {
  try {
    Handle held(openFolder(parent)); if (held.value == INVALID_HANDLE_VALUE) return false;
    return removeTreeBelow(held.value, name);
  } catch (...) { return false; }
}
bool safeLeaf(const std::wstring& name) {
  if (name.empty() || name.size() > maxComponentChars || name == L"." || name == L".." ||
      name.back() == L'.' || name.back() == L' ' || name.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos) return false;
  for (wchar_t value : name) if (value < 32) return false;
  try { validText(name); } catch (...) { return false; }
  auto stem = name.substr(0, name.find(L'.'));
  while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) stem.pop_back();
  for (auto& value : stem) if (value >= L'a' && value <= L'z') value -= L'a' - L'A';
  if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CONIN$" || stem == L"CONOUT$") return false;
  if (stem.size() == 4 && (stem.rfind(L"COM", 0) == 0 || stem.rfind(L"LPT", 0) == 0) &&
      (stem[3] >= L'1' && stem[3] <= L'9')) return false;
  // Windows also treats these superscript digits as DOS device aliases.
  if (stem.size() == 4 && (stem.rfind(L"COM", 0) == 0 || stem.rfind(L"LPT", 0) == 0) &&
      (stem[3] == L'\u00b9' || stem[3] == L'\u00b2' || stem[3] == L'\u00b3')) return false;
  return true;
}

namespace {
// The most memory of one file list (CF_HDROP, Shell IDList Array) that is read or published: 200,000 paths of the usual length.
constexpr size_t maxListBytes = 64 * 1024 * 1024;
// The clipboard sequence after this process last published a paste-pull offer (its own virtual files).
std::atomic<DWORD> ownPullSequence{0};
UINT shellIdListFormat() { static const UINT format = RegisterClipboardFormatW(CFSTR_SHELLIDLIST); return format; }
UINT virtualFilesFormat() { static const UINT format = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW); return format; }
// An item ID list inside [data, data+size): every SHITEMID fits and the list ends with the 0 terminator.
bool idListWithin(const uint8_t* data, size_t size, size_t offset) {
  for (;;) {
    if (offset + sizeof(USHORT) > size) return false;
    USHORT length = 0; std::memcpy(&length, data + offset, sizeof(length));
    if (length == 0) return true;
    if (length < sizeof(USHORT) || offset + length > size) return false;
    offset += length;
  }
}
}
bool hasImageFormat() {
  const UINT png = RegisterClipboardFormatW(L"PNG");
  return (png && IsClipboardFormatAvailable(png)) || IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB);
}
bool preferImageOverFiles(const std::vector<std::filesystem::path>& files, bool imageAvailable) {
  if (!imageAvailable || files.empty() || files.size() > 4) return false;
  for (const auto& file : files) {
    std::wstring ext = file.extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    if (ext != L".png" && ext != L".jpg" && ext != L".jpeg" && ext != L".gif" && ext != L".bmp" && ext != L".webp") return false;
    std::error_code ignored; if (std::filesystem::is_directory(file, ignored)) return false;
  }
  return true;
}
uint32_t availableFormats() {
  uint32_t mask = 0;
  if (IsClipboardFormatAvailable(CF_HDROP)) mask |= 1;
  if (const UINT format = shellIdListFormat(); format && IsClipboardFormatAvailable(format)) mask |= 2;
  if (const UINT format = virtualFilesFormat(); format && IsClipboardFormatAvailable(format)) mask |= 4;
  if (IsClipboardFormatAvailable(CF_UNICODETEXT)) mask |= 8;
  const UINT png = RegisterClipboardFormatW(L"PNG");
  if ((png && IsClipboardFormatAvailable(png)) || IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB)) mask |= 16;
  if (const UINT format = RegisterClipboardFormatW(L"HTML Format"); format && IsClipboardFormatAvailable(format)) mask |= 32;
  return mask;
}
std::vector<std::filesystem::path> pathsFromIdList(const uint8_t* data, size_t size) {
  std::vector<std::filesystem::path> paths;
  if (!data || size < sizeof(UINT) * 3 || size > maxListBytes) return paths;
  UINT count = 0; std::memcpy(&count, data, sizeof(count));
  // cidl, then cidl + 1 offsets (the parent folder first, then each item).
  if (count == 0 || count > maxEntries || size < sizeof(UINT) * (size_t(count) + 2)) throw std::runtime_error(count > maxEntries ? "clipboard_file_count_limit" : "clipboard_file_list_invalid");
  std::vector<UINT> offsets(size_t(count) + 1);
  std::memcpy(offsets.data(), data + sizeof(UINT), offsets.size() * sizeof(UINT));
  for (const UINT offset : offsets) if (offset >= size || !idListWithin(data, size, offset)) return {};
  // The shell resolves item lists through COM; the calling thread may not have it (RPC_E_CHANGED_MODE: already initialised otherwise).
  struct Com { HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); ~Com() { if (SUCCEEDED(result)) CoUninitialize(); } } com;
  const auto* parent = reinterpret_cast<PCIDLIST_ABSOLUTE>(data + offsets[0]);
  for (UINT index = 1; index <= count; ++index) {
    PIDLIST_ABSOLUTE absolute = ILCombine(parent, reinterpret_cast<PCUIDLIST_RELATIVE>(data + offsets[index]));
    if (!absolute) return {};
    PWSTR name = nullptr; const HRESULT result = SHGetNameFromIDList(absolute, SIGDN_FILESYSPATH, &name);
    ILFree(absolute);
    if (FAILED(result) || !name) return {};
    std::wstring path(name); CoTaskMemFree(name);
    if (path.empty() || path.size() > 32767 || !std::filesystem::path(path).is_absolute()) return {};
    paths.emplace_back(std::move(path));
  }
  return paths;
}
bool sensitiveContentIsCurrent() {
  static const UINT monitor = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
  static const UINT viewer = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
  return (monitor && IsClipboardFormatAvailable(monitor)) || (viewer && IsClipboardFormatAvailable(viewer));
}
Snapshot read(HWND owner) {
  return read(owner, true, true);
}
Snapshot read(HWND owner, bool allowText, bool allowFiles) {
  return read(owner, ReadOptions{allowText, allowFiles, false, false});
}
Snapshot read(HWND owner, const ReadOptions& options) {
  const bool allowText = options.text, allowFiles = options.files;
  require(allowText || allowFiles, "clipboard_format_not_allowed");
  // The clipboard holds this process's own delayed-rendering content (the peer's offer): reading any of its formats would pull the peer's data
  // without a paste, and send it straight back. It is never read.
  require(!ownDelayedContentIsCurrent(), "unsupported_clipboard_format");
  // A bitmap is binary content (the files permission). It is read only when neither a file list nor
  // allowed text is on the clipboard; encoding to PNG happens after the clipboard is released.
  std::vector<uint8_t> bitmap;
  Snapshot result;
  bool stageVirtual = false;
  {
  Clipboard clipboard(owner);
  if (options.expectedSequence) require(*options.expectedSequence == GetClipboardSequenceNumber(), "clipboard_changed");
  if (options.textImageOnly) {
    require(options.expectedSequence.has_value() && !options.virtualFiles, "clipboard_invalid_message");
    validateSettledSelection(*options.expectedSequence, GetClipboardSequenceNumber(), availableFormats());
  }
  result.owner = GetClipboardOwner();
  bool took = false;
  if (allowFiles && IsClipboardFormatAvailable(CF_HDROP)) {
    const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    const auto allocation = drop ? GlobalSize(drop) : 0;
    require(drop && allocation >= sizeof(DROPFILES) + 2 && allocation <= maxListBytes, "clipboard_file_list_invalid");
    const auto memory = static_cast<const uint8_t*>(GlobalLock(drop));
    require(memory != nullptr, "clipboard_lock_failed");
    DROPFILES header{}; std::memcpy(&header, memory, sizeof(header));
    bool terminated = false;
    if (header.pFiles >= sizeof(header) && header.pFiles < allocation) {
      const size_t payload = allocation - header.pFiles;
      if (header.fWide && payload % sizeof(wchar_t) == 0) {
        std::vector<wchar_t> chars(payload / sizeof(wchar_t));
        std::memcpy(chars.data(), memory + header.pFiles, payload);
        for (size_t index = 1; index < chars.size(); ++index)
          if (!chars[index-1] && !chars[index]) { terminated = true; break; }
      } else if (!header.fWide) {
        for (size_t index = header.pFiles + 1; index < allocation; ++index)
          if (!memory[index-1] && !memory[index]) { terminated = true; break; }
      }
    }
    GlobalUnlock(drop); require(terminated, "clipboard_file_list_invalid");
    const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    require(count > 0 && count <= maxEntries, "clipboard_file_count_limit");
    for (UINT index = 0; index < count; ++index) {
      const UINT length = DragQueryFileW(drop, index, nullptr, 0);
      require(length > 0 && length <= 32767, "clipboard_file_path_invalid");
      std::wstring path(length + 1, 0);
      require(DragQueryFileW(drop, index, path.data(), length + 1) == length, "clipboard_file_path_invalid");
      path.resize(length); require(std::filesystem::path(path).is_absolute(), "clipboard_file_path_invalid");
      result.files.emplace_back(path);
    }
    took = true;
  } else if (allowFiles && shellIdListFormat() && IsClipboardFormatAvailable(shellIdListFormat()) && [&] {
    // A copy that carries the shell's own file list but no CF_HDROP: use the paths of that list when every item is a file system item.
    const HANDLE list = GetClipboardData(shellIdListFormat()); const SIZE_T size = list ? GlobalSize(list) : 0;
    if (!size || size > maxListBytes) return false;
    const auto* memory = static_cast<const uint8_t*>(GlobalLock(list)); if (!memory) return false;
    std::vector<uint8_t> copy(memory, memory + size); GlobalUnlock(list);
    result.files = pathsFromIdList(copy.data(), copy.size());
    return !result.files.empty();
  }()) {
    took = true;
  } else if (allowText && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
    const auto memory = GetClipboardData(CF_UNICODETEXT); const SIZE_T size = memory ? GlobalSize(memory) : 0;
    require(size >= sizeof(wchar_t) && size % sizeof(wchar_t) == 0, "clipboard_text_too_large_or_invalid");
    require(size <= (maxTextUnits + 1) * sizeof(wchar_t), "clipboard_text_too_large");
    const auto chars = static_cast<const wchar_t*>(GlobalLock(memory)); require(chars != nullptr, "clipboard_lock_failed");
    size_t length = 0; while (length < size / sizeof(wchar_t) && chars[length]) ++length;
    std::wstring text; const bool terminated = length < size / sizeof(wchar_t);
    if (terminated) text.assign(chars, length); GlobalUnlock(memory);
    require(terminated, "clipboard_text_not_terminated"); validText(text); result.text = std::move(text);
    took = true;
    if (options.rich) {
      // Optional companions of the plain text; anything unusable is simply left out.
      const auto companion = [&options](UINT format, std::optional<std::vector<uint8_t>>& into, std::vector<uint8_t> (*clean)(const std::vector<uint8_t>&)) {
        if (!format || !IsClipboardFormatAvailable(format)) return;
        const HANDLE data = GetClipboardData(format); const SIZE_T size = data ? GlobalSize(data) : 0;
        if (!size || size > options.richBytes + 1) return;
        const auto* memory = static_cast<const uint8_t*>(GlobalLock(data)); if (!memory) return;
        size_t length = 0; while (length < size && memory[length]) ++length;
        std::vector<uint8_t> bytes(memory, memory + length); GlobalUnlock(data);
        try { into = clean(bytes); } catch (...) {}
      };
      // HTML is sanitised before it leaves this machine; RTF is not sent at all (it can embed OLE objects and external fetches).
      companion(RegisterClipboardFormatW(L"HTML Format"), result.html, sanitizeHtmlFormat);
    }
  }
  // A screenshot tool (Snipping Tool) puts the bitmap and its auto-saved image file on the clipboard together. When
  // every listed file is a plain image file and a bitmap is offered too, the bitmap is what the user copied: send it
  // as an image (the files stay as the fallback if the image cannot be read).
  std::vector<std::filesystem::path> imageFiles;
  if (took && options.images && allowFiles && preferImageOverFiles(result.files, hasImageFormat())) {
    imageFiles = std::move(result.files); result.files.clear(); took = false;
  }
  if (!took && options.images && allowFiles) {
    const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    const auto copy = [&](UINT format, uint64_t limit) {
      const HANDLE data = GetClipboardData(format); const SIZE_T size = data ? GlobalSize(data) : 0;
      if (!size) return false;
      require(size <= limit, "clipboard_image_too_large");
      const auto* memory = static_cast<const uint8_t*>(GlobalLock(data)); require(memory != nullptr, "clipboard_lock_failed");
      bitmap.assign(memory, memory + size); GlobalUnlock(data); return true;
    };
    // A "PNG" the source offers is sent as it is when it is a plain PNG within the bounds; otherwise the DIB route.
    if (pngFormat && IsClipboardFormatAvailable(pngFormat) && copy(pngFormat, maxImageBytes)) {
      uint32_t width = 0, height = 0;
      if (readPngHeader(bitmap, width, height)) { result.image = Image{std::move(bitmap), width, height}; bitmap.clear(); }
      else bitmap.clear();
    }
    if (!result.image) {
      if (IsClipboardFormatAvailable(CF_DIBV5) && copy(CF_DIBV5, maxImageBytes * 2)) {}
      else if (IsClipboardFormatAvailable(CF_DIB)) copy(CF_DIB, maxImageBytes * 2);
    }
  }
  if (!imageFiles.empty() && !result.image && bitmap.empty()) { result.files = std::move(imageFiles); took = true; }
  // Files that exist only as a shell data object (an archive's contents, a phone, an attachment: FileGroupDescriptorW /
  // FileContents, or a shell list whose items have no path) cannot be read here. Say so, rather than "nothing copied".
  // With a staging folder (the formats profile) a FileGroupDescriptorW object is read after the clipboard is released below.
  if (!took && allowFiles && !result.image && bitmap.empty() &&
      ((virtualFilesFormat() && IsClipboardFormatAvailable(virtualFilesFormat())) || (shellIdListFormat() && IsClipboardFormatAvailable(shellIdListFormat())))) {
    if (options.virtualFiles && virtualFilesFormat() && IsClipboardFormatAvailable(virtualFilesFormat()) &&
        ownPullSequence.load() != GetClipboardSequenceNumber()) stageVirtual = true;
    else throw std::runtime_error("unsupported_clipboard_format");
  }
  // Nothing could be taken, but something was copied: say which rule stopped it instead of "nothing copied".
  // An image or a file list needs the files permission, text needs the text permission; a pair without the images
  // profile (a released peer) cannot carry a bitmap at all.
  if (options.reportDenied && !took && !stageVirtual && !result.image && bitmap.empty()) {
    const bool binaryCopied = hasImageFormat() || IsClipboardFormatAvailable(CF_HDROP) ||
                              (shellIdListFormat() && IsClipboardFormatAvailable(shellIdListFormat())) ||
                              (virtualFilesFormat() && IsClipboardFormatAvailable(virtualFilesFormat()));
    if (!allowFiles && binaryCopied) throw std::runtime_error("clipboard_policy_denied");
    if (!allowText && IsClipboardFormatAvailable(CF_UNICODETEXT)) throw std::runtime_error("clipboard_policy_denied");
    if (allowFiles && !options.images && hasImageFormat()) throw std::runtime_error("unsupported_clipboard_format");
  }
  // Still held open: only the owner rendering a delayed format for the reads
  // above can have changed it, so this is the sequence of what was read.
  result.sequence = GetClipboardSequenceNumber();
  if (options.expectedSequence) require(*options.expectedSequence == result.sequence, "clipboard_changed");
  if (options.textImageOnly) validateSettledSelection(*options.expectedSequence, result.sequence, availableFormats());
  }
  if (stageVirtual) {
    // The OLE object is read with the Win32 clipboard released (OleGetClipboard opens it itself).
    auto staged = stageVirtualFiles(nullptr, *options.virtualFiles);
    require(staged != nullptr, "unsupported_clipboard_format");
    require(GetClipboardSequenceNumber() == result.sequence, "clipboard_changed");
    result.files = staged->tops; result.staged = std::move(staged);
  }
  if (!bitmap.empty()) {
    try { result.image = encodePngFromDib(bitmap.data(), bitmap.size()); } catch (const std::runtime_error& error) {
      if (std::string(error.what()) == "clipboard_image_too_large") throw;
      throw std::runtime_error("unsupported_clipboard_format");
    }
  }
  return result;
}
void requireUnchangedSequence(std::optional<DWORD> expectedSequence,DWORD currentSequence) {
  require(!expectedSequence||*expectedSequence==currentSequence,"clipboard_changed");
}
uint32_t writeFailureDetail() { return failureDetail; }
void clearWriteFailure() { failureDetail = 0; }
bool releaseOwnClipboardHolds(std::optional<DWORD>& expectedSequence) {
  bool released = false;
  // A clipboard this very thread left open (an exception path that skipped CloseClipboard) refuses every other open.
  if (const HWND open = GetOpenClipboardWindow()) {
    DWORD pid = 0; const DWORD thread = GetWindowThreadProcessId(open, &pid);
    if (pid == GetCurrentProcessId() && thread == GetCurrentThreadId() && CloseClipboard()) released = true;
  }
  // Our own OLE data object (a paste-pull offer or a received tree served as virtual files) owns the clipboard through OLE's
  // window: release it the way OLE expects before writing over it.
  if (const HWND owner = GetClipboardOwner()) {
    DWORD pid = 0; GetWindowThreadProcessId(owner, &pid);
    wchar_t name[64]{};
    if (pid == GetCurrentProcessId() && GetClassNameW(owner, name, 64) && std::wstring(name) == L"OleMainThreadWndClass") {
      const DWORD before = GetClipboardSequenceNumber();
      if (SUCCEEDED(OleSetClipboard(nullptr))) {
        released = true;
        if (expectedSequence && *expectedSequence == before) expectedSequence = GetClipboardSequenceNumber();
      }
    }
  }
  return released;
}
namespace {
// Runs one publishing attempt (it builds its memory, opens the clipboard, writes). When the write is refused for a reason that
// can be ours (the clipboard open or owned by this process itself), releases that and tries once more; a failure keeps the first
// attempt's Win32 error, marked as retried.
template <class Attempt>
DWORD publishWithHeal(std::optional<DWORD>& expected, Attempt&& attempt) {
  clearWriteFailure();
  try { return attempt(expected); }
  catch (const std::runtime_error& error) {
    const std::string what = error.what();
    if (what != "clipboard_write_failed" && what != "clipboard_empty_failed" && what != "clipboard_busy") throw;
    const uint32_t first = failureDetail;
    if (!releaseOwnClipboardHolds(expected)) throw;
    try { return attempt(expected); }
    catch (...) { if (!(failureDetail & 0xffff)) failureDetail = first; failureDetail |= healedBit; throw; }
  }
}
}
DWORD putText(HWND owner, const std::wstring& text) { return putText(owner,text,{}); }
DWORD putText(HWND owner, const std::wstring& text,std::optional<DWORD> expectedSequence) {
  validText(text);
  return publishWithHeal(expectedSequence, [&](std::optional<DWORD>& expected) {
    Memory memory((text.size() + 1) * sizeof(wchar_t));
    memory.put(text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    Clipboard clipboard(owner, writeBudgetMs); requireUnchangedSequence(expected,GetClipboardSequenceNumber()); memory.transfer(CF_UNICODETEXT); return GetClipboardSequenceNumber();
  });
}
DWORD putFiles(HWND owner, const std::vector<std::filesystem::path>& paths) { return putFiles(owner,paths,{}); }
namespace {
DWORD putDrop(HWND owner, const std::vector<std::filesystem::path>& paths, std::optional<DWORD> expectedSequence, bool allowDirectories);
}
DWORD putFiles(HWND owner, const std::vector<std::filesystem::path>& paths,std::optional<DWORD> expectedSequence) {
  return putDrop(owner, paths, expectedSequence, false);
}
DWORD putPaths(HWND owner, const std::vector<std::filesystem::path>& paths, std::optional<DWORD> expectedSequence) {
  return putDrop(owner, paths, expectedSequence, true);
}
namespace {
DWORD putDrop(HWND owner, const std::vector<std::filesystem::path>& paths, std::optional<DWORD> expectedSequence, bool allowDirectories) {
  require(!paths.empty() && paths.size() <= maxEntries, "clipboard_file_count_limit");
  size_t units = 1;
  for (const auto& path : paths) {
    require(path.is_absolute() && path.wstring().size() <= 32767, "clipboard_file_path_invalid");
    const DWORD flags = GetFileAttributesW(longPath(path).c_str());
    require(flags != INVALID_FILE_ATTRIBUTES && !(flags & FILE_ATTRIBUTE_REPARSE_POINT) &&
            (allowDirectories || !(flags & FILE_ATTRIBUTE_DIRECTORY)),
            "clipboard_file_not_materialized"); units += path.wstring().size() + 1;
  }
  require(units * sizeof(wchar_t) <= maxListBytes, "clipboard_file_list_invalid");
  return publishWithHeal(expectedSequence, [&](std::optional<DWORD>& expected) {
    Memory memory(sizeof(DROPFILES) + units * sizeof(wchar_t));
    DROPFILES header{}; header.pFiles = sizeof(DROPFILES); header.fWide = TRUE; memory.put(&header, sizeof(header));
    size_t offset = sizeof(header);
    for (const auto& path : paths) { const auto value = path.wstring(); memory.put(value.c_str(), (value.size() + 1) * sizeof(wchar_t), offset); offset += (value.size() + 1) * sizeof(wchar_t); }
    Clipboard clipboard(owner, writeBudgetMs); requireUnchangedSequence(expected,GetClipboardSequenceNumber()); memory.transfer(CF_HDROP); return GetClipboardSequenceNumber();
  });
}
}

#include "clipboard_formats.inl"
#include "clipboard_tree.inl"
#include "pull_impl.inl"
#include "virtual_source.inl"

// A copy source is the user's own file. Windows refuses to rename a folder while any file below it is open (whatever the sharing
// flags), so the file is not kept open between reads: each read opens it, reads its chunk and closes it, and finds it again wherever
// it went (pull_impl.inl openCopiedFile). A writer that changed it meanwhile is reported at the next read and at the digest.
struct FileSource::State {
  std::filesystem::path path; Identity expected; FileDescription description; uint64_t at = 0; Hash hash; std::string digest;
  explicit State(const std::filesystem::path& source) : path(source) {
    Handle file(CreateFileW(longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    const DWORD openError = file.value == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    // Another program has the file open for writing (or exclusively): say so.
    require(openError != ERROR_SHARING_VIOLATION && openError != ERROR_LOCK_VIOLATION, "clipboard_file_locked");
    require(path.is_absolute() && file.value != INVALID_HANDLE_VALUE, "clipboard_selected_file_unavailable");
    FILE_ATTRIBUTE_TAG_INFO attributes{}; LARGE_INTEGER size{};
    require(GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
        !(attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        GetFileType(file.value) == FILE_TYPE_DISK && GetFileSizeEx(file.value, &size) && size.QuadPart >= 0,
        "clipboard_regular_files_only");
    description = {path.filename().wstring(), uint64_t(size.QuadPart)};
    require(safeLeaf(description.name), "clipboard_selected_file_unavailable");
    require(description.size <= scaleBytes, "clipboard_file_name_or_size_limit");
    expected = identityOf(file.value);
  }
};
FileSource::FileSource(const std::filesystem::path& path) : state_(std::make_unique<State>(path)) {}
FileSource::~FileSource() = default;
FileSource::FileSource(FileSource&&) noexcept = default;
FileSource& FileSource::operator=(FileSource&&) noexcept = default;
const FileDescription& FileSource::description() const { return state_->description; }
uint64_t FileSource::position() const { return state_->at; }
std::vector<uint8_t> FileSource::next(size_t requested) {
  require(requested > 0 && requested <= maxChunkBytes, "clipboard_chunk_limit");
  const auto remaining = state_->description.size - state_->at;
  if (!remaining) return {};
  Handle file(openCopiedFile(state_->path, state_->expected));
  LARGE_INTEGER to; to.QuadPart = LONGLONG(state_->at);
  require(SetFilePointerEx(file.value, to, nullptr, FILE_BEGIN) != FALSE, "clipboard_selected_file_unavailable");
  std::vector<uint8_t> bytes(size_t(std::min<uint64_t>(remaining, requested))); DWORD read = 0;
  const BOOL readOk = ReadFile(file.value, bytes.data(), DWORD(bytes.size()), &read, nullptr);
  require(readOk || GetLastError() != ERROR_LOCK_VIOLATION, "clipboard_file_locked");
  require(readOk && read == bytes.size(), "clipboard_selected_file_changed");
  state_->hash.add(bytes.data(), bytes.size()); state_->at += bytes.size(); return bytes;
}
std::string FileSource::digest() const {
  require(state_->at == state_->description.size, "clipboard_file_not_complete");
  Handle file(openCopiedFile(state_->path, state_->expected));  // still the file that was read
  return state_->hash.finish();
}
bool filesHoldContent(const std::vector<std::filesystem::path>& files,
                      const std::vector<FileDescription>& expected,
                      const std::vector<std::string>& digests) {
  try {
    if (files.size() != expected.size() || digests.size() != expected.size()) return false;
    for (size_t index = 0; index < files.size(); ++index) {
      FileSource source(files[index]);
      const auto& description = source.description();
      if (description.name != expected[index].name || description.size != expected[index].size ||
          digests[index].size() != 64) return false;
      while (source.position() < description.size) source.next(maxChunkBytes);
      if (source.digest() != digests[index]) return false;
    }
    return true;
  } catch (...) { return false; }
}

struct FileReceiver::State {
  struct Item { FileDescription description; Handle file; Hash hash; uint64_t at = 0; bool done = false;
    Item(FileDescription description, HANDLE file) : description(std::move(description)), file(file) {} };
  std::filesystem::path root, parent; std::vector<std::unique_ptr<Item>> files;
  std::vector<std::wstring> expectedNames;
  HANDLE directory = INVALID_HANDLE_VALUE, parentFolder = INVALID_HANDLE_VALUE;
  uint64_t received = 0; bool committed = false, cancelled = false;
  // Removes exactly the expected files and then the transfer folder, by the held folder handles.
  void discard() noexcept {
    files.clear();
    if (directory != INVALID_HANDLE_VALUE) {
      for (const auto& name : expectedNames) {
        if (!safeLeaf(name)) continue;
        Handle file(openBelow(directory, name, DELETE | FILE_READ_ATTRIBUTES, shareAll, openExisting, fileOnly));
        if (plainObject(file.value, false)) disposeHandle(file.value);
      }
      CloseHandle(directory); directory = INVALID_HANDLE_VALUE;
      if (parentFolder != INVALID_HANDLE_VALUE && !root.empty()) removeEmptyBelow(parentFolder, root.filename().wstring());
    }
    cancelled = true;
  }
  ~State() {
    if (!committed && !cancelled) discard();
    if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
    if (parentFolder != INVALID_HANDLE_VALUE) CloseHandle(parentFolder);
  }
};
FileReceiver::FileReceiver(const std::filesystem::path& parent, const std::vector<FileDescription>& descriptions)
    : state_(std::make_unique<State>()) {
  require(!descriptions.empty() && descriptions.size() <= maxEntries, "clipboard_file_count_limit");
  uint64_t total = 0; std::set<std::wstring> names;
  for (const auto& file : descriptions) {
    require(safeLeaf(file.name), "clipboard_selected_file_unavailable");
    require(file.size <= scaleBytes, "clipboard_file_name_or_size_limit");
    require(file.size <= scaleBytes - total, "clipboard_total_size_limit"); total += file.size;
    auto folded = file.name; CharUpperBuffW(folded.data(), DWORD(folded.size()));
    require(names.insert(folded).second, "clipboard_duplicate_file_name");
    state_->expectedNames.push_back(file.name);
  }
  existingDirectoryChain(parent); state_->parent = parent;
  // Refuse before any byte is accepted when the volume cannot hold the whole
  // transfer (plus headroom), so the sender learns the real reason up front.
  {
    ULARGE_INTEGER available{};
    if (GetDiskFreeSpaceExW(parent.c_str(), &available, nullptr, nullptr))
      require(available.QuadPart > total && available.QuadPart - total >= freeSpaceHeadroomBytes, "clipboard_disk_full");
  }
  PrivateSecurity security;
  state_->parentFolder = openFolder(parent);
  require(state_->parentFolder != INVALID_HANDLE_VALUE, "clipboard_transfer_parent_untrusted");
  for (int retry = 0; retry < 4; ++retry) {
    const auto name = randomName();
    state_->directory = openBelow(state_->parentFolder, name, folderAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, createNew, directoryOnly, security.descriptor);
    if (state_->directory != INVALID_HANDLE_VALUE) { state_->root = parent / name; break; }
    require(GetLastError() == ERROR_ALREADY_EXISTS, "clipboard_transfer_directory_failed");
  }
  require(!state_->root.empty(), "clipboard_transfer_directory_failed");
  require(plainObject(state_->directory, true), "clipboard_transfer_directory_untrusted");
  // A volume without named streams (FAT/exFAT) cannot carry the mark; NTFS
  // and ReFS always get it, and a failed mark fails the transfer.
  DWORD volumeFlags = 0;
  const bool namedStreams = GetVolumeInformationByHandleW(state_->directory, nullptr, 0, nullptr, nullptr,
      &volumeFlags, nullptr, 0) && (volumeFlags & FILE_NAMED_STREAMS);
  for (const auto& file : descriptions) {
    const auto handle = openBelow(state_->directory, file.name, GENERIC_WRITE, 0, createNew, fileOnly, security.descriptor);
    require(handle != INVALID_HANDLE_VALUE, "clipboard_transfer_file_create_failed");
    state_->files.push_back(std::make_unique<State::Item>(file, handle));
    // Marked before any received byte is written or the file is published.
    if (namedStreams) markInternetZone(handle);
  }
}
FileReceiver::~FileReceiver() = default;
void FileReceiver::append(size_t index, uint64_t offset, const std::vector<uint8_t>& bytes) {
  require(!state_->cancelled && !state_->committed && index < state_->files.size(), "clipboard_transfer_not_active");
  auto& file = *state_->files[index];
  require(!file.done && offset == file.at && !bytes.empty() && bytes.size() <= maxChunkBytes &&
          bytes.size() <= file.description.size - file.at, "clipboard_chunk_out_of_order_or_bounds");
  DWORD written = 0;
  const BOOL writeOk = WriteFile(file.file.value, bytes.data(), DWORD(bytes.size()), &written, nullptr);
  const DWORD writeError = writeOk ? ERROR_SUCCESS : GetLastError();
  require(writeError != ERROR_DISK_FULL && writeError != ERROR_HANDLE_DISK_FULL && writeError != ERROR_DISK_QUOTA_EXCEEDED,
          "clipboard_disk_full");
  require(writeOk && written == bytes.size(), "clipboard_transfer_write_failed");
  file.hash.add(bytes.data(), bytes.size()); file.at += bytes.size(); state_->received += bytes.size();
}
void FileReceiver::finish(size_t index, const std::string& digest) {
  require(!state_->cancelled && !state_->committed && index < state_->files.size(), "clipboard_transfer_not_active");
  auto& file = *state_->files[index];
  require(!file.done && file.at == file.description.size && digest.size() == 64 &&
          std::all_of(digest.begin(), digest.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) &&
          file.hash.finish() == digest, "clipboard_transfer_hash_mismatch");
  require(FlushFileBuffers(file.file.value) != FALSE, "clipboard_transfer_flush_failed");
  CloseHandle(file.file.value); file.file.value = INVALID_HANDLE_VALUE; file.done = true;
}
std::vector<std::filesystem::path> FileReceiver::commit() {
  require(!state_->cancelled && !state_->committed &&
      std::all_of(state_->files.begin(), state_->files.end(), [](const auto& item) { return item->done; }),
      "clipboard_transfer_not_complete");
  std::vector<std::filesystem::path> paths;
  for (const auto& item : state_->files) paths.push_back(state_->root / item->description.name);
  state_->committed = true; return paths;
}
void FileReceiver::cancel() {
  // A published CF_HDROP refers to these paths. Transport cancellation cannot
  // retract another application's clipboard read; deliberate expiry is separate.
  if (!state_->cancelled && !state_->committed) state_->discard();
}
uint64_t FileReceiver::received() const { return state_->received; }

bool expireFiles(const std::filesystem::path& parent, const std::vector<std::filesystem::path>& files) {
  try {
    require(!files.empty() && files.size() <= maxEntries, "clipboard_expiry_invalid");
    existingDirectoryChain(parent);
    const auto root = files.front().parent_path(); const auto name = root.filename().wstring();
    require(root.is_absolute() && root.parent_path() == parent && name.size() == 41 && name.rfind(L"transfer-",0)==0 &&
      std::all_of(name.begin()+9,name.end(),[](wchar_t c){return(c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}),
      "clipboard_expiry_invalid");
    for(const auto& path:files)
      require(path.parent_path()==root && safeLeaf(path.filename().wstring()),"clipboard_expiry_invalid");
    // The parent, the transfer folder and each file are opened relative to the one before (never through a link).
    Handle held(openFolder(parent));
    require(held.value!=INVALID_HANDLE_VALUE,"clipboard_expiry_busy");
    Handle directory(openBelow(held.value,name,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,openExisting,directoryOnly));
    require(directory.value!=INVALID_HANDLE_VALUE,"clipboard_expiry_busy");
    require(plainObject(directory.value,true),"clipboard_expiry_invalid");
    bool erased=true;
    for(const auto& path:files) {
      Handle file(openBelow(directory.value,path.filename().wstring(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,openExisting,fileOnly));
      if(file.value==INVALID_HANDLE_VALUE){if(GetLastError()!=ERROR_FILE_NOT_FOUND)erased=false;continue;}
      if(!plainObject(file.value,false)||!disposeHandle(file.value))erased=false;
    }
    CloseHandle(directory.value);directory.value=INVALID_HANDLE_VALUE;
    // Only an empty folder is removed (through the held parent; a link put there is left alone).
    return removeEmptyBelow(held.value,name) && erased;
  } catch(...) {return false;}
}
}
