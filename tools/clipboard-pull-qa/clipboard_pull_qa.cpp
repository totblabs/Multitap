// Paste-pull profile of native/windows_clipboard (pull.hpp): the OLE data object a receiver publishes (virtual
// files), pulling IStreams, flow control by consumption, cancel, hash mismatch, timeout, changed files, seek and
// restart, and the source offer identity. Needs no clipboard: the data object is called directly and the offering
// side is a fake link that serves a SourceOffer with a sliding window. With --clipboard as the second argument
// it also publishes on the real OLE clipboard and extracts the way Explorer's paste does (IDropTarget::Drop of the
// shell folder), then restores the previous text / file clipboard.
#include "pull.hpp"
#include "ack_window.hpp"
#include "storage.hpp"
#include "publication_retry.hpp"
#include <bcrypt.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

namespace clip = multitap::clipboard;
namespace fs = std::filesystem;
namespace {
using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> std::string reason(F operation) {
  try { operation(); } catch (const std::exception& error) { return error.what(); }
  return {};
}
std::string sha256(const std::vector<uint8_t>& bytes) {
  BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
  require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "sha provider");
  DWORD size = 0, got = 0; BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &got, 0);
  std::vector<uint8_t> object(size); BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0);
  if (!bytes.empty()) BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), ULONG(bytes.size()), 0);
  std::array<uint8_t, 32> digest{}; BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0);
  BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
  std::string out; constexpr char hex[] = "0123456789abcdef"; for (auto b : digest) { out += hex[b >> 4]; out += hex[b & 15]; } return out;
}
std::vector<uint8_t> pseudo(size_t count, uint32_t seed) {
  std::vector<uint8_t> out(count); uint32_t x = seed * 2654435761u + 12345;
  for (auto& b : out) { x = x * 1664525u + 1013904223u; b = uint8_t(x >> 24); }
  return out;
}
void write(const fs::path& path, const std::vector<uint8_t>& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary); stream.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}
std::vector<uint8_t> slurp(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary); return {std::istreambuf_iterator<char>(stream), {}};
}
void expectReason(const std::string& actual, const char* expected, const char* what) {
  if (actual != expected) throw std::runtime_error(std::string(what) + ": got '" + actual + "', wanted '" + expected + "'");
}
template<class P> bool waitFor(P predicate, std::chrono::milliseconds limit = 3000ms) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) { if (predicate()) return true; std::this_thread::sleep_for(5ms); }
  return predicate();
}

// The offering peer and the relay, in one object: serves pulls of a SourceOffer through the sliding window.
struct Link {
  std::shared_ptr<clip::SourceOffer> source; std::weak_ptr<clip::PullOffer> offer;
  std::mutex m; std::condition_variable cv;
  struct Pull { uint64_t id; size_t index; clip::SendWindow window; bool cancelled = false; std::thread thread; };
  std::map<uint64_t, std::unique_ptr<Pull>> pulls;
  std::atomic<bool> silent{false}, corruptDigest{false};
  std::atomic<uint64_t> bytesPerSecond{0};   // 0: as fast as possible; otherwise the link sleeps after each chunk (a throttled fake link)
  std::atomic<size_t> requests{0}, cancels{0}, acks{0}, badAcks{0}, waitOn{0}, waitOff{0};
  std::atomic<uint64_t> sentBytes{0};
  std::vector<std::pair<size_t, uint64_t>> asked; std::string failedReason; std::vector<uint64_t> ackEnds;
  ~Link() {
    { std::lock_guard lock(m); for (auto& pull : pulls) pull.second->cancelled = true; }
    cv.notify_all(); for (auto& pull : pulls) if (pull.second->thread.joinable()) pull.second->thread.join();
  }
  clip::PullHooks hooks() {
    clip::PullHooks h;
    h.request = [this](uint64_t id, const std::string&, size_t index, uint64_t offset) {
      ++requests; { std::lock_guard lock(m); asked.emplace_back(index, offset); }
      if (silent) return;
      auto pull = std::make_unique<Pull>(Pull{id, index, clip::SendWindow(), false, {}});
      auto* raw = pull.get();
      { std::lock_guard lock(m); pulls[id] = std::move(pull); }
      raw->thread = std::thread([this, raw, id, index, offset] { run(*raw, id, index, offset); });
    };
    h.cancel = [this](uint64_t id) { { std::lock_guard lock(m); if (auto found = pulls.find(id); found != pulls.end()) found->second->cancelled = true; } ++cancels; cv.notify_all(); };
    h.ackChunk = [this](uint64_t id, size_t index, uint64_t end) {
      { std::lock_guard lock(m);
        const auto found = pulls.find(id);
        if (found == pulls.end()) { ++badAcks; return; }
        try { found->second->window.ackChunk(index, end); ++acks; ackEnds.push_back(end); } catch (...) { ++badAcks; } }
      cv.notify_all();
    };
    h.failed = [this](uint64_t, size_t, const std::string& why) { std::lock_guard lock(m); failedReason = why; };
    h.waiting = [this](bool on) { if (on) ++waitOn; else ++waitOff; };
    return h;
  }
  void run(Pull& pull, uint64_t id, size_t index, uint64_t offset) {
    try {
      auto reader = source->open(index, offset);
      auto target = offer.lock(); if (!target) return;
      while (reader->position() < reader->size()) {
        const size_t n = size_t(std::min<uint64_t>(clip::formatsChunkBytes, reader->size() - reader->position()));
        { std::unique_lock lock(m); cv.wait(lock, [&] { return pull.cancelled || pull.window.canSend(n); }); if (pull.cancelled) return; }
        const uint64_t at = reader->position(); auto bytes = reader->next(n);
        { std::lock_guard lock(m); pull.window.sentChunk(index, reader->position(), bytes.size()); }
        sentBytes += bytes.size(); target->deliver(id, at, bytes);
        if (const auto rate = bytesPerSecond.load()) std::this_thread::sleep_for(std::chrono::microseconds(bytes.size() * 1000000ull / rate));
      }
      auto digest = reader->digest(); if (corruptDigest) digest[0] = digest[0] == '0' ? '1' : '0';
      if (!pull.cancelled) target->finish(id, digest);
    } catch (const std::exception& error) {
      if (auto target = offer.lock()) target->fail(id, error.what());
    }
  }
};

