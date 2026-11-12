#include "drive_files_core.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <set>
#include <winternl.h>

#include "zone_identifier.h"

namespace multitap::drive {
namespace {

constexpr int64_t kMaxTime = 253402300799999LL;  // 9999-12-31T23:59:59.999Z
constexpr uint64_t kMaxFileSize = 9007199254740991ULL;

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  Handle() = default;
  explicit Handle(HANDLE handle) : value(handle) {}
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
};

[[noreturn]] void Fail(const char* code) { throw Failure(code); }
[[noreturn]] void FailError(DWORD error) { throw Failure(CodeFor(error), error); }
[[noreturn]] void FailLast() { FailError(GetLastError()); }

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

bool EqualIgnoringCase(const wchar_t* a, size_t a_size, const wchar_t* b, size_t b_size) {
  if (a_size != b_size) return false;
  if (!a_size) return true;
  return CompareStringOrdinal(a, static_cast<int>(a_size), b, static_cast<int>(b_size), TRUE) == CSTR_EQUAL;
}

std::wstring FinalPath(HANDLE handle) {
  std::wstring out(512, L'\0');
  for (int attempt = 0; attempt < 4; ++attempt) {
    const DWORD size = GetFinalPathNameByHandleW(handle, out.data(), static_cast<DWORD>(out.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!size) FailLast();
    if (size < out.size()) { out.resize(size); return out; }
    out.resize(size + 1);
  }
  Fail("drive_io_failed");
}

std::wstring Join(const std::vector<std::wstring>& segments) {
  std::wstring out;
  for (const auto& segment : segments) {
    if (!out.empty()) out += L'\\';
    out += segment;
  }
  return out;
}

int64_t Milliseconds(const FILETIME& time) {
  const uint64_t ticks = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  if (ticks <= 116444736000000000ULL) return 0;
  return std::min<int64_t>(static_cast<int64_t>((ticks - 116444736000000000ULL) / 10000ULL), kMaxTime);
}

FILETIME FileTime(int64_t ms) {
  const uint64_t ticks = uint64_t(std::clamp<int64_t>(ms, 0, kMaxTime)) * 10000ULL + 116444736000000000ULL;
  FILETIME out{};
  out.dwLowDateTime = static_cast<DWORD>(ticks);
  out.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
  return out;
}

Entry FromAttributes(std::wstring name, DWORD attributes, uint64_t size, const FILETIME& written, const FILETIME& created) {
  Entry entry;
  entry.name = std::move(name);
  entry.dir = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  entry.size = entry.dir ? 0 : std::min<uint64_t>(size, 9007199254740991ULL);
  entry.modified = Milliseconds(written);
  entry.created = Milliseconds(created);
  entry.readOnly = !entry.dir && (attributes & FILE_ATTRIBUTE_READONLY) != 0;
  return entry;
}

// The long (\\?\) form of an absolute local or UNC path.
std::wstring LongPath(const std::wstring& path) {
  if (path.size() < 3 || path.size() > 32000 || path.find(L'\0') != std::wstring::npos) Fail("drive_invalid_path");
  if (path.rfind(L"\\\\?\\", 0) == 0) return path;
  if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
  const wchar_t drive = path[0];
  if (((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z')) && path[1] == L':' && path[2] == L'\\') return L"\\\\?\\" + path;
  Fail("drive_invalid_path");
}

// The result of walking the components below a share root.
struct Walk {
  size_t existing = 0;  // components that exist (segments.size(): the whole path)
  DWORD attributes = 0;  // of the last existing component (the root when none)
};

}  // namespace

bool SafeName(const std::wstring& name, size_t max) {
  if (name.empty() || name.size() > max) return false;
  for (size_t i = 0; i < name.size(); ++i) {
    const wchar_t c = name[i];
    if (c < 32 || std::wcschr(L"<>:\"/\\|?*", c)) return false;
    if (c >= 0xd800 && c <= 0xdbff) {
      if (++i == name.size() || name[i] < 0xdc00 || name[i] > 0xdfff) return false;
    } else if (c >= 0xdc00 && c <= 0xdfff) {
      return false;
    }
  }
  if (name.back() == L'.' || name.back() == L' ') return false;
  std::wstring stem = name.substr(0, name.find(L'.'));
  while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) stem.pop_back();
  for (auto& c : stem) if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - 32);
  static const std::set<std::wstring> devices{L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
  if (devices.count(stem)) return false;
  if (stem.size() == 4 && (stem.rfind(L"COM", 0) == 0 || stem.rfind(L"LPT", 0) == 0)) {
    const wchar_t digit = stem[3];
    if ((digit >= L'1' && digit <= L'9') || digit == 0xb9 || digit == 0xb2 || digit == 0xb3) return false;
  }
  return true;
}

bool ShellMetadataName(const std::wstring& name) {
  const auto is = [&](const wchar_t* fixed) {
    return EqualIgnoringCase(name.data(), name.size(), fixed, std::wcslen(fixed));
  };
  const auto endsWith = [&](const wchar_t* suffix) {
    const size_t size = std::wcslen(suffix);
    return name.size() >= size && EqualIgnoringCase(name.data() + name.size() - size, size, suffix, size);
  };
  return is(L"desktop.ini") || is(L"autorun.inf") || endsWith(L".lnk") || endsWith(L".url") || endsWith(L".scf") ||
         endsWith(L".library-ms") || endsWith(L".searchConnector-ms");
}

std::vector<std::wstring> Segments(const std::wstring& path) {
  std::vector<std::wstring> segments;
  if (path.empty()) return segments;
  if (path.size() > kMaxPathUnits) Fail("drive_invalid_path");
  size_t start = 0;
  for (;;) {
    const auto end = path.find(L'/', start);
    segments.push_back(path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
    if (!SafeName(segments.back()) || segments.size() > kMaxDepth) Fail("drive_invalid_path");
    if (end == std::wstring::npos) break;
    start = end + 1;
  }
  return segments;
}

const char* CodeFor(DWORD error) {
  switch (error) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_BAD_NETPATH: case ERROR_BAD_NET_NAME:
    case ERROR_INVALID_DRIVE: case ERROR_DEV_NOT_EXIST: case ERROR_NOT_READY:
      return "drive_not_found";
    case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME: case ERROR_FILENAME_EXCED_RANGE:
      return "drive_invalid_path";
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS:
      return "drive_exists";
    case ERROR_DIR_NOT_EMPTY:
      return "drive_not_empty";
    case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: case ERROR_WRITE_PROTECT: case ERROR_CANT_ACCESS_FILE:
    case ERROR_NETWORK_ACCESS_DENIED: case ERROR_EA_ACCESS_DENIED:
      return "drive_denied";
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_USER_MAPPED_FILE: case ERROR_DELETE_PENDING:
      return "drive_busy";
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: case ERROR_DISK_QUOTA_EXCEEDED:
      return "drive_no_space";
    case ERROR_DIRECTORY:
      return "drive_not_directory";
    case ERROR_CANT_RESOLVE_FILENAME:
      return "drive_link_refused";
    default:
      return "drive_io_failed";
  }
}

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) return {};
  if (text.size() > 1 << 20) throw std::invalid_argument("text");
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (count <= 0) throw std::invalid_argument("text");
  std::wstring out(static_cast<size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), count) != count) throw std::invalid_argument("text");
  return out;
}

