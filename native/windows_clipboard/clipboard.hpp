#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace multitap::clipboard {
// Hard ceilings of this library (the "clipboard limits v2" profile). Released
// peers advertise the smaller v1 profile (32768 text units, 128 KiB of UTF-8,
// 256 MiB per file, 512 MiB per transfer); callers enforce the profile the
// relay negotiated for the pair and use these only as the absolute bound.
//
// The "scale" revision (clipboard.ready limits with scaleProfile) removes the product limits that came from eager staging:
// files and totals are bounded only by what a 64-bit size and a JSON number can carry, text and rich text by 64 MiB, the
// entry list by a memory guard (maxEntries) and relative paths by 1024 characters (maxPathChars). The constants below are
// the hard ceilings of this library (the scale profile); a caller enforces the profile its relay negotiated through
// `Bounds` (Bounds{} is the v2 profile of released peers, Bounds::scale() the open one).
inline constexpr size_t maxTextUnits = 64 * 1024 * 1024;
inline constexpr size_t maxTextBytes = 64 * 1024 * 1024;
inline constexpr size_t maxFiles = 128;
inline constexpr size_t maxChunkBytes = 65536;
inline constexpr uint64_t maxFileBytes = 4ull * 1024 * 1024 * 1024;
inline constexpr uint64_t maxTotalBytes = 8ull * 1024 * 1024 * 1024;
// Sizes and offsets travel as JSON numbers: exact up to 2^53 - 1. The scale profile's "no limit" for files and totals.
inline constexpr uint64_t scaleBytes = (1ull << 53) - 1;
// Free space that must remain on the receiving volume after a file transfer.
inline constexpr uint64_t freeSpaceHeadroomBytes = 64ull * 1024 * 1024;
// The "formats" profile (rich text, images, folder trees, sliding window).
inline constexpr size_t maxRichBytes = 64 * 1024 * 1024;
inline constexpr uint64_t maxImageBytes = 256ull * 1024 * 1024;
inline constexpr uint64_t maxImagePixels = 64'000'000;
// Entries of one copy. A memory guard, not a product limit: the FILEGROUPDESCRIPTORW of a paste is 592 bytes per entry, so 200,000
// entries are about 118 MB while a million would be about 590 MB built in one block for Explorer.
inline constexpr size_t maxEntries = 200000;
// Nesting follows from the path length (a component and its separator are at least two characters).
inline constexpr size_t maxDepth = 512;
// Characters of a relative path inside a transfer (each component has at most maxComponentChars).
inline constexpr size_t maxPathChars = 1024;
inline constexpr size_t maxComponentChars = 255;
// FILEDESCRIPTORW::cFileName is WCHAR[MAX_PATH]: a virtual file's relative path has at most 259 characters.
inline constexpr size_t maxDescriptorChars = 259;
// A published absolute path of this many characters or more is beyond MAX_PATH (Explorer's own limit).
inline constexpr size_t maxPathLegacy = 260;
// Explorer creates a pasted item at "<destination>\<relative path>" and cannot create one of MAX_PATH characters or more (measured:
// docs/remote-session-protocol.md, "Copy-source matrix"); a relative path alone above 259 characters can therefore not be pasted into
// any folder. Such a copy is refused up front (clipboard_path_too_long) by the side that sees the path first, by the longer of the
// two limits only up to the wire bound maxPathChars: sender and receiver both enforce min(profile path limit, maxPasteChars).
inline constexpr size_t maxPasteChars = 259;

// clipboard_path_too_long with the numbers the user is told: the longest relative path found and the limit that applied.
class PathTooLongError : public std::runtime_error {
 public:
  PathTooLongError(size_t chars, size_t limit) : std::runtime_error("clipboard_path_too_long"), chars(chars), limit(limit) {}
  size_t chars, limit;
};
// Host IPC frames stay below 64 KiB, so formats chunks are 45 KiB.
inline constexpr size_t formatsChunkBytes = 46080;
// Failure reasons a caller maps to user messages (std::runtime_error::what()):
//   clipboard_text_too_large, clipboard_file_count_limit,
//   clipboard_file_name_or_size_limit, clipboard_total_size_limit,
//   clipboard_file_locked, clipboard_disk_full, clipboard_policy_denied,
//   clipboard_image_too_large, clipboard_entry_count_limit, clipboard_depth_limit,
//   clipboard_path_too_long, clipboard_image_invalid, clipboard_html_invalid,
//   clipboard_rtf_invalid