struct Fixture {
  fs::path root, source; std::vector<fs::path> roots;
  std::shared_ptr<clip::SourceOffer> offer;
  std::vector<uint8_t> a, deep, top, empty, korean;
  explicit Fixture(const fs::path& base) : root(base) {
    source = root / L"src"; fs::create_directories(source);
    a = {'h', 'e', 'l', 'l', 'o', ' ', 'a'}; deep = pseudo(200000, 1); top = pseudo(2 * 1024 * 1024 + 17, 2); korean = pseudo(3000, 3);
    write(source / L"Folder" / L"a.txt", a); fs::create_directories(source / L"Folder" / L"Sub"); write(source / L"Folder" / L"Sub2" / L"deep.bin", deep);
    write(source / L"top.bin", top); write(source / L"empty.txt", empty); write(source / L"한글 파일.txt", korean);
    roots = {source / L"Folder", source / L"top.bin", source / L"empty.txt", source / L"한글 파일.txt"};
    offer = clip::SourceOffer::capture("offer1", roots, clip::Bounds{});
  }
  std::vector<int64_t> mtimes() const { std::vector<int64_t> out; for (size_t i = 0; i < offer->items().size(); ++i) out.push_back(offer->mtimeMs(i)); return out; }
  size_t entry(const std::wstring& name) const {
    for (size_t i = 0; i < offer->items().size(); ++i) if (offer->items()[i].name == name) return i;
    throw std::runtime_error("no such entry");
  }
};
struct Receiver {
  Link link; std::shared_ptr<clip::PullOffer> offer;
  explicit Receiver(Fixture& fixture, clip::PullTimeouts timeouts = {}) {
    link.source = fixture.offer;
    offer = clip::PullOffer::create("offer1", fixture.offer->items(), fixture.mtimes(), link.hooks(), timeouts);
    link.offer = offer;
  }
};
CLIPFORMAT format(const wchar_t* name) { return CLIPFORMAT(RegisterClipboardFormatW(name)); }
IStream* streamOf(IDataObject* data, LONG lindex) {
  FORMATETC fe{format(CFSTR_FILECONTENTS), nullptr, DVASPECT_CONTENT, lindex, TYMED_ISTREAM}; STGMEDIUM medium{};
  const HRESULT result = data->GetData(&fe, &medium); require(SUCCEEDED(result) && medium.tymed == TYMED_ISTREAM && medium.pstm, "FileContents stream");
  return medium.pstm;
}
// Reads a whole stream with odd read sizes; returns the bytes and the HRESULT of the failing read (S_OK if none).
std::vector<uint8_t> drain(IStream* stream, HRESULT& result, size_t block = 70001) {
  std::vector<uint8_t> out, buffer(block); result = S_OK;
  for (;;) {
    ULONG got = 0; result = stream->Read(buffer.data(), ULONG(buffer.size()), &got);
    if (FAILED(result)) return out;
    if (got == 0) return out;
    out.insert(out.end(), buffer.begin(), buffer.begin() + got);
  }
}
std::vector<uint8_t> readExactly(IStream* stream, size_t count) {
  std::vector<uint8_t> out(count); size_t at = 0;
  while (at < count) { ULONG got = 0; const HRESULT result = stream->Read(out.data() + at, ULONG(count - at), &got); require(SUCCEEDED(result) && got > 0, "short read"); at += got; }
  return out;
}
// ---- Cross-process modes (integrity-level check) --------------------------------------------------------
// --publish <dir> <ready file> <done file>: builds the fixture, publishes its offer on the OLE clipboard and keeps the
// message loop running (serving pulls from the fixture) until the done file exists; --paste <dir> <destination>: extracts
// whatever offer is on the clipboard with the folder drop target and compares it with the manifest the publisher wrote.
// Run one of them elevated and the other not to see whether the data object works across integrity levels.
std::string narrow(const std::wstring& text) { if (text.empty()) return {}; const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr); std::string out(size_t(n), 0); WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr); return out; }
std::wstring widen(const std::string& text) { if (text.empty()) return {}; const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0); std::wstring out(size_t(n), 0); MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n); return out; }
std::string integrityName() {
  HANDLE token = nullptr; OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
  DWORD size = 0; GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size); std::vector<uint8_t> bytes(size);
  GetTokenInformation(token, TokenIntegrityLevel, bytes.data(), size, &size); CloseHandle(token);
  const auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(bytes.data());
  const DWORD rid = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);
  return rid >= SECURITY_MANDATORY_SYSTEM_RID ? "system" : rid >= SECURITY_MANDATORY_HIGH_RID ? "high" : rid >= SECURITY_MANDATORY_MEDIUM_RID ? "medium" : "low";
}
int publishMode(const fs::path& dir, const fs::path& ready, const fs::path& done) {
  fs::create_directories(dir); require(SUCCEEDED(OleInitialize(nullptr)), "OleInitialize");
  Fixture fixture(dir); Receiver r(fixture);
  { std::ofstream manifest(dir / L"manifest.txt", std::ios::binary);
    for (size_t i = 0; i < fixture.offer->items().size(); ++i) {
      const auto& item = fixture.offer->items()[i]; const std::string name = narrow(item.name);
      if (item.kind == "dir") manifest << "D\t" << name << "\n"; else manifest << "F\t" << name << "\t" << sha256(slurp(fixture.source / item.name)) << "\t" << item.size << "\n";
    } }
  const HWND owner = CreateWindowExW(0, L"STATIC", L"QA", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  const DWORD sequence = clip::publishPullOffer(r.offer, std::nullopt);
  { std::ofstream stream(ready); stream << integrityName() << " " << sequence; }
  MSG message{}; const auto end = std::chrono::steady_clock::now() + 180s;
  while (std::chrono::steady_clock::now() < end && !fs::exists(done)) { while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, 0); }
  { std::ofstream final(ready.wstring() + L".final"); final << "{\"publisher\":{\"integrity\":\"" << integrityName() << "\",\"requests\":" << r.link.requests.load() << ",\"sentBytes\":" << r.link.sentBytes.load() << "}}\n"; }
  clip::withdrawPullOffer(r.offer); DestroyWindow(owner); OleUninitialize();
  return r.link.requests.load() >= 4 ? 0 : 1;
}
int pasteMode(const fs::path& dir, const fs::path& destination) {
  require(SUCCEEDED(OleInitialize(nullptr)), "OleInitialize");
  fs::create_directories(destination);
  std::vector<std::tuple<std::string, std::string, std::string, uint64_t>> expected; uint64_t total = 0;
  { std::ifstream manifest(dir / L"manifest.txt"); std::string line;
    while (std::getline(manifest, line)) { std::vector<std::string> p; size_t at = 0; for (;;) { const auto tab = line.find('\t', at); p.push_back(line.substr(at, tab == std::string::npos ? tab : tab - at)); if (tab == std::string::npos) break; at = tab + 1; }
      if (p[0] == "D") expected.emplace_back("D", p[1], "", 0); else { expected.emplace_back("F", p[1], p[2], std::stoull(p[3])); total += std::stoull(p[3]); } } }
  IDataObject* data = nullptr; const HRESULT got = OleGetClipboard(&data); require(SUCCEEDED(got) && data, "OleGetClipboard");
  IShellItem* item = nullptr; require(SUCCEEDED(SHCreateItemFromParsingName(destination.c_str(), nullptr, IID_PPV_ARGS(&item))), "destination");
  IDropTarget* target = nullptr; require(SUCCEEDED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target))), "drop target");
  POINTL where{0, 0}; DWORD effect = DROPEFFECT_COPY; require(SUCCEEDED(target->DragEnter(data, MK_LBUTTON, where, &effect)), "DragEnter"); effect = DROPEFFECT_COPY;
  const HRESULT dropped = target->Drop(data, 0, where, &effect);
  const auto sizeOf = [&] { uint64_t sum = 0; std::error_code ec; for (const auto& e : fs::recursive_directory_iterator(destination, ec)) if (e.is_regular_file(ec)) sum += e.file_size(ec); return sum; };
  const auto end = std::chrono::steady_clock::now() + 60s; while (std::chrono::steady_clock::now() < end && sizeOf() < total) std::this_thread::sleep_for(50ms);
  std::this_thread::sleep_for(500ms);
  size_t good = 0;
  for (const auto& [kind, name, hash, size] : expected) { const fs::path p = destination / widen(name); if (kind == "D") { if (fs::is_directory(p)) ++good; } else if (fs::is_regular_file(p) && fs::file_size(p) == size && sha256(slurp(p)) == hash) ++good; }
  std::cout << "{\"paste\":{\"integrity\":\"" << integrityName() << "\",\"dropResult\":" << long(dropped) << ",\"extracted\":" << good << ",\"expected\":" << expected.size() << "}}\n";
  target->Release(); item->Release(); data->Release(); OleUninitialize();
  return good == expected.size() && SUCCEEDED(dropped) ? 0 : 1;
}
}

