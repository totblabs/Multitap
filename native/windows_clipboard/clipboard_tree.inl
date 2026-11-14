// Included by clipboard.cpp inside namespace multitap::clipboard: folder trees, and the Outgoing / Incoming
// transfer objects of the formats profile (rich text, images, folder trees).

std::wstring treePathFromWire(const std::wstring& wire, size_t pathChars, size_t depthMax) {
  require(!wire.empty(), "clipboard_selected_file_unavailable");
  require(wire.size() <= pathChars, "clipboard_path_too_long");
  std::wstring out; size_t depth = 0, start = 0;
  for (;;) {
    const auto slash = wire.find(L'/', start);
    const auto part = wire.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start);
    require(part.size() <= maxComponentChars, "clipboard_path_too_long");
    require(safeLeaf(part), "clipboard_selected_file_unavailable");
    require(++depth <= depthMax, "clipboard_depth_limit");
    if (!out.empty()) out += L'\\';
    out += part;
    if (slash == std::wstring::npos) break;
    start = slash + 1;
  }
  return out;
}

namespace {
constexpr DWORD transferableAttributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;
std::wstring folded(std::wstring text) { if (!text.empty()) CharUpperBuffW(text.data(), DWORD(text.size())); return text; }

struct TreeWalker {
  const Bounds& bounds; TreeScan scan; const TreeScanControl* control = nullptr; size_t reported = 0;
  explicit TreeWalker(const Bounds& bounds, const TreeScanControl* control = nullptr) : bounds(bounds), control(control) {}
  // The probe's stop (a copy that is not small) and the progress callback; `listed` counts a folder's children read so far.
  void step(size_t listed = 0) {
    if (!control) return;
    if (control->stopAboveEntries && scan.entries.size() + listed > control->stopAboveEntries) throw TreeExceeds();
    if (control->stopAboveBytes && scan.bytes > control->stopAboveBytes) throw TreeExceeds();
    if (control->progress && scan.entries.size() >= reported + 256) { reported = scan.entries.size(); control->progress(reported); }
  }
  void add(TreeEntry entry) {
    require(scan.entries.size() < bounds.entries, "clipboard_entry_count_limit");
    scan.entries.push_back(std::move(entry));
    step();
  }
  void file(const std::filesystem::path& source, const std::wstring& relative, const WIN32_FIND_DATAW& data) {
    const uint64_t size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    require(size <= bounds.fileBytes, "clipboard_file_name_or_size_limit");
    require(size <= bounds.totalBytes - scan.bytes, "clipboard_total_size_limit");
    scan.bytes += size; ++scan.files;
    add({relative, source, false, size, data.dwFileAttributes & transferableAttributes});
  }
  // `relative` is the entry's path inside the transfer; `depth` its component count.
  void folder(const std::filesystem::path& source, const std::wstring& relative, size_t depth, DWORD attributes) {
    require(depth <= bounds.depth, "clipboard_depth_limit");
    add({relative, source, true, 0, attributes & transferableAttributes});
    struct Child { std::wstring name; WIN32_FIND_DATAW data; };
    std::vector<Child> children; WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileExW(longPath(source / L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      require(error == ERROR_FILE_NOT_FOUND, error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED ? "clipboard_file_locked" : "clipboard_selected_file_unavailable");
      return;
    }
    struct Closer { HANDLE h; ~Closer() { FindClose(h); } } closer{find};
    do {
      const std::wstring name = data.cFileName;
      if (name == L"." || name == L"..") continue;
      require(children.size() < bounds.entries, "clipboard_entry_count_limit");
      children.push_back({name, data});
      if ((children.size() & 255) == 0) {
        step(children.size());
        // A very large folder takes a while to list: the callback still runs (cancel, keep-alive) while it does.
        if (control && control->progress && (children.size() & 4095) == 0) control->progress(scan.entries.size());
      }
    } while (FindNextFileW(find, &data));
    if (control && control->progress) control->progress(scan.entries.size());
    std::sort(children.begin(), children.end(), [](const Child& a, const Child& b) { return folded(a.name) < folded(b.name); });
    for (const auto& child : children) {
      // A link (symbolic link, junction, mount point) is neither followed nor sent.
      if (child.data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ++scan.skipped; continue; }
      // A name Windows cannot create at a paste destination (AUX.txt, "name." made in WSL or on a NAS) is
      // left out and counted like a link, not the whole copy refused.
      if (!safeLeaf(child.name)) { ++scan.skipped; continue; }
      const std::wstring childRelative = relative + L'\\' + child.name;
      scan.longestPath = std::max(scan.longestPath, childRelative.size());
      if (child.data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) folder(source / child.name, childRelative, depth + 1, child.data.dwFileAttributes);
      else file(source / child.name, childRelative, child.data);
    }
  }
};
}

TreeScan scanTree(const std::vector<std::filesystem::path>& roots, const Bounds& bounds) { return scanTree(roots, bounds, TreeScanControl{}); }
bool treeFitsWithin(const std::vector<std::filesystem::path>& roots, const Bounds& bounds, size_t entries, uint64_t bytes) {
  TreeScanControl control; control.stopAboveEntries = entries; control.stopAboveBytes = bytes;
  try { const auto scan = scanTree(roots, bounds, control); return scan.entries.size() <= entries && scan.bytes <= bytes; }
  catch (const TreeExceeds&) { return false; }
  catch (...) { return true; }
}
TreeScan scanTree(const std::vector<std::filesystem::path>& roots, const Bounds& bounds, const TreeScanControl& control) {
  require(!roots.empty(), "clipboard_empty");
  TreeWalker walker(bounds, &control); std::set<std::wstring> tops;
  for (const auto& root : roots) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    require(root.is_absolute() && GetFileAttributesExW(longPath(root).c_str(), GetFileExInfoStandard, &data), "clipboard_selected_file_unavailable");
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ++walker.scan.skipped; continue; }
    auto name = root.filename().wstring();
    if (!safeLeaf(name)) { ++walker.scan.skipped; continue; }
    // The same leaf from different folders keeps both: "name (2).ext".
    for (int copy = 2; tops.count(folded(name)); ++copy) {
      const auto stem = root.stem().wstring(), extension = root.extension().wstring();
      name = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? root.filename().wstring() + L" (" + std::to_wstring(copy) + L")"
                                                                  : stem + L" (" + std::to_wstring(copy) + L")" + extension;
      require(copy < 1000, "clipboard_selected_file_unavailable");
    }
    require(safeLeaf(name), "clipboard_selected_file_unavailable");
    walker.scan.longestPath = std::max(walker.scan.longestPath, name.size());
    tops.insert(folded(name)); ++walker.scan.tops;
    require(walker.scan.tops <= bounds.tops, "clipboard_file_count_limit");
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) walker.folder(root, name, 1, data.dwFileAttributes);
    else {
      WIN32_FIND_DATAW find{}; find.dwFileAttributes = data.dwFileAttributes; find.nFileSizeHigh = data.nFileSizeHigh; find.nFileSizeLow = data.nFileSizeLow;
      walker.file(root, name, find);
    }
  }
  require(!walker.scan.entries.empty(), "unsupported_clipboard_format");
  // Checked after the whole walk so that the message can name the longest path: a path beyond the profile's limit, or beyond what
  // Explorer can create at a paste destination (maxPasteChars), refuses the copy before any byte is sent.
  const size_t limit = std::min(bounds.pathChars, maxPasteChars);
  if (walker.scan.longestPath > limit) throw PathTooLongError(walker.scan.longestPath, limit);
  return std::move(walker.scan);
}

