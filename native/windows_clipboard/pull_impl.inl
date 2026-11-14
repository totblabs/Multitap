// Included by clipboard.cpp inside namespace multitap::clipboard: the paste-pull profile (pull.hpp).

namespace {
int64_t fileTimeToMs(uint64_t ticks) {
  constexpr uint64_t epoch = 116444736000000000ull;  // 1601 -> 1970 in 100 ns units
  return ticks <= epoch ? 0 : int64_t((ticks - epoch) / 10000);
}
FILETIME msToFileTime(int64_t ms) {
  const uint64_t ticks = uint64_t(ms < 0 ? 0 : ms) * 10000ull + 116444736000000000ull;
  FILETIME value; value.dwLowDateTime = DWORD(ticks & 0xffffffffu); value.dwHighDateTime = DWORD(ticks >> 32); return value;
}
// What identifies a file as the one that was copied.
struct Identity {
  DWORD volume = 0, indexHigh = 0, indexLow = 0; uint64_t size = 0, written = 0; bool directory = false;
  bool operator==(const Identity& other) const {
    return volume == other.volume && indexHigh == other.indexHigh && indexLow == other.indexLow && size == other.size && written == other.written;
  }
};
Identity identityOf(HANDLE file) {
  BY_HANDLE_FILE_INFORMATION info{};
  require(GetFileInformationByHandle(file, &info) != FALSE, "clipboard_selected_file_unavailable");
  Identity identity;
  identity.volume = info.dwVolumeSerialNumber; identity.indexHigh = info.nFileIndexHigh; identity.indexLow = info.nFileIndexLow;
  identity.size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  identity.written = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
  identity.directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  return identity;
}
// Opens a copied file by the volume and file index recorded at the copy, wherever the file is now on that volume. Only a file whose
// identity (including size and write time) is still the recorded one is returned; anything else is INVALID_HANDLE_VALUE.
HANDLE openByIdentity(const std::filesystem::path& lastKnownPath, const Identity& expected, DWORD share, DWORD flags) {
  wchar_t volume[MAX_PATH + 1]{};
  std::wstring root;
  if (GetVolumePathNameW(lastKnownPath.c_str(), volume, MAX_PATH)) root = volume;
  else root = lastKnownPath.root_path().wstring();
  if (root.empty() || (root.rfind(L"\\\\", 0) == 0 && root.rfind(L"\\\\?\\", 0) != 0)) return INVALID_HANDLE_VALUE;  // network shares: by path only
  if (root.rfind(L"\\\\?\\", 0) != 0) root = L"\\\\?\\" + root;
  Handle hint(CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (hint.value == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
  FILE_ID_DESCRIPTOR descriptor{}; descriptor.dwSize = sizeof(descriptor); descriptor.Type = FileIdType;
  descriptor.FileId.QuadPart = LONGLONG((uint64_t(expected.indexHigh) << 32) | expected.indexLow);
  const HANDLE file = OpenFileById(hint.value, &descriptor, GENERIC_READ, share, nullptr, flags);
  if (file == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file, &info)) { CloseHandle(file); return INVALID_HANDLE_VALUE; }
  Identity found;
  found.volume = info.dwVolumeSerialNumber; found.indexHigh = info.nFileIndexHigh; found.indexLow = info.nFileIndexLow;
  found.size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  found.written = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
  if (!(found == expected) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { CloseHandle(file); return INVALID_HANDLE_VALUE; }
  return file;
}
// Opens the file that was copied (read only, every sharing mode) and checks it is still that file: a regular file with the
// recorded identity. Throws the fixed reasons a pull reports (locked, changed, unavailable).
HANDLE openCopiedFile(const std::filesystem::path& path, const Identity& expected) {
  const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, flags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN;
  Handle owned(CreateFileW(longPath(path).c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, flags, nullptr));
  HANDLE file = owned.value;
  DWORD error = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
  // Renamed or moved after the copy (the file, or a folder above it) is still the copied file: find it again by the volume and file
  // index recorded at the copy. The same is done when the path now names a different file.
  if (file == INVALID_HANDLE_VALUE ? (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) : !(identityOf(file) == expected)) {
    Handle byId(openByIdentity(path, expected, share, flags));
    if (byId.value != INVALID_HANDLE_VALUE) { std::swap(owned.value, byId.value); file = owned.value; error = ERROR_SUCCESS; }
  }
  if (file == INVALID_HANDLE_VALUE) {
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) throw std::runtime_error("clipboard_file_locked");
    // Gone since the copy is a changed file; anything else (no access...) is unavailable.
    throw std::runtime_error(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? "clipboard_selected_file_changed" : "clipboard_selected_file_unavailable");
  }
  FILE_ATTRIBUTE_TAG_INFO tag{};
  require(GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) &&
          !(tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)), "clipboard_selected_file_changed");
  require(identityOf(file) == expected, "clipboard_selected_file_changed");
  owned.value = INVALID_HANDLE_VALUE; return file;
}// Opens a file or folder for its attributes only (no data access, every sharing mode): the copy-time identity.
Identity identityOfPath(const std::filesystem::path& path) {
  Handle file(CreateFileW(longPath(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  require(file.value != INVALID_HANDLE_VALUE, "clipboard_selected_file_unavailable");
  FILE_ATTRIBUTE_TAG_INFO tag{};
  require(GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &tag, sizeof(tag)) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
          "clipboard_selected_file_unavailable");
  return identityOf(file.value);
}
}

// A relative path longer than a FILEDESCRIPTORW holds (maxDescriptorChars) cannot be offered: the receiver could not list it in the
// virtual files, so such a copy is never pulled (a pair without the on-paste revision pushes it, which refuses it as too long).
bool shouldOffer(const TreeScan& scan, bool onPaste) {
  // A flat list of files is announced by one begin message (at most 48 KiB of names): a long list of long names is offered too.
  size_t names = 0; bool flat = scan.tops == scan.entries.size();
  if (flat) for (const auto& entry : scan.entries) names += entry.relative.size() * 3 + 40;
  if (!onPaste && !(scan.bytes > pushBelowBytes || scan.entries.size() > pushBelowEntries || (flat && names > 36000))) return false;
  for (const auto& entry : scan.entries) if (entry.relative.size() > maxDescriptorChars) return false;
  return true;
}

struct SourceOffer::State {
  std::string id, format = "tree"; std::vector<Item> items; std::vector<int64_t> mtimes; std::vector<std::filesystem::path> sources;
  std::vector<std::filesystem::path> selection;  // the copied paths themselves (the top-level entries' sources)
  std::vector<size_t> files;  // entry index per file ordinal
  std::vector<Identity> identities;  // per entry
  size_t tops = 0, skipped = 0; uint64_t total = 0;
  std::shared_ptr<StagedSource> keep;  // staged virtual files the offer reads from
  // Text, rich text and images: the bytes of each data item, a snapshot taken at copy time.
  std::vector<std::shared_ptr<const std::vector<uint8_t>>> blobs;
};
void SourceOffer::keepAlive(std::shared_ptr<StagedSource> staged) { state_->keep = std::move(staged); }
SourceOffer::SourceOffer() : state_(std::make_unique<State>()) {}
SourceOffer::~SourceOffer() = default;
const std::string& SourceOffer::id() const { return state_->id; }
const std::string& SourceOffer::format() const { return state_->format; }
const std::vector<Item>& SourceOffer::items() const { return state_->items; }
int64_t SourceOffer::mtimeMs(size_t entry) const { require(entry < state_->mtimes.size(), "clipboard_transfer_not_active"); return state_->mtimes[entry]; }
size_t SourceOffer::tops() const { return state_->tops; }
size_t SourceOffer::skipped() const { return state_->skipped; }
uint64_t SourceOffer::totalBytes() const { return state_->total; }
size_t SourceOffer::fileCount() const { return state_->files.size(); }
size_t SourceOffer::entryOfFile(size_t ordinal) const { require(ordinal < state_->files.size(), "clipboard_transfer_not_active"); return state_->files[ordinal]; }
uint64_t SourceOffer::fileSize(size_t ordinal) const { return state_->items[entryOfFile(ordinal)].size; }

std::unique_ptr<SourceOffer> SourceOffer::fromScan(const std::string& id, TreeScan scan) { return fromScan(id, std::move(scan), {}); }
std::unique_ptr<SourceOffer> SourceOffer::fromScan(const std::string& id, TreeScan scan, const std::function<void(size_t entries)>& progress) {
  require(!id.empty() && !scan.entries.empty(), "clipboard_empty");
  std::unique_ptr<SourceOffer> out(new SourceOffer()); auto& s = *out->state_;
  s.id = id; s.tops = scan.tops; s.skipped = scan.skipped;
  for (auto& entry : scan.entries) {
    if (progress && !s.items.empty() && (s.items.size() & 255) == 0) progress(s.items.size());
    const Identity identity = identityOfPath(entry.source);
    require(identity.directory == entry.directory, "clipboard_selected_file_changed");
    Item item; item.kind = entry.directory ? "dir" : "file"; item.name = entry.relative; item.attributes = entry.attributes;
    if (!entry.directory) {
      item.size = identity.size;  // the size the identity records is the size the manifest announces
      s.total += identity.size; s.files.push_back(s.items.size());
    }
    s.mtimes.push_back(fileTimeToMs(identity.written)); s.identities.push_back(identity);
    if (entry.relative.find(L'\\') == std::wstring::npos) s.selection.push_back(entry.source);
    s.sources.push_back(entry.source); s.items.push_back(std::move(item));
  }
  return out;
}
const std::vector<std::filesystem::path>& SourceOffer::selection() const { return state_->selection; }
bool sameSelection(const std::vector<std::filesystem::path>& a, const std::vector<std::filesystem::path>& b) {
  if (a.empty() || a.size() != b.size()) return false;
  const auto key = [](const std::filesystem::path& path) {
    std::wstring text = path.lexically_normal().wstring();
    while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
    return folded(text);
  };
  std::vector<std::wstring> left, right;
  for (const auto& path : a) left.push_back(key(path));
  for (const auto& path : b) right.push_back(key(path));
  std::sort(left.begin(), left.end()); std::sort(right.begin(), right.end());
  return left == right;
}
std::unique_ptr<SourceOffer> SourceOffer::capture(const std::string& id, const std::vector<std::filesystem::path>& roots, const Bounds& bounds) {
  return fromScan(id, scanTree(roots, bounds));
}
std::unique_ptr<SourceOffer> SourceOffer::fromContent(const std::string& id, const Snapshot& snapshot, const Bounds& bounds, bool onlyWhenLarge) {
  require(!id.empty(), "clipboard_empty");
  std::unique_ptr<SourceOffer> out(new SourceOffer()); auto& s = *out->state_; s.id = id;
  const auto add = [&](Item item, std::vector<uint8_t> bytes) {
    item.size = bytes.size(); s.total += item.size;
    s.files.push_back(s.items.size()); s.mtimes.push_back(0);
    s.items.push_back(std::move(item)); s.blobs.push_back(std::make_shared<const std::vector<uint8_t>>(std::move(bytes)));
  };
  bool large = false;
  if (snapshot.image && !snapshot.text) {
    const auto& image = *snapshot.image;
    require(image.width && image.height && uint64_t(image.width) * image.height <= maxImagePixels && !image.png.empty() && image.png.size() <= maxImageBytes, "clipboard_image_too_large");
    s.format = "image"; Item png; png.kind = "png"; png.width = image.width; png.height = image.height; add(std::move(png), image.png);
    large = image.png.size() > pushImageBytes;
  } else if (snapshot.text) {
    const auto& text = *snapshot.text; std::string utf8;
    if (!text.empty()) {
      const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
      require(count > 0, "clipboard_invalid_unicode");
      utf8.assign(size_t(count), '\0'); WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), utf8.data(), count, nullptr, nullptr);
    }
    require(utf8.size() <= bounds.textBytes && text.size() <= maxTextUnits, "clipboard_text_too_large");
    Item item; item.kind = "text"; item.units = text.size(); add(std::move(item), std::vector<uint8_t>(utf8.begin(), utf8.end()));
    s.format = "text";
    if (snapshot.html && snapshot.html->size() <= bounds.richBytes) { Item html; html.kind = "html"; add(std::move(html), *snapshot.html); s.format = "richtext"; }
    large = s.total > pushTextBytes;
  } else return nullptr;
  if (onlyWhenLarge && !large) return nullptr;
  return out;
}

