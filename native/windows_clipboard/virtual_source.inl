// Included by clipboard.cpp inside namespace multitap::clipboard: virtual files as a copy source (clipboard.hpp,
// stageVirtualFiles). An OLE data object with FileGroupDescriptorW + FileContents (a .zip folder in Explorer, an Outlook or
// Thunderbird attachment, a phone) is read item by item into a new private "source-<id>" folder, then handled like any other
// file / folder-tree copy. The OLE calls run on a dedicated STA thread: a source that stops answering costs that thread, never
// the caller, which gives up after the idle timeout.

namespace {
constexpr wchar_t stagePrefix[] = L"source-";
constexpr DWORD stageAttributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;
constexpr uint64_t staleStageMillis = 24ull * 60 * 60 * 1000;

bool stageNameValid(const std::wstring& name) {
  if (name.size() != 7 + 32 || name.compare(0, 7, stagePrefix) != 0) return false;
  return std::all_of(name.begin() + 7, name.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); });
}
// Deletes a staged folder without following links (a reparse point inside is removed itself, never entered):
// every item is opened relative to its folder (removeWithoutFollowing / removeBelow).
void removeStagedTree(const std::filesystem::path& folder) noexcept { (void)removeWithoutFollowing(folder.parent_path(), folder.filename().wstring()); }
// Folders left by a crashed process: older than a day and not reachable by any live copy.
void sweepStaleStages(const std::filesystem::path& parent) noexcept {
  WIN32_FIND_DATAW data{};
  const HANDLE find = FindFirstFileExW((parent / (std::wstring(stagePrefix) + L"*")).c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
  if (find == INVALID_HANDLE_VALUE) return;
  FILETIME nowFile{}; GetSystemTimeAsFileTime(&nowFile);
  const auto ticks = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
  int seen = 0;
  do {
    if (++seen > 4096) break;
    const std::wstring name = data.cFileName;
    if (!stageNameValid(name) || !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
    const uint64_t now = ticks(nowFile), written = ticks(data.ftLastWriteTime);
    if (now > written && (now - written) / 10000 > staleStageMillis) removeStagedTree(parent / name);
  } while (FindNextFileW(find, &data));
  FindClose(find);
}

struct StageEntry {
  std::wstring relative;  // '\' separated, every component passes safeLeaf
  bool directory = false, implicit = false, sizeKnown = false, hasWritten = false;
  uint64_t size = 0;
  DWORD attributes = 0;
  FILETIME written{};
  LONG index = 0;  // the lindex of its FileContents
};
// Descriptor names: relative paths with '\' (some producers write '/'), a trailing separator marks a folder. Anything the tree rules
// would refuse on disk (a component that fails safeLeaf: "..", a drive letter, reserved device names, control characters; too deep or
// too long a path) refuses the whole copy, exactly as for a folder tree.
std::vector<StageEntry> parseDescriptors(const std::vector<uint8_t>& bytes, const Bounds& bounds, uint64_t& declaredTotal) {
  constexpr size_t header = offsetof(FILEGROUPDESCRIPTORW, fgd);
  require(bytes.size() >= header, "clipboard_selected_file_unavailable");
  UINT count = 0; std::memcpy(&count, bytes.data(), sizeof(count));
  require(count > 0, "unsupported_clipboard_format");
  require(count <= bounds.entries, "clipboard_entry_count_limit");
  require((bytes.size() - header) / sizeof(FILEDESCRIPTORW) >= count, "clipboard_selected_file_unavailable");
  std::vector<StageEntry> entries; std::map<std::wstring, size_t> byName; std::set<std::wstring> tops;
  declaredTotal = 0;
  for (UINT index = 0; index < count; ++index) {
    FILEDESCRIPTORW descriptor{}; std::memcpy(&descriptor, bytes.data() + header + size_t(index) * sizeof(FILEDESCRIPTORW), sizeof(descriptor));
    require(std::wmemchr(descriptor.cFileName, L'\0', MAX_PATH) != nullptr, "clipboard_selected_file_unavailable");
    std::wstring name = descriptor.cFileName;
    std::replace(name.begin(), name.end(), L'/', L'\\');
    bool folderName = false;
    if (!name.empty() && name.back() == L'\\') { name.pop_back(); folderName = true; }
    require(!name.empty() && name.size() <= bounds.pathChars, name.empty() ? "clipboard_selected_file_unavailable" : "clipboard_path_too_long");
    std::vector<std::wstring> parts; size_t start = 0;
    for (;;) {
      const auto slash = name.find(L'\\', start);
      parts.push_back(name.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start));
      if (slash == std::wstring::npos) break;
      start = slash + 1;
    }
    require(parts.size() <= bounds.depth, "clipboard_depth_limit");
    for (const auto& part : parts) require(safeLeaf(part), "clipboard_selected_file_unavailable");
    const bool directory = folderName || ((descriptor.dwFlags & FD_ATTRIBUTES) && (descriptor.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
    // Folders on the way (a name "a\b\c.txt" needs a and a\b) come first, once.
    std::wstring prefix;
    for (size_t at = 0; at + 1 < parts.size(); ++at) {
      prefix += (prefix.empty() ? L"" : L"\\") + parts[at];
      const auto key = folded(prefix);
      const auto found = byName.find(key);
      if (found != byName.end()) { require(entries[found->second].directory, "clipboard_selected_file_unavailable"); continue; }
      require(entries.size() < bounds.entries, "clipboard_entry_count_limit");
      StageEntry parent; parent.relative = prefix; parent.directory = true; parent.implicit = true; parent.index = -1;
      byName[key] = entries.size(); entries.push_back(std::move(parent));
    }
    const auto key = folded(name);
    StageEntry entry; entry.relative = name; entry.directory = directory; entry.index = LONG(index);
    entry.attributes = (descriptor.dwFlags & FD_ATTRIBUTES) ? descriptor.dwFileAttributes & stageAttributes : 0;
    if (descriptor.dwFlags & FD_WRITESTIME) { entry.hasWritten = true; entry.written = descriptor.ftLastWriteTime; }
    if (!directory && (descriptor.dwFlags & FD_FILESIZE)) {
      entry.sizeKnown = true; entry.size = (uint64_t(descriptor.nFileSizeHigh) << 32) | descriptor.nFileSizeLow;
      require(entry.size <= bounds.fileBytes, "clipboard_file_name_or_size_limit");
      require(entry.size <= bounds.totalBytes - declaredTotal, "clipboard_total_size_limit");
      declaredTotal += entry.size;
    }
    const auto found = byName.find(key);
    if (found != byName.end()) {
      // The same folder listed after it was implied by a child: it takes the listed attributes. Anything else repeats a name.
      auto& existing = entries[found->second];
      require(existing.implicit && directory, "clipboard_selected_file_unavailable");
      existing.implicit = false; existing.attributes = entry.attributes; existing.hasWritten = entry.hasWritten; existing.written = entry.written; existing.index = entry.index;
      continue;
    }
    require(entries.size() < bounds.entries, "clipboard_entry_count_limit");
    byName[key] = entries.size(); entries.push_back(std::move(entry));
  }
  for (const auto& entry : entries) if (entry.relative.find(L'\\') == std::wstring::npos) tops.insert(folded(entry.relative));
  require(tops.size() <= bounds.tops, "clipboard_file_count_limit");
  return entries;
}

struct StageJob {
  std::mutex m; std::condition_variable cv; bool finished = false;
  std::atomic<bool> cancelled{false};
  std::atomic<uint64_t> bytes{0}, ticks{0}; std::atomic<size_t> done{0}, total{0};
  std::shared_ptr<StagedSource> result; std::string failure;
  IDataObject* source = nullptr;  // AddRef'd by the caller; released by the worker
  VirtualStageOptions options;
  void step() { ticks.fetch_add(1); }
  void check() const { require(!cancelled.load(), "clipboard_cancelled"); }
};

bool formatMissing(HRESULT hr) {
  return hr == DV_E_FORMATETC || hr == DV_E_LINDEX || hr == DV_E_TYMED || hr == DV_E_DVASPECT || hr == E_NOTIMPL || hr == E_INVALIDARG || hr == DATA_E_FORMATETC;
}
// FileContents of item `index`: an IStream when the source gives one, otherwise an HGLOBAL.
void fetchContents(IDataObject* data, UINT format, LONG index, STGMEDIUM& medium) {
  FORMATETC wanted{CLIPFORMAT(format), nullptr, DVASPECT_CONTENT, index, TYMED_ISTREAM};
  medium = STGMEDIUM{};
  const HRESULT first = data->GetData(&wanted, &medium);
  if (SUCCEEDED(first)) return;
  wanted.tymed = TYMED_HGLOBAL; medium = STGMEDIUM{};
  const HRESULT second = data->GetData(&wanted, &medium);
  if (SUCCEEDED(second)) return;
  // An item that offers no FileContents at all (a shell-only namespace item) cannot be copied; a source that failed or went away is unavailable.
  require(!(formatMissing(first) && formatMissing(second)), "unsupported_clipboard_format");
  throw std::runtime_error("clipboard_selected_file_unavailable");
}
struct Medium { STGMEDIUM value{}; ~Medium() { if (value.tymed != TYMED_NULL || value.pUnkForRelease) ReleaseStgMedium(&value); } };

void writeStaged(HANDLE file, const uint8_t* data, size_t size, StageJob& job) {
  DWORD written = 0;
  const BOOL ok = WriteFile(file, data, DWORD(size), &written, nullptr);
  const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  require(error != ERROR_DISK_FULL && error != ERROR_HANDLE_DISK_FULL && error != ERROR_DISK_QUOTA_EXCEEDED, "clipboard_disk_full");
  require(ok && written == size, "clipboard_transfer_write_failed");
  job.bytes.fetch_add(size); job.step();
}
// Reads the content of one file entry into `path`; returns its size.
uint64_t stageOneFile(IDataObject* data, UINT contentsFormat, const StageEntry& entry, HANDLE root,
                      PSECURITY_DESCRIPTOR security, uint64_t totalSoFar, const Bounds& bounds, StageJob& job) {
  // Relative to the held stage folder; a link put on the way is refused, never followed.
  Handle file(openBelow(root, entry.relative, GENERIC_WRITE | FILE_WRITE_ATTRIBUTES, 0, createNew, fileOnly, security));
  require(file.value != INVALID_HANDLE_VALUE, "clipboard_transfer_file_create_failed");
  Medium medium; fetchContents(data, contentsFormat, entry.index, medium.value);
  job.check(); job.step();
  uint64_t size = 0;
  const auto account = [&](uint64_t more) {
    size += more;
    require(size <= bounds.fileBytes, "clipboard_file_name_or_size_limit");
    require(size <= bounds.totalBytes - totalSoFar, "clipboard_total_size_limit");
  };
  if (medium.value.tymed == TYMED_ISTREAM && medium.value.pstm) {
    // Read to the end: a declared size is only a hint, an unknown size is the normal case for a streaming source.
    std::vector<uint8_t> buffer(maxChunkBytes);
    for (;;) {
      job.check();
      ULONG got = 0; const HRESULT hr = medium.value.pstm->Read(buffer.data(), ULONG(buffer.size()), &got);
      require(SUCCEEDED(hr) && got <= buffer.size(), "clipboard_selected_file_unavailable");
      if (got == 0) break;
      account(got); writeStaged(file.value, buffer.data(), got, job);
    }
  } else if (medium.value.tymed == TYMED_HGLOBAL && medium.value.hGlobal) {
    const SIZE_T allocated = GlobalSize(medium.value.hGlobal);
    // A global block can be larger than the data (allocation granularity): the declared size is what counts when there is one.
    const uint64_t length = entry.sizeKnown ? std::min<uint64_t>(entry.size, allocated) : uint64_t(allocated);
    account(length);
    const auto* memory = static_cast<const uint8_t*>(GlobalLock(medium.value.hGlobal));
    require(memory != nullptr || length == 0, "clipboard_selected_file_unavailable");
    struct Unlock { HGLOBAL h; ~Unlock() { GlobalUnlock(h); } } unlock{medium.value.hGlobal};
    for (uint64_t at = 0; at < length;) {
      job.check();
      const size_t chunk = size_t(std::min<uint64_t>(length - at, maxChunkBytes));
      writeStaged(file.value, memory + at, chunk, job); at += chunk;
    }
  } else if (medium.value.tymed != TYMED_NULL) {
    throw std::runtime_error("unsupported_clipboard_format");  // an IStorage / file name medium is not file data
  }
  if (entry.hasWritten) SetFileTime(file.value, nullptr, nullptr, &entry.written);
  return size;
}

std::shared_ptr<StagedSource> stageFrom(IDataObject* data, StageJob& job) {
  const auto& options = job.options;
  static const UINT descriptorFormat = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
  static const UINT contentsFormat = RegisterClipboardFormatW(CFSTR_FILECONTENTS);
  FORMATETC wanted{CLIPFORMAT(descriptorFormat), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  if (data->QueryGetData(&wanted) != S_OK) return nullptr;
  std::vector<uint8_t> descriptorBytes;
  {
    Medium medium; const HRESULT hr = data->GetData(&wanted, &medium.value);
    require(SUCCEEDED(hr) && medium.value.tymed == TYMED_HGLOBAL && medium.value.hGlobal, "clipboard_selected_file_unavailable");
    const SIZE_T size = GlobalSize(medium.value.hGlobal);
    require(size >= sizeof(UINT) && size <= sizeof(UINT) + (maxEntries + 1) * sizeof(FILEDESCRIPTORW), "clipboard_selected_file_unavailable");
    const auto* memory = static_cast<const uint8_t*>(GlobalLock(medium.value.hGlobal)); require(memory != nullptr, "clipboard_lock_failed");
    descriptorBytes.assign(memory, memory + size); GlobalUnlock(medium.value.hGlobal);
  }
  job.check(); job.step();
  uint64_t declared = 0;
  auto entries = parseDescriptors(descriptorBytes, options.bounds, declared);
  job.total = entries.size();

  require(options.parent.is_absolute(), "clipboard_transfer_parent_not_absolute");
  { Storage ensure(options.parent); }  // creates the private parent when missing and refuses an untrusted one
  existingDirectoryChain(options.parent);
  sweepStaleStages(options.parent);
  {
    ULARGE_INTEGER available{};
    if (GetDiskFreeSpaceExW(options.parent.c_str(), &available, nullptr, nullptr))
      require(available.QuadPart > declared && available.QuadPart - declared >= freeSpaceHeadroomBytes, "clipboard_disk_full");
  }
  PrivateSecurity security;
  auto staged = std::make_shared<StagedSource>();
  staged->parentFolder = openFolder(options.parent);
  require(staged->parentFolder != INVALID_HANDLE_VALUE, "clipboard_transfer_parent_untrusted");
  for (int retry = 0; retry < 4; ++retry) {
    const auto name = std::wstring(stagePrefix) + randomName().substr(9);
    staged->directory = openBelow(staged->parentFolder, name, folderAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, createNew, directoryOnly, security.descriptor);
    if (staged->directory != INVALID_HANDLE_VALUE) { staged->root = options.parent / name; break; }
    require(GetLastError() == ERROR_ALREADY_EXISTS, "clipboard_transfer_directory_failed");
  }
  require(!staged->root.empty(), "clipboard_transfer_directory_failed");
  require(plainObject(staged->directory, true), "clipboard_transfer_directory_untrusted");

  uint64_t total = 0;
  for (const auto& entry : entries) {
    job.check();
    if (entry.directory) {
      Handle folder(openBelow(staged->directory, entry.relative, FILE_READ_ATTRIBUTES, shareAll, createNew, directoryOnly, security.descriptor));
      require(folder.value != INVALID_HANDLE_VALUE, "clipboard_transfer_directory_failed");
      ++staged->folders;
    } else {
      total += stageOneFile(data, contentsFormat, entry, staged->directory, security.descriptor, total, options.bounds, job);
      ++staged->files;
    }
    job.done.fetch_add(1); job.step();
  }
  staged->bytes = total;
  // Attributes last, folders after their contents (a read-only or hidden folder does not stop its children being written).
  // Each item is opened relative to the stage folder, never through a link.
  for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
    const auto path = staged->root / entry->relative;
    if ((entry->directory && entry->hasWritten) || entry->attributes) {
      Handle item(openBelow(staged->directory, entry->relative, FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES, shareAll, openExisting, entry->directory ? directoryOnly : fileOnly));
      if (plainObject(item.value, entry->directory)) {
        if (entry->directory && entry->hasWritten) SetFileTime(item.value, nullptr, nullptr, &entry->written);
        if (entry->attributes) { FILE_BASIC_INFO basic{}; basic.FileAttributes = entry->attributes | (entry->directory ? FILE_ATTRIBUTE_DIRECTORY : 0); SetFileInformationByHandle(item.value, FileBasicInfo, &basic, sizeof(basic)); }
      }
    }
    if (entry->relative.find(L'\\') == std::wstring::npos) staged->tops.push_back(path);
  }
  std::reverse(staged->tops.begin(), staged->tops.end());
  return staged;
}

void runStage(std::shared_ptr<StageJob> job) {
  const HRESULT initialised = OleInitialize(nullptr);  // an STA of its own
  try {
    IDataObject* data = job->source;
    Microsoft::WRL::ComPtr<IDataObject> fromClipboard;
    if (!data) {
      HRESULT hr = E_FAIL;
      for (int attempt = 0; attempt < 50; ++attempt) {
        hr = OleGetClipboard(fromClipboard.ReleaseAndGetAddressOf());
        if (hr != CLIPBRD_E_CANT_OPEN) break;
        Sleep(attempt < 10 ? 10 : 25);
      }
      require(hr != CLIPBRD_E_CANT_OPEN, "clipboard_busy");
      require(SUCCEEDED(hr) && fromClipboard, "clipboard_read_failed");
      data = fromClipboard.Get();
    }
    job->result = stageFrom(data, *job);
  } catch (const std::exception& error) { job->failure = error.what(); }
  catch (...) { job->failure = "clipboard_read_failed"; }
  if (job->source) { job->source->Release(); job->source = nullptr; }
  if (SUCCEEDED(initialised)) OleUninitialize();
  {
    std::lock_guard lock(job->m);
    // The caller gave up (a source that stopped answering): the copy is dropped, which deletes the folder.
    if (job->cancelled.load()) job->result.reset();
    job->finished = true;
  }
  job->cv.notify_all();
}
}

StagedSource::~StagedSource() {
  // Through the parent handle the stage was created with (never a path looked up again).
  if (directory != INVALID_HANDLE_VALUE) { CloseHandle(directory); directory = INVALID_HANDLE_VALUE; }
  if (parentFolder != INVALID_HANDLE_VALUE) {
    if (!root.empty() && stageNameValid(root.filename().wstring())) (void)removeTreeBelow(parentFolder, root.filename().wstring());
    CloseHandle(parentFolder); parentFolder = INVALID_HANDLE_VALUE;
  }
}

std::shared_ptr<StagedSource> stageVirtualFiles(IDataObject* source, const VirtualStageOptions& options) {
  require(options.parent.is_absolute(), "clipboard_transfer_parent_not_absolute");
  auto job = std::make_shared<StageJob>();
  job->options = options; job->source = source;
  if (source) source->AddRef();
  try { std::thread([job] { runStage(job); }).detach(); }
  catch (...) { if (source) source->Release(); throw std::runtime_error("clipboard_read_failed"); }
  using Clock = std::chrono::steady_clock;
  uint64_t lastTicks = 0, reported = UINT64_MAX; auto lastChange = Clock::now();
  std::unique_lock lock(job->m);
  while (!job->finished) {
    job->cv.wait_for(lock, std::chrono::milliseconds(100));
    if (job->finished) break;
    const uint64_t ticks = job->ticks.load();
    if (ticks != lastTicks) { lastTicks = ticks; lastChange = Clock::now(); }
    else if (Clock::now() - lastChange > options.idle) { job->cancelled.store(true); throw std::runtime_error("clipboard_busy"); }
    if (options.progress && ticks != reported) {
      reported = ticks;
      const auto bytes = job->bytes.load(); const auto done = job->done.load(); const auto all = job->total.load();
      lock.unlock();
      try { options.progress(bytes, done, all); } catch (...) {}
      lock.lock();
    }
  }
  if (!job->failure.empty()) throw std::runtime_error(job->failure);
  return std::move(job->result);
}