bool expireTree(const std::filesystem::path& parent, const std::filesystem::path& root, const std::vector<TreeEntry>& nodes) {
  try {
    require(!nodes.empty() && nodes.size() <= maxEntries, "clipboard_expiry_invalid");
    existingDirectoryChain(parent);
    const auto name = root.filename().wstring();
    require(root.is_absolute() && root.parent_path() == parent && name.size() == 41 && name.rfind(L"transfer-", 0) == 0 &&
      std::all_of(name.begin() + 9, name.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }), "clipboard_expiry_invalid");
    for (const auto& node : nodes) {
      std::wstring wire = node.relative; std::replace(wire.begin(), wire.end(), L'\\', L'/');
      require(treePathFromWire(wire) == node.relative, "clipboard_expiry_invalid");
    }
    // Parent, transfer folder and every listed entry relative to a held handle; no link on the way is followed.
    Handle held(openFolder(parent));
    require(held.value != INVALID_HANDLE_VALUE, "clipboard_expiry_busy");
    Handle directory(openBelow(held.value, name, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, openExisting, directoryOnly));
    require(directory.value != INVALID_HANDLE_VALUE, "clipboard_expiry_busy");
    require(plainObject(directory.value, true), "clipboard_expiry_invalid");
    bool erased = true;
    // Children are listed after their parents: removing in reverse order empties a folder before it is removed.
    for (size_t at = nodes.size(); at-- > 0;) {
      const auto& node = nodes[at];
      Handle item(openBelow(directory.value, node.relative, DELETE | FILE_READ_ATTRIBUTES, shareAll, openExisting, node.directory ? directoryOnly : fileOnly));
      if (item.value == INVALID_HANDLE_VALUE) { const DWORD error = GetLastError(); if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) erased = false; continue; }
      if (!plainObject(item.value, node.directory) || !disposeHandle(item.value)) erased = false;
    }
    CloseHandle(directory.value); directory.value = INVALID_HANDLE_VALUE;
    return removeEmptyBelow(held.value, name) && erased;
  } catch (...) { return false; }
}