// A copy source is the user's own file, and Windows refuses to rename a folder while any file below it is open (whatever the
// sharing flags are). So a copied file is not kept open between reads: every read opens it, reads a block and closes it, and
// the file is found again wherever it went (by its path while that still names it, else by its volume and file index).
// A block is readAheadChunks chunks (pull.hpp): one open and identity check per ~1 MiB instead of per 45 KiB chunk (stage_bench,
// measured: open and check ~50 us, read and hash ~65-100 us per chunk). The bytes are hashed as they are handed out, and digest() checks
// the file once more at the end, so a writer that got in after a block was read is still reported.
struct SourceOffer::Reader::State {
  std::filesystem::path path; Identity expected; uint64_t size = 0, at = 0; Hash hash;
  std::shared_ptr<const std::vector<uint8_t>> blob;  // text, rich text, image snapshots
  std::vector<uint8_t> ahead; size_t used = 0;        // the last block read; ahead[used] is the byte at `at`
};
SourceOffer::Reader::Reader() : state_(std::make_unique<State>()) {}
SourceOffer::Reader::~Reader() = default;
uint64_t SourceOffer::Reader::size() const { return state_->size; }
uint64_t SourceOffer::Reader::position() const { return state_->at; }
std::vector<uint8_t> SourceOffer::Reader::next(size_t length) {
  auto& s = *state_; require(length > 0 && length <= maxChunkBytes, "clipboard_chunk_limit");
  const uint64_t remaining = s.size - s.at;
  if (!remaining) return {};
  if (s.blob) {
    std::vector<uint8_t> bytes(size_t(std::min<uint64_t>(remaining, length)));
    std::memcpy(bytes.data(), s.blob->data() + size_t(s.at), bytes.size());
    s.hash.add(bytes.data(), bytes.size()); s.at += bytes.size(); return bytes;
  }
  const size_t count = size_t(std::min<uint64_t>(remaining, length));
  if (s.ahead.size() - s.used < count) {
    Handle file(openCopiedFile(s.path, s.expected));
    LARGE_INTEGER to; to.QuadPart = LONGLONG(s.at);
    require(SetFilePointerEx(file.value, to, nullptr, FILE_BEGIN) != FALSE, "clipboard_selected_file_unavailable");
    s.ahead.resize(size_t(std::min<uint64_t>(remaining, std::max<uint64_t>(count, readAheadChunks * formatsChunkBytes)))); s.used = 0; DWORD read = 0;
    const BOOL ok = ReadFile(file.value, s.ahead.data(), DWORD(s.ahead.size()), &read, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok || read != s.ahead.size()) {
      s.ahead.clear();
      require(error != ERROR_LOCK_VIOLATION, "clipboard_file_locked");
      throw std::runtime_error("clipboard_selected_file_changed");
    }
  }
  std::vector<uint8_t> bytes(s.ahead.begin() + std::ptrdiff_t(s.used), s.ahead.begin() + std::ptrdiff_t(s.used + count)); s.used += count;
  s.hash.add(bytes.data(), bytes.size()); s.at += bytes.size(); return bytes;
}
std::string SourceOffer::Reader::digest() {
  auto& s = *state_; require(s.at == s.size, "clipboard_file_not_complete");
  // A writer that got in after the copy changes size or time: report it instead of vouching for the bytes.
  if (!s.blob) { Handle file(openCopiedFile(s.path, s.expected)); }
  return s.hash.finish();
}
std::unique_ptr<SourceOffer::Reader> SourceOffer::open(size_t ordinal, uint64_t offset) const {
  const auto& s = *state_; const size_t entry = entryOfFile(ordinal);
  std::unique_ptr<Reader> reader(new Reader()); auto& r = *reader->state_;
  if (!s.blobs.empty()) {
    r.blob = s.blobs[ordinal]; r.size = r.blob->size(); require(offset <= r.size, "clipboard_chunk_out_of_order_or_bounds"); r.at = offset; return reader;
  }
  r.path = s.sources[entry]; r.expected = s.identities[entry];
  { Handle file(openCopiedFile(r.path, r.expected)); }  // the file is there, regular, and the one that was copied
  r.size = r.expected.size; require(offset <= r.size, "clipboard_chunk_out_of_order_or_bounds");
  r.at = offset; return reader;
}

bool OfferRegistry::pastingLocked(const Kept& kept) const {
  return kept.servedAt && now_() - *kept.servedAt < pasteHold;
}
void OfferRegistry::trimLocked() {
  // The oldest offer no paste is using goes first; the new one always stays. Only when every older one is
  // pasting does the oldest go anyway (the bound holds).
  while (offers_.size() > maxLiveOffers) {
    auto victim = offers_.begin();
    for (auto at = offers_.begin(); at + 1 != offers_.end(); ++at) if (!pastingLocked(*at)) { victim = at; break; }
    offers_.erase(victim);
  }
}
void OfferRegistry::add(std::shared_ptr<SourceOffer> offer, std::optional<DWORD> sequence) {
  std::lock_guard lock(mutex_);
  const std::string id = offer->id(); auto selection = offer->selection();
  offers_.push_back({id, std::move(offer), sequence, std::nullopt, std::move(selection)});
  trimLocked();
}
void OfferRegistry::addDeferred(const std::string& id, std::vector<std::filesystem::path> selection, DWORD sequence) {
  require(!id.empty() && !selection.empty(), "clipboard_empty");
  std::lock_guard lock(mutex_);
  offers_.push_back({id, nullptr, sequence, std::nullopt, std::move(selection), true});
  trimLocked();
}
std::optional<OfferRegistry::Deferred> OfferRegistry::deferred(const std::string& id) const {
  std::lock_guard lock(mutex_);
  for (const auto& kept : offers_) if (kept.id == id && kept.deferred) return Deferred{kept.selection, kept.sequence.value_or(0), kept.offer};
  return std::nullopt;
}
bool OfferRegistry::listing(const std::string& id) {
  std::lock_guard lock(mutex_);
  for (auto& kept : offers_) if (kept.id == id && kept.deferred) { kept.servedAt = now_(); return true; }
  return false;
}
bool OfferRegistry::materialize(const std::string& id, std::shared_ptr<SourceOffer> offer) {
  require(offer && offer->id() == id, "clipboard_offer_expired");
  std::lock_guard lock(mutex_);
  for (auto& kept : offers_) if (kept.id == id && kept.deferred) {
    if (!kept.offer) kept.offer = std::move(offer);  // two walks of one copy: the first list stays the one served
    kept.servedAt = now_(); return true;
  }
  return false;
}
void OfferRegistry::served(const std::string& id) const {
  std::lock_guard lock(mutex_);
  for (const auto& kept : offers_) if (kept.id == id && kept.offer) kept.servedAt = now_();
}
bool OfferRegistry::pasting(const std::string& id) const {
  std::lock_guard lock(mutex_);
  for (const auto& kept : offers_) if (kept.id == id) return pastingLocked(kept);
  return false;
}
std::vector<std::string> OfferRegistry::dropIdle() {
  std::lock_guard lock(mutex_); std::vector<std::string> out;
  for (auto at = offers_.begin(); at != offers_.end();) {
    if (pastingLocked(*at)) { ++at; continue; }
    out.push_back(at->id); at = offers_.erase(at);
  }
  return out;
}
std::shared_ptr<SourceOffer> OfferRegistry::find(const std::string& id) const {
  std::lock_guard lock(mutex_);
  for (const auto& kept : offers_) if (kept.id == id) return kept.offer;
  return nullptr;
}
std::unique_ptr<SourceOffer::Reader> OfferRegistry::openCurrent(const std::string& id, size_t ordinal, uint64_t offset,
                                                              const std::function<DWORD()>& currentSequence,
                                                              const std::function<std::vector<std::filesystem::path>()>& currentSelection) const {
  Kept selected;
  {
    std::lock_guard lock(mutex_);
    for (const auto& kept : offers_) if (kept.id == id) { selected = kept; break; }
  }
  require(selected.offer != nullptr, "clipboard_offer_expired");
  // A paste already reading this offer goes on after a newer copy (its next file, its next offset).
  bool pastingNow = false;
  { std::lock_guard lock(mutex_); for (const auto& kept : offers_) if (kept.offer == selected.offer) { pastingNow = pastingLocked(kept); break; } }
  // The copy is still the one selected: the same clipboard sequence, or (the number moved without a new copy, or the same items were
  // copied again) the clipboard names exactly the copied paths. Read once per admission, not per check.
  std::optional<bool> sameItems;
  const auto unchanged = [&] {
    if (pastingNow || !selected.sequence || (currentSequence && currentSequence() == *selected.sequence)) return true;
    if (!currentSelection || selected.selection.empty()) return false;
    if (!sameItems) { try { sameItems = sameSelection(currentSelection(), selected.selection); } catch (...) { sameItems = false; } }
    return *sameItems;
  };
  require(unchanged(), "clipboard_offer_expired");
  auto reader = selected.offer->open(ordinal, offset);
  require(unchanged(), "clipboard_offer_expired");
  {
    std::lock_guard lock(mutex_);
    bool retained = false;
    for (const auto& kept : offers_) if (kept.offer == selected.offer && kept.sequence == selected.sequence) { retained = true; kept.servedAt = now_(); break; }
    require(retained, "clipboard_offer_expired");
  }
  return reader;
}
void OfferRegistry::remove(const std::string& id) {
  std::lock_guard lock(mutex_);
  for (auto at = offers_.begin(); at != offers_.end(); ++at) if (at->id == id) { offers_.erase(at); return; }
}
void OfferRegistry::clear() { std::lock_guard lock(mutex_); offers_.clear(); }
size_t OfferRegistry::size() const { std::lock_guard lock(mutex_); return offers_.size(); }
std::vector<std::string> OfferRegistry::ids() const { std::lock_guard lock(mutex_); std::vector<std::string> out; for (const auto& kept : offers_) out.push_back(kept.id); return out; }

// ---------------------------------------------------------------------------------------------------------
// The receiver

namespace {
struct EventHandle {
  HANDLE value = nullptr;
  EventHandle() : value(CreateEventW(nullptr, TRUE, FALSE, nullptr)) { require(value != nullptr, "clipboard_allocation_failed"); }
  ~EventHandle() { if (value) CloseHandle(value); }
  EventHandle(const EventHandle&) = delete;
};
}

// One requested range of one file: the bytes the offering side delivered and the reader has not taken yet.
struct PullRange {
  // `queued`: a pull-queue pair, whose sender has a 4 MiB window: an acknowledgement every readAheadChunks chunks (~1 MiB) keeps it
  // sending and is a sixth of the messages of the released 128 KiB rhythm, which stays for the 1 MiB window of a released pair.
  PullRange(uint64_t id, size_t index, uint64_t start, uint64_t size, bool queued = false)
      : id(id), index(index), start(start), size(size), next(start), consumed(start),
        ackPolicy(true, queued ? readAheadChunks : 4, queued ? readAheadChunks * formatsChunkBytes : 128 * 1024) {}
  const uint64_t id; const size_t index; const uint64_t start, size;
  struct Chunk { uint64_t offset; std::vector<uint8_t> bytes; size_t used = 0; };
  std::deque<Chunk> chunks; uint64_t next, consumed;  // next = offset the next delivered chunk must have
  Hash hash; AckPolicy ackPolicy;
  uint64_t lastWhole = 0; bool wholeSinceAck = false;
  bool requested = false, ended = false, cancelled = false, busy = false;
  std::string failure;
  EventHandle event;  // manual reset: data arrived or the state changed
};

// Range ids are unique in the process, so an application can route data by id alone.
static uint64_t nextRangeId() { static std::atomic<uint64_t> counter{1}; return counter++; }

// A blocking render (GetData of CF_HDROP, text, HTML, an image) runs on its own worker thread, so that the COM apartment thread the
// application called into can keep serving calls while it waits (a second application pasting at once, a Release, our own clipboard
// messages) and so that what it waits for never depends on that thread's stack.
struct RenderJob {
  EventHandle done;                   // manual reset: the job ended
  std::atomic<bool> finished{false}, abandon{false};
  bool pasting = false;               // a deferred offer's list wait: a paste that has begun (supersedeRenders passes it over)
  std::string failure;                // empty on success
  std::vector<std::vector<uint8_t>> blobs;       // the pulled items (text, html, png)
  std::vector<std::filesystem::path> paths;      // CF_HDROP: the top-level staged items
  std::vector<TreeEntry> entries;                // CF_HDROP: every staged entry
  std::vector<std::string> digests;              // CF_HDROP: SHA-256 of each staged file, in file order
  struct StagedFile { std::filesystem::path path; uint64_t size = 0, written = 0; };
  std::vector<StagedFile> files;                 // CF_HDROP: every staged file by ordinal, with the size and write time it was left with
  std::filesystem::path root;
};