// One image as PNG bytes plus its pixel size.
struct Image {
  std::vector<uint8_t> png;
  uint32_t width = 0, height = 0;
};
// Size bounds of the negotiated pair (the relay's limits object).
// The defaults are the v2 / formats profile of released peers (the sizes they check exactly); scale() is the open profile.
struct Bounds {
  size_t tops = 128;
  uint64_t fileBytes = 4ull * 1024 * 1024 * 1024, totalBytes = 8ull * 1024 * 1024 * 1024;
  size_t textBytes = 1024 * 1024;
  size_t entries = 10000, depth = 32, pathChars = 160, richBytes = 8 * 1024 * 1024;
  static Bounds scale() {
    Bounds value;
    value.tops = maxEntries; value.fileBytes = scaleBytes; value.totalBytes = scaleBytes; value.textBytes = maxTextBytes;
    value.entries = maxEntries; value.depth = maxDepth; value.pathChars = maxPathChars; value.richBytes = maxRichBytes;
    return value;
  }
};

// The extended-length form of an absolute path for the Win32 file APIs: "\\?\C:\..." and "\\?\UNC\server\share\...", so a
// source or private-transfer path may be as long as the file system allows (32,767 units) and not only MAX_PATH. A path that
// already has a "\\?\" or "\\.\" prefix, and a relative one, is returned as it is. Every file API call of this library goes
// through it; what is published to other programs (CF_HDROP) stays the ordinary form.
inline std::wstring longPath(std::wstring path) {
  if (path.size() < 3) return path;
  if (path.compare(0, 4, L"\\\\?\\") == 0 || path.compare(0, 4, L"\\\\.\\") == 0) return path;
  for (auto& unit : path) if (unit == L'/') unit = L'\\';
  if (path[0] == L'\\' && path[1] == L'\\') return L"\\\\?\\UNC\\" + path.substr(2);
  if (((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && path[2] == L'\\') return L"\\\\?\\" + path;
  return path;
}
inline std::wstring longPath(const std::filesystem::path& path) { return longPath(path.wstring()); }

// Virtual files as a copy source (Explorer items inside a .zip folder, Outlook / Thunderbird attachments, phone and cloud shell
// namespaces): an OLE data object with FileGroupDescriptorW + FileContents and no CF_HDROP. They are materialised into a new
// private "source-<id>" folder under `parent` (push model), so the file / folder-tree transfer and the paste-pull offer work on them
// unchanged. Every descriptor name goes through the tree path rules (safeLeaf per component, depth, length, entry count) and the
// sizes, declared or not, through `bounds`.
struct VirtualStageOptions {
  std::filesystem::path parent;  // the caller's private transfer parent (created and secured when missing)
  Bounds bounds;
  // The longest wait with no progress (no byte, no new item) on one source call: a delayed-rendering source (Outlook downloading an
  // attachment) may take a while to start, a hung one must not hang the worker. Reported as clipboard_busy.
  std::chrono::milliseconds idle{60000};
  // Called on the waiting thread about every 100 ms while progress changes: bytes staged so far, items done, items in all.
  std::function<void(uint64_t bytes, size_t done, size_t total)> progress;
};
// The materialised copy. Destroying the last reference deletes the folder, so whoever still reads the files (an offer, a send)
// keeps it alive through Snapshot::staged.
struct StagedSource {
  std::filesystem::path root;
  std::vector<std::filesystem::path> tops;  // top-level entries (files and folders), in first-seen order
  uint64_t bytes = 0; size_t files = 0, folders = 0;
  StagedSource() = default;
  StagedSource(const StagedSource&) = delete;
  ~StagedSource();
  HANDLE directory = INVALID_HANDLE_VALUE;  // held without delete sharing while the copy is staged
  HANDLE parentFolder = INVALID_HANDLE_VALUE;  // its parent, through which the folder is removed (never by path)
};
// Reads the virtual files of `source` (nullptr: the OLE clipboard) on a dedicated STA thread with its own OLE initialisation and
// returns the staged copy; nullptr when the object has no FileGroupDescriptorW. The source object must stay valid for the call
// (it is called from that thread: a test object has to be thread-safe). Throws the usual clipboard_* reasons;
// unsupported_clipboard_format when an item has no FileContents, clipboard_busy when a source stops answering for `idle`,
// clipboard_selected_file_unavailable when the source fails or has gone, clipboard_file_name_or_size_limit /
// clipboard_total_size_limit / clipboard_disk_full for the limits.
std::shared_ptr<StagedSource> stageVirtualFiles(IDataObject* source, const VirtualStageOptions& options);

// What the caller wants read: plain text, file lists, and (formats profile) the
// HTML / RTF that accompany text and a bitmap (shared as PNG; files permission).
struct ReadOptions {
  bool text = true, files = true, rich = false, images = false;
  // A copy that the permissions keep out reads as clipboard_policy_denied instead of an empty clipboard (set by the callers that answer a user's copy/paste; internal peeks leave it off).
  bool reportDenied = false;
  // The largest HTML companion of the text that is kept (a larger one is left out and only the text is read): 8 MiB for released
  // pairs, up to maxRichBytes for a pair of the scale revision.
  size_t richBytes = 8 * 1024 * 1024;
  // Set: a copy of virtual files only (no CF_HDROP, no usable text or image) is staged and returned as files; unset: it is refused
  // as unsupported_clipboard_format (released callers, pairs without the formats profile).
  std::optional<VirtualStageOptions> virtualFiles;
  // Negotiated settled text/image read, bound to the actual observed source.
  std::optional<DWORD> expectedSequence;
  bool textImageOnly = false;
};

// Content operations are explicit and policy filtered. The separate change_watch
// listener emits sequence metadata only; no remote paths or content logging.
struct Snapshot {
  DWORD sequence = 0;
  std::optional<std::wstring> text;
  // The sanitised "HTML Format" payload that accompanied the text (only with ReadOptions::rich); one that does not
  // validate, or an oversized one, is left out. rtf is no longer read or sent (kept only so released callers compile).
  std::optional<std::vector<uint8_t>> html, rtf;
  std::optional<Image> image;
  std::vector<std::filesystem::path> files;
  // GetClipboardOwner() while the snapshot was held open: the window whose
  // EmptyClipboard() last replaced the content (metadata, not content).
  HWND owner = nullptr;
  // Virtual files staged for `files`: whoever still reads those files (a send, a pull offer) must keep this alive.
  std::shared_ptr<StagedSource> staged;
};
Snapshot read(HWND owner);
Snapshot read(HWND owner, bool allowText, bool allowFiles);
Snapshot read(HWND owner, const ReadOptions& options);
// The current clipboard content asks not to be read by clipboard monitors: password managers mark a copied password with
// "ExcludeClipboardContentFromMonitorProcessing" (or the older "Clipboard Viewer Ignore"). Automatic sharing skips such a copy
// as clipboard_policy_denied, a code every peer and broker treats as quiet; a transfer the user starts still sends it. Needs no open clipboard.
bool sensitiveContentIsCurrent();
// Which kinds of content are on the clipboard now, from IsClipboardFormatAvailable alone (nothing is opened or read, so a
// delayed-rendering owner is never asked for data): 1 CF_HDROP, 2 "Shell IDList Array", 4 FileGroupDescriptorW (virtual
// files), 8 text, 16 image, 32 HTML. For diagnostics: it says what a copy in the VM actually put there.
uint32_t availableFormats();
// True when a bitmap on the clipboard should be sent instead of the listed files: the bitmap is offered AND there are
// one to four files that are all plain image files (a screenshot tool's auto-saved copy). Folders, other files, or
// more than four files keep the file transfer.
bool preferImageOverFiles(const std::vector<std::filesystem::path>& files, bool imageAvailable);
// File system paths of a "Shell IDList Array" (the shell's own file list: an Explorer copy always carries one; some sources
// publish it without CF_HDROP). Empty when the data is not a valid list or any item has no file system path. Declared for tests.
std::vector<std::filesystem::path> pathsFromIdList(const uint8_t* data, size_t size);
// Publishes text with its sanitised HTML (plain text is always present). The rtf argument is ignored: RTF is never published.
DWORD putRich(HWND owner, const std::wstring& text, const std::vector<uint8_t>* html,
              const std::vector<uint8_t>* rtf, std::optional<DWORD> expectedSequence);
// Publishes one image as "PNG" + CF_DIBV5 + CF_DIB so any paste target finds a format it reads.
DWORD putImage(HWND owner, const Image& image, std::optional<DWORD> expectedSequence);
// Like putFiles, but the listed paths may also be directories (a received folder tree's top level).
DWORD putPaths(HWND owner, const std::vector<std::filesystem::path>& topLevel, std::optional<DWORD> expectedSequence);
// Header offsets of CF_HTML ("Version:", StartHTML, EndHTML, StartFragment, EndFragment) are checked
// against the payload; throws clipboard_html_invalid. Valid UTF-8 and no NUL are required.
void validateHtmlFormat(const std::vector<uint8_t>& payload);
void validateRtf(const std::vector<uint8_t>& payload);
// The HTML of a peer is never published as received. Returns a new CF_HTML payload (header written here, SourceURL and
// everything outside the fragment dropped) whose fragment was re-serialised by a conservative tokenizer: allow-listed
// elements and attributes only; no script/style/object/embed/frames/forms/svg/math/link/meta/base, no comments or
// conditional comments, no event handlers; URLs only http, https, mailto (href) or data:image/png|jpeg|gif|webp (img src),
// judged after entity decoding and whitespace/control stripping; style attributes that could load or run anything are
// removed. Throws clipboard_html_invalid when the input does not validate.
std::vector<uint8_t> sanitizeHtmlFormat(const std::vector<uint8_t>& payload);
// The tokenizer behind it, on a bare fragment (UTF-8). Idempotent.
std::string sanitizeHtmlFragment(const std::string& fragment);
// False when the bytes do not start with a PNG signature and IHDR; throws clipboard_image_too_large when
// the pixel count or byte size is beyond the bounds.
bool readPngHeader(const std::vector<uint8_t>& png, uint32_t& width, uint32_t& height);
// PNG signature and IHDR size (throws clipboard_image_invalid / clipboard_image_too_large).
Image describePng(std::vector<uint8_t> png);
// A packed DIB (BITMAPINFOHEADER / V4 / V5 followed by pixels) as PNG, via WIC. COM must be initialised on the thread.
Image encodePngFromDib(const uint8_t* dib, size_t size);
// A PNG as top-down 32-bit BGRA (straight alpha); validates the size against the expected one.
std::vector<uint8_t> decodePngToBgra(const std::vector<uint8_t>& png, uint32_t expectedWidth, uint32_t expectedHeight);
DWORD putText(HWND owner, const std::wstring& text);
DWORD putFiles(HWND owner, const std::vector<std::filesystem::path>& materializedFiles);
DWORD putText(HWND owner, const std::wstring& text, std::optional<DWORD> expectedSequence);
DWORD putFiles(HWND owner, const std::vector<std::filesystem::path>& materializedFiles, std::optional<DWORD> expectedSequence);
// Why the last write (or open) of this thread's clipboard access failed, for diagnostics (never a path, a name or content):
// bits 0-15 the Win32 error (the low 16 bits of an HRESULT), bits 16-17 who held the clipboard when it could not be opened
// (0 nobody / unknown, 1 another process, 2 this very thread (a leak of ours), 3 another thread of this process), bit 20 set
// when the write was retried after releasing what this process itself held (its own open clipboard, its own OLE data object).
// 0 when nothing failed since clearWriteFailure().
uint32_t writeFailureDetail();
void clearWriteFailure();
// Releases what this process itself holds on the clipboard that could be refusing a write: the clipboard left open by this
// thread and an OLE data object this process published (OleSetClipboard(nullptr)). expectedSequence follows the sequence when
// the release changed it and nothing else had. Returns whether anything was released. Called on the clipboard thread.
bool releaseOwnClipboardHolds(std::optional<DWORD>& expectedSequence);
// Called only while the clipboard is held open, before EmptyClipboard/SetData.
void requireUnchangedSequence(std::optional<DWORD> expectedSequence, DWORD currentSequence);
bool safeLeaf(const std::wstring& name);
// This process runs above medium integrity (an administrator's elevated Host
// worker): what it creates for a transfer is labelled High (no write up).
bool processAboveMedium() noexcept;
// Removes the entry [name] of the folder [parent] and everything below it, each
// item opened relative to its folder: a link (anywhere) is removed as the link
// and never entered. True only when it is gone.
bool removeWithoutFollowing(const std::filesystem::path& parent, const std::wstring& name) noexcept;
// Expire only previously committed paths returned by this library, under the
// caller's fixed private parent. Uses no recursive deletion and follows no links.
bool expireFiles(const std::filesystem::path& privateTransferParent,
                 const std::vector<std::filesystem::path>& committedFiles);

struct FileDescription { std::wstring name; uint64_t size = 0; };
class FileSource {
 public:
  explicit FileSource(const std::filesystem::path& clipboardSelectedPath);
  ~FileSource();
  FileSource(FileSource&&) noexcept;
  FileSource& operator=(FileSource&&) noexcept;
  FileSource(const FileSource&) = delete;
  const FileDescription& description() const;
  std::vector<uint8_t> next(size_t requested = maxChunkBytes);
  uint64_t position() const;
  std::string digest() const;
 private:
  struct State; std::unique_ptr<State> state_;
};
// True only when every listed file can be read now and has exactly the
// expected leaf name, size and SHA-256 (lowercase hex), in order. Used before
// keeping a clipboard whose files a peer sent back unchanged.
bool filesHoldContent(const std::vector<std::filesystem::path>& files,
                      const std::vector<FileDescription>& expected,
                      const std::vector<std::string>& sha256);

// The receiver creates one new, private transfer directory. It never accepts
// a sender-supplied directory/absolute path and never overwrites existing files.
// Partial files are removed on failure/cancel. Completed files must remain until
// the caller deliberately expires them, as CF_HDROP refers to existing paths.
class FileReceiver {
 public:
  FileReceiver(const std::filesystem::path& privateTransferParent,
               const std::vector<FileDescription>& descriptions);
  ~FileReceiver();
  FileReceiver(const FileReceiver&) = delete;
  void append(size_t index, uint64_t offset, const std::vector<uint8_t>& bytes);
  void finish(size_t index, const std::string& expectedSha256);
  std::vector<std::filesystem::path> commit();
  void cancel();
  uint64_t received() const;
 private:
  struct State; std::unique_ptr<State> state_;
};

// ---- The formats profile: rich text, an image or a folder tree --------------------------------

class Storage;
// One tree entry; relative paths use '\\' here and '/' on the wire.
struct TreeEntry {
  std::wstring relative;
  std::filesystem::path source;
  bool directory = false;
  uint64_t size = 0;
  DWORD attributes = 0;
};
struct TreeScan {
  std::vector<TreeEntry> entries;  // parents before children
  size_t tops = 0, files = 0, skipped = 0, longestPath = 0;  // longestPath: characters of the longest relative path
  uint64_t bytes = 0;
};
// Walks the CF_HDROP paths. Symbolic links, junctions and any other reparse point are neither followed
// nor included (counted in `skipped`). Folders are kept, empty ones too. Throws clipboard_depth_limit,
// clipboard_entry_count_limit, clipboard_path_too_long (a PathTooLongError naming the longest relative path, found after the whole walk), clipboard_file_name_or_size_limit,
// clipboard_total_size_limit, clipboard_file_count_limit or clipboard_selected_file_unavailable.
// A top-level name that repeats (same leaf from different folders) gets " (2)", " (3)"... appended.
TreeScan scanTree(const std::vector<std::filesystem::path>& roots, const Bounds& bounds);
// A relative tree path from the wire ('/'-separated) as a native one; throws unless every component
// passes safeLeaf, with no empty, "." or ".." component, within the depth and length limits (the library ceilings by default;
// clipboard_path_too_long, clipboard_depth_limit).
std::wstring treePathFromWire(const std::wstring& wire, size_t pathChars = maxPathChars, size_t depth = maxDepth);

// ---- Receiver placement of a received folder tree --------------------------------------------------
// A tree received by push (small content) is kept in a private transfer folder. When every entry's absolute path is short enough
// (below maxPathLegacy characters) its top level goes on the clipboard as CF_HDROP, as before. When some absolute path would be
// maxPathLegacy characters or more, not every Windows version's Explorer copies it from CF_HDROP, so the same, already received, content is published
// as a virtual-file data object (FILEGROUPDESCRIPTORW + FileContents streams served from the local copy: no second transfer) and
// Explorer creates the files at the paste destination. A relative path longer than maxPasteChars (= maxDescriptorChars) is refused
// before anything is received (PathTooLongError), so every received entry fits a descriptor.
// True when some entry of the received tree has an absolute path of maxPathLegacy characters or more under `root`.
bool treeNeedsVirtual(const std::filesystem::path& root, const std::vector<TreeEntry>& entries);
// True when every relative path fits a FILEDESCRIPTORW (maxDescriptorChars).
bool localTreeSupported(const std::vector<TreeEntry>& entries);
// A data object that serves the received tree (entries with `source` = the local path) from the local files. AddRef'd (1).
IDataObject* createLocalTreeObject(const std::vector<TreeEntry>& entries);
// Puts it on the clipboard with OleSetClipboard (the calling thread is an STA with OleInitialize that pumps messages) and
// remembers it as this process's own content; `expected` as in publishPullOffer. Returns the sequence after publishing.
DWORD publishLocalTree(const std::vector<TreeEntry>& entries, std::optional<DWORD> expectedSequence);
// Whether the data object published by publishLocalTree is still the clipboard (our own content must not be read back).
bool localTreeIsCurrent();
// True while the data object published by publishLocalTree is the clipboard and serves the transfer folder 	ransferRoot: its files must not be expired.
bool localTreeHolds(const std::filesystem::path& transferRoot);

struct Item {
  std::string kind;  // text html rtf png file dir
  uint64_t size = 0;
  std::wstring name;  // flat file name, or the tree's relative path ('\\')
  uint32_t width = 0, height = 0;
  DWORD attributes = 0;
  size_t units = 0;  // text: length in UTF-16 units
};
// A captured clipboard (richtext, image or tree) ready to be streamed as indexed data items.
// Data items are the entries that carry bytes: text/html/rtf, the png, or the files of a tree
// (ordinal = position among files). Reads are sequential per item.
class Outgoing {
 public:
  ~Outgoing();
  Outgoing(const Outgoing&) = delete;
  // nullptr when the snapshot is plain text or a flat file list (the caller's existing path).
  static std::unique_ptr<Outgoing> capture(const Snapshot& snapshot, const Bounds& bounds);
  const std::string& format() const;  // richtext | image | tree
  const std::vector<Item>& items() const;
  size_t tops() const; size_t skipped() const; uint64_t totalBytes() const;
  size_t dataCount() const;
  const Item& data(size_t ordinal) const;
  std::vector<uint8_t> read(size_t ordinal, uint64_t offset, size_t length);
  std::string digest(size_t ordinal);
 private:
  Outgoing();
  struct State; std::unique_ptr<State> state_;
};
// How a completed Incoming is published; a caller (the client) may substitute its own, for example to publish
// through a helper process or, in tests, to a stand-in clipboard. Unset members use the library functions.
struct Publishers {
  std::function<DWORD(HWND, const std::wstring&, const std::vector<uint8_t>*, const std::vector<uint8_t>*, std::optional<DWORD>)> rich;
  std::function<DWORD(HWND, const Image&, std::optional<DWORD>)> image;
  std::function<DWORD(HWND, const std::vector<std::filesystem::path>&, std::optional<DWORD>)> paths;
  // A received tree whose absolute paths are too long for CF_HDROP (see treeNeedsVirtual): served from the local copy as virtual files.
  std::function<DWORD(HWND, const std::vector<TreeEntry>&, std::optional<DWORD>)> localTree;
};
// Receives one such clipboard into memory (rich text, image) or into a new private transfer folder (tree),
// verifies every item's SHA-256 and publishes the whole thing at commit.
class Incoming {
 public:
  // `storage` is required for a folder tree (receipts and expiry) and unused otherwise.
  Incoming(const std::string& format, std::vector<Item> items, const std::filesystem::path& transferParent,
           Storage* storage, const std::vector<std::filesystem::path>& currentClipboardFiles, const Bounds& bounds);
  ~Incoming();
  Incoming(const Incoming&) = delete;
  void append(size_t ordinal, uint64_t offset, const std::vector<uint8_t>& bytes);
  void finish(size_t ordinal, const std::string& expectedSha256);
  bool complete() const;
  DWORD commit(HWND owner, std::optional<DWORD> expectedSequence, const Publishers* publishers = nullptr);
  // What a committed tree published at the top level (empty for other formats); valid after commit.
  const std::vector<std::filesystem::path>& publishedPaths() const;
  // True when the committed tree was published as virtual files (localTree) instead of CF_HDROP: no window of ours owns that
  // clipboard content, so the caller marks the returned sequence as its own, like a paste-pull offer.
  bool publishedVirtual() const;
  void cancel();
  uint64_t received() const;
  const std::string& format() const;
  // The new private transfer folder of a tree (empty for other formats); for diagnostics and tests.
  std::filesystem::path stagingRoot() const;
 private:
  struct State; std::unique_ptr<State> state_;
};
// Removes a committed tree under the caller's private parent: listed files, then listed folders
// deepest first, then the transfer folder if empty. Follows no links and deletes nothing unlisted.
bool expireTree(const std::filesystem::path& privateTransferParent, const std::filesystem::path& transferRoot,
                const std::vector<TreeEntry>& committed);
}