std::string WideToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (count <= 0) throw std::invalid_argument("text");
  std::string out(static_cast<size_t>(count), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr) != count) throw std::invalid_argument("text");
  return out;
}

namespace {

// The opened object must still lie under the share root's final path.
std::wstring RequireWithin(const std::wstring& root_final, const std::wstring& prefix, HANDLE handle) {
  const auto final = FinalPath(handle);
  if (EqualIgnoringCase(final.data(), final.size(), root_final.data(), root_final.size())) return final;
  if (final.size() > prefix.size() && EqualIgnoringCase(final.data(), prefix.size(), prefix.data(), prefix.size())) return final;
  Fail("drive_link_refused");
}

std::wstring LeafOf(const std::wstring& final) {
  const auto slash = final.find_last_of(L'\\');
  return slash == std::wstring::npos ? final : final.substr(slash + 1);
}

// Every existing component below the root: never a link; every component
// before the last must be a directory.
Walk CheckComponents(const std::wstring& prefix, const std::vector<std::wstring>& segments) {
  Walk walk;
  walk.attributes = FILE_ATTRIBUTE_DIRECTORY;
  std::wstring current = prefix;
  for (size_t i = 0; i < segments.size(); ++i) {
    current += segments[i];
    Handle handle(CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle.valid()) {
      const DWORD error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return walk;
      FailError(error);
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(handle.value, FileAttributeTagInfo, &tag, sizeof(tag))) FailLast();
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(tag.ReparseTag)) Fail("drive_link_refused");
    walk.existing = i + 1;
    walk.attributes = tag.FileAttributes;
    if (i + 1 < segments.size()) {
      if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) Fail("drive_not_directory");
      current += L'\\';
    }
  }
  return walk;
}