namespace {
// A received folder tree: the folders at once, each file when its first byte arrives, all inside one
// new private transfer folder. Files are marked as Internet-zone content like every received file.
class TreeReceiver {
 public:
  TreeReceiver(const std::filesystem::path& parent, const std::vector<Item>& items, const Bounds& bounds) {
    require(!items.empty() && items.size() <= bounds.entries, "clipboard_entry_count_limit");
    std::set<std::wstring> seen, directories; uint64_t total = 0; size_t tops = 0;
    for (const auto& item : items) {
      std::wstring wire = item.name; std::replace(wire.begin(), wire.end(), L'\\', L'/');
      if (item.name.size() > std::min(bounds.pathChars, maxPasteChars)) throw PathTooLongError(item.name.size(), std::min(bounds.pathChars, maxPasteChars));
      require(treePathFromWire(wire, bounds.pathChars, bounds.depth) == item.name && (item.kind == "file" || item.kind == "dir"), "clipboard_selected_file_unavailable");
      const auto key = folded(item.name);
      const auto slash = item.name.rfind(L'\\');
      if (slash == std::wstring::npos) ++tops; else require(directories.count(folded(item.name.substr(0, slash))), "clipboard_selected_file_unavailable");
      require(seen.insert(key).second, "clipboard_duplicate_file_name");
      if (item.kind == "dir") directories.insert(key);
      else {
        require(item.size <= bounds.fileBytes, "clipboard_file_name_or_size_limit");
        require(item.size <= bounds.totalBytes - total, "clipboard_total_size_limit"); total += item.size;
      }
    }
    require(tops >= 1 && tops <= bounds.tops, "clipboard_file_count_limit");
    total_ = total; existingDirectoryChain(parent); parent_ = parent;
    ULARGE_INTEGER available{};
    if (GetDiskFreeSpaceExW(parent.c_str(), &available, nullptr, nullptr))
      require(available.QuadPart > total && available.QuadPart - total >= freeSpaceHeadroomBytes, "clipboard_disk_full");
    {
      Handle held(openFolder(parent));
      require(held.value != INVALID_HANDLE_VALUE, "clipboard_transfer_parent_untrusted");
      for (int retry = 0; retry < 4; ++retry) {
        const auto name = randomName();
        directory_ = openBelow(held.value, name, folderAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, createNew, directoryOnly, security_.descriptor);
        if (directory_ != INVALID_HANDLE_VALUE) { root_ = parent / name; break; }
        require(GetLastError() == ERROR_ALREADY_EXISTS, "clipboard_transfer_directory_failed");
      }
      require(!root_.empty(), "clipboard_transfer_directory_failed");
      parentFolder_ = held.value; held.value = INVALID_HANDLE_VALUE;
    }
    try {
      require(plainObject(directory_, true), "clipboard_transfer_directory_untrusted");
      DWORD volumeFlags = 0;
      namedStreams_ = GetVolumeInformationByHandleW(directory_, nullptr, 0, nullptr, nullptr, &volumeFlags, nullptr, 0) && (volumeFlags & FILE_NAMED_STREAMS);
      for (const auto& item : items) {
        nodes_.emplace_back(); Node& node = nodes_.back(); node.item = item; node.path = root_ / item.name;
        if (item.kind == "dir") {
          // Relative to the transfer folder; a link put on the way is refused, never followed.
          Handle folder(openBelow(directory_, item.name, FILE_READ_ATTRIBUTES, shareAll, createNew, directoryOnly, security_.descriptor));
          require(folder.value != INVALID_HANDLE_VALUE, "clipboard_transfer_file_create_failed");
          node.created = true;
        } else files_.push_back(nodes_.size() - 1);
      }
    } catch (...) { discard(); CloseHandle(parentFolder_); parentFolder_ = INVALID_HANDLE_VALUE; throw; }
  }
  ~TreeReceiver() {
    if (!committed_ && !discarded_) discard();
    if (directory_ != INVALID_HANDLE_VALUE) CloseHandle(directory_);
    if (parentFolder_ != INVALID_HANDLE_VALUE) CloseHandle(parentFolder_);
  }
  TreeReceiver(const TreeReceiver&) = delete;
  size_t files() const { return files_.size(); }
  const std::filesystem::path& root() const { return root_; }
  uint64_t total() const { return total_; }
  uint64_t received() const { return received_; }
  void append(size_t ordinal, uint64_t offset, const std::vector<uint8_t>& bytes) {
    require(!discarded_ && !committed_ && ordinal < files_.size(), "clipboard_transfer_not_active");
    auto& node = nodes_[files_[ordinal]];
    require(!node.done && offset == node.at && !bytes.empty() && bytes.size() <= maxChunkBytes && bytes.size() <= node.item.size - node.at, "clipboard_chunk_out_of_order_or_bounds");
    open(node);
    DWORD written = 0;
    const BOOL ok = WriteFile(node.file.value, bytes.data(), DWORD(bytes.size()), &written, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    require(error != ERROR_DISK_FULL && error != ERROR_HANDLE_DISK_FULL && error != ERROR_DISK_QUOTA_EXCEEDED, "clipboard_disk_full");
    require(ok && written == bytes.size(), "clipboard_transfer_write_failed");
    node.hash->add(bytes.data(), bytes.size()); node.at += bytes.size(); received_ += bytes.size();
  }
  void finish(size_t ordinal, const std::string& digest) {
    require(!discarded_ && !committed_ && ordinal < files_.size(), "clipboard_transfer_not_active");
    auto& node = nodes_[files_[ordinal]];
    require(!node.done && node.at == node.item.size && digest.size() == 64 &&
        std::all_of(digest.begin(), digest.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) &&
        (node.hash ? node.hash->finish() : Hash().finish()) == digest, "clipboard_transfer_hash_mismatch");
    open(node);  // an empty file is created here
    require(FlushFileBuffers(node.file.value) != FALSE, "clipboard_transfer_flush_failed");
    CloseHandle(node.file.value); node.file.value = INVALID_HANDLE_VALUE; node.done = true; ++finished_;
  }
  bool complete() const { return finished_ == files_.size(); }
  // The received folders and files, parents first, for the receipt and for expiry.
  std::vector<TreeEntry> entries() const {
    std::vector<TreeEntry> out;
    for (const auto& node : nodes_) { TreeEntry entry; entry.relative = node.item.name; entry.directory = node.item.kind == "dir"; entry.size = node.item.size; entry.source = node.path; entry.attributes = node.item.attributes; out.push_back(std::move(entry)); }
    return out;
  }
  std::vector<std::filesystem::path> topLevel() const {
    std::vector<std::filesystem::path> out;
    for (const auto& node : nodes_) if (node.item.name.find(L'\\') == std::wstring::npos) out.push_back(node.path);
    return out;
  }
  void markCommitted() { committed_ = true; }
  void discard() noexcept {
    discarded_ = true;
    for (auto& node : nodes_) if (node.file.value != INVALID_HANDLE_VALUE) { CloseHandle(node.file.value); node.file.value = INVALID_HANDLE_VALUE; }
    // Only what this receiver created, children first, never a recursive delete; by the held folder handle.
    if (directory_ == INVALID_HANDLE_VALUE) return;
    for (size_t at = nodes_.size(); at-- > 0;) {
      auto& node = nodes_[at]; if (!node.created) continue;
      const bool folder = node.item.kind == "dir";
      Handle item(openBelow(directory_, node.item.name, DELETE | FILE_READ_ATTRIBUTES, shareAll, openExisting, folder ? directoryOnly : fileOnly));
      if (plainObject(item.value, folder)) disposeHandle(item.value);
    }
    CloseHandle(directory_); directory_ = INVALID_HANDLE_VALUE;
    if (parentFolder_ != INVALID_HANDLE_VALUE && !root_.empty()) removeEmptyBelow(parentFolder_, root_.filename().wstring());
  }
 private:
  struct Node {
    Item item; std::filesystem::path path; Handle file; std::unique_ptr<Hash> hash; uint64_t at = 0; bool done = false, created = false;
  };
  void open(Node& node) {
    if (node.file.value != INVALID_HANDLE_VALUE || node.created) return;
    node.file.value = openBelow(directory_, node.item.name, GENERIC_WRITE, 0, createNew, fileOnly, security_.descriptor);
    require(node.file.value != INVALID_HANDLE_VALUE, "clipboard_transfer_file_create_failed");
    node.created = true; node.hash = std::make_unique<Hash>();
    if (namedStreams_) markInternetZone(node.file.value);
  }
  PrivateSecurity security_; std::filesystem::path parent_, root_; HANDLE directory_ = INVALID_HANDLE_VALUE, parentFolder_ = INVALID_HANDLE_VALUE;
  std::deque<Node> nodes_; std::vector<size_t> files_; uint64_t total_ = 0, received_ = 0; size_t finished_ = 0;
  bool namedStreams_ = false, committed_ = false, discarded_ = false;
};

// A blob received into memory (text, HTML, RTF or PNG) with its running hash.
struct Blob { std::vector<uint8_t> bytes; uint64_t size = 0; Hash hash; bool done = false; };
}

// ---------------------------------------------------------------------------------------------

struct Outgoing::State {
  std::string format; std::vector<Item> items; size_t tops = 0, skipped = 0; uint64_t total = 0;
  std::vector<std::vector<uint8_t>> blobs; std::vector<uint64_t> position; std::vector<std::unique_ptr<Hash>> hashes;
  std::vector<size_t> files;  // tree: index into items per data ordinal
  std::vector<TreeEntry> tree;
  std::optional<FileSource> open; size_t openOrdinal = SIZE_MAX;
  std::shared_ptr<StagedSource> keep;  // staged virtual files this capture reads from
};
Outgoing::Outgoing() : state_(std::make_unique<State>()) {}
Outgoing::~Outgoing() = default;
const std::string& Outgoing::format() const { return state_->format; }
const std::vector<Item>& Outgoing::items() const { return state_->items; }
size_t Outgoing::tops() const { return state_->tops; }
size_t Outgoing::skipped() const { return state_->skipped; }
uint64_t Outgoing::totalBytes() const { return state_->total; }
size_t Outgoing::dataCount() const { return state_->format == "tree" ? state_->files.size() : state_->items.size(); }
const Item& Outgoing::data(size_t ordinal) const {
  require(ordinal < dataCount(), "clipboard_transfer_not_active");
  return state_->items[state_->format == "tree" ? state_->files[ordinal] : ordinal];
}
std::unique_ptr<Outgoing> Outgoing::capture(const Snapshot& snapshot, const Bounds& bounds) {
  std::unique_ptr<Outgoing> out(new Outgoing()); auto& s = *out->state_;
  s.keep = snapshot.staged;
  if (!snapshot.files.empty()) {
    bool hasFolder = false;
    for (const auto& path : snapshot.files) { const DWORD flags = GetFileAttributesW(longPath(path).c_str()); if (flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_DIRECTORY)) hasFolder = true; }
    if (!hasFolder) return nullptr;
    auto scan = scanTree(snapshot.files, bounds);
    s.format = "tree"; s.tops = scan.tops; s.skipped = scan.skipped; s.total = scan.bytes;
    for (auto& entry : scan.entries) {
      Item item; item.kind = entry.directory ? "dir" : "file"; item.size = entry.size; item.name = entry.relative; item.attributes = entry.attributes;
      if (!entry.directory) s.files.push_back(s.items.size());
      s.items.push_back(std::move(item));
    }
    s.tree = std::move(scan.entries);
    return out;
  }
  if (snapshot.image) {
    s.format = "image"; Item item; item.kind = "png"; item.size = snapshot.image->png.size(); item.width = snapshot.image->width; item.height = snapshot.image->height;
    require(item.size >= 1 && item.size <= maxImageBytes && uint64_t(item.width) * item.height <= maxImagePixels, "clipboard_image_too_large");
    s.items.push_back(item); s.blobs.push_back(snapshot.image->png); s.total = item.size;
  } else if (snapshot.text && snapshot.html) {
    s.format = "richtext";
    const auto encoded = [&] {
      const auto& text = *snapshot.text; if (text.empty()) return std::string();
      const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
      require(count > 0, "clipboard_invalid_unicode");
      std::string value(size_t(count), '\0'); WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), value.data(), count, nullptr, nullptr); return value;
    }();
    require(encoded.size() <= bounds.textBytes && snapshot.text->size() <= maxTextUnits, "clipboard_text_too_large");
    Item text; text.kind = "text"; text.size = encoded.size(); text.units = snapshot.text->size(); s.items.push_back(text);
    s.blobs.emplace_back(encoded.begin(), encoded.end());
    if (snapshot.html && snapshot.html->size() <= bounds.richBytes) { Item item; item.kind = "html"; item.size = snapshot.html->size(); s.items.push_back(item); s.blobs.push_back(*snapshot.html); }
    for (const auto& item : s.items) s.total += item.size;
  } else return nullptr;
  s.position.assign(s.blobs.size(), 0); for (size_t i = 0; i < s.blobs.size(); ++i) s.hashes.push_back(std::make_unique<Hash>());
  return out;
}
std::vector<uint8_t> Outgoing::read(size_t ordinal, uint64_t offset, size_t length) {
  auto& s = *state_; require(ordinal < dataCount() && length > 0 && length <= maxChunkBytes, "clipboard_chunk_limit");
  if (s.format != "tree") {
    const auto& blob = s.blobs[ordinal];
    require(offset == s.position[ordinal] && offset <= blob.size(), "clipboard_chunk_out_of_order_or_bounds");
    const size_t count = size_t(std::min<uint64_t>(length, blob.size() - offset));
    std::vector<uint8_t> bytes(blob.begin() + ptrdiff_t(offset), blob.begin() + ptrdiff_t(offset + count));
    if (!bytes.empty()) s.hashes[ordinal]->add(bytes.data(), bytes.size());
    s.position[ordinal] += count; return bytes;
  }
  if (s.openOrdinal != ordinal) {
    require(offset == 0, "clipboard_chunk_out_of_order_or_bounds");
    s.open.reset(); s.open.emplace(s.tree[s.files[ordinal]].source); s.openOrdinal = ordinal;
    require(s.open->description().size == data(ordinal).size, "clipboard_selected_file_changed");
  }
  require(offset == s.open->position(), "clipboard_chunk_out_of_order_or_bounds");
  return s.open->next(length);
}
std::string Outgoing::digest(size_t ordinal) {
  auto& s = *state_; require(ordinal < dataCount(), "clipboard_transfer_not_active");
  if (s.format != "tree") { require(s.position[ordinal] == s.blobs[ordinal].size(), "clipboard_file_not_complete"); return s.hashes[ordinal]->finish(); }
  if (s.openOrdinal != ordinal) {  // an empty file is never read
    s.open.reset(); s.open.emplace(s.tree[s.files[ordinal]].source); s.openOrdinal = ordinal;
    require(s.open->description().size == data(ordinal).size, "clipboard_selected_file_changed");
  }
  const auto digest = s.open->digest(); s.open.reset(); s.openOrdinal = SIZE_MAX; return digest;
}

