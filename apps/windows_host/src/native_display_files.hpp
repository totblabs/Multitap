#pragma once
// The boot service loop's two native display files (backlog #70, idle cost).
// Reading the provider marker twice in each one-second pass, checking its
// owner and ACL each time, and rewriting and flushing native-display-state.json
// although only its liveness tick has moved costs about ten of the loop's
// eleven context switches a pass (measured).
#include "support.hpp"
#include "native_display_contract.hpp"
#include <mutex>

namespace host {
// An unchanged snapshot is rewritten once it is this old: on every second loop
// pass (passes are ~1.01 s apart; 1.9 s allows for a timer that fires a tick
// early). Readers accept a tick up to nativeDisplaySnapshotMaxAgeMs old, so ~3 s
// remain for a pass held up, e.g. while a session change waits for the stream
// worker's stop, whose display restore reads the snapshot (it was ~4 s with a
// write every pass). An unflushed rewrite costs ~0.6 wakeups and ~0.8 ms CPU,
// a flushed one ~9.4 and ~1.3 ms (measured).
inline constexpr uint64_t nativeDisplaySnapshotRefreshMs = 1900;
static_assert(nativeDisplaySnapshotRefreshMs + 1000 + 2000 <= nativeDisplaySnapshotMaxAgeMs, "A refreshed snapshot must stay fresh across a pass held up for two seconds");

enum class NativeSnapshotWrite { none, refresh, change };
// Decides whether a snapshot is written. A change of anything but the tick is
// written at once and flushed to the disk, as every pass used to be. The same
// content is only rewritten for its tick, without a flush: a reader proves the
// service PID anyway, so a refresh lost with the machine says nothing wrong.
class NativeSnapshotPublication {
 public:
  NativeSnapshotWrite plan(const std::string& bytes, uint64_t now) const {
    if (!published_ || content(bytes) != content_) return NativeSnapshotWrite::change;
    return now < at_ || now - at_ >= nativeDisplaySnapshotRefreshMs ? NativeSnapshotWrite::refresh : NativeSnapshotWrite::none;
  }
  void written(const std::string& bytes, uint64_t now) { content_ = content(bytes); at_ = now; published_ = true; }
  // Unknown on disk: the next snapshot is written (and flushed) whatever it holds.
  void failed() { published_ = false; }
 private:
  static std::string content(std::string bytes) {
    if (const auto at = bytes.find(",\"tick\":"); at != std::string::npos) bytes.erase(at, bytes.find(',', at + 1) - at);
    return bytes;
  }
  std::string content_; uint64_t at_ = 0; bool published_ = false;
};
// Atomic replace through a new temporary file. `durable` flushes the data and
// writes the rename through.
inline bool writeNativeDisplaySnapshot(const std::filesystem::path& path, const std::string& bytes, bool durable) {
  const auto temporary = path.wstring() + temporarySuffix();
  HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr); DWORD wrote = 0;
  const bool saved = output != INVALID_HANDLE_VALUE && WriteFile(output, bytes.data(), DWORD(bytes.size()), &wrote, nullptr) && wrote == bytes.size() && (!durable || FlushFileBuffers(output));
  if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
  // Readers (the worker's lease reads, capability probes) open the file
  // briefly without delete sharing; the replace then fails with a sharing
  // violation for that instant. A v2 SDK size change used to be reported
  // as refused after it had already happened. Retry the replace briefly.
  const auto replaced = [&] {
    for (int attempt = 0;; ++attempt) {
      if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0))) return true;
      const auto error = GetLastError();
      if (attempt >= 40 || (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED && error != ERROR_LOCK_VIOLATION)) return false;
      Sleep(5);
    }
  };
  if (!saved || !replaced()) { DeleteFileW(temporary.c_str()); return false; }
  return true;
}

// Identifies one version of a file: the same file (volume and file ID; a
// replaced file has a new ID), size, last write and change time. NTFS moves the
// change time with the owner, ACL and attributes too.
struct MachineFileStamp {
  uint64_t volume = 0; std::array<uint8_t, 16> id{}; int64_t written = 0, changed = 0; uint64_t size = 0; DWORD attributes = 0;
  bool operator==(const MachineFileStamp&) const = default;
};
inline std::optional<MachineFileStamp> machineFileStamp(const std::filesystem::path& path) {
  const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (file == INVALID_HANDLE_VALUE) return std::nullopt;
  FILE_ID_INFO id{}; FILE_BASIC_INFO basic{}; FILE_STANDARD_INFO standard{};
  const bool known = GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) && GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) && GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard));
  CloseHandle(file);
  if (!known) return std::nullopt;
  MachineFileStamp stamp; stamp.volume = id.VolumeSerialNumber; std::copy(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), stamp.id.begin());
  stamp.written = basic.LastWriteTime.QuadPart; stamp.changed = basic.ChangeTime.QuadPart; stamp.size = uint64_t(standard.EndOfFile.QuadPart); stamp.attributes = basic.FileAttributes;
  return stamp;
}
// Keeps the value of a protected file's full trusted read (`read` checks the
// owner and ACL and throws, as without the cache) while the file keeps its
// stamp, and reads again when it changed. Only what passed that check is ever
// returned; a read that failed, or raced a change, is not kept.
template<class T> class TrustedFileCache {
 public:
  template<class Read> T read(const std::filesystem::path& path, Read&& read) {
    const auto before = machineFileStamp(path);
    if (before) { std::lock_guard lock(mutex_); if (stamp_ == before) return value_; }
    T value = read();
    const auto after = machineFileStamp(path);
    std::lock_guard lock(mutex_);
    if (before && before == after) { stamp_ = before; value_ = value; } else stamp_.reset();
    return value;
  }
 private:
  std::mutex mutex_; std::optional<MachineFileStamp> stamp_; T value_{};
};
}