bool IsDirectory(DWORD attributes) { return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }

// Creation and rename must not re-resolve a replaceable parent after checking
// it. Keep the validated directory open without delete sharing until mutation
// completes, which also pins its ancestors against a move out of the share.
std::unique_ptr<Handle> PinParent(const std::wstring& root, const std::wstring& prefix, const std::vector<std::wstring>& segments, bool rename = false) {
  const auto parent = segments.size() <= 1 ? root : prefix + Join(std::vector<std::wstring>(segments.begin(), segments.end() - 1));
  auto handle = std::make_unique<Handle>(CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE | (rename ? FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY : 0),
      rename ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!handle->valid()) FailLast();
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (!GetFileInformationByHandleEx(handle->value, FileAttributeTagInfo, &tag, sizeof(tag))) FailLast();
  if (!IsDirectory(tag.FileAttributes)) Fail("drive_not_directory");
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(tag.ReparseTag)) Fail("drive_link_refused");
  (void)RequireWithin(root, prefix, handle->value);
  return handle;
}

void SetLength(HANDLE handle, uint64_t length) {
  FILE_END_OF_FILE_INFO info{};
  info.EndOfFile.QuadPart = static_cast<LONGLONG>(length);
  if (!SetFileInformationByHandle(handle, FileEndOfFileInfo, &info, sizeof(info))) FailLast();
}

void WriteAt(HANDLE handle, uint64_t offset, const uint8_t* data, size_t size) {
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) FailLast();
  size_t written = 0;
  while (written < size) {
    DWORD count = 0;
    if (!WriteFile(handle, data + written, static_cast<DWORD>(std::min<size_t>(size - written, 1 << 20)), &count, nullptr)) FailLast();
    if (!count) Fail("drive_io_failed");
    written += count;
  }
}

// Mark-of-the-Web (zone_identifier.h): what the remote desktop writes into a
// shared folder is an Internet zone file. The leaf was just opened and
// checked inside the share and its parent is pinned.
void MarkWritten(HANDLE file, const std::wstring& full) {
  if (const DWORD error = multitap::MarkInternetZone(file, full)) FailError(error);
}

}  // namespace