// ---------------------------------------------------------------------------------------------

struct Incoming::State {
  std::string format; std::vector<Item> items; Bounds bounds; Storage* storage = nullptr;
  std::deque<Blob> blobs; std::unique_ptr<TreeReceiver> tree; uint64_t received = 0; size_t finished = 0;
  bool committed = false, cancelled = false;
  std::vector<std::filesystem::path> published;
  bool virtualPublished = false;
};
Incoming::Incoming(const std::string& format, std::vector<Item> items, const std::filesystem::path& parent, Storage* storage,
                   const std::vector<std::filesystem::path>& currentClipboardFiles, const Bounds& bounds)
    : state_(std::make_unique<State>()) {
  auto& s = *state_; s.format = format; s.bounds = bounds; s.storage = storage;
  if (format == "richtext") {
    require(!items.empty() && items.size() <= 3 && items[0].kind == "text", "clipboard_rtf_invalid");
    const char* order[] = {"text", "html", "rtf"}; size_t previous = SIZE_MAX;
    for (const auto& item : items) {
      size_t rank = 0; while (rank < 3 && item.kind != order[rank]) ++rank;
      require(rank < 3 && (previous == SIZE_MAX || rank > previous), "clipboard_rtf_invalid");
      require(item.size <= (item.kind == "text" ? bounds.textBytes : bounds.richBytes), "clipboard_text_too_large");
      require(item.kind == "text" || item.size >= 1, "clipboard_rtf_invalid");
      previous = rank;
    }
  } else if (format == "image") {
    require(items.size() == 1 && items[0].kind == "png" && items[0].size >= 1 && items[0].size <= maxImageBytes && items[0].width && items[0].height &&
            uint64_t(items[0].width) * items[0].height <= maxImagePixels, "clipboard_image_too_large");
  } else if (format == "tree") {
    require(storage != nullptr, "clipboard_transfer_directory_failed");
    // Expire old receipts first, so that the free-space check below sees the space they give back.
    uint64_t total = 0; for (const auto& item : items) if (item.kind == "file") { require(item.size <= bounds.totalBytes - total, "clipboard_total_size_limit"); total += item.size; }
    storage->prepare(total, currentClipboardFiles);
    s.tree = std::make_unique<TreeReceiver>(parent, items, bounds);
  } else throw std::runtime_error("unsupported_clipboard_format");
  if (format != "tree") {
    s.blobs.resize(items.size());
    for (size_t i = 0; i < items.size(); ++i) { s.blobs[i].size = items[i].size; }  // no up-front reserve of the declared size: a peer may declare 256 MiB and send nothing; the buffer grows as bytes arrive
  }
  s.items = std::move(items);
}
Incoming::~Incoming() = default;
const std::string& Incoming::format() const { return state_->format; }
std::filesystem::path Incoming::stagingRoot() const { return state_->tree ? state_->tree->root() : std::filesystem::path(); }
uint64_t Incoming::received() const { return state_->received; }
void Incoming::append(size_t ordinal, uint64_t offset, const std::vector<uint8_t>& bytes) {
  auto& s = *state_; require(!s.cancelled && !s.committed, "clipboard_transfer_not_active");
  if (s.tree) { s.tree->append(ordinal, offset, bytes); s.received += bytes.size(); return; }
  require(ordinal < s.blobs.size(), "clipboard_transfer_not_active");
  auto& blob = s.blobs[ordinal];
  require(!blob.done && offset == blob.bytes.size() && !bytes.empty() && bytes.size() <= maxChunkBytes && bytes.size() <= blob.size - blob.bytes.size(), "clipboard_chunk_out_of_order_or_bounds");
  blob.hash.add(bytes.data(), bytes.size()); blob.bytes.insert(blob.bytes.end(), bytes.begin(), bytes.end()); s.received += bytes.size();
}
void Incoming::finish(size_t ordinal, const std::string& digest) {
  auto& s = *state_; require(!s.cancelled && !s.committed, "clipboard_transfer_not_active");
  if (s.tree) { s.tree->finish(ordinal, digest); ++s.finished; return; }
  require(ordinal < s.blobs.size(), "clipboard_transfer_not_active");
  auto& blob = s.blobs[ordinal];
  require(!blob.done && blob.bytes.size() == blob.size && blob.hash.finish() == digest, "clipboard_transfer_hash_mismatch");
  blob.done = true; ++s.finished;
}
bool Incoming::complete() const {
  const auto& s = *state_; return s.tree ? s.tree->complete() : s.finished == s.blobs.size();
}
const std::vector<std::filesystem::path>& Incoming::publishedPaths() const { return state_->published; }
bool Incoming::publishedVirtual() const { return state_->virtualPublished; }
DWORD Incoming::commit(HWND owner, std::optional<DWORD> expectedSequence, const Publishers* publishers) {
  auto& s = *state_; require(!s.cancelled && !s.committed && complete(), "clipboard_transfer_not_complete");
  if (s.tree) {
    const auto tops = s.tree->topLevel(); const auto entries = s.tree->entries(); const auto root = s.tree->root();
    s.tree->markCommitted(); s.committed = true;
    try {
      s.storage->recordTree(root, entries);
      s.published = tops;
      // Absolute paths of MAX_PATH or more cannot be copied from CF_HDROP by Explorer: serve the received files as virtual files.
      if (treeNeedsVirtual(root, entries)) {
        s.virtualPublished = true;
        return publishers && publishers->localTree ? publishers->localTree(owner, entries, expectedSequence) : publishLocalTree(entries, expectedSequence);
      }
      return publishers && publishers->paths ? publishers->paths(owner, tops, expectedSequence) : putPaths(owner, tops, expectedSequence);
    } catch (...) {
      if (expireTree(s.storage->parent(), root, entries)) s.storage->forget({tops.front()});
      throw;
    }
  }
  s.committed = true;
  if (s.format == "image") {
    const Image image{std::move(s.blobs[0].bytes), s.items[0].width, s.items[0].height};
    return publishers && publishers->image ? publishers->image(owner, image, expectedSequence) : putImage(owner, image, expectedSequence);
  }
  const auto& utf8 = s.blobs[0].bytes; std::wstring text;
  if (!utf8.empty()) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(utf8.data()), int(utf8.size()), nullptr, 0);
    require(count > 0, "clipboard_invalid_unicode");
    text.resize(size_t(count)); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(utf8.data()), int(utf8.size()), text.data(), count);
  }
  // A peer's HTML is sanitised again here (the sender is not trusted); one that does not validate refuses the transfer
  // (clipboard_html_invalid) as before. An RTF item is received, hashed and dropped: RTF is never published.
  std::optional<std::vector<uint8_t>> html;
  for (size_t i = 1; i < s.items.size(); ++i) if (s.items[i].kind == "html") html = sanitizeHtmlFormat(s.blobs[i].bytes);
  return publishers && publishers->rich ? publishers->rich(owner, text, html ? &*html : nullptr, nullptr, expectedSequence) : putRich(owner, text, html ? &*html : nullptr, nullptr, expectedSequence);
}
void Incoming::cancel() {
  auto& s = *state_; if (s.cancelled || s.committed) return;
  s.cancelled = true; if (s.tree) s.tree->discard();
  for (auto& blob : s.blobs) { blob.bytes.clear(); blob.bytes.shrink_to_fit(); }
}