struct PullOffer::Impl : std::enable_shared_from_this<PullOffer::Impl> {
  std::mutex m; std::string id, format = "tree"; std::vector<Item> entries; std::vector<int64_t> mtimes;
  std::vector<size_t> fileEntry;     // entry index per file ordinal
  std::vector<int64_t> ordinalOfEntry;  // file ordinal per entry, -1 for folders
  uint64_t total = 0; PullHooks hooks; PullTimeouts timeouts; PullOptions options; PullRenderPolicy renderPolicy;
  std::map<uint64_t, std::shared_ptr<PullRange>> ranges; std::deque<std::shared_ptr<PullRange>> queued; std::shared_ptr<PullRange> active;
  uint64_t delivered = 0, consumed = 0; bool dead = false; std::string deadReason; int readers = 0;
  // Blocking renders. `abortGeneration` counts the abortRenders() calls; a render that started before one gives up with `abortReason`.
  std::function<bool()> isCurrent;
  // Every abort counts in abortGeneration; one that ends a paste under way too (the user's cancel, a withdrawn offer) also in hardAborts.
  uint64_t abortGeneration = 0, hardAborts = 0; std::string abortReason, hardReason;
  std::chrono::steady_clock::time_point refuseStreamsUntil{}; std::string refuseReason;
  std::map<std::string, std::shared_ptr<RenderJob>> jobs;
  struct Cached { std::chrono::steady_clock::time_point at; std::vector<uint8_t> bytes; };
  std::map<std::string, Cached> cache;
  struct Staged { std::chrono::steady_clock::time_point at; std::shared_ptr<RenderJob> result; };
  std::optional<Staged> staged;
  int waiters = 0;
  bool hdropOffered = false;
  // Deferred manifest: `deferred` until the list arrives (`listed`); a request in flight fails with `manifestFailure`. `manifestHeard`
  // is the last word from the offering side (the request, a progress message), for the idle bound.
  bool deferred = false, listed = true; std::string manifestFailure; std::chrono::steady_clock::time_point manifestHeard{};
  EventHandle manifestEvent;
  // One list request at a time per copy (the hooks name the copy, not the attempt): `manifestAsking` while one is out, owned by attempt
  // `manifestOwner`. An attempt that starts while an abandoned one still winds down takes its request over (the older one then leaves
  // without cancelling it); `manifestCancelling` while the owner's cancel is on its way, so no newer request can overtake it.
  bool manifestAsking = false, manifestCancelling = false; uint64_t manifestAttempts = 0, manifestOwner = 0;
  // The validated list: entries, write times, the file ordinals (shared by create() and deliverManifest()). Lock held or not yet shared.
  void setEntries(std::vector<Item> list, std::vector<int64_t> mtimeMs) {
    require(!list.empty() && list.size() <= maxEntries && mtimeMs.size() == list.size(), "clipboard_entry_count_limit");
    std::set<std::wstring> seen, directories; size_t tops = 0; uint64_t sum = 0;
    std::vector<size_t> files; std::vector<int64_t> ordinals;
    for (size_t i = 0; i < list.size(); ++i) {
      auto& item = list[i];
      std::wstring wire = item.name; std::replace(wire.begin(), wire.end(), L'\\', L'/');
      if (item.name.size() > maxDescriptorChars) throw PathTooLongError(item.name.size(), maxDescriptorChars);
      require((item.kind == "file" || item.kind == "dir") && treePathFromWire(wire) == item.name, "clipboard_selected_file_unavailable");
      const auto key = folded(item.name); const auto slash = item.name.rfind(L'\\');
      if (slash == std::wstring::npos) ++tops; else require(directories.count(folded(item.name.substr(0, slash))) != 0, "clipboard_selected_file_unavailable");
      require(seen.insert(key).second, "clipboard_duplicate_file_name");
      item.attributes &= transferableAttributes;
      if (item.kind == "dir") { directories.insert(key); item.size = 0; ordinals.push_back(-1); }
      else {
        require(item.size <= scaleBytes && item.size <= scaleBytes - sum, "clipboard_total_size_limit"); sum += item.size;
        ordinals.push_back(int64_t(files.size())); files.push_back(i);
      }
    }
    require(tops >= 1, "clipboard_file_count_limit");
    entries = std::move(list); mtimes = std::move(mtimeMs); fileEntry = std::move(files); ordinalOfEntry = std::move(ordinals); total = sum;
  }
  // The list for the data object, asked for (once per attempt) when there is none yet. nullptr with the clipboard_* reason on failure.
  struct Listing { std::vector<Item> entries; std::vector<int64_t> mtimes, ordinalOfEntry; };
  std::shared_ptr<const Listing> listing;
  std::shared_ptr<const Listing> manifest(std::string& failure) {
    { std::lock_guard lock(m); if (listing) return listing; if (dead) { failure = deadReason; return nullptr; } }
    auto render = job("manifest", [self = shared_from_this()](RenderJob&, const std::function<bool()>& abandoned) { self->awaitManifest(abandoned); });
    failure = await(render);
    if (!failure.empty()) return nullptr;
    std::lock_guard lock(m);
    if (!listing) failure = dead ? deadReason : std::string("clipboard_transfer_failed");
    return listing;
  }
  // Render thread: one request for the list, waited for until it arrives, fails, goes quiet for manifestIdle, or nobody wants it.
  void awaitManifest(const std::function<bool()>& abandoned) {
    if (!hooks.manifest) throw std::runtime_error("clipboard_transfer_failed");
    uint64_t mine = 0; bool ask = false;
    for (;;) {
      {
        std::lock_guard lock(m);
        if (listed) return;
        if (dead) throw std::runtime_error(deadReason);
        if (!manifestCancelling) {
          mine = ++manifestAttempts; manifestOwner = mine;
          if (!manifestAsking) {
            manifestAsking = true; ask = true;
            manifestFailure.clear(); manifestHeard = std::chrono::steady_clock::now(); ResetEvent(manifestEvent.value);
          }
          break;
        }
      }
      if (abandoned()) throw std::runtime_error("clipboard_cancelled");
      Sleep(10);  // an older attempt's cancel is on its way: this request must follow it
    }
    // The request ends (answered, failed or cancelled) only by its owner; an attempt that was taken over leaves it running.
    const auto finish = [&](const char* reason) {
      {
        std::lock_guard lock(m);
        if (manifestOwner != mine) throw std::runtime_error("clipboard_cancelled");
        manifestAsking = false;
        if (listed) return;
        if (!manifestFailure.empty()) throw std::runtime_error(manifestFailure);
        if (dead) throw std::runtime_error(deadReason);
        manifestCancelling = true;
      }
      if (hooks.manifestCancel) { try { hooks.manifestCancel(id, reason); } catch (...) {} }
      std::lock_guard lock(m);
      manifestCancelling = false;
      if (!listed) throw std::runtime_error(reason);  // the list may have arrived while the cancel went out: it is used
    };
    if (ask) {
      try { hooks.manifest(id); }
      catch (...) { std::lock_guard lock(m); if (manifestOwner == mine) manifestAsking = false; throw; }
    }
    for (;;) {
      WaitForSingleObject(manifestEvent.value, 250);
      {
        std::lock_guard lock(m);
        if (manifestOwner != mine) throw std::runtime_error("clipboard_cancelled");
        if (listed || !manifestFailure.empty() || dead) { /* the owner ends the request below */ }
        else {
          ResetEvent(manifestEvent.value);
          if (std::chrono::steady_clock::now() - manifestHeard < manifestIdle && !abandoned()) continue;
        }
      }
      return finish(abandoned() ? "clipboard_cancelled" : "clipboard_timeout");
    }
  }
  // Small files asked for before anybody reads them (see prefetchAfter), in the order they were asked for; a reader that asks for one
  // of them takes it over. At most depth() of them.
  std::deque<std::shared_ptr<PullRange>> prefetched;
  size_t depth() const { return std::clamp<size_t>(options.prefetchDepth, 1, maxPrefetchDepth); }
  bool isPrefetched(const std::shared_ptr<PullRange>& range) const { return std::find(prefetched.begin(), prefetched.end(), range) != prefetched.end(); }
  void unprefetch(const std::shared_ptr<PullRange>& range) { prefetched.erase(std::remove(prefetched.begin(), prefetched.end(), range), prefetched.end()); }
  size_t inFlight() const { size_t n = 0; for (const auto& entry : ranges) n += entry.second->requested && !entry.second->ended && entry.second->failure.empty() && !entry.second->cancelled; return n; }

  bool renderAllowed() const {
    if(!renderPolicy.allowed)return true;
    try{return renderPolicy.allowed();}catch(...){return false;}
  }
  // The hooks are invoked after the lock is released, in the order the events happened.
  struct Call { int kind; std::shared_ptr<PullRange> range; uint64_t value = 0; std::string text; };
  void run(std::vector<Call>& calls) {
    for (auto& call : calls) {
      try {
        if (call.kind == 0 && hooks.request) hooks.request(call.range->id, id, call.range->index, call.range->start);
        else if (call.kind == 1 && hooks.cancel) hooks.cancel(call.range->id);
        else if (call.kind == 2 && hooks.ackChunk) hooks.ackChunk(call.range->id, call.range->index, call.value);
        else if (call.kind == 3 && hooks.failed) hooks.failed(call.range->id, call.range->index, call.text);
      } catch (...) {}
    }
    calls.clear();
  }
  // Starts the next queued range when nothing is active. Lock held.
  void promote(std::vector<Call>& calls) {
    while (!active && !queued.empty()) {
      auto next = std::move(queued.front()); queued.pop_front();
      if (next->cancelled || !next->failure.empty()) continue;
      active = next; next->requested = true; calls.push_back({0, next});
    }
  }
  // The range is over (done, failed or cancelled): free the slot and start the next. Lock held.
  void retire(const std::shared_ptr<PullRange>& range, std::vector<Call>& calls) {
    if (active == range) active.reset();
    for (auto at = queued.begin(); at != queued.end(); ++at) if (*at == range) { queued.erase(at); break; }
    promote(calls);
  }
  void wake(PullRange& range) { SetEvent(range.event.value); }
  bool refusing() const { return !refuseReason.empty() && std::chrono::steady_clock::now() < refuseStreamsUntil; }

  // A paste reads the files one after another (the shell's copy engine does, in descriptor order), and every file costs a round trip to
  // the offering side that the relay allows only one at a time. So once a reader has a file, the next small one is asked for at once: it
  // arrives while the reader is still busy with this one (writing it, closing it). Small means it fits the sender's window, so the sender
  // finishes it without any acknowledgement and the relay's pull slot is free again even if nobody ever reads it. A prefetched file nobody
  // has taken over never starts another. Depth 1 (the released relay: one pull at a time per pair): only when nothing else is asked for, and
  // the prefetch holds that one slot. Depth K (a pull-queue pair): up to K files ahead, asked for while the reader's own file still arrives;
  // the offering side serves them in order. Lock held.
  void prefetchAfter(size_t ordinal, std::vector<Call>& calls) {
    const size_t limit = depth();
    if (dead || format != "tree" || refusing() || prefetched.size() >= limit || (limit == 1 && (active || !queued.empty()))) return;
    size_t next = prefetched.empty() ? ordinal + 1 : prefetched.back()->index + 1;
    while (prefetched.size() < limit && (limit == 1 || inFlight() < limit)) {
      while (next < fileEntry.size() && entries[fileEntry[next]].size == 0) ++next;  // an empty file is never pulled
      if (next >= fileEntry.size() || entries[fileEntry[next]].size > prefetchBytes) return;
      auto range = std::make_shared<PullRange>(nextRangeId(), next, 0, entries[fileEntry[next]].size, limit > 1);
      ranges[range->id] = range; range->requested = true; prefetched.push_back(range); calls.push_back({0, range});
      if (limit == 1) active = range;
      ++next;
    }
  }
  // A prefetch that will not be read: a range still arriving is cancelled (the offering side stops or skips it), a complete one goes. Lock held.
  void dropPrefetch(const std::shared_ptr<PullRange>& range, std::vector<Call>& calls) {
    if (range->requested && !range->ended && range->failure.empty() && !range->cancelled) {
      range->cancelled = true; range->chunks.clear(); ranges.erase(range->id); wake(*range); calls.push_back({1, range}); retire(range, calls);
    }
  }