namespace {

// A label: the folder's name, or the drive letter of a volume root.
std::wstring LabelOf(const std::wstring& final) {
  std::wstring trimmed = final;
  while (!trimmed.empty() && trimmed.back() == L'\\') trimmed.pop_back();
  std::wstring label = LeafOf(trimmed);
  if (label.size() == 2 && label[1] == L':') label = label.substr(0, 1);
  return label;
}

std::wstring DisplayPath(const std::wstring& final) {
  if (final.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + final.substr(8);
  if (final.rfind(L"\\\\?\\", 0) == 0) return final.substr(4);
  return final;
}

bool InRecycleBin(const std::wstring& final) {
  static const std::wstring needle = L"\\$RECYCLE.BIN\\";
  if (final.size() < needle.size()) return false;
  for (size_t i = 0; i + needle.size() <= final.size(); ++i) {
    if (EqualIgnoringCase(final.data() + i, needle.size(), needle.data(), needle.size())) return true;
  }
  return false;
}

}  // namespace

// The Win32 errors a handle on a pulled device or a dropped network share
// answers with. Only these mean "offline"; anything else is a hiccup.
static bool DeviceGone(DWORD error) {
  switch (error) {
    case ERROR_DEVICE_NOT_CONNECTED: case ERROR_NO_SUCH_DEVICE: case ERROR_UNRECOGNIZED_VOLUME: case ERROR_NOT_READY:
    case ERROR_BAD_NETPATH: case ERROR_NETNAME_DELETED: case ERROR_DEV_NOT_EXIST: case ERROR_NETWORK_UNREACHABLE:
    case ERROR_HOST_UNREACHABLE: case ERROR_VC_DISCONNECTED: case ERROR_UNEXP_NET_ERR: case ERROR_CONNECTION_UNAVAIL:
    case ERROR_GEN_FAILURE: case ERROR_BAD_NET_NAME: case ERROR_INVALID_DRIVE: case ERROR_PATH_NOT_FOUND:
      return true;
    default:
      return false;
  }
}

// Re-reads where the root folder is now through the handle kept since Open.
// Called WITHOUT the mutex (it may wait for a network share's timeout).
void Files::Refresh(Root& root) {
  if (root.deleted) return;
  HANDLE handle = root.handle.get();
  FILE_STANDARD_INFO standard{};
  // A failed query is not a deletion (that would end and forget the share on
  // a hiccup): keep the last known state; the next operation tells. A handle
  // whose device is gone is "offline" (not deleted: it may come back).
  if (!GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))) {
    const DWORD error = GetLastError();
    root.offline = DeviceGone(error);
    root.offlineError = root.offline ? error : 0;
    return;
  }
  root.offline = false;
  root.offlineError = 0;
  if (standard.DeletePending) {
    root.deleted = true;
    return;
  }
  FILE_ID_INFO id{};
  if (GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) &&
      (id.VolumeSerialNumber != root.id.VolumeSerialNumber || std::memcmp(&id.FileId, &root.id.FileId, sizeof(id.FileId)) != 0)) {
    root.deleted = true;
    return;
  }
  std::wstring final;
  try {
    final = FinalPath(handle);
  } catch (const Failure&) {
    return;  // keep the last known location; the next operation tells
  }
  // Moved to the Recycle Bin: the folder was deleted from the user's view.
  if (InRecycleBin(final) && !InRecycleBin(root.final)) {
    root.deleted = true;
    return;
  }
  root.final = final;
  root.prefix = final.back() == L'\\' ? final : final + L'\\';
}

Files::Root Files::Refreshed(const std::string& share) {
  Root copy;
  {
    std::lock_guard lock(mutex_);
    const auto found = roots_.find(share);
    if (found == roots_.end()) Fail("drive_share_unknown");
    copy = found->second;
  }
  Refresh(copy);  // I/O on the root's handle, outside the lock
  {
    std::lock_guard lock(mutex_);
    const auto found = roots_.find(share);
    // Stored back only when the share is still the one that was refreshed
    // (it may have been closed or re-opened meanwhile).
    if (found != roots_.end() && found->second.handle == copy.handle) {
      found->second.deleted = copy.deleted;
      found->second.offline = copy.offline;
      found->second.offlineError = copy.offlineError;
      found->second.final = copy.final;
      found->second.prefix = copy.prefix;
    }
  }
  return copy;
}

Files::Root Files::RootOf(const std::string& share) {
  Root root = Refreshed(share);
  if (root.deleted) Fail("drive_not_found");
  // The device is gone: "not found" at once instead of waiting on a dead
  // handle in every request (the VM's apps get a clear missing-path error).
  if (root.offline) throw Failure("drive_not_found", root.offlineError);
  return root;
}

