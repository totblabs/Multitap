#pragma once
// Paste-pull profile (pull-on-paste of files and folder trees), declarations. The implementation is
// pull_impl.inl, included by clipboard.cpp. Like RDP and AnyDesk, copying announces only a manifest; the bytes of
// a file cross when the user pastes: the receiving side publishes an OLE data object with virtual files
// (FileGroupDescriptorW + FileContents streams) and each IStream::Read asks the offering side for the bytes.
#include "clipboard.hpp"
#include <objidl.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>

namespace multitap::clipboard {
// ---- Hard bounds of the profile -------------------------------------------------------------------------
// Offers kept by the offering peer (and the relay) per direction: the latest two.
inline constexpr size_t maxLiveOffers = 2;
// Copies up to this size stay on the push model (CF_HDROP published as soon as the copy arrives, so every
// application can paste them); larger ones are offered and pulled. Folders count with their entries.
inline constexpr uint64_t pushBelowBytes = 16ull * 1024 * 1024;
inline constexpr size_t pushBelowEntries = 256;
// A pull id the relay accepts: pullc-<n> from the client, pullh-<n> from the Host, n rising per requester.
inline bool pullRequestId(const std::string& id) {
  if (id.size() < 7 || id.size() > 15 || id.compare(0, 4, "pull") != 0 || (id[4] != 'c' && id[4] != 'h') || id[5] != '-' || id[6] < '1' || id[6] > '9') return false;
  for (size_t i = 7; i < id.size(); ++i) if (id[i] < '0' || id[i] > '9') return false;
  return true;
}

// ---- On paste (a copy is silent, data moves when pasted) --------------------------------------------------
// Text (and rich text) up to this many bytes of UTF-8, and images up to this many bytes of PNG, are still pushed at copy time (silently);
// anything bigger is offered and rendered when an application asks for it. Files and folders are never pushed in such a pair.
inline constexpr uint64_t pushTextBytes = 1ull * 1024 * 1024;
inline constexpr uint64_t pushImageBytes = 8ull * 1024 * 1024;
// A set of files is also offered as CF_HDROP (for applications that cannot take virtual files) when it is no larger than this: the
// application's GetData(CF_HDROP) blocks while everything is pulled into the private transfer folder. A larger set is virtual files only.
inline constexpr uint64_t hdropCompatBytes = 16ull * 1024 * 1024;
inline constexpr size_t hdropCompatEntries = 256;
// A staged copy made for CF_HDROP is reused by a further paste of the same offer for this long.
inline constexpr std::chrono::minutes hdropReuse{10};
// A paste reads files one after another; the next file up to this size is asked for while the current one is still being read. It fits
// the sender's window (ack_window.hpp: 1 MiB, 32 units), so the sender completes it without waiting for any acknowledgement.
inline constexpr uint64_t prefetchBytes = 1024 * 1024;
// How many files a pull-queue pair asks for ahead (the relay's pull queue: pull-queue-4-v1); a released pair asks for one at a time.
inline constexpr size_t maxPrefetchDepth = 4;
// A served file is read in blocks of this many formats chunks (about 1 MiB): one open and identity check per block in
// SourceOffer::Reader instead of one per chunk, and the client's serving reads one block per platform call (pullRead).
inline constexpr size_t readAheadChunks = 22;

// ---- Offering side ------------------------------------------------------------------------------------
// Whether a capture of these paths should be offered (pulled on paste) rather than pushed: it has more than
// pushBelowBytes of file data or pushBelowEntries entries. With `onPaste` (the on-paste revision) files and folders are always
// offered, never pushed; only a relative path a descriptor cannot hold still keeps it from being offered (the copy is then refused).
bool shouldOffer(const TreeScan& scan, bool onPaste = false);

// The manifest of a pull offer plus the identity of every file at copy time: volume serial, file ID, size and
// last write time. Files are opened lazily, only when a pull reads them, and a file that no longer matches fails
// that item with clipboard_selected_file_changed.
class SourceOffer {
 public:
  // The offer of an already walked tree (the caller decided with shouldOffer).
  static std::unique_ptr<SourceOffer> fromScan(const std::string& id, TreeScan scan);
  // The offer of copied text, rich text or an image (on-paste revision): the bytes are kept in memory, a snapshot of the clipboard at
  // copy time, and served by item index (text 0, html 1; png 0). Returns nullptr when the snapshot holds none of them, or, with
  // `onlyWhenLarge`, when it is small enough to be pushed (pushTextBytes / pushImageBytes). Throws the usual limit reasons.
  static std::unique_ptr<SourceOffer> fromContent(const std::string& id, const Snapshot& snapshot, const Bounds& bounds, bool onlyWhenLarge);
  // "tree", "text", "richtext" or "image".
  const std::string& format() const;
  // Walks the CF_HDROP paths exactly like scanTree (folders, empty folders, links skipped) and records identity.
  static std::unique_ptr<SourceOffer> capture(const std::string& id, const std::vector<std::filesystem::path>& roots, const Bounds& bounds);
  ~SourceOffer();
  const std::string& id() const;
  // Staged virtual files (Snapshot::staged) the offer's entries live in: they are deleted when the offer goes.
  void keepAlive(std::shared_ptr<StagedSource> staged);
  // Every entry, parents first (kind file/dir, relative '\' path, size, attributes) and its last write time in
  // milliseconds since 1970 (mtimeMs()).
  const std::vector<Item>& items() const;
  int64_t mtimeMs(size_t entry) const;
  size_t tops() const; size_t skipped() const; uint64_t totalBytes() const;
  size_t fileCount() const;
  // The entry (index into items()) of file ordinal `ordinal`, and the size of that file.
  size_t entryOfFile(size_t ordinal) const;
  uint64_t fileSize(size_t ordinal) const;