  std::shared_ptr<PullRange> start(size_t ordinal, uint64_t offset) {
    std::vector<Call> calls; std::shared_ptr<PullRange> range;
    {
      std::lock_guard lock(m);
      const auto found = std::find_if(prefetched.begin(), prefetched.end(), [&](const std::shared_ptr<PullRange>& candidate) {
        return candidate->index == ordinal && candidate->start == offset && !candidate->cancelled && candidate->failure.empty(); });
      if (found != prefetched.end() && !dead && !refusing()) {
        // The file asked for ahead: taken over as it is (still arriving, or complete and checked). Files asked for before it were skipped.
        const std::vector<std::shared_ptr<PullRange>> skipped(prefetched.begin(), found);
        range = *found; prefetched.erase(prefetched.begin(), found + 1);
        for (const auto& old : skipped) dropPrefetch(old, calls);
        if (depth() > 1 && !active && !range->ended) active = range;
        if (range->ended || depth() > 1) prefetchAfter(ordinal, calls);
      }
      // Any other file: the guess was wrong. With one pull at a time a prefetch still arriving is outranked below like any range nobody
      // reads; in a pull queue every prefetch goes now, so the reader's own file is not queued behind them.
      else if (depth() > 1) { const auto all = std::vector<std::shared_ptr<PullRange>>(prefetched.begin(), prefetched.end()); prefetched.clear(); for (const auto& old : all) dropPrefetch(old, calls); }
      else if (!prefetched.empty() && prefetched.front()->ended) prefetched.clear();
    }
    run(calls);
    if (range) return range;
    {
      std::lock_guard lock(m);
      range = std::make_shared<PullRange>(nextRangeId(), ordinal, offset, entries[fileEntry[ordinal]].size, depth() > 1);
      if (dead) { range->failure = deadReason; return range; }
      // The user cancelled the paste a moment ago: Explorer asking for the next file does not carry it on.
      if (refusing()) { range->failure = refuseReason; return range; }
      if (!prefetched.empty() && prefetched.front() == active) prefetched.clear();
      ranges[range->id] = range;
      if (!active) { active = range; range->requested = true; calls.push_back({0, range}); }
      else if (!active->busy && active->failure.empty() && !active->ended) {
        // A new reader outranks a range nobody is reading right now (an Explorer thread that moved on to
        // another file): that one is cancelled and its stream restarts it when it reads again.
        auto old = active; old->cancelled = true; wake(*old); ranges.erase(old->id); active.reset();
        calls.push_back({1, old}); active = range; range->requested = true; calls.push_back({0, range});
      } else queued.push_back(range);
      // A pull queue: the files after this one are asked for now; the offering side serves them right after it.
      if (depth() > 1 && offset == 0 && range->requested) prefetchAfter(ordinal, calls);
    }
    run(calls); return range;
  }
  // The reader no longer wants the range (released, repositioned, timed out or failed).
  void cancel(const std::shared_ptr<PullRange>& range, const std::string& reason = std::string()) {
    if (!range) return;
    std::vector<Call> calls;
    {
      std::lock_guard lock(m);
      if (range->cancelled) return;
      unprefetch(range);
      const bool inFlight = range->requested && !range->ended && range->failure.empty();
      range->cancelled = true; range->chunks.clear(); ranges.erase(range->id); wake(*range);
      if (inFlight) calls.push_back({1, range});
      if (inFlight && !reason.empty()) calls.push_back({3, range, 0, reason});
      retire(range, calls);
    }
    run(calls);
  }
  void failRange(const std::shared_ptr<PullRange>& range, const std::string& reason, std::vector<Call>& calls, bool tellPeer) {
    if (range->ended || !range->failure.empty()) return;
    unprefetch(range);
    range->failure = reason; range->chunks.clear(); wake(*range);
    if (tellPeer && range->requested) { calls.push_back({1, range}); calls.push_back({3, range, 0, reason}); }
    ranges.erase(range->id); retire(range, calls);
  }
  // Waits for data. Returns the bytes copied (0 with `eof` at the end of the range) or throws the failure reason.
  // Counts the readers that are blocked waiting for data; the first one and the last one to leave are reported (PullHooks::waiting).
  void waitingChanged(int delta) {
    bool report = false, value = false;
    { std::lock_guard lock(m); waiters += delta; if (delta > 0 && waiters == 1) { report = true; value = true; } else if (delta < 0 && waiters == 0) { report = true; value = false; } }
    if (report && hooks.waiting) { try { hooks.waiting(value); } catch (...) {} }
  }
  struct WaitScope {
    Impl* impl; bool on = false;
    explicit WaitScope(Impl* impl) : impl(impl) {}
    void begin() { if (!on) { on = true; impl->waitingChanged(+1); } }
    ~WaitScope() { if (on) impl->waitingChanged(-1); }
  };
  size_t read(const std::shared_ptr<PullRange>& range, uint8_t* out, size_t max, bool wait, bool& eof, const std::function<bool()>& abandoned) {
    eof = false; WaitScope waiting(this);
    require(renderAllowed(),"clipboard_cancelled");
    auto deadline = std::chrono::steady_clock::now() + timeouts.idle;
    for (;;) {
      require(renderAllowed(),"clipboard_cancelled");
      std::vector<Call> calls; size_t copied = 0;
      {
        std::unique_lock lock(m);
        if (!range->failure.empty()) throw std::runtime_error(range->failure);
        if (range->cancelled) throw std::runtime_error("clipboard_cancelled");
        while (!range->chunks.empty() && copied < max) {
          auto& chunk = range->chunks.front(); const size_t count = std::min(max - copied, chunk.bytes.size() - chunk.used);
          std::memcpy(out + copied, chunk.bytes.data() + chunk.used, count); chunk.used += count; copied += count;
          range->consumed += count; consumed += count;
          if (chunk.used == chunk.bytes.size()) {
            const size_t whole = chunk.bytes.size(); range->lastWhole = chunk.offset + whole; range->chunks.pop_front();
            // Cumulative acknowledgement of whole consumed chunks (the window opens as Explorer takes data).
            if (!range->ended && range->ackPolicy.chunk(whole ? whole : 1)) calls.push_back({2, range, range->lastWhole});
          }
        }
        if (copied == 0) {
          if (range->ended && range->consumed >= range->size) { eof = true; }
          else if (!wait) return 0;
          else ResetEvent(range->event.value);
        }
      }
      run(calls);
      require(renderAllowed(),"clipboard_cancelled");
      if (copied > 0 || eof) return copied;
      waiting.begin();
      const auto now = std::chrono::steady_clock::now();
      if (abandoned()) throw std::runtime_error("clipboard_cancelled");
      if (now >= deadline) throw std::runtime_error("clipboard_timeout");
      const DWORD millis = DWORD(std::min<int64_t>(250, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() + 1));
      DWORD signalled = 0; HANDLE handle = range->event.value;
      // Serve COM calls of this apartment while waiting (a Release of this stream, another stream's Read).
      HRESULT woke = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS, millis, 1, &handle, &signalled);
      constexpr HRESULT timedOut = HRESULT(0x80010115L);  // RPC_E_CALL_PENDING / RPC_S_CALLPENDING: the wait timed out
      if (woke != S_OK && woke != timedOut) woke = WaitForSingleObject(handle, millis) == WAIT_OBJECT_0 ? S_OK : timedOut;  // no COM on this thread
      if (woke == S_OK) deadline = now + timeouts.idle;  // data arrived or the state changed: the idle clock restarts
    }
  }

  // ---- Blocking renders -------------------------------------------------------------------------------
  // Pulls file/item `ordinal` completely, from offset 0, handing the bytes to `sink` in order. Returns the SHA-256 the offering side's hash
  // was checked against. Runs on a render thread (no COM apartment: the waits fall back to plain handle waits).
  std::string fetch(size_t ordinal, const std::function<void(const uint8_t*, size_t)>& sink, const std::function<bool()>& abandoned) {
    require(renderAllowed(),"clipboard_cancelled");
    if (entries[fileEntry[ordinal]].size == 0) return Hash().finish();  // an empty file is never pulled (nothing could carry its hash)
    auto range = start(ordinal, 0);
    std::vector<uint8_t> buffer(maxChunkBytes);
    try {
      for (;;) {
        require(renderAllowed(),"clipboard_cancelled");
        bool eof = false; const size_t got = read(range, buffer.data(), buffer.size(), true, eof, abandoned);
        if (got) sink(buffer.data(), got);
        if (eof) break;
      }
    } catch (...) { cancel(range); throw; }
    std::lock_guard lock(m); return range->hash.finish();
  }
  // The job named `key`, started when none runs: `work` runs on its own thread and throws a clipboard_* reason to fail it.
  std::shared_ptr<RenderJob> job(const std::string& key, std::function<void(RenderJob&, const std::function<bool()>&)> work) {
    std::shared_ptr<RenderJob> created;
    {
      std::lock_guard lock(m);
      if (dead) { auto failed = std::make_shared<RenderJob>(); failed->failure = deadReason; failed->finished = true; SetEvent(failed->done.value); return failed; }
      const auto found = jobs.find(key);
      if (found != jobs.end() && !found->second->finished && !found->second->abandon) return found->second;
      created = std::make_shared<RenderJob>(); created->pasting = key == "manifest"; jobs[key] = created;
    }
    auto self = shared_from_this();
    auto runRender = [self, created, key, work = std::move(work)] {
      try { work(*created, [created] { return created->abandon.load(); }); }
      catch (const PathTooLongError&) { created->failure = "clipboard_path_too_long"; }
      catch (const std::exception& error) { created->failure = error.what(); if (created->failure.empty()) created->failure = "clipboard_transfer_failed"; }
      catch (...) { created->failure = "clipboard_transfer_failed"; }
      created->finished = true; SetEvent(created->done.value);
      std::lock_guard lock(self->m);
      const auto found = self->jobs.find(key); if (found != self->jobs.end() && found->second == created) self->jobs.erase(found);
    };
    if(renderPolicy.synchronous)runRender();else std::thread(std::move(runRender)).detach();
    return created;
  }
  // Waits on the apartment thread for a render, serving COM calls meanwhile. Returns "" on success or the clipboard_* reason it ended with.
  // Gives up when the offer is no longer the clipboard (clipboard_changed) or abortRenders() was called (its reason).
  std::string await(const std::shared_ptr<RenderJob>& render) {
    uint64_t generation, hard; { std::lock_guard lock(m); generation = abortGeneration; hard = hardAborts; }
    WaitScope waiting(this); waiting.begin();
    for (;;) {
      if (render->finished) return render->failure;
      HANDLE handle = render->done.value; DWORD signalled = 0;
      HRESULT woke = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS, 100, 1, &handle, &signalled);
      constexpr HRESULT timedOut = HRESULT(0x80010115L);
      if (woke != S_OK && woke != timedOut) WaitForSingleObject(handle, 100);
      if (render->finished) return render->failure;
      {
        std::lock_guard lock(m);
        // A paste waiting for its list outlives a newer copy (supersedeRenders, the clipboard changing); not a hard abort.
        if (hardAborts != hard) { render->abandon = true; return hardReason; }
        if (abortGeneration != generation && !render->pasting) { render->abandon = true; return abortReason; }
        if (dead) { render->abandon = true; return deadReason; }
      }
      if (render->pasting) continue;
      std::function<bool()> check; { std::lock_guard lock(m); check = isCurrent; }
      if (check) { bool current = true; try { current = check(); } catch (...) {} if (!current) { render->abandon = true; return "clipboard_changed"; } }
    }
  }
  void abortAll(const std::string& reason, bool streams, bool keepPasting = false) {
    std::vector<std::shared_ptr<RenderJob>> running; std::vector<std::shared_ptr<PullRange>> live;
    {
      std::lock_guard lock(m);
      ++abortGeneration; abortReason = reason; if (!keepPasting) { ++hardAborts; hardReason = reason; }
      if (streams) prefetched.clear();
      for (auto& entry : jobs) { if (keepPasting && entry.second->pasting) continue; entry.second->abandon = true; running.push_back(entry.second); }
      if (streams) {
        refuseReason = reason; refuseStreamsUntil = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        for (auto& entry : ranges) live.push_back(entry.second);
      }
    }
    // A job that waits for bytes wakes up on its range's event and sees `abandon`.
    for (auto& range : live) { cancel(range, std::string()); }
    std::vector<std::shared_ptr<PullRange>> woken; { std::lock_guard lock(m); if (active) woken.push_back(active); for (auto& range : queued) woken.push_back(range); for (auto& entry : ranges) woken.push_back(entry.second); }
    for (auto& range : woken) SetEvent(range->event.value);
  }

  // One data item (text, html, png) in memory, pulled on demand; a copy pulled less than hdropReuse ago is reused.
  bool itemBytes(size_t ordinal, std::vector<uint8_t>& out, std::string& failure) {
    const std::string key = "item" + std::to_string(ordinal);
    {
      std::lock_guard lock(m);
      if (dead) { failure = deadReason; return false; }
      const auto found = cache.find(key);
      if (found != cache.end()) {
        if (std::chrono::steady_clock::now() - found->second.at < hdropReuse) { out = found->second.bytes; return true; }
        cache.erase(found);
      }
    }
    const uint64_t size = entries[fileEntry[ordinal]].size;
    auto render = job(key, [self = shared_from_this(), ordinal, size](RenderJob& result, const std::function<bool()>& abandoned) {
      auto& bytes = result.blobs.emplace_back(); bytes.reserve(size_t(std::min<uint64_t>(size, 64ull * 1024 * 1024)));
      self->fetch(ordinal, [&bytes](const uint8_t* data, size_t count) { bytes.insert(bytes.end(), data, data + count); }, abandoned);
      if (bytes.size() != size) throw std::runtime_error("clipboard_transfer_failed");
    });
    failure = await(render);
    if (!failure.empty()) return false;
    std::lock_guard lock(m);
    if (dead) { failure = deadReason; return false; }
    cache[key] = Cached{std::chrono::steady_clock::now(), render->blobs.front()};
    out = render->blobs.front(); return true;
  }
  // Pulls every file into a new private transfer folder (CF_HDROP for an application that cannot take virtual files).
  void stage(RenderJob& result, const std::function<bool()>& abandoned) {
    require(!options.stagingParent.empty() && options.storage, "clipboard_busy");
    options.storage->prepare(total, {});
    TreeReceiver receiver(options.stagingParent, entries, Bounds::scale());
    size_t ordinal = 0;
    for (const auto& entry : entries) {
      if (entry.kind != "file") continue;
      uint64_t offset = 0;
      const auto digest = fetch(ordinal, [&](const uint8_t* data, size_t count) {
        receiver.append(ordinal, offset, std::vector<uint8_t>(data, data + count)); offset += count;
      }, abandoned);
      receiver.finish(ordinal, digest); result.digests.push_back(digest); ++ordinal;
    }
    require(receiver.complete(), "clipboard_transfer_failed");
    options.storage->recordTree(receiver.root(), receiver.entries());
    receiver.markCommitted();
    result.entries = receiver.entries(); result.paths = receiver.topLevel(); result.root = receiver.root();
    for (const auto& entry : result.entries) {
      if (entry.directory) continue;
      WIN32_FILE_ATTRIBUTE_DATA data{}; require(GetFileAttributesExW(longPath(entry.source).c_str(), GetFileExInfoStandard, &data) != FALSE, "clipboard_transfer_failed");
      result.files.push_back({entry.source, (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow, (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime});
    }
  }
  // A staged file is still what was left there (nobody changed, replaced or removed it since): same size and write time.
  static bool stagedFileIntact(const RenderJob::StagedFile& file) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(longPath(file.path).c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    return ((uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow) == file.size && ((uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime) == file.written;
  }
  static bool stagedIntact(const RenderJob& result) {
    for (const auto& entry : result.entries) if (entry.directory && !(GetFileAttributesW(longPath(entry.source).c_str()) & FILE_ATTRIBUTE_DIRECTORY)) return false;
    for (const auto& file : result.files) if (!stagedFileIntact(file)) return false;
    return true;
  }
  // The staged copy made for CF_HDROP serves the virtual files too: a shell that asked for CF_HDROP (Explorer always does, first) and then
  // streams the files must not pull everything a second time. The staged path of file `ordinal`, when the copy is fresh and intact.
  bool stagedFile(size_t ordinal, std::filesystem::path& path, int64_t& written) {
    std::lock_guard lock(m);
    if (dead || !staged || std::chrono::steady_clock::now() - staged->at >= hdropReuse || ordinal >= staged->result->files.size() || !stagedFileIntact(staged->result->files[ordinal])) return false;
    path = staged->result->files[ordinal].path; written = fileTimeToMs(staged->result->files[ordinal].written); return true;
  }  // The staged copy for CF_HDROP: reused within hdropReuse when intact, otherwise pulled again.
  bool stagedCopy(std::shared_ptr<RenderJob>& result, std::string& failure) {
    {
      std::lock_guard lock(m);
      if (dead) { failure = deadReason; return false; }
      if (staged && std::chrono::steady_clock::now() - staged->at < hdropReuse && stagedIntact(*staged->result)) { result = staged->result; return true; }
      staged.reset();
    }
    auto render = job("hdrop", [self = shared_from_this()](RenderJob& made, const std::function<bool()>& abandoned) { self->stage(made, abandoned); });
    failure = await(render);
    if (!failure.empty()) return false;
    std::lock_guard lock(m);
    if (dead) { failure = deadReason; return false; }
    staged = Staged{std::chrono::steady_clock::now(), render}; result = render; return true;
  }
};

// ---------------------------------------------------------------------------------------------------------

namespace {
constexpr DWORD zoneInternet = 3;  // URLZONE_INTERNET
constexpr size_t pullBufferBytes = 8u * 1024 * 1024;  // twice the largest window (4 MiB, a pull queue): only a misbehaving sender gets here
HRESULT hresultOfReason(const std::string& reason) {
  if (reason == "clipboard_timeout") return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  if (reason == "clipboard_transfer_hash_mismatch" || reason == "clipboard_hash_mismatch") return HRESULT_FROM_WIN32(ERROR_CRC);
  if (reason == "clipboard_selected_file_changed" || reason == "clipboard_selected_file_unavailable" || reason == "clipboard_file_unavailable") return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
  if (reason == "clipboard_file_locked") return HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
  if (reason == "clipboard_cancelled" || reason == "clipboard_changed") return HRESULT_FROM_WIN32(ERROR_CANCELLED);
  if (reason == "clipboard_policy_denied") return E_ACCESSDENIED;
  if (reason == "clipboard_offer_expired" || reason == "clipboard_transport_closed" || reason == "authorization_revoked") return HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED);
  return E_FAIL;
}

// One file of the offer as an IStream. Reads are served from the range being pulled; a read that does not
// continue where the range is (a Seek) cancels that range and pulls a new one from the new position. At the end of
// the file the stream waits for the offering side's SHA-256 of the range and reports an error on a mismatch.
class PullStream final : public IStream {
 public:
  PullStream(std::shared_ptr<PullOffer::Impl> impl, size_t ordinal) : impl_(std::move(impl)), ordinal_(ordinal) {
    const auto& entry = impl_->entries[impl_->fileEntry[ordinal]]; size_ = entry.size; written_ = impl_->mtimes[impl_->fileEntry[ordinal]];
  }
  ~PullStream() { release(); }
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    if (id == __uuidof(IUnknown) || id == __uuidof(IStream) || id == __uuidof(ISequentialStream)) { *out = static_cast<IStream*>(this); AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ULONG(InterlockedIncrement(&refs_)); }
  STDMETHODIMP_(ULONG) Release() override {
    const LONG left = InterlockedDecrement(&refs_);
    if (left == 0) { delete this; return 0; }
    // The consumer let go while a Read is still waiting inside this object (Explorer's cancel): wake that Read.
    if (left == readers_) { abandoned_ = true; if (range_) SetEvent(range_->event.value); }
    return ULONG(left);
  }
  STDMETHODIMP Read(void* buffer, ULONG count, ULONG* done) override {
    if (done) *done = 0;
    if (!buffer && count) return E_POINTER;
    struct Guard { PullStream* self; explicit Guard(PullStream* s) : self(s) { self->AddRef(); ++self->readers_; } ~Guard() { --self->readers_; self->Release(); } } guard(this);
    if (!failure_.empty()) return hresultOfReason(failure_);
    auto* out = static_cast<uint8_t*>(buffer); ULONG total = 0;
    try {
      while (total < count) {
        if (pos_ >= size_) {
          break;
        }
        if (!range_ || range_->consumed != pos_ || range_->cancelled) {
          drop(std::string());
          range_ = impl_->start(ordinal_, pos_);
        }
        bool eof = false; setBusy(true);
        const size_t got = [&] {
          // A pause between relay chunks is not EOF. Keep this synchronous Read pending until
          // its requested count, the verified end, cancellation or an actual error.
          try { return impl_->read(range_, out + total, count - total, true, eof, [this] { return abandoned_; }); }
          catch (...) { setBusy(false); throw; }
        }();
        if (got == 0) throw std::runtime_error("clipboard_transfer_failed");
        total += ULONG(got); pos_ += got;
      }
      // A consumer may read exactly Stat::cbSize and then release the stream, without a
      // separate EOF read. Its final successful Read must therefore include the digest ACK.
      if (count && pos_ == size_ && range_) {
        bool eof = false; setBusy(true);
        impl_->read(range_, out, 0, true, eof, [this] { return abandoned_; });
        if (!eof) throw std::runtime_error("clipboard_transfer_failed");
      }
      setBusy(false);
    } catch (const std::exception& error) {
      setBusy(false);
      failure_ = error.what();
      // A reason the range already carries was reported; a local one (timeout) is reported through the hook.
      drop(failure_ == "clipboard_timeout" ? failure_ : std::string());
      // Bytes already placed in the caller's buffer do not turn a failed or unverified
      // read into success. Report both its byte count and the failure in this same call.
      if (done) *done = total;
      return hresultOfReason(failure_);
    }
    if (done) *done = total;
    return total == count ? S_OK : S_FALSE;
  }
  STDMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
  STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
    int64_t base = 0;
    if (origin == STREAM_SEEK_SET) base = 0; else if (origin == STREAM_SEEK_CUR) base = int64_t(pos_); else if (origin == STREAM_SEEK_END) base = int64_t(size_); else return STG_E_INVALIDFUNCTION;
    // base is never negative, so only a positive move can overflow int64 (a caller-chosen LARGE_INTEGER).
    if (base < 0 || (move.QuadPart > 0 && base > INT64_MAX - move.QuadPart)) return STG_E_INVALIDFUNCTION;
    const int64_t target = base + move.QuadPart;
    if (target < 0) return STG_E_INVALIDFUNCTION;
    if (uint64_t(target) > size_) { if (result) result->QuadPart = pos_; return STG_E_INVALIDFUNCTION; }
    // A seek abandons the old range immediately. In particular, seeking to EOF
    // must not wait for a digest of data that this consumer no longer reads.
    if (uint64_t(target) != pos_) { drop(std::string()); pos_ = uint64_t(target); failure_.clear(); }
    if (result) result->QuadPart = pos_;
    return S_OK;
  }
  STDMETHODIMP SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
  STDMETHODIMP CopyTo(IStream* target, ULARGE_INTEGER count, ULARGE_INTEGER* read, ULARGE_INTEGER* written) override {
    if (!target) return STG_E_INVALIDPOINTER;
    uint64_t moved = 0, wrote = 0; std::vector<uint8_t> block(64 * 1024);
    HRESULT result = S_OK;
    while (moved < count.QuadPart) {
      ULONG got = 0; result = Read(block.data(), ULONG(std::min<uint64_t>(block.size(), count.QuadPart - moved)), &got);
      if (FAILED(result)) break;
      if (got == 0) { result = S_OK; break; }  // CopyTo(~0) copies the remainder, including EOF.
      moved += got; ULONG put = 0; result = target->Write(block.data(), got, &put); wrote += put;
      if (FAILED(result)) break;
      if (put != got) { result = STG_E_MEDIUMFULL; break; }
    }
    if (read) read->QuadPart = moved; if (written) written->QuadPart = wrote;
    return result;
  }
  STDMETHODIMP Commit(DWORD) override { return S_OK; }
  STDMETHODIMP Revert() override { return S_OK; }
  STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
  STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
  STDMETHODIMP Stat(STATSTG* stat, DWORD) override {
    if (!stat) return STG_E_INVALIDPOINTER;
    *stat = STATSTG{}; stat->type = STGTY_STREAM; stat->cbSize.QuadPart = size_; stat->grfMode = STGM_READ;
    stat->mtime = msToFileTime(written_); stat->ctime = stat->mtime; stat->atime = stat->mtime;
    return S_OK;
  }
  STDMETHODIMP Clone(IStream** clone) override {
    if (!clone) return STG_E_INVALIDPOINTER;
    auto* copy = new PullStream(impl_, ordinal_); copy->pos_ = pos_; *clone = copy; return S_OK;
  }
 private:
  void setBusy(bool busy) { std::lock_guard lock(impl_->m); if (range_) range_->busy = busy; }
  // Gives the current range up (and tells the application why, for a local failure).
  void drop(const std::string& reason) { if (range_) { impl_->cancel(range_, reason); range_.reset(); } }
  void release() { drop(std::string()); }
  std::shared_ptr<PullOffer::Impl> impl_; size_t ordinal_; uint64_t size_ = 0, pos_ = 0; int64_t written_ = 0;
  std::shared_ptr<PullRange> range_; std::string failure_; LONG refs_ = 1; LONG readers_ = 0; bool abandoned_ = false;
};

const UINT cfDescriptor() { static const UINT f = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW); return f; }
const UINT cfContents() { static const UINT f = RegisterClipboardFormatW(CFSTR_FILECONTENTS); return f; }
const UINT cfPreferred() { static const UINT f = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT); return f; }
const UINT cfZone() { static const UINT f = RegisterClipboardFormatW(CFSTR_ZONEIDENTIFIER); return f; }
const UINT cfUntrusted() { static const UINT f = RegisterClipboardFormatW(CFSTR_UNTRUSTEDDRAGDROP); return f; }
// Delayed content of the peer stays out of clipboard history, the cloud clipboard and clipboard monitors: Windows'
// history service reads every format of a new clipboard at once, which would pull the peer's files and show the transfer
// card on a copy alone, without any paste. Each is a DWORD 0; the monitor format's presence is the signal.
const UINT cfNoMonitor() { static const UINT f = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"); return f; }
const UINT cfNoHistory() { static const UINT f = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"); return f; }
const UINT cfNoCloud() { static const UINT f = RegisterClipboardFormatW(L"CanUploadToCloudClipboard"); return f; }
inline bool privacyFormat(UINT format) { return format == cfNoMonitor() || format == cfNoHistory() || format == cfNoCloud(); }

// The data object Explorer pastes from. Content is virtual: the descriptor lists every entry, FileContents hands
// out a pulling stream per file. Formats the target writes back (Performed DropEffect ...) are accepted and ignored.
struct VirtualTree {
  std::vector<Item> entries; std::vector<int64_t> mtimes, ordinalOfEntry;  // ordinal among files per entry, -1 for folders
  // A deferred offer has no list when it is published: `listing` returns it (asking for it and waiting the first time), or nullptr with
  // the reason. Unset: the fields above are the list.
  std::function<std::shared_ptr<const PullOffer::Impl::Listing>(std::string& failure)> listing;
  std::function<IStream*(size_t ordinal)> open;  // a new stream of file `ordinal` (an AddRef'd IStream), nullptr on failure
  std::function<void(bool started)> operation;  // Explorer's own copy operation started / ended (its dialog is the progress); may be empty
  std::function<void(bool started, HRESULT result)> operationState;
  // Remote offers are terminal after shutdown. Completed local trees have no status hook.
  std::function<HRESULT()> status;
  // Virtual files (FileGroupDescriptorW + FileContents) are offered; off for an offer of text or an image.
  bool virtualFiles = true;
  // FD_PROGRESSUI: Explorer shows its own copy dialog for the virtual files (off when the application shows the progress).
  bool progressUi = true;
  // Formats rendered on demand as global memory (CF_HDROP, CF_UNICODETEXT, "HTML Format", "PNG", CF_DIBV5, CF_DIB). `render` blocks until the data is
  // there and returns it (the caller frees it), or nullptr with the clipboard_* reason it failed with.
  struct Extra { CLIPFORMAT format; std::function<HGLOBAL(std::string& failure)> render; };
  std::vector<Extra> extras;
};
// Every data object of this kind that is alive: read() asks whether one of them is the clipboard (our own delayed content must never be read
// back, that would pull the peer's data without a paste).
struct LiveObjects { std::recursive_mutex m; std::set<IDataObject*> objects; };
LiveObjects& liveObjects() { static LiveObjects live; return live; }
class PullDataObject final : public IDataObject, public IDataObjectAsyncCapability {
 public:
  explicit PullDataObject(std::shared_ptr<const VirtualTree> tree) : tree_(std::move(tree)) {
    auto& live = liveObjects(); std::lock_guard lock(live.m); live.objects.insert(static_cast<IDataObject*>(this));
  }
  ~PullDataObject() { auto& live = liveObjects(); std::lock_guard lock(live.m); live.objects.erase(static_cast<IDataObject*>(this)); }
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    if (id == __uuidof(IUnknown) || id == __uuidof(IDataObject)) *out = static_cast<IDataObject*>(this);
    else if (id == __uuidof(IDataObjectAsyncCapability) && tree_->virtualFiles) *out = static_cast<IDataObjectAsyncCapability*>(this);
    else { *out = nullptr; return E_NOINTERFACE; }
    AddRef(); return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ULONG(InterlockedIncrement(&refs_)); }
  STDMETHODIMP_(ULONG) Release() override { const LONG left = InterlockedDecrement(&refs_); if (left == 0) delete this; return ULONG(left); }

  STDMETHODIMP GetData(FORMATETC* format, STGMEDIUM* medium) override {
    if (!format || !medium) return E_POINTER;
    *medium = STGMEDIUM{};
    const HRESULT query = QueryGetData(format);
    if (FAILED(query)) return query;
    for (const auto& extra : tree_->extras) {
      if (extra.format != format->cfFormat) continue;
      std::string failure; HGLOBAL memory = nullptr;
      try { memory = extra.render(failure); } catch (const std::exception& error) { failure = error.what(); } catch (...) { failure = "clipboard_transfer_failed"; }
      if (!memory) return failure == "clipboard_cancelled" || failure == "clipboard_changed" || failure == "clipboard_offer_expired" ? E_ABORT : hresultOfReason(failure.empty() ? "clipboard_transfer_failed" : failure);
      medium->tymed = TYMED_HGLOBAL; medium->hGlobal = memory; return S_OK;
    }
    if (format->cfFormat == cfContents() || format->cfFormat == cfDescriptor()) {
      // The list: fixed, or (a deferred offer) asked for now and waited for.
      std::shared_ptr<const PullOffer::Impl::Listing> held; const std::vector<Item>* entries = &tree_->entries;
      const std::vector<int64_t>* mtimes = &tree_->mtimes; const std::vector<int64_t>* ordinals = &tree_->ordinalOfEntry;
      if (tree_->listing) {
        std::string failure;
        try { held = tree_->listing(failure); } catch (const std::exception& error) { failure = error.what(); } catch (...) { failure = "clipboard_transfer_failed"; }
        if (!held) return failure == "clipboard_cancelled" || failure == "clipboard_changed" || failure == "clipboard_offer_expired" ? E_ABORT : hresultOfReason(failure.empty() ? "clipboard_transfer_failed" : failure);
        entries = &held->entries; mtimes = &held->mtimes; ordinals = &held->ordinalOfEntry;
      }
      if (format->cfFormat == cfDescriptor()) { medium->tymed = TYMED_HGLOBAL; medium->hGlobal = descriptor(*entries, *mtimes); return medium->hGlobal ? S_OK : E_OUTOFMEMORY; }
      const LONG lindex = format->lindex;
      if (lindex < 0 || size_t(lindex) >= entries->size()) return DV_E_LINDEX;
      const int64_t ordinal = (*ordinals)[size_t(lindex)];
      if (ordinal < 0) return DV_E_LINDEX;  // a folder has no contents
      medium->tymed = TYMED_ISTREAM; medium->pstm = tree_->open(size_t(ordinal));
      return medium->pstm ? S_OK : STG_E_FILENOTFOUND;
    }
    medium->tymed = TYMED_HGLOBAL;
    if (privacyFormat(format->cfFormat)) medium->hGlobal = dword(0);
    else medium->hGlobal = dword(format->cfFormat == cfPreferred() ? DWORD(DROPEFFECT_COPY) : zoneInternet);
    return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
  }
  STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
  STDMETHODIMP QueryGetData(FORMATETC* format) override {
    if (!format) return E_POINTER;
    if (tree_->status) { const HRESULT status = tree_->status(); if (FAILED(status)) return status; }
    if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    if (privacyFormat(format->cfFormat)) return (format->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_TYMED;
    for (const auto& extra : tree_->extras) if (extra.format == format->cfFormat) return (format->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_TYMED;
    if (!tree_->virtualFiles) return DV_E_FORMATETC;
    if (format->cfFormat == cfContents()) return (format->tymed & TYMED_ISTREAM) ? S_OK : DV_E_TYMED;
    if (format->cfFormat == cfDescriptor() || format->cfFormat == cfPreferred() || format->cfFormat == cfZone() || format->cfFormat == cfUntrusted())
      return (format->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_TYMED;
    return DV_E_FORMATETC;
  }
  STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override { if (out) out->ptd = nullptr; return DATA_S_SAMEFORMATETC; }
  STDMETHODIMP SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {
    if (!format || !medium) return E_POINTER;
    // What the paste target reports after copying; nothing here depends on it.
    static const wchar_t* const accepted[] = {CFSTR_PERFORMEDDROPEFFECT, CFSTR_LOGICALPERFORMEDDROPEFFECT, CFSTR_PASTESUCCEEDED, CFSTR_INDRAGLOOP, L"DropDescription", L"DragContext", L"DragImageBits", L"IsShowingText", L"IsShowingLayered", L"UsingDefaultDragImage", L"DragWindow"};
    for (const auto* name : accepted) if (format->cfFormat == RegisterClipboardFormatW(name)) { if (release) ReleaseStgMedium(medium); return S_OK; }
    return E_NOTIMPL;
  }
  STDMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
    if (!out) return E_POINTER;
    if (direction != DATADIR_GET) return E_NOTIMPL;
    std::vector<FORMATETC> formats;
    if (tree_->virtualFiles) {
      formats.push_back({CLIPFORMAT(cfDescriptor()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
      formats.push_back({CLIPFORMAT(cfContents()), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM});
      formats.push_back({CLIPFORMAT(cfPreferred()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
      formats.push_back({CLIPFORMAT(cfZone()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
      formats.push_back({CLIPFORMAT(cfUntrusted()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
    }
    for (const auto& extra : tree_->extras) formats.push_back({extra.format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
    for (const UINT privacy : {cfNoMonitor(), cfNoHistory(), cfNoCloud()}) formats.push_back({CLIPFORMAT(privacy), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
    return SHCreateStdEnumFmtEtc(UINT(formats.size()), formats.data(), out);
  }
  STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
  STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

  // Asynchronous extraction: Explorer copies on a background thread and shows its own progress and cancel button.
  STDMETHODIMP SetAsyncMode(BOOL async) override { async_ = async != FALSE; return S_OK; }
  STDMETHODIMP GetAsyncMode(BOOL* async) override { if (!async) return E_POINTER; *async = TRUE; return S_OK; }
  STDMETHODIMP StartOperation(IBindCtx*) override { if (!inOperation_) { inOperation_ = true; notify(true, S_OK); } return S_OK; }
  STDMETHODIMP InOperation(BOOL* inOperation) override { if (!inOperation) return E_POINTER; *inOperation = inOperation_ ? TRUE : FALSE; return S_OK; }
  STDMETHODIMP EndOperation(HRESULT result, IBindCtx*, DWORD) override { if (inOperation_) { inOperation_ = false; notify(false, result); } return S_OK; }

 private:
  void notify(bool started, HRESULT result) {
    if (tree_->operation) { try { tree_->operation(started); } catch (...) {} }
    if (tree_->operationState) { try { tree_->operationState(started, result); } catch (...) {} }
  }
  HGLOBAL dword(DWORD value) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (memory) { if (auto* p = static_cast<DWORD*>(GlobalLock(memory))) { *p = value; GlobalUnlock(memory); } }
    return memory;
  }
  // FILEGROUPDESCRIPTORW with every entry, folders included, as relative paths (parents first).
  HGLOBAL descriptor(const std::vector<Item>& entries, const std::vector<int64_t>& mtimes) {
    const SIZE_T bytes = sizeof(UINT) + entries.size() * sizeof(FILEDESCRIPTORW);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes); if (!memory) return nullptr;
    auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(memory)); if (!group) { GlobalFree(memory); return nullptr; }
    group->cItems = UINT(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
      FILEDESCRIPTORW& d = group->fgd[i]; const Item& item = entries[i]; const bool dir = item.kind == "dir";
      d.dwFlags = FD_ATTRIBUTES | FD_WRITESTIME | (tree_->progressUi ? FD_PROGRESSUI : 0) | (dir ? 0 : FD_FILESIZE);
      d.dwFileAttributes = dir ? (FILE_ATTRIBUTE_DIRECTORY | item.attributes) : (item.attributes ? item.attributes : FILE_ATTRIBUTE_NORMAL);
      d.ftLastWriteTime = msToFileTime(mtimes[i]);
      if (!dir) { d.nFileSizeHigh = DWORD(item.size >> 32); d.nFileSizeLow = DWORD(item.size & 0xffffffffu); }
      wcsncpy_s(d.cFileName, item.name.c_str(), _TRUNCATE);
    }
    GlobalUnlock(memory); return memory;
  }
  std::shared_ptr<const VirtualTree> tree_; LONG refs_ = 1; bool async_ = true, inOperation_ = false;
};
}

namespace {
IStream* openLocalStream(const std::filesystem::path& path, int64_t written);   // (defined with LocalFileStream below)
// Global memory from bytes (the caller of GetData frees it).
HGLOBAL globalFrom(const void* data, size_t bytes) {
  Memory memory(bytes ? bytes : 1); if (bytes) memory.put(data, bytes); return memory.release();
}
// CF_HDROP of the staged top-level items.
HGLOBAL makeDropFiles(const std::vector<std::filesystem::path>& paths) {
  require(!paths.empty() && paths.size() <= maxEntries, "clipboard_file_count_limit");
  size_t units = 1; for (const auto& path : paths) units += path.wstring().size() + 1;
  require(units * sizeof(wchar_t) <= maxListBytes, "clipboard_file_list_invalid");
  Memory memory(sizeof(DROPFILES) + units * sizeof(wchar_t));
  DROPFILES header{}; header.pFiles = sizeof(DROPFILES); header.fWide = TRUE; memory.put(&header, sizeof(header));
  size_t offset = sizeof(header);
  for (const auto& path : paths) { const auto value = path.wstring(); memory.put(value.c_str(), (value.size() + 1) * sizeof(wchar_t), offset); offset += (value.size() + 1) * sizeof(wchar_t); }
  return memory.release();
}
std::wstring wideOfUtf8(const std::vector<uint8_t>& utf8) {
  if (utf8.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(utf8.data()), int(utf8.size()), nullptr, 0);
  require(count > 0, "clipboard_invalid_unicode");
  std::wstring text(size_t(count), L'\0'); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(utf8.data()), int(utf8.size()), text.data(), count);
  require(text.find(L'\0') == std::wstring::npos && text.size() <= maxTextUnits, "clipboard_text_too_large");
  return text;
}
}

PullOffer::PullOffer() = default;
std::shared_ptr<PullOffer> PullOffer::create(std::string offerId, std::vector<Item> entries, std::vector<int64_t> mtimeMs, PullHooks hooks, PullTimeouts timeouts, PullOptions options) {
  require(!offerId.empty() && !entries.empty() && entries.size() <= maxEntries && mtimeMs.size() == entries.size(), "clipboard_entry_count_limit");
  std::shared_ptr<PullOffer> offer(new PullOffer()); offer->impl_ = std::make_shared<PullOffer::Impl>(); auto& s = *offer->impl_;
  s.setEntries(std::move(entries), std::move(mtimeMs));
  s.id = std::move(offerId); s.hooks = std::move(hooks); s.timeouts = timeouts; s.options = options;
  // CF_HDROP as well, for applications that cannot take virtual files: only for a small set whose staged paths stay below MAX_PATH.
  if (!options.stagingParent.empty() && options.storage && s.total <= hdropCompatBytes && s.entries.size() <= hdropCompatEntries) {
    const size_t base = options.stagingParent.wstring().size() + 1 + 41 + 1;  // <parent>\transfer-<32 hex>\<relative>
    s.hdropOffered = true;
    for (const auto& entry : s.entries) if (base + entry.name.size() >= maxPathLegacy) { s.hdropOffered = false; break; }
  }
  auto tree = std::make_shared<VirtualTree>(); tree->entries = offer->impl_->entries; tree->mtimes = offer->impl_->mtimes; tree->ordinalOfEntry = offer->impl_->ordinalOfEntry;
  tree->status = [impl = offer->impl_]() -> HRESULT {
    std::lock_guard lock(impl->m);
    return impl->dead ? hresultOfReason(impl->deadReason) : S_OK;
  };
  tree->progressUi = options.explorerProgress;
  tree->open = [impl = offer->impl_](size_t ordinal) -> IStream* {
    std::filesystem::path local; int64_t written = 0;
    if (impl->stagedFile(ordinal, local, written)) if (IStream* stream = openLocalStream(local, written)) return stream;
    return new PullStream(impl, ordinal);
  };
  tree->operation = [impl = offer->impl_](bool started) { if (impl->hooks.operation) impl->hooks.operation(started); };
  tree->operationState = [impl = offer->impl_](bool started, HRESULT result) { if (impl->hooks.operationState) impl->hooks.operationState(started, result); };
  if (s.hdropOffered) {
    tree->extras.push_back({CF_HDROP, [impl = offer->impl_](std::string& failure) -> HGLOBAL {
      std::shared_ptr<RenderJob> staged; if (!impl->stagedCopy(staged, failure)) return nullptr;
      return makeDropFiles(staged->paths);
    }});
  }
  offer->object_ = new PullDataObject(tree);
  return offer;
}
std::shared_ptr<PullOffer> PullOffer::createDeferred(std::string offerId, PullHooks hooks, PullTimeouts timeouts, PullOptions options) {
  require(!offerId.empty() && hooks.manifest, "clipboard_transfer_failed");
  std::shared_ptr<PullOffer> offer(new PullOffer()); offer->impl_ = std::make_shared<PullOffer::Impl>(); auto& s = *offer->impl_;
  s.id = std::move(offerId); s.hooks = std::move(hooks); s.timeouts = timeouts; s.options = options;
  s.deferred = true; s.listed = false;  // never CF_HDROP: a deferred copy is larger than the CF_HDROP bounds
  auto tree = std::make_shared<VirtualTree>();
  tree->listing = [impl = offer->impl_](std::string& failure) { return impl->manifest(failure); };
  tree->status = [impl = offer->impl_]() -> HRESULT {
    std::lock_guard lock(impl->m);
    return impl->dead ? hresultOfReason(impl->deadReason) : S_OK;
  };
  tree->progressUi = options.explorerProgress;
  tree->open = [impl = offer->impl_](size_t ordinal) -> IStream* {
    { std::lock_guard lock(impl->m); if (!impl->listed || ordinal >= impl->fileEntry.size()) return nullptr; }
    return new PullStream(impl, ordinal);
  };
  tree->operation = [impl = offer->impl_](bool started) { if (impl->hooks.operation) impl->hooks.operation(started); };
  tree->operationState = [impl = offer->impl_](bool started, HRESULT result) { if (impl->hooks.operationState) impl->hooks.operationState(started, result); };
  offer->object_ = new PullDataObject(tree);
  return offer;
}
std::string PullOffer::deliverManifest(std::vector<Item> entries, std::vector<int64_t> mtimeMs) {
  auto& s = *impl_;
  std::lock_guard lock(s.m);
  if (!s.deferred || s.listed || s.dead) return {};
  try {
    s.setEntries(std::move(entries), std::move(mtimeMs));
    s.listing = std::make_shared<const Impl::Listing>(Impl::Listing{s.entries, s.mtimes, s.ordinalOfEntry});
    s.listed = true;
  } catch (const PathTooLongError&) { s.manifestFailure = "clipboard_path_too_long"; }
  catch (const std::exception& error) { s.manifestFailure = error.what(); if (s.manifestFailure.empty()) s.manifestFailure = "clipboard_transfer_failed"; }
  SetEvent(s.manifestEvent.value);
  return s.listed ? std::string() : s.manifestFailure;
}
void PullOffer::failManifest(const std::string& reason) {
  auto& s = *impl_;
  std::lock_guard lock(s.m);
  if (!s.deferred || s.listed) return;
  s.manifestFailure = reason.empty() ? std::string("clipboard_transfer_failed") : reason; SetEvent(s.manifestEvent.value);
}
void PullOffer::manifestProgress() { auto& s = *impl_; std::lock_guard lock(s.m); s.manifestHeard = std::chrono::steady_clock::now(); }
bool PullOffer::deferredPending() const { std::lock_guard lock(impl_->m); return impl_->deferred && !impl_->listed; }
std::shared_ptr<PullOffer> PullOffer::createContent(std::string offerId, const std::string& format, std::vector<Item> items, PullHooks hooks, PullTimeouts timeouts, PullRenderPolicy renderPolicy) {
  require(!renderPolicy.synchronous||bool(renderPolicy.allowed),"clipboard_transfer_failed");
  require(!offerId.empty() && !items.empty() && items.size() <= 3, "clipboard_entry_count_limit");
  if (format == "text") require(items.size() == 1 && items[0].kind == "text", "clipboard_selected_file_unavailable");
  else if (format == "richtext") require(items[0].kind == "text" && (items.size() == 1 || (items.size() == 2 && items[1].kind == "html" && items[1].size >= 1)), "clipboard_selected_file_unavailable");
  else if (format == "image") require(items.size() == 1 && items[0].kind == "png" && items[0].size >= 1 && items[0].size <= maxImageBytes && items[0].width && items[0].height &&
                                       uint64_t(items[0].width) * items[0].height <= maxImagePixels, "clipboard_image_too_large");
  else throw std::runtime_error("unsupported_clipboard_format");
  std::shared_ptr<PullOffer> offer(new PullOffer()); offer->impl_ = std::make_shared<PullOffer::Impl>(); auto& s = *offer->impl_;
  for (size_t i = 0; i < items.size(); ++i) {
    if (format != "image") require(items[i].size <= (items[i].kind == "text" ? maxTextBytes : maxRichBytes), "clipboard_text_too_large");
    s.total += items[i].size; s.fileEntry.push_back(i); s.ordinalOfEntry.push_back(int64_t(i)); s.mtimes.push_back(0);
  }
  s.id = std::move(offerId); s.format = format; s.entries = std::move(items); s.hooks = std::move(hooks); s.timeouts = timeouts; s.renderPolicy = std::move(renderPolicy);
  auto tree = std::make_shared<VirtualTree>(); tree->virtualFiles = false;
  auto impl = offer->impl_;
  tree->status = [impl]() -> HRESULT {
    std::lock_guard lock(impl->m);
    return impl->dead ? hresultOfReason(impl->deadReason) : S_OK;
  };
  if (format == "text" || format == "richtext") {
    tree->extras.push_back({CF_UNICODETEXT, [impl](std::string& failure) -> HGLOBAL {
      std::vector<uint8_t> utf8; if (!impl->itemBytes(0, utf8, failure)) return nullptr;
      const std::wstring text = wideOfUtf8(utf8);
      return globalFrom(text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    }});
  }
  if (format == "richtext" && s.entries.size() == 2) {
    tree->extras.push_back({CLIPFORMAT(RegisterClipboardFormatW(L"HTML Format")), [impl](std::string& failure) -> HGLOBAL {
      std::vector<uint8_t> html; if (!impl->itemBytes(1, html, failure)) return nullptr;
      auto clean = sanitizeHtmlFormat(html); clean.push_back(0);   // the peer is not trusted: its HTML is re-serialised, as for a pushed copy
      return globalFrom(clean.data(), clean.size());
    }});
  }
  if (format == "image") {
    const uint32_t width = s.entries[0].width, height = s.entries[0].height;
    tree->extras.push_back({CLIPFORMAT(RegisterClipboardFormatW(L"PNG")), [impl](std::string& failure) -> HGLOBAL {
      std::vector<uint8_t> png; if (!impl->itemBytes(0, png, failure)) return nullptr;
      return globalFrom(png.data(), png.size());
    }});
    // The bitmap formats are decoded from the pulled PNG on the render thread (WIC, up to 64 megapixels), not on the apartment thread.
    const auto bitmap = [impl, width, height](bool v5) {
      return [impl, width, height, v5](std::string& failure) -> HGLOBAL {
        auto render = impl->job(v5 ? "dibv5" : "dib", [impl, width, height](RenderJob& made, const std::function<bool()>& abandoned) {
          std::vector<uint8_t> png; png.reserve(size_t(std::min<uint64_t>(impl->entries[0].size, 64ull * 1024 * 1024)));
          impl->fetch(0, [&png](const uint8_t* data, size_t count) { png.insert(png.end(), data, data + count); }, abandoned);
          if (png.size() != impl->entries[0].size) throw std::runtime_error("clipboard_transfer_failed");
          made.blobs.push_back(decodePngToBgra(png, width, height));
        });
        failure = impl->await(render);
        if (!failure.empty()) return nullptr;
        return v5 ? makeDibV5(render->blobs.front(), width, height) : makeDib24(render->blobs.front(), width, height);
      };
    };
    tree->extras.push_back({CF_DIBV5, bitmap(true)});
    tree->extras.push_back({CF_DIB, bitmap(false)});
  }
  offer->object_ = new PullDataObject(tree);
  return offer;
}
PullOffer::~PullOffer() {
  if (impl_) shutdown("clipboard_offer_expired");  // the application no longer serves this offer
  if (object_) object_->Release();
}
const std::string& PullOffer::format() const { return impl_->format; }
void PullOffer::setCurrentCheck(std::function<bool()> current) { std::lock_guard lock(impl_->m); impl_->isCurrent = std::move(current); }
void PullOffer::abortRenders(const std::string& reason, bool streams) { impl_->abortAll(reason.empty() ? std::string("clipboard_cancelled") : reason, streams); }
void PullOffer::supersedeRenders(const std::string& reason) { impl_->abortAll(reason.empty() ? std::string("clipboard_offer_expired") : reason, false, true); }
bool PullOffer::rendering() const { std::lock_guard lock(impl_->m); return !impl_->jobs.empty(); }const std::string& PullOffer::id() const { return impl_->id; }
size_t PullOffer::fileCount() const { return impl_->fileEntry.size(); }
uint64_t PullOffer::totalBytes() const { return impl_->total; }
IDataObject* PullOffer::dataObject() { object_->AddRef(); return object_; }
uint64_t PullOffer::deliveredBytes() const { std::lock_guard lock(impl_->m); return impl_->delivered; }
uint64_t PullOffer::consumedBytes() const { std::lock_guard lock(impl_->m); return impl_->consumed; }
bool PullOffer::reading() const { std::lock_guard lock(impl_->m); return impl_->readers > 0; }

void PullOffer::deliver(uint64_t rangeId, uint64_t offset, const std::vector<uint8_t>& bytes) {
  auto& s = *impl_; std::vector<Impl::Call> calls;
  {
    std::lock_guard lock(s.m);
    const auto found = s.ranges.find(rangeId);
    if (found == s.ranges.end()) return;  // data of a range that was cancelled or finished meanwhile
    const auto range = found->second;
    if (range->cancelled || range->ended || !range->failure.empty()) return;
    if (bytes.empty() || bytes.size() > maxChunkBytes || offset != range->next || bytes.size() > range->size - range->next) s.failRange(range, "clipboard_chunk_out_of_order_or_bounds", calls, true);
    else if (range->next - range->consumed + bytes.size() > pullBufferBytes) s.failRange(range, "clipboard_transfer_failed", calls, true);
    else {
      range->hash.add(bytes.data(), bytes.size());
      range->chunks.push_back({offset, bytes, 0}); range->next += bytes.size(); s.delivered += bytes.size(); s.wake(*range);
    }
  }
  s.run(calls);
}
void PullOffer::finish(uint64_t rangeId, const std::string& sha256) {
  auto& s = *impl_; std::vector<Impl::Call> calls;
  {
    std::lock_guard lock(s.m);
    const auto found = s.ranges.find(rangeId);
    if (found == s.ranges.end()) return;
    const auto range = found->second;
    if (range->cancelled || range->ended || !range->failure.empty()) return;
    if (range->next != range->size) s.failRange(range, "clipboard_transfer_failed", calls, true);
    else if (range->hash.finish() != sha256) s.failRange(range, "clipboard_transfer_hash_mismatch", calls, true);
    else {
      range->ended = true; s.ranges.erase(rangeId); s.retire(range, calls); s.wake(*range);
      // A file somebody read from its start is complete: the next one is asked for now. A prefetch nobody has taken over starts none,
      // and neither does a range a seek started (that reader is not copying the files in order).
      if (!s.isPrefetched(range) && range->start == 0) s.prefetchAfter(range->index, calls);
    }
  }
  s.run(calls);
}
void PullOffer::fail(uint64_t rangeId, const std::string& reason) {
  auto& s = *impl_; std::vector<Impl::Call> calls;
  {
    std::lock_guard lock(s.m);
    const auto found = s.ranges.find(rangeId);
    if (found == s.ranges.end()) return;
    s.failRange(found->second, reason.empty() ? std::string("clipboard_transfer_failed") : reason, calls, false);
  }
  s.run(calls);
}
void PullOffer::shutdown(const std::string& reason) {
  auto& s = *impl_;
  std::lock_guard lock(s.m);
  s.dead = true; s.deadReason = reason.empty() ? std::string("clipboard_offer_expired") : reason;
  for (auto& entry : s.ranges) { auto& range = *entry.second; if (range.failure.empty() && !range.ended) { range.failure = s.deadReason; range.chunks.clear(); } SetEvent(range.event.value); }
  s.ranges.clear(); s.queued.clear(); s.active.reset(); s.prefetched.clear();
  for (auto& entry : s.jobs) entry.second->abandon = true;   // renders waiting for data end with the reason
}

namespace {
// Puts a data object on the clipboard with OleSetClipboard (never OleFlushClipboard) and marks the sequence as this process's own.
DWORD publishDataObject(IDataObject* object, std::optional<DWORD> expected) {
  // Other listeners hold the clipboard briefly after a change. Every attempt
  // rechecks the expected sequence so a copy made while waiting is preserved.
  clearWriteFailure();
  const HRESULT result = detail::retryPublication(expected, writeBudgetMs,
      [] { return GetClipboardSequenceNumber(); }, [object] { return OleSetClipboard(object); },
      [](unsigned milliseconds) { Sleep(milliseconds); });
  if (FAILED(result)) noteWin32(DWORD(result), result == CLIPBRD_E_CANT_OPEN ? clipboardHolder() : 0);
  require(result != CLIPBRD_E_CANT_OPEN, "clipboard_busy");
  require(SUCCEEDED(result), "clipboard_write_failed");
  const DWORD sequence = GetClipboardSequenceNumber();
  ownPullSequence.store(sequence);  // read() never stages this process's own virtual files back out of the clipboard
  return sequence;
}
}
DWORD publishPullOffer(const std::shared_ptr<PullOffer>& offer, std::optional<DWORD> expected) {
  IDataObject* object = offer->dataObject();
  struct Release { IDataObject* object; ~Release() { object->Release(); } } release{object};
  // A blocking render gives up as soon as another copy replaced this offer on the clipboard.
  offer->setCurrentCheck([weak = std::weak_ptr<PullOffer>(offer)] {
    const auto live = weak.lock(); if (!live) return false;
    IDataObject* data = live->dataObject(); const bool current = OleIsCurrentClipboard(data) == S_OK; data->Release(); return current;
  });
  return publishDataObject(object, expected);
}
bool pullOfferIsCurrent(const std::shared_ptr<PullOffer>& offer) {
  IDataObject* object = offer->dataObject(); const bool current = OleIsCurrentClipboard(object) == S_OK; object->Release(); return current;
}
DWORD withdrawPullOffer(const std::shared_ptr<PullOffer>& offer) {
  if (!pullOfferIsCurrent(offer)) return 0;
  if (OleSetClipboard(nullptr) != S_OK) return 0;
  const DWORD sequence = GetClipboardSequenceNumber(); ownPullSequence.store(sequence); return sequence;
}
bool ownDelayedContentIsCurrent() {
  auto& live = liveObjects(); std::lock_guard lock(live.m);
  const std::vector<IDataObject*> objects(live.objects.begin(), live.objects.end());
  for (IDataObject* object : objects) if (OleIsCurrentClipboard(object) == S_OK) return true;
  return false;
}

// ---------------------------------------------------------------------------------------------------------
// A received folder tree served from its local copy (receiver placement for absolute paths of MAX_PATH or more)

namespace {
// A read-only IStream over a local file (the private transfer folder), by extended-length path.
class LocalFileStream final : public IStream {
 public:
  static LocalFileStream* open(const std::filesystem::path& path, int64_t written) {
    const HANDLE file = CreateFileW(longPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) { CloseHandle(file); return nullptr; }
    return new LocalFileStream(file, path, uint64_t(size.QuadPart), written);
  }
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    if (id == __uuidof(IUnknown) || id == __uuidof(IStream) || id == __uuidof(ISequentialStream)) { *out = static_cast<IStream*>(this); AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ULONG(InterlockedIncrement(&refs_)); }
  STDMETHODIMP_(ULONG) Release() override { const LONG left = InterlockedDecrement(&refs_); if (left == 0) delete this; return ULONG(left); }
  STDMETHODIMP Read(void* buffer, ULONG count, ULONG* done) override {
    if (done) *done = 0;
    if (!buffer && count) return E_POINTER;
    DWORD got = 0;
    if (count && !ReadFile(file_, buffer, count, &got, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    if (done) *done = got;
    return got == count ? S_OK : S_FALSE;
  }
  STDMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
  STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
    if (origin != STREAM_SEEK_SET && origin != STREAM_SEEK_CUR && origin != STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
    LARGE_INTEGER now{};
    if (!SetFilePointerEx(file_, move, &now, origin == STREAM_SEEK_SET ? FILE_BEGIN : origin == STREAM_SEEK_CUR ? FILE_CURRENT : FILE_END)) return STG_E_INVALIDFUNCTION;
    if (result) result->QuadPart = uint64_t(now.QuadPart);
    return S_OK;
  }
  STDMETHODIMP SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
  STDMETHODIMP CopyTo(IStream* target, ULARGE_INTEGER count, ULARGE_INTEGER* read, ULARGE_INTEGER* written) override {
    if (!target) return STG_E_INVALIDPOINTER;
    uint64_t moved = 0, wrote = 0; std::vector<uint8_t> block(64 * 1024); HRESULT result = S_OK;
    while (moved < count.QuadPart) {
      ULONG got = 0; result = Read(block.data(), ULONG(std::min<uint64_t>(block.size(), count.QuadPart - moved)), &got);
      if (FAILED(result)) break;
      if (got == 0) { result = S_OK; break; }
      moved += got; ULONG put = 0; result = target->Write(block.data(), got, &put); wrote += put;
      if (FAILED(result)) break;
      if (put != got) { result = STG_E_MEDIUMFULL; break; }
    }
    if (read) read->QuadPart = moved; if (written) written->QuadPart = wrote;
    return result;
  }
  STDMETHODIMP Commit(DWORD) override { return S_OK; }
  STDMETHODIMP Revert() override { return S_OK; }
  STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
  STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
  STDMETHODIMP Stat(STATSTG* stat, DWORD) override {
    if (!stat) return STG_E_INVALIDPOINTER;
    *stat = STATSTG{}; stat->type = STGTY_STREAM; stat->cbSize.QuadPart = size_; stat->grfMode = STGM_READ;
    stat->mtime = msToFileTime(written_); stat->ctime = stat->mtime; stat->atime = stat->mtime;
    return S_OK;
  }
  STDMETHODIMP Clone(IStream** clone) override {
    if (!clone) return STG_E_INVALIDPOINTER;
    auto* copy = open(path_, written_); if (!copy) return STG_E_FILENOTFOUND;
    LARGE_INTEGER here{}, now{}; SetFilePointerEx(file_, here, &now, FILE_CURRENT); SetFilePointerEx(copy->file_, now, nullptr, FILE_BEGIN);
    *clone = copy; return S_OK;
  }
 private:
  LocalFileStream(HANDLE file, std::filesystem::path path, uint64_t size, int64_t written) : file_(file), path_(std::move(path)), size_(size), written_(written) {}
  ~LocalFileStream() { CloseHandle(file_); }
  HANDLE file_; std::filesystem::path path_; uint64_t size_; int64_t written_; LONG refs_ = 1;
};

IStream* openLocalStream(const std::filesystem::path& path, int64_t written) { return LocalFileStream::open(path, written); }
struct LiveLocalTree { std::mutex mutex; IDataObject* object = nullptr; std::filesystem::path root; };
LiveLocalTree& liveLocalTree() { static LiveLocalTree live; return live; }
int64_t writeTimeMs(const std::filesystem::path& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(longPath(path).c_str(), GetFileExInfoStandard, &data)) return fileTimeToMs(0);
  return fileTimeToMs((uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime);
}
}

bool treeNeedsVirtual(const std::filesystem::path& root, const std::vector<TreeEntry>& entries) {
  const size_t base = root.wstring().size();
  for (const auto& entry : entries) if (base + 1 + entry.relative.size() >= maxPathLegacy) return true;
  return false;
}
bool localTreeSupported(const std::vector<TreeEntry>& entries) {
  for (const auto& entry : entries) if (entry.relative.empty() || entry.relative.size() > maxDescriptorChars) return false;
  return !entries.empty() && entries.size() <= maxEntries;
}
IDataObject* createLocalTreeObject(const std::vector<TreeEntry>& entries) {
  require(localTreeSupported(entries), "clipboard_path_too_long");
  auto tree = std::make_shared<VirtualTree>(); auto paths = std::make_shared<std::vector<std::filesystem::path>>(); auto written = std::make_shared<std::vector<int64_t>>();
  for (const auto& entry : entries) {
    Item item; item.kind = entry.directory ? "dir" : "file"; item.name = entry.relative; item.size = entry.directory ? 0 : entry.size; item.attributes = entry.attributes & transferableAttributes;
    tree->mtimes.push_back(writeTimeMs(entry.source));
    if (entry.directory) tree->ordinalOfEntry.push_back(-1);
    else { tree->ordinalOfEntry.push_back(int64_t(paths->size())); paths->push_back(entry.source); written->push_back(tree->mtimes.back()); }
    tree->entries.push_back(std::move(item));
  }
  tree->open = [paths, written](size_t ordinal) -> IStream* { return ordinal < paths->size() ? LocalFileStream::open((*paths)[ordinal], (*written)[ordinal]) : nullptr; };
  return new PullDataObject(tree);
}
DWORD publishLocalTree(const std::vector<TreeEntry>& entries, std::optional<DWORD> expectedSequence) {
  IDataObject* object = createLocalTreeObject(entries);
  DWORD sequence = 0;
  try { sequence = publishDataObject(object, expectedSequence); } catch (...) { object->Release(); throw; }
  auto& live = liveLocalTree(); IDataObject* previous = nullptr;
  {
    std::lock_guard lock(live.mutex); previous = live.object; live.object = object;
    live.root = entries.front().source.parent_path();  // entries.front() is a top-level entry of the transfer folder
  }
  if (previous) previous->Release();
  return sequence;
}
bool localTreeIsCurrent() {
  auto& live = liveLocalTree(); std::lock_guard lock(live.mutex);
  return live.object && OleIsCurrentClipboard(live.object) == S_OK;
}
bool localTreeHolds(const std::filesystem::path& transferRoot) {
  auto& live = liveLocalTree(); std::lock_guard lock(live.mutex);
  return live.object && live.root == transferRoot && OleIsCurrentClipboard(live.object) == S_OK;
}