Files::Status Files::StatusOf(const std::string& share) {
  const Root root = Refreshed(share);
  Status status;
  status.deleted = root.deleted;
  status.offline = root.offline;
  status.path = DisplayPath(root.final);
  status.label = LabelOf(root.final);
  return status;
}

bool Files::Has(const std::string& share) const {
  std::lock_guard lock(mutex_);
  return roots_.count(share) != 0;
}

std::wstring Files::Open(const std::string& share, const std::wstring& path, bool readOnly) {
  if (share.empty() || share.size() > 32 || share.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) Fail("drive_invalid_path");
  const auto full = LongPath(path);
  // The root the user picked may itself be a link; it is resolved once here.
  // The handle stays open (share-all, so the PC can still rename or delete the
  // folder) and is what the share follows.
  HANDLE opened = CreateFileW(full.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (opened == INVALID_HANDLE_VALUE) FailLast();
  Root root;
  root.handle = std::shared_ptr<void>(opened, [](void* value) { CloseHandle(value); });
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(opened, &info)) FailLast();
  if (!IsDirectory(info.dwFileAttributes)) Fail("drive_not_directory");
  if (!GetFileInformationByHandleEx(opened, FileIdInfo, &root.id, sizeof(root.id))) FailLast();
  root.final = FinalPath(opened);
  root.prefix = root.final.back() == L'\\' ? root.final : root.final + L'\\';
  root.readOnly = readOnly;
  const std::wstring label = LabelOf(root.final);
  {
    std::lock_guard lock(mutex_);
    roots_[share] = std::move(root);
  }
  return label;
}

void Files::Close(const std::string& share) {
  std::lock_guard lock(mutex_);
  roots_.erase(share);
}

Entry Files::Stat(const std::string& share, const std::wstring& path) {
  const auto root = RootOf(share);
  const auto segments = Segments(path);
  if (CheckComponents(root.prefix, segments).existing < segments.size()) Fail("drive_not_found");
  const auto full = segments.empty() ? root.final : root.prefix + Join(segments);
  Handle handle(CreateFileW(full.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!handle.valid()) FailLast();
  const auto final = RequireWithin(root.final, root.prefix, handle.value);
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle.value, &info)) FailLast();
  Entry entry = FromAttributes(segments.empty() ? std::wstring() : LeafOf(final), info.dwFileAttributes,
                               (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow, info.ftLastWriteTime, info.ftCreationTime);
  if (segments.empty()) {
    entry.name.clear();
    entry.dir = true;
    entry.size = 0;
    ULARGE_INTEGER available{}, total{};
    if (GetDiskFreeSpaceExW(root.prefix.c_str(), &available, &total, nullptr) && available.QuadPart <= total.QuadPart) {
      entry.hasSpace = true;
      entry.free = std::min<uint64_t>(available.QuadPart, 9007199254740991ULL);
      entry.total = std::min<uint64_t>(total.QuadPart, 9007199254740991ULL);
    }
  } else if (!SafeName(entry.name)) {
    entry.name = segments.back();
  }
  return entry;
}

std::vector<Entry> Files::List(const std::string& share, const std::wstring& path, size_t max) {
  const auto root = RootOf(share);
  const auto segments = Segments(path);
  const auto walk = CheckComponents(root.prefix, segments);
  if (walk.existing < segments.size()) Fail("drive_not_found");
  if (!IsDirectory(walk.attributes)) Fail("drive_not_directory");
  const auto directory = segments.empty() ? root.final : root.prefix + Join(segments);
  Handle handle(CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!handle.valid()) FailLast();
  (void)RequireWithin(root.final, root.prefix, handle.value);
  const auto pattern = (segments.empty() ? root.prefix : root.prefix + Join(segments) + L'\\') + L'*';
  WIN32_FIND_DATAW data{};
  HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
  std::vector<Entry> entries;
  if (find == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES) return entries;
    FailError(error);
  }
  struct Finder { HANDLE value; ~Finder() { FindClose(value); } } finder{find};
  do {
    const std::wstring name = data.cFileName;
    if (name == L"." || name == L"..") continue;
    // Links (symbolic links, junctions, mount points) are never offered.
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(data.dwReserved0)) continue;
    if (!SafeName(name)) continue;
    entries.push_back(FromAttributes(name, data.dwFileAttributes, (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
                                     data.ftLastWriteTime, data.ftCreationTime));
    if (entries.size() >= max) break;
  } while (FindNextFileW(find, &data));
  return entries;
}

