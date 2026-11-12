#pragma once
// WinFsp file system for one streaming drive share (docs/drive-redirection.md,
// "WinFsp backend"). Runs in the session user's helper process. Every callback
// becomes one or more drive-v1 operations on a `Backend` (the relay channel via
// the service); nothing here touches the VM's own files.
//
// The WinFsp DLL is never linked: `Api` is a table of the few exports used,
// resolved from the DLL FspLoad found, so a Host on a machine without WinFsp
// starts normally and the unit tests drive the callbacks with a fake table.
#include "drive_winfsp_wire.hpp"
#pragma warning(push, 0)
#include <winfsp/winfsp.h>
#pragma warning(pop)
#include <atomic>
#include <chrono>
#include <list>
#include <sddl.h>
#include <shared_mutex>
#include <set>
#include <tuple>

namespace host::drive::fs {
using Clock = std::chrono::steady_clock;

// ---- the WinFsp exports (names as in winfsp.h) -------------------------------
struct Api {
  NTSTATUS (*create)(PWSTR, const FSP_FSCTL_VOLUME_PARAMS*, const FSP_FILE_SYSTEM_INTERFACE*, FSP_FILE_SYSTEM**) = nullptr;
  VOID (*destroy)(FSP_FILE_SYSTEM*) = nullptr;
  NTSTATUS (*setMountPoint)(FSP_FILE_SYSTEM*, PWSTR) = nullptr;
  VOID (*removeMountPoint)(FSP_FILE_SYSTEM*) = nullptr;
  NTSTATUS (*startDispatcher)(FSP_FILE_SYSTEM*, ULONG) = nullptr;
  VOID (*stopDispatcher)(FSP_FILE_SYSTEM*) = nullptr;
  BOOLEAN (*acquireDirectoryBuffer)(PVOID*, BOOLEAN, PNTSTATUS) = nullptr;
  BOOLEAN (*fillDirectoryBuffer)(PVOID*, FSP_FSCTL_DIR_INFO*, PNTSTATUS) = nullptr;
  VOID (*releaseDirectoryBuffer)(PVOID*) = nullptr;
  VOID (*readDirectoryBuffer)(PVOID*, PWSTR, PVOID, ULONG, PULONG) = nullptr;
  VOID (*deleteDirectoryBuffer)(PVOID*) = nullptr;
  NTSTATUS (*version)(PUINT32) = nullptr;
  bool complete() const { return create && destroy && setMountPoint && removeMountPoint && startDispatcher && stopDispatcher && acquireDirectoryBuffer && fillDirectoryBuffer && releaseDirectoryBuffer && readDirectoryBuffer && deleteDirectoryBuffer; }
};
inline Api resolveApi(HMODULE module) {
  Api api;
  const auto symbol = [&](const char* name) { return module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr; };
#define MULTITAP_FSP(field, name) api.field = reinterpret_cast<decltype(api.field)>(symbol(name))
  MULTITAP_FSP(create, "FspFileSystemCreate"); MULTITAP_FSP(destroy, "FspFileSystemDelete"); MULTITAP_FSP(setMountPoint, "FspFileSystemSetMountPoint");
  MULTITAP_FSP(removeMountPoint, "FspFileSystemRemoveMountPoint"); MULTITAP_FSP(startDispatcher, "FspFileSystemStartDispatcher"); MULTITAP_FSP(stopDispatcher, "FspFileSystemStopDispatcher");
  MULTITAP_FSP(acquireDirectoryBuffer, "FspFileSystemAcquireDirectoryBuffer"); MULTITAP_FSP(fillDirectoryBuffer, "FspFileSystemFillDirectoryBuffer");
  MULTITAP_FSP(releaseDirectoryBuffer, "FspFileSystemReleaseDirectoryBuffer"); MULTITAP_FSP(readDirectoryBuffer, "FspFileSystemReadDirectoryBuffer");
  MULTITAP_FSP(deleteDirectoryBuffer, "FspFileSystemDeleteDirectoryBuffer"); MULTITAP_FSP(version, "FspVersion");
#undef MULTITAP_FSP
  return api;
}

// ---- pure helpers ------------------------------------------------------------
inline constexpr uint64_t unixEpochFileTime = 116444736000000000ULL;
inline uint64_t toFileTime(int64_t ms) { return uint64_t(std::clamp<int64_t>(ms, 0, 253402300799999LL)) * 10000ULL + unixEpochFileTime; }
inline int64_t toUnixMs(uint64_t fileTime) { return fileTime < unixEpochFileTime ? 0 : int64_t(std::min<uint64_t>((fileTime - unixEpochFileTime) / 10000ULL, 253402300799999ULL)); }
inline int64_t nowMs() { FILETIME f; GetSystemTimeAsFileTime(&f); return toUnixMs((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime); }
inline uint64_t indexNumber(const std::string& path) { uint64_t h = 1469598103934665603ULL; for (const unsigned char c : path) { h ^= c; h *= 1099511628211ULL; } return h ? h : 1; }
inline constexpr uint64_t allocationUnit = 4096;
inline uint64_t allocationFor(uint64_t size) { return (size + allocationUnit - 1) / allocationUnit * allocationUnit; }

// "\dir\file" (WinFsp) -> "dir/file" (drive-v1); "\" -> "". nullopt when the name is
// not a plain drive-v1 path (stream names, device names, trailing dots, too deep).
inline std::optional<std::string> toDrivePath(const wchar_t* name) {
  if (!name || name[0] != L'\\') return std::nullopt;
  std::wstring text(name + 1);
  while (!text.empty() && text.back() == L'\\') text.pop_back();
  if (text.empty()) return std::string();
  for (auto& c : text) if (c == L'\\') c = L'/';
  try {
    (void)pathSegments(text);
    return utf8(text);
  } catch (...) { return std::nullopt; }
}

// drive-v1 error -> NTSTATUS. `stale` is an operation on an already open handle:
// a vanished file or share there means the PC side went away.
inline NTSTATUS statusFor(const std::string& code, bool stale = false) {
  if (code.empty()) return STATUS_SUCCESS;
  if (code == "drive_not_found") return stale ? STATUS_FILE_INVALID : STATUS_OBJECT_NAME_NOT_FOUND;
  if (code == "drive_exists") return STATUS_OBJECT_NAME_COLLISION;
  if (code == "drive_not_empty") return STATUS_DIRECTORY_NOT_EMPTY;
  if (code == "drive_read_only") return STATUS_MEDIA_WRITE_PROTECTED;
  if (code == "drive_denied" || code == "drive_link_refused") return STATUS_ACCESS_DENIED;
  if (code == "drive_invalid_path") return STATUS_OBJECT_NAME_INVALID;
  if (code == "drive_share_unknown" || code == "drive_unavailable") return STATUS_NETWORK_NAME_DELETED;
  if (code == "drive_busy") return STATUS_SHARING_VIOLATION;
  if (code == "drive_no_space") return STATUS_DISK_FULL;
  if (code == "drive_is_directory") return STATUS_FILE_IS_A_DIRECTORY;
  if (code == "drive_not_directory") return STATUS_NOT_A_DIRECTORY;
  if (code == "drive_timeout") return STATUS_IO_TIMEOUT;
  return STATUS_UNEXPECTED_IO_ERROR;
}

struct Options {
  std::string shareId;                 // drive-v1 share id
  std::string token;                   // "Multitap-<43>": the UNC prefix \multitap\<token>
  std::wstring label;                  // "<name> (PC)"
  bool readOnly = false;
  std::wstring ownerSid;               // S-1-5-21-...: the only account (besides SYSTEM and Administrators) that may open the volume
  std::chrono::milliseconds callTimeout{70000};
  size_t blockBytes = chunkBytes;      // read cache block (= one drive-v1 read)
  size_t cacheBlocks = 48;             // 12 MiB per share
  size_t prefetchBlocks = maxInFlight; // read-ahead beyond a sequential read: up to the relay's 8 requests (2 MiB) in flight
  uint32_t fileInfoTimeoutMs = 1000;
  ULONG dispatcherThreads = 8;
};

class ShareFs {
 public:
  ShareFs(const Api& api, Backend& backend, Options options) : api_(api), backend_(backend), options_(std::move(options)), readOnly_(options_.readOnly) {
    buildSecurity();
  }
  ShareFs(const ShareFs&) = delete; ShareFs& operator=(const ShareFs&) = delete;
  ~ShareFs() { unmount(); }

  // ---- lifecycle ----
  // Creates the volume, starts the dispatcher and takes the next free drive
  // letter (Z down). `letter` is "Z" on success. The mount belongs to the
  // calling process's logon session (DefineDosDevice without the launcher).
  NTSTATUS mount(std::wstring& letter) {
    if (fs_) return STATUS_INVALID_PARAMETER;
    if (!api_.complete() || sd_.empty()) return STATUS_NOT_SUPPORTED;
    if (!options_.blockBytes || options_.blockBytes > chunkBytes) return STATUS_INVALID_PARAMETER;
    FSP_FSCTL_VOLUME_PARAMS params{};
    params.Version = sizeof(FSP_FSCTL_VOLUME_PARAMS);
    params.SectorSize = 4096; params.SectorsPerAllocationUnit = 1; params.MaxComponentLength = 255;
    FILETIME now; GetSystemTimeAsFileTime(&now); params.VolumeCreationTime = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    params.VolumeSerialNumber = uint32_t(indexNumber(options_.shareId + options_.token));
    params.IrpTimeout = 120000;                       // above the relay's 60 s request deadline: the PC side answers or the pair ends first
    params.FileInfoTimeout = options_.fileInfoTimeoutMs;
    params.VolumeInfoTimeoutValid = 1; params.VolumeInfoTimeout = 2000;
    params.DirInfoTimeoutValid = 1; params.DirInfoTimeout = options_.fileInfoTimeoutMs;
    params.SecurityTimeoutValid = 1; params.SecurityTimeout = 5000;
    params.CaseSensitiveSearch = 0; params.CasePreservedNames = 1; params.UnicodeOnDisk = 1; params.PersistentAcls = 1;
    params.UmFileContextIsUserContext2 = 1;            // each Open returns its own descriptor, not a shared file-node pointer
    params.ReparsePoints = 0; params.NamedStreams = 0; params.ExtendedAttributes = 0;
    params.ReadOnlyVolume = 0;                        // read-only is enforced per operation: the switch in the client changes it live
    params.FlushAndPurgeOnCleanup = 1;                // data is on the PC when the program closes the file
    const std::wstring prefix = L"\\multitap\\" + wide(options_.token);
    if (prefix.size() >= FSP_FSCTL_VOLUME_PREFIX_SIZE / sizeof(WCHAR)) return STATUS_INVALID_PARAMETER;
    wcscpy_s(params.Prefix, prefix.c_str()); wcscpy_s(params.FileSystemName, L"Multitap");
    FSP_FILE_SYSTEM* created = nullptr;
    NTSTATUS status = api_.create(const_cast<PWSTR>(L"" FSP_FSCTL_NET_DEVICE_NAME), &params, &interface_(), &created);
    if (!NT_SUCCESS(status)) return status;
    created->UserContext = this;
    fs_ = created;
    // Dispatcher first, then the mount point: shell notifications caused by the
    // new drive are answered instead of waiting for a dispatcher.
    status = api_.startDispatcher(fs_, options_.dispatcherThreads);
    if (NT_SUCCESS(status)) { dispatching_ = true; status = api_.setMountPoint(fs_, const_cast<PWSTR>(L"*:")); }
    if (!NT_SUCCESS(status)) { teardown(); return status; }
    letter = fs_->MountPoint && fs_->MountPoint[0] ? std::wstring(1, fs_->MountPoint[0]) : std::wstring();
    return STATUS_SUCCESS;
  }
  // Revocation / session end: the drive disappears first, in-flight callbacks
  // fail fast, then the dispatcher stops and the volume is deleted.
  void unmount() {
    revoked_ = true;
    teardown();
  }
  void setReadOnly(bool value) { readOnly_ = value; clearCaches(); }
  bool readOnly() const { return readOnly_; }
  const Options& options() const { return options_; }
  size_t cachedBlocks() { std::lock_guard lock(cacheMutex_); return blocks_.size(); }
  bool hasSecurity() const { return !sd_.empty(); }
  // Read only, called on the helper control thread while it owns this volume.
  std::wstring ownedVolumeName() const {
    if (!fs_ || !dispatching_ || revoked_) return {};
    const auto length = wcsnlen_s(fs_->VolumeName, std::size(fs_->VolumeName));
    return length && length < std::size(fs_->VolumeName) ? std::wstring(fs_->VolumeName, length) : std::wstring();
  }

  // ---- WinFsp callbacks (public for the tests; the trampolines call them) ----
  struct MutationGate { std::timed_mutex mutex; };
  struct File {
    std::string path; bool dir = false, dirty = false, deleteOnClose = false, invalid = false;
    FSP_FSCTL_FILE_INFO info{}; PVOID dirBuffer = nullptr; uint64_t lastEnd = ~0ULL; unsigned streak = 0;
    int64_t fetchedSize = -1, fetchedModified = -1;  // identity the read cache is keyed with
    Clock::time_point metadataAt = Clock::now(); uint64_t mutation = 0;
    // Shared by case-insensitive aliases of this path in this share. Lifetime
    // follows open descriptors, so the coordination table cannot grow per name.
    std::shared_ptr<MutationGate> mutationGate = std::make_shared<MutationGate>();
    std::mutex mutex;
  };

  NTSTATUS getVolumeInfo(FSP_FSCTL_VOLUME_INFO* info) {
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    uint64_t total = 1ULL << 40, free = 1ULL << 40;   // unknown space must not look full
    const Result root = call(statCall(""));
    if (root.ok() && root.entry.hasSpace) { total = root.entry.total; free = root.entry.free; }
    else if (!root.ok() && root.error != "drive_unavailable") return statusFor(root.error);
    *info = FSP_FSCTL_VOLUME_INFO{}; info->TotalSize = total; info->FreeSize = free;
    const size_t chars = std::min<size_t>(options_.label.size(), 31);
    wmemcpy(info->VolumeLabel, options_.label.c_str(), chars); info->VolumeLabelLength = UINT16(chars * sizeof(WCHAR));
    return STATUS_SUCCESS;
  }
  NTSTATUS getSecurityByName(PWSTR name, PUINT32 attributes, PSECURITY_DESCRIPTOR descriptor, SIZE_T* descriptorSize) {
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    const auto path = toDrivePath(name); if (!path) return STATUS_OBJECT_NAME_INVALID;
    const Result found = stat(*path);
    if (!found.ok()) return found.error == "drive_not_directory" ? STATUS_OBJECT_PATH_NOT_FOUND : statusFor(found.error);
    if (attributes) *attributes = attributesOf(found.entry);
    return copySecurity(descriptor, descriptorSize);
  }
  NTSTATUS open(PWSTR name, UINT32 createOptions, UINT32, PVOID* context, FSP_FSCTL_FILE_INFO* info) {
    std::shared_lock namespaceLock(namespaceMutex_);
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    const auto path = toDrivePath(name); if (!path) return STATUS_OBJECT_NAME_INVALID;
    const Result found = stat(*path);
    if (!found.ok()) return statusFor(found.error);
    if ((createOptions & FILE_DIRECTORY_FILE) && !found.entry.dir) return STATUS_NOT_A_DIRECTORY;
    if ((createOptions & FILE_NON_DIRECTORY_FILE) && found.entry.dir) return STATUS_FILE_IS_A_DIRECTORY;
    auto file = std::make_unique<File>(); file->path = *path; file->dir = found.entry.dir;
    fill(found.entry, *path, file->info); file->fetchedSize = int64_t(found.entry.size); file->fetchedModified = found.entry.modified;
    *info = file->info; *context = remember(std::move(file));
    return STATUS_SUCCESS;
  }
  NTSTATUS create(PWSTR name, UINT32 createOptions, UINT32, UINT32, PSECURITY_DESCRIPTOR, UINT64, PVOID* context, FSP_FSCTL_FILE_INFO* info) {
    std::shared_lock namespaceLock(namespaceMutex_);
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    if (readOnly_) return STATUS_MEDIA_WRITE_PROTECTED;
    const auto path = toDrivePath(name); if (!path || path->empty()) return STATUS_OBJECT_NAME_INVALID;
    const bool dir = (createOptions & FILE_DIRECTORY_FILE) != 0;
    Call c; c.op = Op::create; c.share = options_.shareId; c.path = *path; c.dir = dir; c.truncate = false;
    const Result made = call(std::move(c));
    clearStats();
    if (!made.ok()) return made.error == "drive_not_found" ? STATUS_OBJECT_PATH_NOT_FOUND : statusFor(made.error);
    auto file = std::make_unique<File>(); file->path = *path; file->dir = dir;
    Entry entry; entry.name = path->substr(path->rfind('/') == std::string::npos ? 0 : path->rfind('/') + 1); entry.dir = dir; entry.modified = entry.created = nowMs();
    fill(entry, *path, file->info); file->fetchedSize = 0; file->fetchedModified = entry.modified;
    *info = file->info; *context = remember(std::move(file));
    return STATUS_SUCCESS;
  }
  NTSTATUS overwrite(File* file, UINT32, BOOLEAN, UINT64, FSP_FSCTL_FILE_INFO* info) {
    std::shared_lock namespaceLock(namespaceMutex_);
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    if (file->invalid) return STATUS_FILE_DELETED;
    if (readOnly_) return STATUS_MEDIA_WRITE_PROTECTED;
    if (file->dir) return STATUS_FILE_IS_A_DIRECTORY;
    std::unique_lock mutationLock(file->mutationGate->mutex, std::defer_lock);
    if (!mutationLock.try_lock_for(options_.callTimeout)) return STATUS_IO_TIMEOUT;
    const NTSTATUS status = setSizeLocked(file, 0);
    if (!NT_SUCCESS(status)) return status;
    std::lock_guard lock(file->mutex); *info = file->info; return STATUS_SUCCESS;
  }
  VOID cleanup(File* file, PWSTR, ULONG flags) {
    std::shared_lock namespaceLock(namespaceMutex_);
    if (!file || revoked_ || file->invalid) return;
    if (flags & FspCleanupDelete) {
      if (readOnly_) return;
      Call c; c.op = Op::remove; c.share = options_.shareId; c.path = file->path;
      (void)call(std::move(c)); clearCaches();
      return;
    }
    bool flush = false; uint64_t mutation; { std::lock_guard lock(file->mutex); flush = file->dirty; mutation = file->mutation; }
    if (flush && !file->dir) {
      Call c; c.op = Op::flush; c.share = options_.shareId; c.path = file->path;
      const Result r = call(std::move(c));
      if (r.ok()) { std::lock_guard lock(file->mutex); if (file->mutation == mutation) file->dirty = false; }
    }
  }
  VOID close(File* file) {
    if (!file) return;
    std::lock_guard lock(filesMutex_); files_.erase(file);
    if (file->dirBuffer) api_.deleteDirectoryBuffer(&file->dirBuffer);
    delete file;
  }
  NTSTATUS read(File* file, PVOID buffer, UINT64 offset, ULONG length, PULONG transferred) {
    std::shared_lock namespaceLock(namespaceMutex_);
    *transferred = 0;
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    if (file->invalid) return STATUS_FILE_DELETED;
    if (file->dir) return STATUS_FILE_IS_A_DIRECTORY;
    if (!length) return STATUS_SUCCESS;
    if (offset > wire::maxFileSize) return STATUS_INVALID_PARAMETER;
    bool expired; { std::lock_guard lock(file->mutex); expired = Clock::now() - file->metadataAt >= std::chrono::milliseconds(options_.fileInfoTimeoutMs); }
    if (expired) { const NTSTATUS status = refreshFile(file); if (!NT_SUCCESS(status)) return status; }
    uint64_t size; int64_t modified; bool sequential;
    { std::lock_guard lock(file->mutex); size = file->info.FileSize; modified = file->fetchedModified; sequential = offset == file->lastEnd; file->streak = sequential ? file->streak + 1 : 0; }
    if (offset >= size) return STATUS_END_OF_FILE;
    const uint64_t want = std::min<uint64_t>(length, size - offset), last = offset + want - 1, block = options_.blockBytes;
    // Bound retained futures/data even if an application requests a very large
    // read. At most one relay window is dispatched ahead of the copied bytes.
    uint64_t done = 0, next = offset / block; bool eof = false;
    while (next <= last / block && !eof) {
      const uint64_t first = next;
      std::vector<std::shared_future<Result>> parts;
      while (next <= last / block && parts.size() < maxInFlight) parts.push_back(blockFuture(file->path, next++, size, modified));
      for (size_t i = 0; i < parts.size(); ++i) {
        const Result& part = wait(parts[i]);
        if (!part.ok()) { dropBlock(file->path, first + i, size, modified); return statusFor(part.error, true); }
        const uint64_t blockStart = (first + i) * block, from = std::max<uint64_t>(offset + done, blockStart) - blockStart;
        if (from >= part.data.size()) { eof = true; break; }
        const uint64_t take = std::min<uint64_t>(part.data.size() - from, want - done);
        memcpy(static_cast<uint8_t*>(buffer) + done, part.data.data() + from, size_t(take)); done += take;
        if (part.data.size() < block) { eof = true; break; }
      }
    }
    // Read-ahead only after a sequential pattern; random reads never pay for it. It grows with the run of sequential reads
    // (2, 4, then prefetchBlocks blocks ahead), so a short sequential burst costs little and a copy keeps the relay's window full.
    unsigned streak; { std::lock_guard lock(file->mutex); streak = file->streak; }
    const size_t ahead = std::min<size_t>(options_.prefetchBlocks, size_t(1) << std::min(streak, 3u));
    if (!eof && streak >= 1) for (size_t i = 1; i <= ahead; ++i) { const uint64_t b = last / block + i; if (b * block < size) (void)blockFuture(file->path, b, size, modified); }
    { std::lock_guard lock(file->mutex); file->lastEnd = offset + done; }
    *transferred = ULONG(done);
    return done ? STATUS_SUCCESS : STATUS_END_OF_FILE;
  }
  NTSTATUS write(File* file, PVOID buffer, UINT64 offset, ULONG length, BOOLEAN toEnd, BOOLEAN constrained, PULONG transferred, FSP_FSCTL_FILE_INFO* info) {
    std::shared_lock namespaceLock(namespaceMutex_);
    *transferred = 0;
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    if (file->invalid) return STATUS_FILE_DELETED;
    if (readOnly_) return STATUS_MEDIA_WRITE_PROTECTED;
    if (file->dir) return STATUS_FILE_IS_A_DIRECTORY;
    // Serialize successful local writes/size changes, including fresh EOF lookup
    // through observed replies (a timeout does not prove backend cancellation).
    // The eight chunks within a write still run together;
    // unrelated files and all reads do not acquire this gate.
    std::unique_lock mutationLock(file->mutationGate->mutex, std::defer_lock);
    if (!mutationLock.try_lock_for(options_.callTimeout)) return STATUS_IO_TIMEOUT;
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    if (readOnly_) return STATUS_MEDIA_WRITE_PROTECTED;
    if (toEnd || constrained) { const NTSTATUS status = refreshFile(file, true); if (!NT_SUCCESS(status)) return status; }
    uint64_t size; { std::lock_guard lock(file->mutex); size = file->info.FileSize; }
    if (toEnd) offset = size;
    if (constrained) { if (offset >= size) { std::lock_guard lock(file->mutex); *info = file->info; return STATUS_SUCCESS; } length = ULONG(std::min<uint64_t>(length, size - offset)); }
    if (offset > wire::maxFileSize || uint64_t(length) > wire::maxFileSize - offset) return STATUS_INVALID_PARAMETER;
    if (!length) { std::lock_guard lock(file->mutex); *info = file->info; return STATUS_SUCCESS; }
    // Chunks go out together (the relay carries 8), answers are checked in order.
    std::vector<std::pair<uint64_t, std::future<Result>>> sent; uint64_t written = 0; NTSTATUS failure = STATUS_SUCCESS;
    const auto settle = [&](size_t until) {
      for (size_t i = 0; i < until && i < sent.size(); ++i) {
        if (!sent[i].second.valid()) continue;
        const Result r = wait(sent[i].second);
        if (!r.ok()) { if (NT_SUCCESS(failure)) failure = statusFor(r.error, true); } else if (NT_SUCCESS(failure)) written = std::max(written, sent[i].first);
      }
    };
    for (uint64_t at = 0; at < length && NT_SUCCESS(failure);) {
      const size_t n = size_t(std::min<uint64_t>(chunkBytes, length - at));
      Call c; c.op = Op::write; c.share = options_.shareId; c.path = file->path; c.offset = offset + at; c.truncate = false;
      c.data.assign(static_cast<const uint8_t*>(buffer) + at, static_cast<const uint8_t*>(buffer) + at + n);
      at += n; sent.emplace_back(at, submit(std::move(c)));
      if (sent.size() >= maxInFlight) { settle(1); sent.erase(sent.begin()); }
    }
    settle(sent.size());
    invalidatePath(file->path);
    if (!NT_SUCCESS(failure) && !written) return failure;
    *transferred = ULONG(written);
    {
      std::lock_guard lock(file->mutex);
      if (offset + written > file->info.FileSize) { file->info.FileSize = offset + written; file->info.AllocationSize = allocationFor(file->info.FileSize); }
      file->info.LastWriteTime = file->info.ChangeTime = toFileTime(nowMs()); file->dirty = true; file->fetchedSize = -1; file->metadataAt = Clock::now(); ++file->mutation;
      *info = file->info;
    }
    propagateInfo(file);
    return STATUS_SUCCESS;  // a partial write reports the bytes that reached the PC
  }
  NTSTATUS flush(File* file, FSP_FSCTL_FILE_INFO* info) {
    std::shared_lock namespaceLock(namespaceMutex_);
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    if (!file) {
      std::map<std::string, uint64_t> dirty;
      { std::lock_guard filesLock(filesMutex_); for (auto* open : files_) { std::lock_guard lock(open->mutex); if (open->dirty && !open->dir && !open->invalid) dirty[open->path] = open->mutation; } }
      for (const auto& [path, mutation] : dirty) {
        Call c; c.op = Op::flush; c.share = options_.shareId; c.path = path;
        const Result r = call(std::move(c)); if (!r.ok()) return statusFor(r.error, true);
        std::lock_guard filesLock(filesMutex_); for (auto* open : files_) { std::lock_guard lock(open->mutex); if (open->path == path && open->mutation == mutation) open->dirty = false; }
      }
      return STATUS_SUCCESS;
    }
    if (file->invalid) return STATUS_FILE_DELETED;
    if (!file->dir) {
      uint64_t mutation; { std::lock_guard lock(file->mutex); mutation = file->mutation; }
      Call c; c.op = Op::flush; c.share = options_.shareId; c.path = file->path;
      const Result r = call(std::move(c)); if (!r.ok()) return statusFor(r.error, true);
      // A later write may have finished while this flush was in flight. Its
      // dirty state belongs to a newer mutation and still needs a flush.
      std::lock_guard lock(file->mutex); if (file->mutation == mutation) file->dirty = false;
    }
    std::lock_guard lock(file->mutex); *info = file->info; return STATUS_SUCCESS;
  }
  NTSTATUS getFileInfo(File* file, FSP_FSCTL_FILE_INFO* info) {
    std::shared_lock namespaceLock(namespaceMutex_);
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    if (file->invalid) return STATUS_FILE_DELETED;
    const NTSTATUS status = refreshFile(file); if (!NT_SUCCESS(status)) return status;
    std::lock_guard lock(file->mutex); *info = file->info; return STATUS_SUCCESS;
  }
  NTSTATUS setBasicInfo(File* file, UINT32, UINT64, UINT64, UINT64 lastWrite, UINT64, FSP_FSCTL_FILE_INFO* info) {
    std::shared_lock namespaceLock(namespaceMutex_);
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    if (file->invalid) return STATUS_FILE_DELETED;
    // Attributes, creation and access times cannot be applied to the PC file (case matrix D44/D45); the last write time can.
    if (lastWrite && lastWrite != ~0ULL && !file->dir) {
      if (readOnly_) return STATUS_MEDIA_WRITE_PROTECTED;
      Call c; c.op = Op::touch; c.share = options_.shareId; c.path = file->path; c.modified = toUnixMs(lastWrite);
      const Result r = call(std::move(c)); if (!r.ok()) return statusFor(r.error, true);
      clearStats();
      std::lock_guard lock(file->mutex); file->info.LastWriteTime = file->info.ChangeTime = lastWrite;
    }
    std::lock_guard lock(file->mutex); *info = file->info; return STATUS_SUCCESS;
  }
  NTSTATUS setFileSize(File* file, UINT64 newSize, BOOLEAN allocationOnly, FSP_FSCTL_FILE_INFO* info) {
    std::shared_lock namespaceLock(namespaceMutex_);
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    if (file->invalid) return STATUS_FILE_DELETED;
    if (readOnly_) return STATUS_MEDIA_WRITE_PROTECTED;
    if (file->dir) return STATUS_FILE_IS_A_DIRECTORY;
    if (newSize > wire::maxFileSize) return STATUS_INVALID_PARAMETER;
    std::unique_lock mutationLock(file->mutationGate->mutex, std::defer_lock);
    if (!mutationLock.try_lock_for(options_.callTimeout)) return STATUS_IO_TIMEOUT;
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    if (readOnly_) return STATUS_MEDIA_WRITE_PROTECTED;
    uint64_t current; { std::lock_guard lock(file->mutex); current = file->info.FileSize; }
    // An allocation size is only a promise; it shortens the file when smaller (WinFsp contract), never extends it.
    if (allocationOnly) { if (newSize >= current) { std::lock_guard lock(file->mutex); file->info.AllocationSize = std::max(allocationFor(newSize), file->info.AllocationSize); *info = file->info; return STATUS_SUCCESS; } }
    const NTSTATUS status = setSizeLocked(file, newSize);
    if (!NT_SUCCESS(status)) return status;
    std::lock_guard lock(file->mutex); *info = file->info; return STATUS_SUCCESS;
  }
  NTSTATUS canDelete(File* file, PWSTR) {
    std::shared_lock namespaceLock(namespaceMutex_);
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    if (file->invalid) return STATUS_FILE_DELETED;
    if (readOnly_) return STATUS_MEDIA_WRITE_PROTECTED;
    if (file->path.empty()) return STATUS_ACCESS_DENIED;
    if (!file->dir) return STATUS_SUCCESS;
    Call c; c.op = Op::list; c.share = options_.shareId; c.path = file->path; c.cursor = 0;
    const Result r = call(std::move(c)); if (!r.ok()) return statusFor(r.error, true);
    return r.entries.empty() && !r.next ? STATUS_SUCCESS : STATUS_DIRECTORY_NOT_EMPTY;
  }
  NTSTATUS setDelete(File* file, PWSTR name, BOOLEAN remove) { return remove ? canDelete(file, name) : STATUS_SUCCESS; }
  NTSTATUS rename(File* file, PWSTR from, PWSTR to, BOOLEAN replace) {
    std::unique_lock namespaceLock(namespaceMutex_);
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    if (file->invalid) return STATUS_FILE_DELETED;
    if (readOnly_) return STATUS_MEDIA_WRITE_PROTECTED;
    const auto source = toDrivePath(from), target = toDrivePath(to);
    if (!source || !target || source->empty() || target->empty()) return STATUS_OBJECT_NAME_INVALID;
    Call c; c.op = Op::rename; c.share = options_.shareId; c.path = *source; c.to = *target; c.replace = replace != FALSE;
    const Result r = call(std::move(c)); clearCaches();
    if (!r.ok()) return r.error == "drive_not_found" ? STATUS_OBJECT_NAME_NOT_FOUND : statusFor(r.error);
    // Every open descriptor under a renamed directory follows the new path.
    // An overwritten destination descriptor must never rebind to the new file.
    std::lock_guard filesLock(filesMutex_);
    for (auto* open : files_) {
      std::lock_guard lock(open->mutex);
      const auto moved = remapPath(open->path, *source, *target);
      if (moved) { open->path = *moved; open->metadataAt = Clock::time_point{}; }
      else if (replace && remapPath(open->path, *target, *target)) open->invalid = true;
    }
    return STATUS_SUCCESS;
  }
  NTSTATUS getSecurity(File*, PSECURITY_DESCRIPTOR descriptor, SIZE_T* size) { return copySecurity(descriptor, size); }
  NTSTATUS setSecurity(File*, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR) { return STATUS_SUCCESS; }  // ACLs are not stored on the PC (like RDP's drives)
  NTSTATUS readDirectory(File* file, PWSTR, PWSTR marker, PVOID buffer, ULONG length, PULONG transferred) {
    std::shared_lock namespaceLock(namespaceMutex_);
    *transferred = 0;
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    if (file->invalid) return STATUS_FILE_DELETED;
    if (!file->dir) return STATUS_NOT_A_DIRECTORY;
    NTSTATUS status = STATUS_SUCCESS;
    if (api_.acquireDirectoryBuffer(&file->dirBuffer, marker == nullptr, &status)) {
      std::vector<Entry> all; uint32_t cursor = 0; NTSTATUS failure = STATUS_SUCCESS;
      if (!listed(file->path, all)) {
        const uint64_t generation = cacheGeneration();
        for (int pages = 0; pages < 200 && all.size() < 20000; ++pages) {
          Call c; c.op = Op::list; c.share = options_.shareId; c.path = file->path; c.cursor = cursor;
          const Result page = call(std::move(c));
          if (!page.ok()) { failure = statusFor(page.error, true); break; }
          all.insert(all.end(), page.entries.begin(), page.entries.end());
          if (!page.next) break;
          cursor = *page.next;
        }
        if (NT_SUCCESS(failure)) rememberListing(file->path, all, generation);
      }
      if (NT_SUCCESS(failure)) {
        Entry self; self.dir = true; self.modified = self.created = nowMs();
        if (!file->path.empty()) {
          std::vector<std::pair<std::wstring, Entry>> rows{{L".", self}, {L"..", self}};
          for (const auto& e : all) try { rows.emplace_back(wide(e.name), e); } catch (...) {}
          for (const auto& [name, e] : rows) if (!addEntry(file, name, e, &status)) break;
        } else for (const auto& e : all) { try { if (!addEntry(file, wide(e.name), e, &status)) break; } catch (...) {} }
      } else status = failure;
      api_.releaseDirectoryBuffer(&file->dirBuffer);
    }
    if (!NT_SUCCESS(status)) return status;
    api_.readDirectoryBuffer(&file->dirBuffer, marker, buffer, length, transferred);
    return STATUS_SUCCESS;
  }

  // ---- test observation ----
  std::atomic<size_t> statCalls{0};  // backend stat calls (the lookup cache's effect)
 private:
  const Api& api_; Backend& backend_; Options options_;
  std::atomic<bool> readOnly_{false}, revoked_{false};
  FSP_FILE_SYSTEM* fs_ = nullptr; bool dispatching_ = false;
  std::vector<BYTE> sd_;
  std::shared_mutex namespaceMutex_;
  std::mutex filesMutex_; std::set<File*> files_;

  File* remember(std::unique_ptr<File> file) {
    std::lock_guard lock(filesMutex_);
    for (auto* open : files_) if (!open->invalid && samePath(open->path, file->path)) { file->mutationGate = open->mutationGate; break; }
    auto* out = file.release(); files_.insert(out); return out;
  }
  static bool samePath(const std::string& a, const std::string& b) {
    const auto first = wide(a), second = wide(b);
    return first.size() == second.size() && CompareStringOrdinal(first.data(), int(first.size()), second.data(), int(second.size()), TRUE) == CSTR_EQUAL;
  }
  static std::optional<std::string> remapPath(const std::string& path, const std::string& source, const std::string& target) {
    const auto name = wide(path), from = wide(source);
    if (name.size() < from.size() || CompareStringOrdinal(name.data(), int(from.size()), from.data(), int(from.size()), TRUE) != CSTR_EQUAL) return std::nullopt;
    if (name.size() > from.size() && name[from.size()] != L'/') return std::nullopt;
    return utf8(wide(target) + name.substr(from.size()));
  }
  NTSTATUS refreshFile(File* file, bool uncached = false) {
    // An append must not reserve its EOF from the lookup cache: an older read
    // may have populated that cache while the previous write was in flight.
    const Result found = uncached ? call(statCall(file->path)) : stat(file->path);
    if (!found.ok()) return found.error == "drive_not_found" ? STATUS_FILE_DELETED : statusFor(found.error, true);
    bool changed;
    { std::lock_guard lock(file->mutex);
      changed = file->fetchedSize != int64_t(found.entry.size) || file->fetchedModified != found.entry.modified;
      const auto index = file->info.IndexNumber; fill(found.entry, file->path, file->info); file->info.IndexNumber = index;
      file->fetchedSize = int64_t(found.entry.size); file->fetchedModified = found.entry.modified; file->metadataAt = Clock::now();
    }
    if (changed) invalidatePath(file->path);
    propagateInfo(file);
    return STATUS_SUCCESS;
  }
  void propagateInfo(File* file) {
    FSP_FSCTL_FILE_INFO info; int64_t fetchedSize, fetchedModified; Clock::time_point at;
    { std::lock_guard lock(file->mutex); info = file->info; fetchedSize = file->fetchedSize; fetchedModified = file->fetchedModified; at = file->metadataAt; }
    std::lock_guard filesLock(filesMutex_);
    for (auto* other : files_) {
      if (other == file || other->invalid) continue;
      if (!samePath(file->path, other->path)) continue;
      std::lock_guard lock(other->mutex); const auto index = other->info.IndexNumber; other->info = info; other->info.IndexNumber = index;
      other->fetchedSize = fetchedSize; other->fetchedModified = fetchedModified; other->metadataAt = at;
    }
  }

  void buildSecurity() {
    // Owner (the session user) and SYSTEM / Administrators may use the volume; nobody else, even
    // with the UNC name: full control, no inheritance flags needed (the volume has no ACL storage).
    const std::wstring text = volumeSecurityText(options_.ownerSid);
    if (text.empty()) return;
    PSECURITY_DESCRIPTOR descriptor = nullptr; ULONG size = 0;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1, &descriptor, &size) && descriptor) {
      sd_.assign(static_cast<BYTE*>(descriptor), static_cast<BYTE*>(descriptor) + size); LocalFree(descriptor);
    }
  }
  NTSTATUS copySecurity(PSECURITY_DESCRIPTOR out, SIZE_T* size) {
    if (sd_.empty()) return STATUS_ACCESS_DENIED;  // fail closed: never serve a volume without an owner
    if (size) {
      if (sd_.size() > *size) { *size = sd_.size(); return STATUS_BUFFER_OVERFLOW; }
      *size = sd_.size(); if (out) memcpy(out, sd_.data(), sd_.size());
    }
    return STATUS_SUCCESS;
  }
  static FSP_FILE_SYSTEM_INTERFACE& interface_() {
    static FSP_FILE_SYSTEM_INTERFACE table = [] {
      FSP_FILE_SYSTEM_INTERFACE t{};
      t.GetVolumeInfo = [](FSP_FILE_SYSTEM* f, FSP_FSCTL_VOLUME_INFO* i) { return self(f)->getVolumeInfo(i); };
      t.GetSecurityByName = [](FSP_FILE_SYSTEM* f, PWSTR n, PUINT32 a, PSECURITY_DESCRIPTOR d, SIZE_T* s) { return self(f)->getSecurityByName(n, a, d, s); };
      t.Create = [](FSP_FILE_SYSTEM* f, PWSTR n, UINT32 o, UINT32 g, UINT32 a, PSECURITY_DESCRIPTOR d, UINT64 z, PVOID* c, FSP_FSCTL_FILE_INFO* i) { return self(f)->create(n, o, g, a, d, z, c, i); };
      t.Open = [](FSP_FILE_SYSTEM* f, PWSTR n, UINT32 o, UINT32 g, PVOID* c, FSP_FSCTL_FILE_INFO* i) { return self(f)->open(n, o, g, c, i); };
      t.Overwrite = [](FSP_FILE_SYSTEM* f, PVOID c, UINT32 a, BOOLEAN r, UINT64 z, FSP_FSCTL_FILE_INFO* i) { return self(f)->overwrite(file(c), a, r, z, i); };
      t.Cleanup = [](FSP_FILE_SYSTEM* f, PVOID c, PWSTR n, ULONG fl) { self(f)->cleanup(file(c), n, fl); };
      t.Close = [](FSP_FILE_SYSTEM* f, PVOID c) { self(f)->close(file(c)); };
      t.Read = [](FSP_FILE_SYSTEM* f, PVOID c, PVOID b, UINT64 o, ULONG l, PULONG t2) { return self(f)->read(file(c), b, o, l, t2); };
      t.Write = [](FSP_FILE_SYSTEM* f, PVOID c, PVOID b, UINT64 o, ULONG l, BOOLEAN e, BOOLEAN k, PULONG t2, FSP_FSCTL_FILE_INFO* i) { return self(f)->write(file(c), b, o, l, e, k, t2, i); };
      t.Flush = [](FSP_FILE_SYSTEM* f, PVOID c, FSP_FSCTL_FILE_INFO* i) { return self(f)->flush(file(c), i); };
      t.GetFileInfo = [](FSP_FILE_SYSTEM* f, PVOID c, FSP_FSCTL_FILE_INFO* i) { return self(f)->getFileInfo(file(c), i); };
      t.SetBasicInfo = [](FSP_FILE_SYSTEM* f, PVOID c, UINT32 a, UINT64 ct, UINT64 at, UINT64 wt, UINT64 ch, FSP_FSCTL_FILE_INFO* i) { return self(f)->setBasicInfo(file(c), a, ct, at, wt, ch, i); };
      t.SetFileSize = [](FSP_FILE_SYSTEM* f, PVOID c, UINT64 z, BOOLEAN a, FSP_FSCTL_FILE_INFO* i) { return self(f)->setFileSize(file(c), z, a, i); };
      t.CanDelete = [](FSP_FILE_SYSTEM* f, PVOID c, PWSTR n) { return self(f)->canDelete(file(c), n); };
      t.Rename = [](FSP_FILE_SYSTEM* f, PVOID c, PWSTR n, PWSTR m, BOOLEAN r) { return self(f)->rename(file(c), n, m, r); };
      t.GetSecurity = [](FSP_FILE_SYSTEM* f, PVOID c, PSECURITY_DESCRIPTOR d, SIZE_T* s) { return self(f)->getSecurity(file(c), d, s); };
      t.SetSecurity = [](FSP_FILE_SYSTEM* f, PVOID c, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR d) { return self(f)->setSecurity(file(c), si, d); };
      t.ReadDirectory = [](FSP_FILE_SYSTEM* f, PVOID c, PWSTR p, PWSTR m, PVOID b, ULONG l, PULONG t2) { return self(f)->readDirectory(file(c), p, m, b, l, t2); };
      t.SetDelete = [](FSP_FILE_SYSTEM* f, PVOID c, PWSTR n, BOOLEAN d) { return self(f)->setDelete(file(c), n, d); };
      return t;
    }();
    return table;
  }
  static ShareFs* self(FSP_FILE_SYSTEM* f) { return static_cast<ShareFs*>(f->UserContext); }
  static File* file(PVOID c) { return static_cast<File*>(c); }