  // One pull: a sequential reader of file `ordinal` starting at `offset`, hashing what it hands out.
  class Reader {
   public:
    ~Reader();
    uint64_t size() const;
    uint64_t position() const;
    // Up to `length` bytes (0 only at the end). Throws clipboard_selected_file_changed, clipboard_file_locked,
    // clipboard_selected_file_unavailable.
    std::vector<uint8_t> next(size_t length);
    // SHA-256 (hex) of the bytes handed out by this reader; needs position() == size(). Re-checks that the file is
    // still the one that was offered (clipboard_selected_file_changed).
    std::string digest();
   private:
    friend class SourceOffer;
    Reader();
    struct State; std::unique_ptr<State> state_;
  };
  std::unique_ptr<Reader> open(size_t ordinal, uint64_t offset) const;
 private:
  SourceOffer();
  struct State; std::unique_ptr<State> state_;
};

// The latest maxLiveOffers offers of one peer, oldest dropped first; ends with the session.
//
// A paste in progress keeps its offer (RDP keeps clipboard data locked for a
// paste the same way): an offer that served a range within pasteHold is "pasting". A newer copy does not
// end it: openCurrent still admits its next file although the clipboard selection changed, trimming and
// dropIdle() pass it over, and it leaves once the paste has been quiet for pasteHold: the relay's own idle
// bound for a paused paste (Explorer's "replace or skip" question waits for the user between two files).
class OfferRegistry {
 public:
  static constexpr std::chrono::milliseconds pasteHold{15 * 60 * 1000};
  using Clock = std::chrono::steady_clock;
  // The negotiated on-paste profile binds a source to the clipboard selection
  // captured with it. Older profiles leave this unset and keep their lifetime.
  void add(std::shared_ptr<SourceOffer> offer, std::optional<DWORD> sequence = {});
  std::shared_ptr<SourceOffer> find(const std::string& id) const;
  // Admit a new range only while that copy is still selected, even before its
  // debounced change notification runs, or while that offer is pasting.
  // Metadata callbacks run outside our lock. A Reader already handed to a
  // paste retains its existing lifetime. An admitted range marks it pasting.
  std::unique_ptr<SourceOffer::Reader> openCurrent(const std::string& id, size_t ordinal, uint64_t offset,
                                                 const std::function<DWORD()>& currentSequence) const;
  // A range of `id` was served to its end (or is still being served): the paste goes on.
  void served(const std::string& id) const;
  bool pasting(const std::string& id) const;
  // This side's clipboard changed: the offers no paste is using leave (their ids come back, oldest
  // first, for the withdraw notice); a pasting one stays until it is quiet for pasteHold.
  std::vector<std::string> dropIdle();
  void remove(const std::string& id);
  void clear();
  size_t size() const;
  std::vector<std::string> ids() const;
  // Tests: the clock the hold is measured with.
  void setClockForTesting(std::function<Clock::time_point()> now) { std::lock_guard lock(mutex_); now_ = std::move(now); }
 private:
  struct Kept { std::shared_ptr<SourceOffer> offer; std::optional<DWORD> sequence; mutable std::optional<Clock::time_point> servedAt; };
  bool pastingLocked(const Kept& kept) const;
  mutable std::mutex mutex_; std::deque<Kept> offers_;
  std::function<Clock::time_point()> now_ = [] { return Clock::now(); };
};

// ---- Receiving side -----------------------------------------------------------------------------------
// What the application provides: how a pull is asked for and acknowledged. These hooks are called from the
// thread that reads a stream (the clipboard's STA thread) or from the thread that delivers data; they must be
// quick and must not call back into the PullOffer while holding its own locks. `rangeId` names one requested
// range of one file; the application maps it to the relay's pull id.
struct PullHooks {
  // Asks for file `index` (ordinal among files) of the offer from `offset` to its end.
  std::function<void(uint64_t rangeId, const std::string& offerId, size_t index, uint64_t offset)> request;
  // The range is no longer wanted (the stream was released, repositioned, timed out or failed).
  std::function<void(uint64_t rangeId)> cancel;
  // A failure this library detected itself (timeout, hash mismatch, out-of-order data): the pull is over.
  std::function<void(uint64_t rangeId, size_t index, const std::string& reason)> failed;
  // The reader consumed whole chunks up to absolute offset `chunkEnd` of file `index`: cumulative acknowledgement.
  std::function<void(uint64_t rangeId, size_t index, uint64_t chunkEnd)> ackChunk;
  // The consumer started (true) or finished (false) an asynchronous operation on the data object
  // (IDataObjectAsyncCapability): Explorer does, and then shows its own copy progress.
  std::function<void(bool started)> operation;
  // Internal lifecycle outcome. EndOperation's HRESULT is the extraction result,
  // not proof that every advertised file was saved. The legacy bool hook stays.
  std::function<void(bool started, HRESULT result)> operationState;
  // An application is blocked waiting for data of this offer (true: the first reader began to wait; false: none waits any more). Raw
  // transitions, any thread: the application debounces them (a paste of many small files waits again and again).
  std::function<void(bool waiting)> waiting;
};
// EndOperation's input HRESULT describes extraction outcome. S_FALSE and
// DROPEFFECT_NONE alone are not evidence of failure or cancellation.
inline const char* pullOperationFailure(HRESULT result) {
  if (!FAILED(result)) return nullptr;
  return result == E_ABORT || result == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? "clipboard_cancelled" : "clipboard_transfer_failed";
}
struct PullTimeouts {
  // Longest wait of one IStream::Read with no byte arriving (also for a queued range that never starts).
  std::chrono::milliseconds idle{30000};
};
// Optional only for a pending-clipboard delegate. Its GetData runs on the
// caller's owned delivery worker, with no detached render worker. allowed()
// binds one whole-render deadline plus the exact source/token/epoch lifetime.
// Defaults preserve every released OLE/file-offer behavior.
struct PullRenderPolicy {
  bool synchronous = false;
  std::function<bool()> allowed;
};
// How the data object is built.
struct PullOptions {
  // FD_PROGRESSUI in the file descriptors: Explorer shows its own copy dialog. Off when the application shows the progress itself (a pair of
  // the on-paste revision: the client card follows every paste); on for older pairs, so that nobody is left without any progress.
  bool explorerProgress = true;
  // Files and folders only: also offer CF_HDROP when the set fits hdropCompatBytes / hdropCompatEntries and every staged absolute path
  // stays below MAX_PATH. `stagingParent` is the private transfer parent, `storage` its receipt store (both from the caller; the
  // pulled copy is recorded there so that it expires like a pushed one). Empty parent: CF_HDROP is not offered.
  std::filesystem::path stagingParent;
  std::shared_ptr<Storage> storage;
  // Files asked for ahead of their reader: 1 for a released pair (one pull at a time), up to maxPrefetchDepth for a pull-queue pair.
  size_t prefetchDepth = 1;
};

// The receiver's view of one offer: the entries, the data object, and the ranges being pulled.
class PullOffer : public std::enable_shared_from_this<PullOffer> {
 public:
  // `entries`: every manifest entry, parents first; kind "file" or "dir"; name = relative path with '\';
  // `mtimeMs` per entry (same order). Throws on an invalid list.
  static std::shared_ptr<PullOffer> create(std::string offerId, std::vector<Item> entries, std::vector<int64_t> mtimeMs,
                                           PullHooks hooks, PullTimeouts timeouts = {}, PullOptions options = {});
  // An offer of text, rich text or an image (on-paste revision): `format` is "text", "richtext" or "image"; `items` are the data items
  // (text, html; png with width and height). The data object renders CF_UNICODETEXT / "HTML Format" / "PNG", CF_DIBV5 and CF_DIB on
  // demand: GetData blocks (pumping COM calls) until the item has been pulled and checked. Throws on an invalid list.
  static std::shared_ptr<PullOffer> createContent(std::string offerId, const std::string& format, std::vector<Item> items,
                                                  PullHooks hooks, PullTimeouts timeouts = {}, PullRenderPolicy renderPolicy = {});
  ~PullOffer();
  const std::string& format() const;
  // Whether this offer's data object is still the clipboard; a blocking render (GetData) gives up when it is not. Set by
  // publishPullOffer; a stand-in clipboard sets its own. Unset: always current.
  void setCurrentCheck(std::function<bool()> current);
  // Ends what is blocked waiting for data. `reason` is a clipboard_* code (clipboard_cancelled for the user's cancel, clipboard_offer_expired
  // for a newer copy). With `streams` the pulls Explorer's streams started end too, and a stream that asks again within a few seconds is
  // refused with the same reason (the user cancelled the paste; Explorer must not carry on with the next file). Without it only the blocking
  // renders (GetData of CF_HDROP, text, image) end; a paste Explorer is already extracting keeps going.
  void abortRenders(const std::string& reason, bool streams);
  // Whether a blocking render is running (GetData waiting for data).
  bool rendering() const;
  const std::string& id() const;
  size_t fileCount() const; uint64_t totalBytes() const;
  // The OLE data object (FileGroupDescriptorW, FileContents, Preferred DropEffect, ZoneIdentifier ...). AddRef'd.
  IDataObject* dataObject();
  // The application delivers what the offering side sent for range `rangeId`.
  void deliver(uint64_t rangeId, uint64_t offset, const std::vector<uint8_t>& bytes);
  // The offering side sent clipboard.item.end with the SHA-256 of the delivered range.
  void finish(uint64_t rangeId, const std::string& sha256);
  // The range failed: reason is a clipboard_* code (clipboard_selected_file_changed, clipboard_offer_expired, ...).
  void fail(uint64_t rangeId, const std::string& reason);
  // The session ended or the permission was withdrawn: every range and later read fails with `reason`.
  void shutdown(const std::string& reason);
  // Counters for tests and the progress notice.
  uint64_t deliveredBytes() const; uint64_t consumedBytes() const;
  // Whether a read is currently waiting for data.
  bool reading() const;
  struct Impl;
 private:
  PullOffer();
  std::shared_ptr<Impl> impl_;
  IDataObject* object_ = nullptr;
};

// Publishes the offer's data object on the clipboard with OleSetClipboard (never OleFlushClipboard: that would
// render every file at once). The calling thread must be an STA initialised with OleInitialize that keeps
// pumping messages for as long as the offer is on the clipboard. `expected` is the clipboard sequence the
// caller saw; a change since then fails with clipboard_changed. Returns the sequence after publishing.
DWORD publishPullOffer(const std::shared_ptr<PullOffer>& offer, std::optional<DWORD> expected);
// Whether this offer's data object is still on the clipboard (OleIsCurrentClipboard); our own content must
// never be sent back to the peer it came from.
bool pullOfferIsCurrent(const std::shared_ptr<PullOffer>& offer);
// Clears the clipboard if (and only if) this offer's data object is still on it. Returns the clipboard sequence after clearing it
// (the caller marks it as its own change), 0 when the offer was not on the clipboard and nothing was touched.
DWORD withdrawPullOffer(const std::shared_ptr<PullOffer>& offer);
// Whether the content this process last published as a delayed-rendering data object (an offer, a served local tree) is still the
// clipboard. read() refuses to read such content back: asking for its formats would pull the peer's data without a paste.
bool ownDelayedContentIsCurrent();
}