std::vector<uint8_t> Files::Read(const std::string& share, const std::wstring& path, uint64_t offset, uint32_t length) {
  if (!length || length > kMaxChunk || offset > kMaxFileSize) Fail("drive_invalid_path");
  const auto root = RootOf(share);
  const auto segments = Segments(path);
  if (segments.empty()) Fail("drive_is_directory");
  const auto walk = CheckComponents(root.prefix, segments);
  if (walk.existing < segments.size()) Fail("drive_not_found");
  if (IsDirectory(walk.attributes)) Fail("drive_is_directory");
  // No FILE_FLAG_OPEN_REPARSE_POINT: a cloud placeholder is read (hydrated).
  Handle handle(CreateFileW((root.prefix + Join(segments)).c_str(), GENERIC_READ, kShareAll, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!handle.valid()) FailLast();
  (void)RequireWithin(root.final, root.prefix, handle.value);
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle.value, &info)) FailLast();
  if (IsDirectory(info.dwFileAttributes)) Fail("drive_is_directory");
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(std::min<uint64_t>(offset, uint64_t(INT64_MAX)));
  if (!SetFilePointerEx(handle.value, position, nullptr, FILE_BEGIN)) FailLast();
  std::vector<uint8_t> out(length);
  DWORD total = 0;
  while (total < length) {
    DWORD count = 0;
    if (!ReadFile(handle.value, out.data() + total, length - total, &count, nullptr)) FailLast();
    if (!count) break;
    total += count;
  }
  out.resize(total);
  return out;
}

void Files::Write(const std::string& share, const std::wstring& path, uint64_t offset, const uint8_t* data, size_t size, bool truncate) {
  const auto root = RootOf(share);
  if (root.readOnly) Fail("drive_read_only");
  if (!size || size > kMaxChunk || (truncate && offset) || offset > kMaxFileSize || size > kMaxFileSize - offset) Fail("drive_invalid_path");
  const auto segments = Segments(path);
  if (segments.empty()) Fail("drive_is_directory");
  if (ShellMetadataName(segments.back())) Fail("drive_denied");
  const auto walk = CheckComponents(root.prefix, segments);
  if (walk.existing + 1 < segments.size()) Fail("drive_not_found");
  if (walk.existing == segments.size() && IsDirectory(walk.attributes)) Fail("drive_is_directory");
  if (!truncate && walk.existing < segments.size()) Fail("drive_not_found");
  const auto parent = PinParent(root.final, root.prefix, segments);
  const auto full = root.prefix + Join(segments);
  Handle handle(CreateFileW(full.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, kShareAll, nullptr,
                            truncate ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!handle.valid()) FailLast();
  // Nothing is changed before the opened file is known to be inside the share.
  (void)RequireWithin(root.final, root.prefix, handle.value);
  MarkWritten(handle.value, full);
  if (truncate) SetLength(handle.value, 0);
  WriteAt(handle.value, offset, data, size);
}

void Files::SetSize(const std::string& share, const std::wstring& path, uint64_t size) {
  const auto root = RootOf(share);
  if (root.readOnly) Fail("drive_read_only");
  if (size > 9007199254740991ULL) Fail("drive_invalid_path");
  const auto segments = Segments(path);
  if (segments.empty()) Fail("drive_is_directory");
  const auto walk = CheckComponents(root.prefix, segments);
  if (walk.existing < segments.size()) Fail("drive_not_found");
  if (IsDirectory(walk.attributes)) Fail("drive_is_directory");
  Handle handle(CreateFileW((root.prefix + Join(segments)).c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, kShareAll, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!handle.valid()) FailLast();
  // Nothing is changed before the opened file is known to be inside the share.
  (void)RequireWithin(root.final, root.prefix, handle.value);
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle.value, &info)) FailLast();
  if (IsDirectory(info.dwFileAttributes)) Fail("drive_is_directory");
  SetLength(handle.value, size);  // growing zero-fills (sparse where the volume allows)
}