  void teardown() {
    if (!fs_) return;
    FSP_FILE_SYSTEM* f = fs_; fs_ = nullptr;
    api_.removeMountPoint(f);
    if (dispatching_) { api_.stopDispatcher(f); dispatching_ = false; }
    api_.destroy(f);
  }

  // ---- backend access ----
  Call statCall(const std::string& path) const { Call c; c.op = Op::stat; c.share = options_.shareId; c.path = path; return c; }
  std::future<Result> submit(Call c) { return backend_.submit(std::move(c)); }
  template <class F> Result waitOn(F& future) {
    if (future.wait_for(options_.callTimeout) != std::future_status::ready) { Result r; r.error = "drive_timeout"; return r; }
    return future.get();
  }
  Result wait(std::future<Result>& future) { return waitOn(future); }
  const Result& wait(std::shared_future<Result>& future) {
    static const Result timeout = [] { Result r; r.error = "drive_timeout"; return r; }();
    if (future.wait_for(options_.callTimeout) != std::future_status::ready) return timeout;
    return future.get();
  }
  Result call(Call c) { auto future = submit(std::move(c)); return wait(future); }
  // Caller holds this file's mutation gate, including its allocation-only test.
  NTSTATUS setSizeLocked(File* file, uint64_t size) {
    if (revoked_) return STATUS_NETWORK_NAME_DELETED;
    if (readOnly_) return STATUS_MEDIA_WRITE_PROTECTED;
    Call c; c.op = Op::setsize; c.share = options_.shareId; c.path = file->path; c.size = size;
    const Result r = call(std::move(c)); invalidatePath(file->path);
    if (!r.ok()) return statusFor(r.error, true);
    {
      std::lock_guard lock(file->mutex);
      file->info.FileSize = size; file->info.AllocationSize = allocationFor(size); file->info.LastWriteTime = file->info.ChangeTime = toFileTime(nowMs()); file->dirty = true; file->fetchedSize = -1; file->metadataAt = Clock::now(); ++file->mutation;
    }
    propagateInfo(file);
    return STATUS_SUCCESS;
  }
  UINT32 attributesOf(const Entry& e) const {
    if (e.dir) return FILE_ATTRIBUTE_DIRECTORY;
    return FILE_ATTRIBUTE_ARCHIVE | (e.readOnly || readOnly_ ? FILE_ATTRIBUTE_READONLY : 0);
  }
  void fill(const Entry& e, const std::string& path, FSP_FSCTL_FILE_INFO& info) const {
    info = FSP_FSCTL_FILE_INFO{};
    info.FileAttributes = attributesOf(e); info.FileSize = e.dir ? 0 : e.size; info.AllocationSize = e.dir ? 0 : allocationFor(e.size);
    info.CreationTime = toFileTime(e.created); info.LastWriteTime = info.ChangeTime = info.LastAccessTime = toFileTime(e.modified);
    info.IndexNumber = indexNumber(path);
  }
  bool addEntry(File* directory, const std::wstring& name, const Entry& e, PNTSTATUS status) {
    (void)directory;
    std::vector<uint8_t> raw(sizeof(FSP_FSCTL_DIR_INFO) + name.size() * sizeof(WCHAR), 0);
    auto* info = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(raw.data());
    info->Size = UINT16(raw.size()); fill(e, std::string(), info->FileInfo);
    info->FileInfo.IndexNumber = indexNumber(utf8(name) + (e.dir ? "/" : ""));
    memcpy(info->FileNameBuf, name.data(), name.size() * sizeof(WCHAR));
    return api_.fillDirectoryBuffer(&directory->dirBuffer, info, status) != FALSE;
  }

