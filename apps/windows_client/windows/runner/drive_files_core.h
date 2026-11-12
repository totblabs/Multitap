#pragma once
// drive-v1 (docs/drive-redirection.md): the PC side of streaming drive
// redirection. Pure Win32, no Flutter types, so it is tested natively
// (apps/windows_host MultitapClientDriveFilesTest).
//
// Every path the VM names is relative to a share root the user picked. The
// root is resolved to its final path once; every operation re-validates the
// relative path (plain Windows file-name segments only), refuses any existing
// component below the root that is a link (a name-surrogate reparse point:
// symbolic link, junction, mount point) and checks that the object it opened
// still lies under the root's final path before changing anything. Cloud
// placeholders (OneDrive) are not name surrogates and work as files.
// The caller decides whose token the calling thread runs with (the channel
// adapter impersonates the desktop user; see drive_channel.h).
#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace multitap::drive {

// A failed operation; what() is one fixed drive_* code.
class Failure : public std::runtime_error {
 public:
  explicit Failure(const char* code, DWORD win32 = 0) : std::runtime_error(code), win32_(win32) {}
  // The Win32 error behind the code (0: none), for diagnostics only.
  DWORD win32() const { return win32_; }

 private:
  DWORD win32_;
};

struct Entry {
  std::wstring name;
  bool dir = false;
  uint64_t size = 0;
  int64_t modified = 0, created = 0;  // ms since the Unix epoch, >= 0
  bool readOnly = false;
  bool hasSpace = false;  // the share root only
  uint64_t free = 0, total = 0;
};

constexpr size_t kMaxNameUnits = 255, kMaxPathUnits = 1024, kMaxDepth = 64;
constexpr size_t kMaxListEntries = 20000;
constexpr uint32_t kMaxChunk = 262144;

// One plain Windows file name (the rule of server/src/drive.mjs safeDriveName).
bool SafeName(const std::wstring& name, size_t max = kMaxNameUnits);
// A name Explorer reads as instructions rather than as a document: folder
// customisation (desktop.ini), shortcuts and shell commands (*.lnk, *.url,
// *.scf), libraries and search connectors, autorun.inf. The remote desktop may
// not create, overwrite or rename anything to such a name in a shared folder
// (drive_denied): opening the folder on the PC would otherwise act on them.
bool ShellMetadataName(const std::wstring& name);
// '' or 1..64 safe segments joined with '/', <= 1024 units; throws
// Failure("drive_invalid_path").
std::vector<std::wstring> Segments(const std::wstring& path);
// The fixed code of a Win32 error.
const char* CodeFor(DWORD error);
std::wstring Utf8ToWide(const std::string& text);  // throws on invalid UTF-8
std::string WideToUtf8(const std::wstring& text);

class Files {
 public:
  // Optional fixture interleaving after rename handles have been checked.
  // The production channel uses the default (no callback).
  explicit Files(std::function<void()> beforeRename = {}) : beforeRename_(std::move(beforeRename)) {}
  // Registers (or replaces) a share: an existing, accessible directory.
  // Returns a default label for it (its leaf name, or the drive letter).
  std::wstring Open(const std::string& share, const std::wstring& path, bool readOnly);
  void Close(const std::string& share);
  bool Has(const std::string& share) const;

  // Where the shared folder stands now. The share follows the folder itself
  // (a handle opened at Open with share-all, whose file ID is checked), not the
  // path: a rename or move on the PC changes `path`/`label`; a deleted (or
  // recycled) folder is `deleted` and every later operation fails
  // drive_not_found. A recreated folder of the same name is never picked up.
  struct Status {
    bool deleted = false;
    // The folder's device is gone (a pulled USB stick, a network share that
    // dropped): its handle answered "device not connected" and the like. Every
    // operation fails drive_not_found meanwhile; the share keeps its drive
    // letter in the VM, and re-opening it (Open again) is what brings it back.
    bool offline = false;
    std::wstring path;   // the folder's current path (no \\?\ prefix)
    std::wstring label;  // its current leaf name, or the drive letter
  };
  Status StatusOf(const std::string& share);

  Entry Stat(const std::string& share, const std::wstring& path);
  std::vector<Entry> List(const std::string& share, const std::wstring& path, size_t max = kMaxListEntries);
  std::vector<uint8_t> Read(const std::string& share, const std::wstring& path, uint64_t offset, uint32_t length);
  // Write and Create (of a file) give the file a Mark-of-the-Web (an Internet
  // zone Zone.Identifier stream) before any content lands, so SmartScreen and
  // Office Protected View treat what the remote desktop wrote as downloaded.
  // A volume without named streams (FAT, exFAT) has no place for the mark.
  void Write(const std::string& share, const std::wstring& path, uint64_t offset, const uint8_t* data, size_t size, bool truncate);
  // Sets a file's length exactly (shrink truncates, grow zero-fills).
  void SetSize(const std::string& share, const std::wstring& path, uint64_t size);
  // FlushFileBuffers on a file; the root, a directory and a read-only share
  // are a successful no-op.
  void Flush(const std::string& share, const std::wstring& path);
  void Create(const std::string& share, const std::wstring& path, bool dir, bool truncate);
  void Rename(const std::string& share, const std::wstring& path, const std::wstring& to, bool replace);
  void Delete(const std::string& share, const std::wstring& path);
  void Touch(const std::string& share, const std::wstring& path, int64_t modified);

 private:
  struct Root {
    std::wstring final;   // GetFinalPathNameByHandle of the root, e.g. \\?\C:\Data or \\?\C:\ .
    std::wstring prefix;  // final with exactly one trailing backslash
    bool readOnly = false;
    bool deleted = false;
    bool offline = false;
    DWORD offlineError = 0;
    std::shared_ptr<void> handle;  // the root directory, open for the share's life
    FILE_ID_INFO id{};             // its identity (volume serial + file ID)
  };
  // Asks the root's handle where the folder is now. Called WITHOUT mutex_
  // held: a hung network share must stall only its own share's calls, never
  // the other shares' (the handle queries can take a network timeout).
  static void Refresh(Root& root);
  // The root as refreshed just now (a copy); stores the result back.
  Root Refreshed(const std::string& share);
  Root RootOf(const std::string& share);
  mutable std::mutex mutex_;
  std::map<std::string, Root> roots_;
  std::function<void()> beforeRename_;
};

}  // namespace multitap::drive