void Files::Flush(const std::string& share, const std::wstring& path) {
  const auto root = RootOf(share);
  const auto segments = Segments(path);
  // The root, a directory and a read-only share have nothing to make durable.
  if (segments.empty()) return;
  const auto walk = CheckComponents(root.prefix, segments);
  if (walk.existing < segments.size()) Fail("drive_not_found");
  if (root.readOnly || IsDirectory(walk.attributes)) return;
  Handle handle(CreateFileW((root.prefix + Join(segments)).c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, kShareAll, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  // A file may have been written successfully before another PC application
  // locked it or its attributes changed. Failure to flush is not a durable ACK.
  if (!handle.valid()) FailLast();
  (void)RequireWithin(root.final, root.prefix, handle.value);
  if (!FlushFileBuffers(handle.value)) FailLast();
}

void Files::Create(const std::string& share, const std::wstring& path, bool dir, bool truncate) {
  const auto root = RootOf(share);
  if (root.readOnly) Fail("drive_read_only");
  if (dir && truncate) Fail("drive_invalid_path");
  const auto segments = Segments(path);
  if (segments.empty()) Fail("drive_exists");
  if (ShellMetadataName(segments.back())) Fail("drive_denied");
  const auto walk = CheckComponents(root.prefix, segments);
  if (walk.existing + 1 < segments.size()) Fail("drive_not_found");
  const bool exists = walk.existing == segments.size();
  const auto parent = PinParent(root.final, root.prefix, segments);
  const auto full = root.prefix + Join(segments);
  if (dir) {
    if (exists) Fail("drive_exists");
    if (!CreateDirectoryW(full.c_str(), nullptr)) FailLast();
    Handle handle(CreateFileW(full.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle.valid()) FailLast();
    (void)RequireWithin(root.final, root.prefix, handle.value);
    return;
  }
  if (exists && IsDirectory(walk.attributes)) Fail(truncate ? "drive_is_directory" : "drive_exists");
  if (exists && !truncate) Fail("drive_exists");
  Handle handle(CreateFileW(full.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, kShareAll, nullptr,
                            truncate ? OPEN_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!handle.valid()) FailLast();
  (void)RequireWithin(root.final, root.prefix, handle.value);
  MarkWritten(handle.value, full);
  if (truncate) SetLength(handle.value, 0);
}

void Files::Rename(const std::string& share, const std::wstring& path, const std::wstring& to, bool replace) {
  const auto root = RootOf(share);
  if (root.readOnly) Fail("drive_read_only");
  const auto from = Segments(path), target = Segments(to);
  if (from.empty() || target.empty()) Fail("drive_invalid_path");
  if (ShellMetadataName(target.back())) Fail("drive_denied");
  if (CheckComponents(root.prefix, from).existing < from.size()) Fail("drive_not_found");
  const auto destination = CheckComponents(root.prefix, target);
  if (destination.existing + 1 < target.size()) Fail("drive_not_found");
  // "report.txt" -> "Report.txt": the destination "exists" because it is the
  // same file under another capitalisation. A case-only rename is allowed
  // without Overwrite (Explorer sends none), but only when the two paths name
  // the same object, i.e. differ in capitalisation alone.
  bool caseOnly = from.size() == target.size();
  for (size_t i = 0; caseOnly && i < from.size(); ++i) caseOnly = EqualIgnoringCase(from[i].data(), from[i].size(), target[i].data(), target[i].size());
  if (destination.existing == target.size() && !replace && !caseOnly) Fail("drive_exists");
  const auto parent = PinParent(root.final, root.prefix, target, true);
  Handle source(CreateFileW((root.prefix + Join(from)).c_str(), DELETE | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!source.valid()) FailLast();
  (void)RequireWithin(root.final, root.prefix, source.value);
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (!GetFileInformationByHandleEx(source.value, FileAttributeTagInfo, &tag, sizeof(tag))) FailLast();
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(tag.ReparseTag)) Fail("drive_link_refused");
  // Use the documented user-mode native operation with the checked parent.
  // SetFileInformationByHandle rejects RootDirectory here (Win32 error 87),
  // and its absolute-path wrapper reopens a parent pinned against writes.
  // The native call preserves handle-relative resolution without weakening
  // the parent's delete sharing or falling back to MoveFileEx paths.
  const auto& name = target.back();
  std::vector<BYTE> storage(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t), 0);
  auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
  info->ReplaceIfExists = replace ? TRUE : FALSE; info->RootDirectory = parent->value;
  info->FileNameLength = DWORD(name.size() * sizeof(wchar_t));
  memcpy(info->FileName, name.data(), info->FileNameLength);
  using SetInformation = LONG(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
  using StatusToError = ULONG(NTAPI*)(LONG);
  const auto module = GetModuleHandleW(L"ntdll.dll");
  const auto set = reinterpret_cast<SetInformation>(reinterpret_cast<void*>(GetProcAddress(module, "NtSetInformationFile")));
  const auto toError = reinterpret_cast<StatusToError>(reinterpret_cast<void*>(GetProcAddress(module, "RtlNtStatusToDosError")));
  if (!set || !toError) FailError(ERROR_NOT_SUPPORTED);
  if (beforeRename_) beforeRename_();
  IO_STATUS_BLOCK io{};
  LONG status = set(source.value, &io, info, ULONG(storage.size()), FILE_INFORMATION_CLASS(10));
  if (status == 0x103) { if (WaitForSingleObject(source.value, INFINITE) != WAIT_OBJECT_0) FailLast(); status = io.Status; }
  if (status < 0) FailError(toError(status));
}

void Files::Delete(const std::string& share, const std::wstring& path) {
  const auto root = RootOf(share);
  if (root.readOnly) Fail("drive_read_only");
  const auto segments = Segments(path);
  if (segments.empty()) Fail("drive_denied");
  if (CheckComponents(root.prefix, segments).existing < segments.size()) Fail("drive_not_found");
  Handle handle(CreateFileW((root.prefix + Join(segments)).c_str(), DELETE | FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!handle.valid()) FailLast();
  (void)RequireWithin(root.final, root.prefix, handle.value);
  // The handle is deleted, never a path re-resolved later; a directory only
  // when empty (ERROR_DIR_NOT_EMPTY otherwise).
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  if (!SetFileInformationByHandle(handle.value, FileDispositionInfo, &disposition, sizeof(disposition))) FailLast();
}

void Files::Touch(const std::string& share, const std::wstring& path, int64_t modified) {
  const auto root = RootOf(share);
  if (root.readOnly) Fail("drive_read_only");
  const auto segments = Segments(path);
  if (segments.empty()) Fail("drive_is_directory");
  const auto walk = CheckComponents(root.prefix, segments);
  if (walk.existing < segments.size()) Fail("drive_not_found");
  if (IsDirectory(walk.attributes)) Fail("drive_is_directory");
  Handle handle(CreateFileW((root.prefix + Join(segments)).c_str(), FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES, kShareAll, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!handle.valid()) FailLast();
  (void)RequireWithin(root.final, root.prefix, handle.value);
  const FILETIME time = FileTime(modified);
  if (!SetFileTime(handle.value, nullptr, nullptr, &time)) FailLast();
}

}  // namespace multitap::drive