  // ---- lookup and listing caches (FileInfoTimeout-sized, the same bound as WinFsp's own FileInfo/DirInfo caching; every
  // mutation through this volume clears both). A listing also fills the lookups of its entries: a folder view lists, then looks
  // up every entry, one round trip each (drive_winfsp_bench: 500 files, 503 requests). An answer to a request that
  // was sent before a mutation cleared the caches is used but never cached (`generation`): it may predate that mutation.
  struct Looked { Result result; Clock::time_point at; };
  struct Listing { std::vector<Entry> entries; Clock::time_point at; };
  std::mutex statMutex_; std::map<std::string, Looked> stats_; std::map<std::string, Listing> listings_; size_t listedEntries_ = 0; uint64_t generation_ = 0;
  static constexpr size_t maxStats = 4096, maxListedEntries = 20000;
  uint64_t cacheGeneration() { std::lock_guard lock(statMutex_); return generation_; }
  Result stat(const std::string& path) {
    const auto ttl = std::chrono::milliseconds(options_.fileInfoTimeoutMs); uint64_t generation;
    { std::lock_guard lock(statMutex_); const auto found = stats_.find(path); if (found != stats_.end() && Clock::now() - found->second.at < ttl) return found->second.result; generation = generation_; }
    ++statCalls;
    Result fresh = call(statCall(path));
    // Failures other than "not there" are never cached: the next try asks again.
    if (fresh.ok() || fresh.error == "drive_not_found") { std::lock_guard lock(statMutex_); if (generation == generation_) { if (stats_.size() > maxStats) stats_.clear(); stats_[path] = {fresh, Clock::now()}; } }
    return fresh;
  }
  bool listed(const std::string& path, std::vector<Entry>& out) {
    std::lock_guard lock(statMutex_); const auto found = listings_.find(path);
    if (found == listings_.end() || Clock::now() - found->second.at >= std::chrono::milliseconds(options_.fileInfoTimeoutMs)) return false;
    out = found->second.entries; return true;
  }
  void rememberListing(const std::string& path, const std::vector<Entry>& entries, uint64_t generation) {
    std::lock_guard lock(statMutex_); if (generation != generation_) return;
    const auto now = Clock::now();
    if (listedEntries_ + entries.size() > maxListedEntries) { listings_.clear(); listedEntries_ = 0; }
    if (entries.size() <= maxListedEntries) { if (const auto old = listings_.find(path); old != listings_.end()) listedEntries_ -= old->second.entries.size(); listings_[path] = {entries, now}; listedEntries_ += entries.size(); }
    if (entries.size() > maxStats) return;
    if (stats_.size() + entries.size() > maxStats) stats_.clear();
    for (const auto& e : entries) { Result r; r.entry = e; stats_[path.empty() ? e.name : path + "/" + e.name] = {std::move(r), now}; }
  }
  void clearStats() { std::lock_guard lock(statMutex_); stats_.clear(); listings_.clear(); listedEntries_ = 0; ++generation_; }