#include "onpaste_qa.inl"

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc == 5 && std::wstring(argv[1]) == L"--publish") return publishMode(argv[2], argv[3], argv[4]);
    if (argc == 4 && std::wstring(argv[1]) == L"--paste") return pasteMode(argv[2], argv[3]);
    require(argc >= 2, "usage: clipboard_pull_qa <private directory> [--clipboard]");
    const bool useClipboard = argc >= 3 && std::wstring(argv[2]) == L"--clipboard";
    const fs::path root = fs::path(argv[1]) / L"pull";
    fs::remove_all(root); fs::create_directories(root);
    require(SUCCEEDED(OleInitialize(nullptr)), "OleInitialize");
    int checks = 0;
    // A received tree served as virtual files from its local copy (staging path beyond MAX_PATH), extracted by the shell's own folder drop
    // target, the engine of an Explorer paste: the object works with the real shell, and the staging path never reaches it.
    {
      fs::path staging = root / L"localtree"; for (int i = 0; i < 5; ++i) staging /= std::wstring(60, wchar_t(L'a' + i));  // > 300 characters
      const auto put = [](const fs::path& path, const std::string& content) { fs::create_directories(clip::longPath(path.parent_path())); std::ofstream out(clip::longPath(path), std::ios::binary); out << content; };
      put(staging / L"Top" / L"sub" / L"한글 file.txt", "korean name"); put(staging / L"Top" / L"data.bin", std::string(300000, 'q')); fs::create_directories(clip::longPath(staging / L"Top" / L"Empty"));
      std::vector<clip::TreeEntry> entries;
      const auto add = [&](const std::wstring& relative, bool directory, uint64_t size) { clip::TreeEntry e; e.relative = relative; e.source = staging / relative; e.directory = directory; e.size = size; entries.push_back(e); };
      add(L"Top", true, 0); add(L"Top\\data.bin", false, 300000); add(L"Top\\Empty", true, 0); add(L"Top\\sub", true, 0); add(L"Top\\sub\\한글 file.txt", false, 11);
      require(clip::treeNeedsVirtual(staging, entries) && staging.wstring().size() > 300, "fixture reaches MAX_PATH");
      IDataObject* object = clip::createLocalTreeObject(entries);
      const fs::path destination = root / L"dropped"; fs::create_directories(destination);
      IShellItem* item = nullptr; require(SUCCEEDED(SHCreateItemFromParsingName(destination.c_str(), nullptr, IID_PPV_ARGS(&item))), "destination");
      IDropTarget* target = nullptr; require(SUCCEEDED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target))), "drop target");
      POINTL where{0, 0}; DWORD effect = DROPEFFECT_COPY; require(SUCCEEDED(target->DragEnter(object, MK_LBUTTON, where, &effect)), "DragEnter"); effect = DROPEFFECT_COPY;
      const HRESULT dropped = target->Drop(object, 0, where, &effect);
      // The shell copies on a background thread and calls back into this STA: keep pumping messages while it works.
      const auto pumpFor = [](std::chrono::milliseconds span) { const auto end = std::chrono::steady_clock::now() + span; MSG message; while (std::chrono::steady_clock::now() < end) { while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, 0); } };
      for (int i = 0; i < 100 && !(fs::exists(destination / L"Top" / L"sub" / L"한글 file.txt") && fs::exists(destination / L"Top" / L"data.bin") && fs::file_size(destination / L"Top" / L"data.bin") == 300000); ++i) pumpFor(100ms);
      pumpFor(300ms);
      require(SUCCEEDED(dropped), "the shell refused the drop of the local tree");
      { const auto content = slurp(destination / L"Top" / L"sub" / L"한글 file.txt");
        if (std::string(content.begin(), content.end()) != "korean name") { std::string seen; std::error_code ec; for (const auto& e : fs::recursive_directory_iterator(destination, ec)) seen += e.path().string() + (e.is_regular_file(ec) ? " [" + std::to_string(e.file_size(ec)) + "] " : " "); throw std::runtime_error("pasted small file differs (drop hr " + std::to_string(long(dropped)) + "): " + seen); } }
      require(fs::file_size(destination / L"Top" / L"data.bin") == 300000 && fs::is_directory(destination / L"Top" / L"Empty"), "pasted large file and the empty folder");
      target->Release(); item->Release(); object->Release();
      std::function<void(const fs::path&)> removeTree = [&](const fs::path& folder) {  // (std::filesystem cannot walk paths beyond MAX_PATH)
        WIN32_FIND_DATAW data{}; const HANDLE find = FindFirstFileExW(clip::longPath(folder / L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
        if (find != INVALID_HANDLE_VALUE) { do { const std::wstring name = data.cFileName; if (name == L"." || name == L"..") continue; const fs::path child = folder / name;
          if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { removeTree(child); } else DeleteFileW(clip::longPath(child).c_str()); } while (FindNextFileW(find, &data)); FindClose(find); }
        RemoveDirectoryW(clip::longPath(folder).c_str());
      };
      removeTree(root / L"localtree");
      ++checks;
    }
    Fixture fixture(root);

    // ---- The source offer: manifest, identity, registry -------------------------------------------------
    {
      const auto& items = fixture.offer->items();
      require(fixture.offer->tops() == 4 && fixture.offer->fileCount() == 5 && items.size() == 8, "manifest counts");
      require(items[0].kind == "dir" && items[0].name == L"Folder", "parents first");
      uint64_t sum = 0; for (const auto& item : items) if (item.kind == "file") sum += item.size;
      require(sum == fixture.offer->totalBytes(), "total bytes");
      bool oneMtime = false; for (size_t i = 0; i < items.size(); ++i) oneMtime |= fixture.offer->mtimeMs(i) > 1'000'000'000'000;
      require(oneMtime, "mtime in ms since 1970");
      clip::TreeScan big; big.bytes = clip::pushBelowBytes + 1; big.entries.resize(1);
      clip::TreeScan many; many.bytes = 5; many.entries.resize(clip::pushBelowEntries + 1);
      clip::TreeScan quick; quick.bytes = clip::pushBelowBytes; quick.entries.resize(clip::pushBelowEntries);
      require(clip::shouldOffer(big) && clip::shouldOffer(many) && !clip::shouldOffer(quick), "push/pull threshold");
      clip::OfferRegistry registry;
      for (int i = 1; i <= 3; ++i) registry.add(clip::SourceOffer::capture("o" + std::to_string(i), {fixture.source / L"top.bin"}, clip::Bounds{}));
      require(registry.size() == 2 && !registry.find("o1") && registry.find("o2") && registry.find("o3"), "latest two offers"); registry.remove("o2");
      require(registry.size() == 1, "remove"); registry.clear(); require(registry.size() == 0, "clear");
      require(clip::pullRequestId("pullc-1") && clip::pullRequestId("pullh-123456789") && !clip::pullRequestId("pull-1") && !clip::pullRequestId("pullh-0") && !clip::pullRequestId("pullh-01") && !clip::pullRequestId("pullc-1x") && !clip::pullRequestId("pullc-") && !clip::pullRequestId("pullx-1"), "pull id shape");
      checks += 7;
    }

    // ---- Reader: random offsets, identity and the range hash ------------------------------------------
    {
      const auto ordinalOfTop = [&] { size_t n = 0; for (const auto& item : fixture.offer->items()) { if (item.kind == "file") { if (item.name == L"top.bin") return n; ++n; } } throw std::runtime_error("top"); }();
      auto reader = fixture.offer->open(ordinalOfTop, 1000);
      require(reader->size() == fixture.top.size() && reader->position() == 1000, "reader position");
      std::vector<uint8_t> got; while (reader->position() < reader->size()) { auto part = reader->next(clip::maxChunkBytes); got.insert(got.end(), part.begin(), part.end()); }
      require(got == std::vector<uint8_t>(fixture.top.begin() + 1000, fixture.top.end()), "reader bytes from an offset");
      require(reader->digest() == sha256(got), "digest covers the delivered range");
      expectReason(reason([&] { fixture.offer->open(ordinalOfTop, fixture.top.size() + 1); }), "clipboard_chunk_out_of_order_or_bounds", "offset past the end");
      expectReason(reason([&] { fixture.offer->open(99, 0); }), "clipboard_transfer_not_active", "unknown ordinal");
      auto early = fixture.offer->open(ordinalOfTop, 0); early->next(10);
      expectReason(reason([&] { early->digest(); }), "clipboard_file_not_complete", "digest before the end");
      // A file locked for writing by another program.
      {
        const HANDLE writer = CreateFileW((fixture.source / L"Folder" / L"a.txt").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(writer != INVALID_HANDLE_VALUE, "lock a.txt");
        size_t ordinalOfA = 0; for (const auto& item : fixture.offer->items()) { if (item.kind == "file") { if (item.name == L"Folder\\a.txt") break; ++ordinalOfA; } }
        expectReason(reason([&] { fixture.offer->open(ordinalOfA, 0); }), "clipboard_file_locked", "file open elsewhere for writing");
        CloseHandle(writer);
      }
      checks += 6;
    }

    // ---- The user's own files stay theirs: rename, move, delete and edit during and right after a copy ---------------------
    {
      const fs::path base = root / L"user-files";
      const auto a = pseudo(300000, 11), b = pseudo(120000, 12), c = pseudo(70000, 13);
      write(base / L"Dir" / L"a.bin", a); write(base / L"Dir" / L"Sub" / L"b.bin", b); write(base / L"Dir" / L"c.bin", c);
      auto offer = clip::SourceOffer::capture("rename1", {base / L"Dir"}, clip::Bounds{});
      const auto ordinalOf = [&](const std::wstring& name) { size_t n = 0; for (const auto& item : offer->items()) { if (item.kind == "file") { if (item.name == name) return n; ++n; } } throw std::runtime_error("no such file"); };
      const auto readAll = [&](clip::SourceOffer::Reader& reader) { std::vector<uint8_t> got; while (reader.position() < reader.size()) { auto part = reader.next(clip::maxChunkBytes); got.insert(got.end(), part.begin(), part.end()); } return got; };
      // A file being read does not stop the user from renaming it, the folder above it, or from editing it.
      auto reading = offer->open(ordinalOf(L"Dir\\a.bin"), 0); auto firstPart = reading->next(1000);
      require(MoveFileExW((base / L"Dir").c_str(), (base / L"Renamed").c_str(), 0) != FALSE, "the folder of a file being read could not be renamed");
      auto rest = readAll(*reading); firstPart.insert(firstPart.end(), rest.begin(), rest.end());
      require(firstPart == a && reading->digest() == sha256(a), "a file renamed while it was read is read to its end");
      reading.reset();
      // Files not opened yet are found by their file index after the rename, and after a move to another folder of the volume.
      { auto later = offer->open(ordinalOf(L"Dir\\Sub\\b.bin"), 0); require(readAll(*later) == b && later->digest() == sha256(b), "a renamed folder's other file"); }
      fs::create_directories(base / L"Elsewhere");
      require(MoveFileExW((base / L"Renamed").c_str(), (base / L"Elsewhere" / L"Moved").c_str(), 0) != FALSE, "move after the copy");
      { auto later = offer->open(ordinalOf(L"Dir\\c.bin"), 1000); require(readAll(*later) == std::vector<uint8_t>(c.begin() + 1000, c.end()), "a moved folder's file from an offset"); }
      // A writer that gets in changes the file: it is never vouched for (and the writer is not refused either). Reads come in blocks
      // of readAheadChunks chunks, so the change is reported at the next block or, as here, at the digest.
      { const fs::path file = base / L"Elsewhere" / L"Moved" / L"c.bin";
        auto held = offer->open(ordinalOf(L"Dir\\c.bin"), 0); held->next(100);
        const HANDLE writer = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(writer != INVALID_HANDLE_VALUE, "a copy source must not be closed to writers"); DWORD done = 0; WriteFile(writer, "x", 1, &done, nullptr); CloseHandle(writer);
        expectReason(reason([&] { readAll(*held); held->digest(); }), "clipboard_selected_file_changed", "a file edited during the copy is reported before it is vouched for"); }
      expectReason(reason([&] { offer->open(ordinalOf(L"Dir\\c.bin"), 0); }), "clipboard_selected_file_changed", "a file edited after the copy");
      // Deleted after the copy (the whole folder), also while another file of it is open.
      { auto open = offer->open(ordinalOf(L"Dir\\a.bin"), 0); open->next(10);
        require(DeleteFileW((base / L"Elsewhere" / L"Moved" / L"Sub" / L"b.bin").c_str()) != FALSE, "delete of a source file while another is read");
        expectReason(reason([&] { offer->open(ordinalOf(L"Dir\\Sub\\b.bin"), 0); }), "clipboard_selected_file_changed", "a file deleted after the copy");
        require(readAll(*open).size() == a.size() - 10, "the file being read goes on"); }
      // A copied file is replaced by another file of the same name: the path now names a different file, the copied one is found by its index.
      { write(base / L"Swap" / L"s.bin", pseudo(5000, 21));
        auto swapOffer = clip::SourceOffer::capture("swap1", {base / L"Swap" / L"s.bin"}, clip::Bounds{});
        require(MoveFileExW((base / L"Swap" / L"s.bin").c_str(), (base / L"Swap" / L"kept.bin").c_str(), 0) != FALSE, "swap rename");
        write(base / L"Swap" / L"s.bin", pseudo(5000, 22));
        auto reader = swapOffer->open(0, 0); require(readAll(*reader) == pseudo(5000, 21), "the copied file, not the one that took its name"); }
      // The pushed (flat) source: a FileSource does not hold the file against rename or delete either.
      { const fs::path flat = base / L"flat.bin"; write(flat, pseudo(4000, 31));
        clip::FileSource source(flat); source.next(100);
        require(MoveFileExW(flat.c_str(), (base / L"flat-renamed.bin").c_str(), 0) != FALSE, "a pushed source file could not be renamed");
        require(DeleteFileW((base / L"flat-renamed.bin").c_str()) != FALSE, "a pushed source file could not be deleted"); }
      checks += 11;
    }

    // ---- The data object: formats, descriptor, no CF_HDROP ----------------------------------------------
    {
      Receiver r(fixture);
      IDataObject* data = r.offer->dataObject();
      FORMATETC fe{format(CFSTR_FILEDESCRIPTORW), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM medium{};
      require(SUCCEEDED(data->GetData(&fe, &medium)) && medium.tymed == TYMED_HGLOBAL, "descriptor");
      const auto* group = static_cast<const FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
      const auto& items = fixture.offer->items();
      require(group->cItems == items.size(), "descriptor count");
      size_t folders = 0;
      for (size_t i = 0; i < items.size(); ++i) {
        const FILEDESCRIPTORW& d = group->fgd[i]; const bool dir = items[i].kind == "dir";
        require(std::wstring(d.cFileName) == items[i].name, "descriptor name");
        require((d.dwFlags & FD_ATTRIBUTES) && (d.dwFlags & FD_WRITESTIME) && (d.dwFlags & FD_PROGRESSUI), "descriptor flags");
        require(bool(d.dwFlags & FD_FILESIZE) == !dir, "file size flag only for files");
        require(bool(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == dir, "directory attribute");
        if (!dir) require((uint64_t(d.nFileSizeHigh) << 32 | d.nFileSizeLow) == items[i].size, "descriptor size");
        else ++folders;
        const uint64_t ticks = (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
        require(int64_t((ticks - 116444736000000000ull) / 10000) == fixture.offer->mtimeMs(i), "descriptor write time");
      }
      require(folders == 3, "folders incl. the empty one are listed");
      GlobalUnlock(medium.hGlobal); ReleaseStgMedium(&medium);
      const auto dword = [&](const wchar_t* name) {
        FORMATETC f{format(name), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM m{};
        require(SUCCEEDED(data->GetData(&f, &m)), "dword format"); DWORD v = *static_cast<DWORD*>(GlobalLock(m.hGlobal)); GlobalUnlock(m.hGlobal); ReleaseStgMedium(&m); return v;
      };
      require(dword(CFSTR_PREFERREDDROPEFFECT) == DROPEFFECT_COPY && dword(CFSTR_ZONEIDENTIFIER) == 3 && dword(CFSTR_UNTRUSTEDDRAGDROP) == 3, "Preferred DropEffect copy; Internet zone");
      IEnumFORMATETC* enumerator = nullptr; require(SUCCEEDED(data->EnumFormatEtc(DATADIR_GET, &enumerator)), "enum");
      std::set<CLIPFORMAT> listed; FORMATETC each{}; ULONG fetched = 0; while (enumerator->Next(1, &each, &fetched) == S_OK) listed.insert(each.cfFormat);
      enumerator->Release();
      require(listed.count(format(CFSTR_FILEDESCRIPTORW)) && listed.count(format(CFSTR_FILECONTENTS)) && listed.count(format(CFSTR_PREFERREDDROPEFFECT)) && !listed.count(CF_HDROP), "offered formats (no CF_HDROP)");
      FORMATETC hdrop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM none{};
      require(data->QueryGetData(&hdrop) == DV_E_FORMATETC && FAILED(data->GetData(&hdrop, &none)), "CF_HDROP is not offered");
      FORMATETC wrongTymed{format(CFSTR_FILECONTENTS), nullptr, DVASPECT_CONTENT, 0, TYMED_HGLOBAL};
      require(data->QueryGetData(&wrongTymed) == DV_E_TYMED, "FileContents is a stream only");
      FORMATETC folder{format(CFSTR_FILECONTENTS), nullptr, DVASPECT_CONTENT, LONG(fixture.entry(L"Folder")), TYMED_ISTREAM}; STGMEDIUM nothing{};
      require(data->GetData(&folder, &nothing) == DV_E_LINDEX, "a folder has no contents");
      folder.lindex = 99; require(data->GetData(&folder, &nothing) == DV_E_LINDEX, "lindex out of range");
      FORMATETC performed{format(CFSTR_PERFORMEDDROPEFFECT), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM written{};
      written.tymed = TYMED_HGLOBAL; written.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
      require(data->SetData(&performed, &written, TRUE) == S_OK, "the target may report Performed DropEffect");
      IDataObjectAsyncCapability* async = nullptr; require(SUCCEEDED(data->QueryInterface(IID_PPV_ARGS(&async))), "IDataObjectAsyncCapability");
      BOOL mode = FALSE, inOperation = TRUE; require(async->GetAsyncMode(&mode) == S_OK && mode, "asynchronous extraction offered");
      require(async->StartOperation(nullptr) == S_OK && async->InOperation(&inOperation) == S_OK && inOperation, "start operation");
      require(async->EndOperation(S_OK, nullptr, DROPEFFECT_COPY) == S_OK && async->InOperation(&inOperation) == S_OK && !inOperation, "end operation");
      async->Release();
      require(r.link.requests == 0 && r.offer->deliveredBytes() == 0, "describing the offer pulls nothing");
      data->Release(); checks += 14;
    }

    // ---- Streams: content, hash, EOF, seek ------------------------------------------------------------
    {
      Receiver r(fixture); IDataObject* data = r.offer->dataObject();
      const struct { const wchar_t* name; const std::vector<uint8_t>* bytes; } files[] = {{L"Folder\\a.txt", &fixture.a}, {L"Folder\\Sub2\\deep.bin", &fixture.deep}, {L"top.bin", &fixture.top}, {L"한글 파일.txt", &fixture.korean}};
      for (const auto& file : files) {
        IStream* stream = streamOf(data, LONG(fixture.entry(file.name)));
        STATSTG stat{}; require(stream->Stat(&stat, STATFLAG_NONAME) == S_OK && stat.cbSize.QuadPart == file.bytes->size() && stat.type == STGTY_STREAM, "Stat");
        HRESULT result = S_OK; const auto got = drain(stream, result);
        require(SUCCEEDED(result) && got == *file.bytes, "stream content equals the file");
        ULONG more = 1; require(stream->Read(&more, 1, &more) == S_FALSE && more == 0, "end of stream reads zero bytes");
        stream->Release();
      }
      // An empty file needs no pull at all.
      const auto requestsBefore = r.link.requests.load();
      IStream* empty = streamOf(data, LONG(fixture.entry(L"empty.txt"))); HRESULT result = S_OK; require(drain(empty, result).empty() && SUCCEEDED(result), "empty file"); empty->Release();
      require(r.link.requests == requestsBefore, "an empty file is not pulled");
      require(r.link.badAcks == 0 && r.link.acks > 0, "acknowledgements came from consumption and named real chunk ends");
      // Seek: forward, backward, relative, from the end; every new position restarts the range.
      IStream* top = streamOf(data, LONG(fixture.entry(L"top.bin")));
      auto first = readExactly(top, 5000); require(first == std::vector<uint8_t>(fixture.top.begin(), fixture.top.begin() + 5000), "first bytes");
      ULARGE_INTEGER at{}; LARGE_INTEGER zero{}; require(top->Seek(zero, STREAM_SEEK_CUR, &at) == S_OK && at.QuadPart == 5000, "tell");
      const auto asked = r.link.requests.load(); auto next = readExactly(top, 1000);
      require(next == std::vector<uint8_t>(fixture.top.begin() + 5000, fixture.top.begin() + 6000) && r.link.requests == asked, "Seek(0, CUR) and a sequential read keep the range");
      LARGE_INTEGER to{}; to.QuadPart = 1'000'000; require(top->Seek(to, STREAM_SEEK_SET, &at) == S_OK && at.QuadPart == 1'000'000, "seek forward");
      auto middle = readExactly(top, 100000); require(middle == std::vector<uint8_t>(fixture.top.begin() + 1'000'000, fixture.top.begin() + 1'100'000) && r.link.requests == asked + 1, "read after a seek pulls from the new offset");
      to.QuadPart = -10; require(top->Seek(to, STREAM_SEEK_END, &at) == S_OK && at.QuadPart == fixture.top.size() - 10, "seek from the end");
      HRESULT tail = S_OK; auto last = drain(top, tail); require(SUCCEEDED(tail) && last == std::vector<uint8_t>(fixture.top.end() - 10, fixture.top.end()), "the tail range is verified and read");
      to.QuadPart = 0; require(top->Seek(to, STREAM_SEEK_SET, &at) == S_OK, "rewind");
      HRESULT again = S_OK; require(sha256(drain(top, again)) == sha256(fixture.top) && SUCCEEDED(again), "a rewound stream reads the whole file again");
      to.QuadPart = LONGLONG(fixture.top.size()) + 1; require(top->Seek(to, STREAM_SEEK_SET, &at) == STG_E_INVALIDFUNCTION, "seek past the end");
      // A move that would overflow int64 from CUR/END is refused, not wrapped into a valid-looking position.
      to.QuadPart = 0; top->Seek(to, STREAM_SEEK_SET, nullptr); to.QuadPart = 5; top->Seek(to, STREAM_SEEK_SET, nullptr);
      to.QuadPart = INT64_MAX; require(top->Seek(to, STREAM_SEEK_CUR, &at) == STG_E_INVALIDFUNCTION && top->Seek(to, STREAM_SEEK_END, &at) == STG_E_INVALIDFUNCTION, "seek overflow");
      to.QuadPart = INT64_MIN; require(top->Seek(to, STREAM_SEEK_CUR, &at) == STG_E_INVALIDFUNCTION && top->Seek(to, STREAM_SEEK_END, &at) == STG_E_INVALIDFUNCTION, "seek far before the start");
      // Clone and CopyTo.
      IStream* clone = nullptr; to.QuadPart = 0; top->Seek(to, STREAM_SEEK_SET, nullptr); require(top->Clone(&clone) == S_OK, "clone");
      IStream* memory = SHCreateMemStream(nullptr, 0); ULARGE_INTEGER all; all.QuadPart = ~0ull; ULARGE_INTEGER moved{}, wrote{};
      require(clone->CopyTo(memory, all, &moved, &wrote) == S_OK && moved.QuadPart == fixture.top.size() && wrote.QuadPart == moved.QuadPart, "CopyTo");
      memory->Release(); clone->Release(); top->Release();
      data->Release(); checks += 22;
    }

    // ---- Flow control: nothing is acknowledged until it is consumed ------------------------------------
    {
      Receiver r(fixture); IDataObject* data = r.offer->dataObject();
      IStream* top = streamOf(data, LONG(fixture.entry(L"top.bin")));
      auto head = readExactly(top, 1);  // starts the pull; the first chunk is only partly consumed
      require(waitFor([&] { return r.link.sentBytes + clip::formatsChunkBytes >= clip::SendWindow::defaultBytes; }, 4000ms), "the sender fills its window");
      std::this_thread::sleep_for(300ms);
      require(r.link.sentBytes <= clip::SendWindow::defaultBytes + clip::formatsChunkBytes && r.link.acks == 0, "an unread stream stalls the sender at its window");
      require(r.offer->deliveredBytes() <= clip::SendWindow::defaultBytes + clip::formatsChunkBytes, "the receiver buffers no more than the window");
      auto rest = readExactly(top, fixture.top.size() - 1);
      require(head[0] == fixture.top[0] && rest == std::vector<uint8_t>(fixture.top.begin() + 1, fixture.top.end()), "all bytes arrive once the reader consumes");
      HRESULT result = S_OK; ULONG got = 0; uint8_t one = 0; require(top->Read(&one, 1, &got) == S_FALSE && got == 0, "verified end"); (void)result;
      require(r.link.acks > 0 && r.link.badAcks == 0, "consumption acknowledged chunk ends cumulatively");
      require(r.offer->consumedBytes() == fixture.top.size(), "consumed counter");
      top->Release(); data->Release(); checks += 7;
    }

    // ---- Release, preemption and queue -----------------------------------------------------------------
    {
      Receiver r(fixture); IDataObject* data = r.offer->dataObject();
      IStream* top = streamOf(data, LONG(fixture.entry(L"top.bin")));
      readExactly(top, 1000); top->Release();
      require(waitFor([&] { return r.link.cancels == 1; }), "releasing a stream cancels its pull");
      // A reader that is not inside a Read gives way to a new one; its stream restarts where it was.
      IStream* a = streamOf(data, LONG(fixture.entry(L"top.bin"))); IStream* b = streamOf(data, LONG(fixture.entry(L"Folder\\Sub2\\deep.bin")));
      auto aHead = readExactly(a, 1000);
      HRESULT result = S_OK; require(drain(b, result) == fixture.deep && SUCCEEDED(result), "second stream while the first is idle");
      require(waitFor([&] { return r.link.cancels == 2; }), "the idle range was cancelled for the new reader");
      auto aRest = drain(a, result); aHead.insert(aHead.end(), aRest.begin(), aRest.end());
      require(SUCCEEDED(result) && aHead == fixture.top, "the preempted stream resumes from its position");
      a->Release(); b->Release();
      require(r.link.badAcks == 0, "no stale acknowledgement named a retired range"); data->Release(); checks += 5;
    }

    // ---- Failure: hash mismatch, timeout, changed file, revocation -----------------------------------------
    {
      Receiver r(fixture); IDataObject* data = r.offer->dataObject(); r.link.corruptDigest = true;
      IStream* stream = streamOf(data, LONG(fixture.entry(L"Folder\\Sub2\\deep.bin"))); HRESULT result = S_OK;
      const auto got = drain(stream, result);
      require(result == HRESULT_FROM_WIN32(ERROR_CRC) && got.size() <= fixture.deep.size(), "a hash mismatch fails the stream and drops what was not yet consumed");
      require(waitFor([&] { return r.link.cancels >= 1; }) && r.link.failedReason == "clipboard_transfer_hash_mismatch", "the application hears about it");
      ULONG count = 0; uint8_t one = 0; require(stream->Read(&one, 1, &count) == HRESULT_FROM_WIN32(ERROR_CRC), "the failure is sticky");
      stream->Release(); data->Release(); checks += 3;
    }
    {
      Receiver r(fixture, clip::PullTimeouts{300ms}); IDataObject* data = r.offer->dataObject(); r.link.silent = true;
      IStream* stream = streamOf(data, LONG(fixture.entry(L"top.bin"))); uint8_t one = 0; ULONG got = 0;
      const auto start = std::chrono::steady_clock::now();
      const HRESULT result = stream->Read(&one, 1, &got);
      const auto waited = std::chrono::steady_clock::now() - start;
      require(result == HRESULT_FROM_WIN32(ERROR_TIMEOUT) && waited >= 250ms && waited < 3000ms, "an unanswered pull times out");
      require(waitFor([&] { return r.link.cancels == 1; }) && r.link.failedReason == "clipboard_timeout", "the timeout cancels the pull");
      stream->Release(); data->Release(); checks += 2;
    }
    {
      Receiver r(fixture); IDataObject* data = r.offer->dataObject(); HRESULT result = S_OK;
      { std::ofstream append(fixture.source / L"Folder" / L"a.txt", std::ios::binary | std::ios::app); append << "!"; }
      IStream* changed = streamOf(data, LONG(fixture.entry(L"Folder\\a.txt")));
      drain(changed, result); require(result == HRESULT_FROM_WIN32(ERROR_FILE_INVALID), "a file changed since the copy fails that item"); changed->Release();
      write(fixture.source / L"Folder" / L"a.txt", fixture.a);  // same bytes, new write time: still a different file
      IStream* rewritten = streamOf(data, LONG(fixture.entry(L"Folder\\a.txt")));
      drain(rewritten, result); require(result == HRESULT_FROM_WIN32(ERROR_FILE_INVALID), "identical bytes written again are a change"); rewritten->Release();
      // The other items of the offer are unaffected.
      IStream* fine = streamOf(data, LONG(fixture.entry(L"한글 파일.txt"))); require(drain(fine, result) == fixture.korean && SUCCEEDED(result), "other items still work"); fine->Release();
      require(r.link.failedReason.empty(), "source-side failures arrive through fail(), not as local failures");
      fs::remove(fixture.source / L"한글 파일.txt");
      IStream* gone = streamOf(data, LONG(fixture.entry(L"한글 파일.txt"))); drain(gone, result);
      require(result == HRESULT_FROM_WIN32(ERROR_FILE_INVALID), "a deleted file is a changed file"); gone->Release();
      write(fixture.source / L"한글 파일.txt", fixture.korean);
      data->Release(); checks += 5;
    }
    {
      Receiver r(fixture); IDataObject* data = r.offer->dataObject();
      IStream* stream = streamOf(data, LONG(fixture.entry(L"top.bin"))); readExactly(stream, 1000);
      r.offer->shutdown("authorization_revoked");
      uint8_t one = 0; ULONG got = 0; require(stream->Read(&one, 1, &got) == HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED), "a revoked session stops the pull");
      // A shut-down offer is terminal: the object refuses later streams already at GetData (tree status), not only their reads.
      { FORMATETC fe{format(CFSTR_FILECONTENTS), nullptr, DVASPECT_CONTENT, LONG(fixture.entry(L"top.bin")), TYMED_ISTREAM}; STGMEDIUM medium{};
        require(data->GetData(&fe, &medium) == HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED), "and later streams"); }
      stream->Release(); data->Release(); checks += 2;
    }
    {
      // Wrong data from the offering side fails the range and tells the application.
      Receiver r(fixture); uint64_t id = 0; std::mutex m;
      auto hooks = r.link.hooks(); hooks.request = [&](uint64_t range, const std::string&, size_t, uint64_t) { std::lock_guard lock(m); id = range; };
      auto offer = clip::PullOffer::create("offer1", fixture.offer->items(), fixture.mtimes(), hooks); IDataObject* data = offer->dataObject();
      IStream* stream = streamOf(data, LONG(fixture.entry(L"Folder\\a.txt")));
      std::thread reader([&] { uint8_t buffer[16]; ULONG got = 0; const HRESULT result = stream->Read(buffer, sizeof(buffer), &got); require(result == E_FAIL || FAILED(result), "out-of-order data fails the read"); });
      require(waitFor([&] { std::lock_guard lock(m); return id != 0; }), "request");
      offer->deliver(id, 3, std::vector<uint8_t>{1, 2, 3});  // not at the range's next offset
      reader.join(); require(r.link.failedReason == "clipboard_chunk_out_of_order_or_bounds", "reason");
      stream->Release(); data->Release(); checks += 3;
    }
    require(std::set<std::string>{"clipboard_selected_file_changed"}.size() == 1, "reasons"); ++checks;

    // ---- The real OLE clipboard (optional) -----------------------------------------------------------
    if (useClipboard) {
      const HWND owner = CreateWindowExW(0, L"STATIC", L"QA", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
      require(owner != nullptr, "No message window");
      // The user's clipboard (text or file list) is put back at the end; other formats cannot be restored.
      std::optional<std::wstring> savedText; std::vector<fs::path> savedFiles;
      try { auto saved = clip::read(owner, true, true); savedText = saved.text; savedFiles = saved.files; } catch (...) {}
      Fixture real(root / L"real"); Receiver r(real); const auto before = GetClipboardSequenceNumber();
      const DWORD sequence = clip::publishPullOffer(r.offer, std::nullopt);
      require(sequence != before && clip::pullOfferIsCurrent(r.offer), "published and current (OleIsCurrentClipboard)");
      require(!IsClipboardFormatAvailable(CF_HDROP) && IsClipboardFormatAvailable(format(CFSTR_FILEDESCRIPTORW)) && IsClipboardFormatAvailable(format(CFSTR_FILECONTENTS)), "virtual files and no CF_HDROP on the clipboard");
      // Copying delivered nothing: the bytes are pulled by the paste.
      const auto pump = [](std::chrono::milliseconds span) { const auto end = std::chrono::steady_clock::now() + span; MSG message; while (std::chrono::steady_clock::now() < end) { while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, 0); } };
      pump(400ms); require(r.link.requests == 0 && r.link.sentBytes == 0, "no file was requested at copy time");
      const fs::path destination = root / L"pasted"; fs::create_directories(destination);
      std::atomic<bool> finished{false}; HRESULT dropResult = E_FAIL; std::string workerError;
      std::thread paste([&] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        try {
          IDataObject* data = nullptr; require(SUCCEEDED(OleGetClipboard(&data)) && data, "OleGetClipboard");
          IShellItem* item = nullptr; require(SUCCEEDED(SHCreateItemFromParsingName(destination.c_str(), nullptr, IID_PPV_ARGS(&item))), "destination item");
          IDropTarget* target = nullptr; require(SUCCEEDED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target))), "folder drop target");
          POINTL where{0, 0}; DWORD effect = DROPEFFECT_COPY;
          require(SUCCEEDED(target->DragEnter(data, MK_LBUTTON, where, &effect)), "DragEnter");
          effect = DROPEFFECT_COPY; dropResult = target->Drop(data, 0, where, &effect);
          // The shell copies on a background thread when the data object is asynchronous: wait for the files.
          const auto end = std::chrono::steady_clock::now() + 60s; const auto expected = real.offer->totalBytes();
          const auto sizeOf = [&] { uint64_t sum = 0; std::error_code ec; for (const auto& e : fs::recursive_directory_iterator(destination, ec)) if (e.is_regular_file(ec)) sum += e.file_size(ec); return sum; };
          while (std::chrono::steady_clock::now() < end && sizeOf() < expected) std::this_thread::sleep_for(50ms);
          std::this_thread::sleep_for(500ms);
          target->Release(); item->Release(); data->Release();
        } catch (const std::exception& error) { workerError = error.what(); }
        if (SUCCEEDED(com)) CoUninitialize();
        finished = true;
      });
      while (!finished) pump(50ms);
      paste.join();
      require(workerError.empty(), workerError.c_str());
      require(SUCCEEDED(dropResult), "Drop succeeded");
      // The extracted tree equals the source tree.
      size_t compared = 0, zoned = 0;
      for (const auto& item : real.offer->items()) {
        const fs::path target = destination / item.name;
        if (item.kind == "dir") { require(fs::is_directory(target), "folder extracted (empty ones too)"); continue; }
        const fs::path original = item.name == L"Folder\\a.txt" ? real.source / L"Folder" / L"a.txt" : real.source / item.name;
        require(fs::is_regular_file(target) && sha256(slurp(target)) == sha256(slurp(original)), "an extracted file differs from its source");
        ++compared;
        const HANDLE zone = CreateFileW((target.wstring() + L":Zone.Identifier").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (zone != INVALID_HANDLE_VALUE) { ++zoned; CloseHandle(zone); }
      }
      require(compared == 5, "every file was extracted");
      require(r.link.badAcks == 0 && r.link.requests >= 4 && r.link.sentBytes >= real.offer->totalBytes() - real.top.size() / 2, "the paste pulled the bytes through the link");
      require(clip::pullOfferIsCurrent(r.offer), "still ours after the paste (nothing was flushed)");
      // The same clipboard pasted a second way: the folder's own "paste" verb (what Explorer's Paste command invokes).
      {
        const fs::path second = root / L"pasted-verb"; fs::create_directories(second); std::atomic<bool> verbDone{false}; HRESULT verbResult = E_FAIL; std::string verbError;
        const auto pulledBefore = r.link.sentBytes.load();
        std::thread verb([&] {
          const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
          try {
            IShellItem* item = nullptr; require(SUCCEEDED(SHCreateItemFromParsingName(second.c_str(), nullptr, IID_PPV_ARGS(&item))), "verb destination");
            IContextMenu* menu = nullptr; require(SUCCEEDED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&menu))), "folder context menu");
            HMENU hmenu = CreatePopupMenu(); require(SUCCEEDED(menu->QueryContextMenu(hmenu, 0, 1, 0x7fff, CMF_NORMAL)), "QueryContextMenu");
            CMINVOKECOMMANDINFO invoke{}; invoke.cbSize = sizeof(invoke); invoke.lpVerb = "paste"; invoke.nShow = SW_HIDE; invoke.fMask = CMIC_MASK_FLAG_NO_UI | CMIC_MASK_NOASYNC;
            verbResult = menu->InvokeCommand(&invoke);
            const auto end = std::chrono::steady_clock::now() + 60s;
            const auto size = [&] { uint64_t sum = 0; std::error_code ec; for (const auto& e : fs::recursive_directory_iterator(second, ec)) if (e.is_regular_file(ec)) sum += e.file_size(ec); return sum; };
            while (std::chrono::steady_clock::now() < end && size() < real.offer->totalBytes()) std::this_thread::sleep_for(50ms);
            std::this_thread::sleep_for(500ms); DestroyMenu(hmenu); menu->Release(); item->Release();
          } catch (const std::exception& error) { verbError = error.what(); }
          if (SUCCEEDED(com)) CoUninitialize();
          verbDone = true;
        });
        while (!verbDone) pump(50ms);
        verb.join(); require(verbError.empty(), verbError.c_str());
        size_t verbCompared = 0;
        for (const auto& item : real.offer->items()) {
          if (item.kind == "dir") { require(fs::is_directory(second / item.name), "paste verb: folder missing"); continue; }
          const fs::path original = real.source / item.name;
          require(fs::is_regular_file(second / item.name) && sha256(slurp(second / item.name)) == sha256(slurp(original)), "paste verb: a file differs"); ++verbCompared;
        }
        std::cout << "{\"pasteVerb\":{\"invokeResult\":" << long(verbResult) << ",\"filesExtracted\":" << verbCompared << ",\"pulledBytes\":" << (r.link.sentBytes.load() - pulledBefore) << "}}\n";
        require(verbCompared == 5 && SUCCEEDED(verbResult), "the folder paste verb did not extract every file");
      }      std::cout << "{\"realClipboard\":{\"sequence\":" << sequence << ",\"requests\":" << r.link.requests.load() << ",\"filesExtracted\":" << compared << ",\"zoneMarked\":" << zoned << ",\"clipboardSequenceAfter\":" << GetClipboardSequenceNumber() << "}}\n";
      clip::withdrawPullOffer(r.offer); require(!IsClipboardFormatAvailable(format(CFSTR_FILEDESCRIPTORW)), "withdrawn");
      // Restore.
      if (!savedFiles.empty()) { try { clip::putPaths(owner, savedFiles, std::nullopt); } catch (...) {} }
      else if (savedText && !savedText->empty()) { try { clip::putText(owner, *savedText); } catch (...) {} }
      DestroyWindow(owner); checks += 8;
    }

    // A retry after clipboard contention must not overwrite a newer user copy.
    { DWORD sequence = 41; unsigned attempts = 0;
      bool changed = false;
      try { clip::detail::retryPublication(std::optional<DWORD>(41), 2000,
          [&] { return sequence; }, [&] { ++attempts; return CLIPBRD_E_CANT_OPEN; },
          [&](unsigned) { ++sequence; }); }
      catch (const std::runtime_error& error) { changed = std::string(error.what()) == "clipboard_changed"; }
      require(changed && attempts == 1, "OLE retry overwrote a copy made while clipboard was busy"); ++checks;
      attempts = 0;
      const HRESULT result = clip::detail::retryPublication(std::optional<DWORD>(sequence), 2000,
          [&] { return sequence; }, [&] { return ++attempts == 1 ? CLIPBRD_E_CANT_OPEN : S_OK; }, [](unsigned) {});
      require(result == S_OK && attempts == 2, "OLE retry did not recover from unchanged transient contention"); ++checks;
    }
    onPasteChecks(root, checks);
    fs::remove_all(root);
    OleUninitialize();
    std::cout << "{\"passed\":true,\"checks\":" << checks << ",\"clipboardTouched\":" << (useClipboard ? "true" : "false") << "}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