  // ---- read cache: aligned blocks, shared between callers, keyed by file identity ----
  struct BlockKey { std::string path; uint64_t index, size; int64_t modified; bool operator<(const BlockKey& o) const { return std::tie(path, index, size, modified) < std::tie(o.path, o.index, o.size, o.modified); } bool operator==(const BlockKey& o) const { return std::tie(path, index, size, modified) == std::tie(o.path, o.index, o.size, o.modified); } };
  std::mutex cacheMutex_; std::map<BlockKey, std::shared_future<Result>> blocks_; std::list<BlockKey> order_;
  std::shared_future<Result> blockFuture(const std::string& path, uint64_t index, uint64_t size, int64_t modified) {
    const BlockKey key{path, index, size, modified};
    std::lock_guard lock(cacheMutex_);
    const auto found = blocks_.find(key);
    if (found != blocks_.end()) { order_.remove(key); order_.push_back(key); return found->second; }
    Call c; c.op = Op::read; c.share = options_.shareId; c.path = path; c.offset = index * options_.blockBytes;
    c.length = uint32_t(std::min<uint64_t>(options_.blockBytes, size - c.offset));
    std::shared_future<Result> future = submit(std::move(c)).share();
    blocks_[key] = future; order_.push_back(key);
    while (blocks_.size() > options_.cacheBlocks && !order_.empty()) { blocks_.erase(order_.front()); order_.pop_front(); }
    return future;
  }
  void dropBlock(const std::string& path, uint64_t index, uint64_t size, int64_t modified) {
    std::lock_guard lock(cacheMutex_); const BlockKey key{path, index, size, modified}; blocks_.erase(key); order_.remove(key);
  }
  void invalidatePath(const std::string& path) {
    clearStats();
    std::lock_guard lock(cacheMutex_);
    for (auto it = blocks_.begin(); it != blocks_.end();) if (samePath(it->first.path, path)) { order_.remove(it->first); it = blocks_.erase(it); } else ++it;
  }
  void clearCaches() { clearStats(); std::lock_guard lock(cacheMutex_); blocks_.clear(); order_.clear(); }
};
}
